"""Runs ariane_mcp.py against a fake Ariane bridge, through MCP over stdio."""
from pathlib import Path
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

SERVER = Path(__file__).resolve().parent / 'ariane_mcp.py'


def write_bmp(path, width, height):
    row = (width * 3 + 3) & ~3
    pixels = bytearray()
    for y in range(height):	# bottom-up: first row is the bottom one
        line = bytearray()
        for x in range(width):
            line += bytes((0, 0, 255)) if y == 0 else bytes((255, 0, 0))	# BGR: bottom red, rest blue
        pixels += line + bytes(row - len(line))
    header = b'BM' + struct.pack('<IHHI', 54 + len(pixels), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    Path(path).write_bytes(header + info + bytes(pixels))


class FakeAriane(threading.Thread):
    def __init__(self, capture):
        super().__init__(daemon=True)
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.capture = capture
        self.requests = []
        self.next_id = 100

    def answer(self, name, args):
        if name == 'status':
            return {'ok': True, 'map': 'Garage', 'selected': 0}
        if name == 'place':
            if args['model'] == '-5':
                return {'ok': False, 'error': 'Unknown model -5.'}
            self.next_id += 1
            return {'ok': True, 'object': {'id': self.next_id, 'model': int(args['model'])}}
        if name == 'screenshot':
            return {'ok': True, 'path': self.capture, 'width': 4, 'height': 2}
        return {'ok': False, 'error': 'Unknown command %s.' % name}

    def run(self):
        conn, _ = self.listener.accept()
        buffer = b''
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buffer += data
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                fields = line.decode().split('\t')
                args = dict(f.split('=', 1) for f in fields[1:])
                self.requests.append((fields[0], args))
                conn.sendall((json.dumps(self.answer(fields[0], args)) + '\n').encode())
        conn.close()
        self.listener.close()


def png_pixels(data):
    """RGB rows of an 8-bit RGB, unfiltered PNG."""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, width = 8, b'', 0
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width = struct.unpack_from('>I', body)[0]
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = 1 + width * 3
    return [raw[i + 1:i + stride] for i in range(0, len(raw), stride)]


class McpTests(unittest.TestCase):
    def test_tools_over_stdio(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = str(Path(tmp) / 'capture.bmp')
            write_bmp(capture, 4, 2)
            fake = FakeAriane(capture)
            fake.start()
            proc = subprocess.Popen([sys.executable, str(SERVER)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    env={'ARIANE_BRIDGE_PORT': str(fake.port), 'SYSTEMROOT': os.environ.get('SYSTEMROOT', '')},
                                    text=True, encoding='utf-8')

            def rpc(msg_id, method, params=None):
                message = {'jsonrpc': '2.0', 'id': msg_id, 'method': method}
                if params is not None:
                    message['params'] = params
                proc.stdin.write(json.dumps(message) + '\n')
                proc.stdin.flush()
                return json.loads(proc.stdout.readline())

            try:
                init = rpc(1, 'initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                              'clientInfo': {'name': 'test', 'version': '1'}})
                self.assertEqual(init['result']['serverInfo']['name'], 'ariane')
                self.assertIn('tools', init['result']['capabilities'])
                proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')

                tools = rpc(2, 'tools/list')['result']['tools']
                names = {t['name'] for t in tools}
                self.assertIn('ariane_place', names)
                self.assertIn('ariane_set_material', names)
                self.assertTrue(all('command' not in t for t in tools))

                status = rpc(3, 'tools/call', {'name': 'ariane_status', 'arguments': {}})['result']
                self.assertFalse(status['isError'])
                self.assertEqual(json.loads(status['content'][0]['text'])['map'], 'Garage')

                placed = rpc(4, 'tools/call', {'name': 'ariane_place', 'arguments': {
                    'objects': [{'model': 1337, 'x': 1.5, 'y': 2, 'z': 3, 'rz': 90}, {'model': -5, 'x': 0, 'y': 0, 'z': 0}],
                    'focus': True}})['result']
                body = json.loads(placed['content'][0]['text'])
                self.assertEqual([o['id'] for o in body['placed']], [101])
                self.assertEqual(body['errors'][0]['index'], 1)
                self.assertEqual(fake.requests[1][1]['rz'], '90')
                self.assertEqual(fake.requests[1][1]['focus'], '0')	# only the last object
                self.assertEqual(fake.requests[2][1]['focus'], '1')
                self.assertNotIn('rx', fake.requests[1][1])

                shot = rpc(5, 'tools/call', {'name': 'ariane_screenshot', 'arguments': {'width': 640}})['result']
                image = shot['content'][0]
                self.assertEqual(image['mimeType'], 'image/png')
                rows = png_pixels(base64.b64decode(image['data']))
                self.assertEqual(len(rows), 2)
                self.assertEqual(rows[0][:3], bytes((0, 0, 255)))	# top row blue
                self.assertEqual(rows[1][:3], bytes((255, 0, 0)))	# bottom row red
                self.assertNotIn('path', json.loads(shot['content'][1]['text']))

                unknown = rpc(6, 'nope')
                self.assertEqual(unknown['error']['code'], -32601)
            finally:
                proc.stdin.close()
                proc.wait(timeout=10)
                proc.stdout.close()


if __name__ == '__main__':
    unittest.main()
