"""MCP server that lets an AI assistant (Claude Code...) drive Ariane.

Ariane must be running with Window > AI Bridge checked (the default): it listens
on 127.0.0.1:47811 (or ARIANE_BRIDGE_PORT). This script speaks MCP over stdio
and forwards each tool call to Ariane as one line, "command<TAB>key=value...",
then reads one JSON line back. Python 3.8+, standard library only.

Register it in Claude Code:
    claude mcp add ariane -- python D:/sa-mp/ariane/tools/mcp/ariane_mcp.py
"""
import base64
import json
import os
import socket
import struct
import sys
import zlib

PROTOCOL_VERSION = '2025-06-18'
HOST = '127.0.0.1'
PORT = int(os.environ.get('ARIANE_BRIDGE_PORT', '47811'))

INSTRUCTIONS = """Ariane is a GTA San Andreas map editor; these tools edit the map open in it, live.
- World units are metres, Z is up. Rotations are SA-MP angles in degrees (rx, ry, rz of CreateObject), unless rotation="ariane".
- Every change goes through Ariane's undo history (ariane_undo) and stays unsaved until ariane_save.
- Look before and after you edit: ariane_screenshot shows the 3D view; move the view with ariane_camera.
- Objects have an "id" (session-only, changes after a restart). Stock map objects can be moved, deleted or retextured too.
- Sizes: ariane_model_info gives the bounding box (bbox_min/bbox_max relative to the object origin) to align objects.
- Floor height: ariane_ground_z. Objects placed at the ground Z sink by their bbox_min.z if it is negative.
- Retexturing is SA-MP SetObjectMaterial: per object, per material index, a texture of a TXD (ariane_list_textures) and/or an ARGB colour."""


class BridgeError(Exception):
    pass


class Bridge:
    def __init__(self, host=HOST, port=PORT):
        self.address = (host, port)
        self.sock = None
        self.buffer = b''

    def _connect(self):
        try:
            self.sock = socket.create_connection(self.address, timeout=5)
        except OSError:
            self.sock = None
            raise BridgeError('Ariane is not reachable on %s:%d. Start Ariane and check Window > AI Bridge.'
                              % self.address)
        self.sock.settimeout(90)
        self.buffer = b''

    def call(self, command, **args):
        fields = [command]
        for key, value in args.items():
            if value is None:
                continue
            if isinstance(value, bool):
                value = '1' if value else '0'
            elif isinstance(value, (list, tuple)):
                value = ','.join(str(v) for v in value)
            text = str(value).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')
            fields.append('%s=%s' % (key, text))
        line = ('\t'.join(fields) + '\n').encode('utf-8')
        # A kept connection may have been closed by an Ariane restart: retry once.
        for attempt in range(2):
            reused = self.sock is not None
            if not reused:
                self._connect()
            try:
                self.sock.sendall(line)
                return self._read_line()
            except (OSError, BridgeError) as error:
                self.close()
                if not reused or attempt == 1:
                    raise BridgeError('Lost the connection to Ariane: %s' % error)
        raise BridgeError('unreachable')

    def _read_line(self):
        while b'\n' not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise BridgeError('Ariane closed the connection')
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b'\n', 1)
        return json.loads(line.decode('utf-8'))

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None


def bmp_to_png(path):
    """24-bit bottom-up BMP (as Ariane writes it) to PNG bytes."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        raise BridgeError('Not a BMP capture: %s' % path)
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp != 24:
        raise BridgeError('Unexpected BMP depth %d' % bpp)
    row = (width * 3 + 3) & ~3
    bottom_up = height > 0
    height = abs(height)
    raw = bytearray()
    for y in range(height):
        src_y = height - 1 - y if bottom_up else y
        start = offset + src_y * row
        bgr = data[start:start + width * 3]
        rgb = bytearray(len(bgr))
        rgb[0::3] = bgr[2::3]
        rgb[1::3] = bgr[1::3]
        rgb[2::3] = bgr[0::3]
        raw.append(0)
        raw += rgb

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + \
        chunk(b'IEND', b'')


# ------------------------------------------------------------------ tools

NUM = {'type': 'number'}
INT = {'type': 'integer'}
STR = {'type': 'string'}
BOOL = {'type': 'boolean'}
ROTATION_MODE = {'type': 'string', 'enum': ['samp', 'ariane'],
                 'description': 'samp (default): rx/ry/rz of CreateObject. ariane: the Transform panel angles.'}


def schema(properties, required=()):
    return {'type': 'object', 'properties': properties, 'required': list(required), 'additionalProperties': False}


def described(base, text):
    out = dict(base)
    out['description'] = text
    return out


TOOLS = [
    {
        'name': 'ariane_status',
        'description': 'State of the editor: game folder, current map, camera, counts of selected / added / modified / retextured objects.',
        'inputSchema': schema({}),
        'command': 'status',
    },
    {
        'name': 'ariane_camera',
        'description': 'Moves the 3D view. Give the camera position (x, y, z) and/or the point it looks at (tx, ty, tz). '
                       'With only a target, the view keeps its direction at "distance" metres. Returns the new camera.',
        'inputSchema': schema({'x': NUM, 'y': NUM, 'z': NUM, 'tx': NUM, 'ty': NUM, 'tz': NUM,
                               'distance': described(NUM, 'Metres from the target when only the target is given.'),
                               'fov': described(NUM, 'Field of view, 1 to 90 degrees.')}),
        'command': 'camera',
    },
    {
        'name': 'ariane_screenshot',
        'description': 'Captures the 3D view (without the editor windows) as an image. Use it to check your work.',
        'inputSchema': schema({'width': described(INT, 'Image width in pixels, 64 to 4096 (default 1280).'),
                               'frames': described(INT, 'Frames to draw first so that streamed models appear (default 3).')}),
        'command': 'screenshot',
    },
    {
        'name': 'ariane_find_objects',
        'description': 'Placed objects within radius of a point (default: the camera target), nearest first. '
                       'Each has id, model, name, pos, rot (SA-MP degrees) and flags (added, modified, lod, materials).',
        'inputSchema': schema({'x': NUM, 'y': NUM, 'z': NUM,
                               'radius': described(NUM, 'Metres (default 30).'),
                               'limit': described(INT, 'Maximum objects returned (default 100).'),
                               'model': described(INT, 'Only this model id.'),
                               'name': described(STR, 'Only models whose name contains this text.'),
                               'lods': described(BOOL, 'Include LOD objects (default false).'),
                               'added': described(BOOL, 'Only objects added in the editor.')}),
        'command': 'objects',
    },
    {
        'name': 'ariane_selection',
        'description': 'The objects selected in Ariane by the user.',
        'inputSchema': schema({}),
        'command': 'selection',
    },
    {
        'name': 'ariane_select',
        'description': 'Selects objects in Ariane (shows them to the user) and moves the view to the last one.',
        'inputSchema': schema({'ids': {'type': 'array', 'items': INT},
                               'add': described(BOOL, 'Keep the current selection.'),
                               'focus': described(BOOL, 'Move the view to the object (default true).')}, ['ids']),
        'command': 'select',
    },
    {
        'name': 'ariane_find_models',
        'description': 'Searches the models (object definitions) by name, for example "fence", "lamp", "bench". '
                       'Returns model id, name, txd and size (x, y, z in metres).',
        'inputSchema': schema({'query': STR, 'limit': described(INT, 'Default 50.'),
                               'lods': described(BOOL, 'Include LOD models.')}, ['query']),
        'command': 'find_models',
    },
    {
        'name': 'ariane_model_info',
        'description': 'One model: name, txd, bounding box relative to its origin, LOD model, and its materials '
                       '(texture names in material index order, for retexturing).',
        'inputSchema': schema({'model': INT, 'name': STR}),
        'command': 'model_info',
    },
    {
        'name': 'ariane_place',
        'description': 'Places objects (each is one undo step). Returns their ids. The LOD of a stock model is placed with it.',
        'inputSchema': schema({
            'objects': {'type': 'array', 'items': schema({'model': INT, 'x': NUM, 'y': NUM, 'z': NUM,
                                                           'rx': NUM, 'ry': NUM, 'rz': NUM},
                                                          ['model', 'x', 'y', 'z'])},
            'rotation': ROTATION_MODE,
            'focus': described(BOOL, 'Move the view to the last object.')}, ['objects']),
    },
    {
        'name': 'ariane_transform',
        'description': 'Moves or rotates one object: absolute x/y/z, relative dx/dy/dz, and rx/ry/rz (a missing angle keeps its value). '
                       'Its LOD follows.',
        'inputSchema': schema({'id': INT, 'x': NUM, 'y': NUM, 'z': NUM, 'dx': NUM, 'dy': NUM, 'dz': NUM,
                               'rx': NUM, 'ry': NUM, 'rz': NUM, 'rotation': ROTATION_MODE}, ['id']),
        'command': 'transform',
    },
    {
        'name': 'ariane_delete',
        'description': 'Deletes objects (one undo step). Deleting a stock object removes it from the map (RemoveBuildingForPlayer at export).',
        'inputSchema': schema({'ids': {'type': 'array', 'items': INT}}, ['ids']),
        'command': 'delete',
    },
    {
        'name': 'ariane_undo',
        'description': 'Undoes (or redoes) the last changes, as Ctrl+Z / Ctrl+Y.',
        'inputSchema': schema({'times': described(INT, 'Default 1.'), 'redo': BOOL}),
        'command': 'undo',
    },
    {
        'name': 'ariane_ground_z',
        'description': 'Height of the surface (ground, floor, roof) under a point. With z, the first surface below z '
                       '(a floor under a roof); without, the top surface.',
        'inputSchema': schema({'x': NUM, 'y': NUM, 'z': NUM}, ['x', 'y']),
        'command': 'ground',
    },
    {
        'name': 'ariane_save',
        'description': 'Saves the map (Ctrl+S): IPL files and the retextures. Ask the user before saving unless told to.',
        'inputSchema': schema({}),
        'command': 'save',
    },
    {
        'name': 'ariane_set_material',
        'description': 'Retextures one material of one object, as SetDynamicObjectMaterial(obj, index, model, txd, texture, color). '
                       'Give txd + texture (the model is found from the txd if missing), and/or color "0xAARRGGBB" '
                       '(0 keeps the model colour; colour only: no txd/texture).',
        'inputSchema': schema({'id': INT, 'index': described(INT, 'Material index, 0 to 15 (see ariane_model_info).'),
                               'model': INT, 'txd': STR, 'texture': STR,
                               'color': described(STR, 'ARGB, for example "0xFFFFFFFF".')}, ['id', 'index']),
        'command': 'material_set',
    },
    {
        'name': 'ariane_clear_material',
        'description': 'Removes the retexture of one material index, or all of them without index.',
        'inputSchema': schema({'id': INT, 'index': INT}, ['id']),
        'command': 'material_clear',
    },
    {
        'name': 'ariane_list_textures',
        'description': 'Textures of a TXD (or of the TXD of a model), to choose a retexture.',
        'inputSchema': schema({'txd': STR, 'model': INT, 'filter': described(STR, 'Only names containing this text.')}),
        'command': 'textures',
    },
]

TOOLS_BY_NAME = {tool['name']: tool for tool in TOOLS}


def text_result(data, error=False):
    return {'content': [{'type': 'text', 'text': json.dumps(data, ensure_ascii=False)}], 'isError': error}


def call_tool(bridge, name, args):
    tool = TOOLS_BY_NAME.get(name)
    if tool is None:
        return text_result({'error': 'Unknown tool %s' % name}, True)
    try:
        if name == 'ariane_place':
            return place(bridge, args)
        response = bridge.call(tool['command'], **args)
        if not response.get('ok'):
            return text_result({'error': response.get('error', 'failed')}, True)
        response.pop('ok', None)
        if name == 'ariane_screenshot':
            png = bmp_to_png(response['path'])
            info = {k: v for k, v in response.items() if k != 'path'}
            return {'content': [{'type': 'image', 'data': base64.b64encode(png).decode('ascii'), 'mimeType': 'image/png'},
                                {'type': 'text', 'text': json.dumps(info)}], 'isError': False}
        return text_result(response)
    except BridgeError as error:
        return text_result({'error': str(error)}, True)


def place(bridge, args):
    placed, errors = [], []
    objects = args.get('objects') or []
    for i, obj in enumerate(objects):
        response = bridge.call('place', model=obj.get('model'), x=obj.get('x'), y=obj.get('y'), z=obj.get('z'),
                               rx=obj.get('rx'), ry=obj.get('ry'), rz=obj.get('rz'),
                               rotation=args.get('rotation'),
                               focus=bool(args.get('focus')) and i == len(objects) - 1)
        if response.get('ok'):
            placed.append(response['object'])
        else:
            errors.append({'index': i, 'error': response.get('error')})
    return text_result({'placed': placed, 'errors': errors}, error=not placed and bool(errors))


# ------------------------------------------------------------------ MCP stdio

def send(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False) + '\n')
    sys.stdout.flush()


def handle(bridge, message):
    method = message.get('method')
    msg_id = message.get('id')
    params = message.get('params') or {}
    if msg_id is None:
        return None	# notification
    if method == 'initialize':
        return {'jsonrpc': '2.0', 'id': msg_id, 'result': {
            'protocolVersion': params.get('protocolVersion', PROTOCOL_VERSION),
            'capabilities': {'tools': {}},
            'serverInfo': {'name': 'ariane', 'version': '1.0.0'},
            'instructions': INSTRUCTIONS,
        }}
    if method == 'ping':
        return {'jsonrpc': '2.0', 'id': msg_id, 'result': {}}
    if method == 'tools/list':
        tools = [{k: v for k, v in tool.items() if k != 'command'} for tool in TOOLS]
        return {'jsonrpc': '2.0', 'id': msg_id, 'result': {'tools': tools}}
    if method == 'tools/call':
        result = call_tool(bridge, params.get('name'), params.get('arguments') or {})
        return {'jsonrpc': '2.0', 'id': msg_id, 'result': result}
    return {'jsonrpc': '2.0', 'id': msg_id, 'error': {'code': -32601, 'message': 'Method not found: %s' % method}}


def main():
    sys.stdin.reconfigure(encoding='utf-8')
    sys.stdout.reconfigure(encoding='utf-8', newline='\n')
    bridge = Bridge()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            send({'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'Parse error'}})
            continue
        try:
            response = handle(bridge, message)
        except Exception as error:	# keep serving: one bad call must not kill the server
            response = {'jsonrpc': '2.0', 'id': message.get('id'),
                        'error': {'code': -32603, 'message': '%s: %s' % (type(error).__name__, error)}}
        if response is not None:
            send(response)


if __name__ == '__main__':
    main()
