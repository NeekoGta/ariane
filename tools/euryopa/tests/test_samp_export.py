"""Compile the SA-MP export core against a small stock map written to disk."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
EURYOPA = ROOT / 'tools/euryopa'

GTA_DAT = 'IDE DATA\\MAPS\\TEST.IDE\nIPL DATA\\MAPS\\TEST.IPL\n'
TEST_IDE = 'objs\n1000, hq, hq, 100, 0\n1001, lod, lod, 1000, 0\n1002, box, box, 100, 0\n1003, rock, rock, 100, 0\nend\n'
TEST_IPL = '''inst
1001, lod, 0, 100, 100, 10, 0, 0, 0, 1, -1
# a commented placement takes no ordinal
1000, hq, 0, 100, 100, 10, 0, 0, 0, 1, 0
1002, box, 0, 200, 200, 5, 0, 0, 0.7071068, 0.7071068, -1
9999, undefined, 0, 1, 1, 1, 0, 0, 0, 1, -1
end
'''


def binary_ipl(insts):
    header = bytearray(0x4C)
    struct.pack_into('<IH', header, 0, 0x79726E62, len(insts))
    struct.pack_into('<I', header, 0x1C, 0x4C)
    body = b''.join(struct.pack('<7f3i', *inst) for inst in insts)
    return bytes(header) + body


def img_archive(entries):
    directory = bytearray()
    data = bytearray()
    first = (8 + 32 * len(entries) + 2047) // 2048
    for name, content in entries:
        sectors = (len(content) + 2047) // 2048
        directory += struct.pack('<IHH24s', first + len(data) // 2048, sectors, 0, name.encode())
        data += content + bytes(sectors * 2048 - len(content))
    head = b'VER2' + struct.pack('<I', len(entries)) + bytes(directory)
    return head + bytes(first * 2048 - len(head)) + bytes(data)


FIXTURE = r'''
#include "sampexport.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
using namespace SampExport;

static std::string gRoot;

static FILE *openFile(const std::string &logical)
{
	std::string path = gRoot + "/" + logical;
	for(char &c : path) if(c == '\\') c = '/';
	return fopen(path.c_str(), "rb");
}

static bool has(const Result &r, const char *text) { return r.code.find(text) != std::string::npos; }
static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static SceneObject obj(int id, float x, float y, float z, float qx, float qy, float qz, float qw)
{
	SceneObject o;
	o.p = { id, x, y, z, qx, qy, qz, qw, 0 };
	return o;
}

// open.mp's GTAQuat(Vector3 degrees), to check the round trip.
static void eulerToQuat(float x, float y, float z, float q[4])
{
	const float r = 3.14159265f / 180.0f * -0.5f;
	float cx = cosf(x*r), cy = cosf(y*r), cz = cosf(z*r);
	float sx = sinf(x*r), sy = sinf(y*r), sz = sinf(z*r);
	q[3] = cx*cy*cz + sx*sy*sz;
	q[0] = cx*sy*sz + sx*cy*cz;
	q[1] = cx*sy*cz - sx*cy*sz;
	q[2] = cx*cy*sz - sx*sy*cz;
}

int main(int argc, char **argv)
{
	gRoot = argv[1];
	StockMap map;
	assert(LoadStockMap(openFile, map));
	assert(map.numTextFiles == 1 && map.numBinaryFiles == 1);	// crack.ipl: missions only
	assert(map.insts.size() == 5);	// the undefined model is skipped
	assert(map.insts[1].lod == 0);	// text LOD ordinal, the comment does not count
	assert(map.insts[4].lod == 0);	// stream LOD reference into test.ipl

	ModelNameFn name = [](int id) { return std::string("m") + std::to_string(id); };
	std::vector<SceneObject> stock = {
		obj(1001, 100, 100, 10, 0, 0, 0, 1),
		obj(1000, 100, 100, 10, 0, 0, 0, 1),
		obj(1002, 200, 200, 5, 0, 0, 0.7071068f, 0.7071068f),
		obj(1003, 300, 300, 1, 0, 0, 0, 1),
		obj(1000, 110, 100, 10, 0, 0, 0, 1),
	};
	Options options;

	// Unchanged scene.
	Result r = BuildPawn(map, stock, options, name);
	assert(r.numRemoved == 0 && r.numCreated == 0 && has(r, "No change"));

	// One child of the LOD removed: the LOD stays.
	std::vector<SceneObject> scene = stock;
	scene.erase(scene.begin() + 1);
	r = BuildPawn(map, scene, options, name);
	assert(r.numRemoved == 1 && r.numLods == 0);
	assert(has(r, "RemoveBuildingForPlayer(playerid, 1000, 100.0000, 100.0000, 10.0000, 0.25); // m1000"));

	// Both children removed: the LOD goes too, unless disabled.
	scene.pop_back();
	r = BuildPawn(map, scene, options, name);
	assert(r.numRemoved == 2 && r.numLods == 1 && has(r, "RemoveBuildingForPlayer(playerid, 1001,") && has(r, "(LOD)"));
	options.removeOrphanLods = false;
	r = BuildPawn(map, scene, options, name);
	assert(r.numLods == 0);
	options.removeOrphanLods = true;

	// Moved: removed at the stock place, created at the new one, rotation kept.
	scene = stock;
	scene[2].p.x = 201.0f;
	r = BuildPawn(map, scene, options, name);
	assert(r.numRemoved == 1 && r.numCreated == 1);
	assert(has(r, "CreateDynamicObject(1002, 201.0000, 200.0000, 5.0000, 0.0000, 0.0000, 270.0000); // m1002"));

	// Rotated in place only.
	scene = stock;
	scene[3].p.qz = -0.7071068f;
	scene[3].p.qw = 0.7071068f;
	r = BuildPawn(map, scene, options, name);
	assert(r.numRemoved == 1 && r.numCreated == 1 && has(r, "CreateDynamicObject(1003, 300.0000, 300.0000, 1.0000, 0.0000, 0.0000, 90.0000)"));
	options.function = FUNC_NATIVE;
	r = BuildPawn(map, scene, options, name);
	assert(has(r, "CreateObject(1003, 300.0000, 300.0000, 1.0000, 0.0000, 0.0000, 90.0000); // m1003"));
	options.function = FUNC_STREAMER;

	// Tiny position noise is not a change.
	scene = stock;
	scene[3].p.x += 0.005f;
	assert(BuildPawn(map, scene, options, name).numCreated == 0);

	// New objects: stock model, custom model (the editor imports them from 18631) commented out.
	scene = stock;
	scene.push_back(obj(1002, 10, 20, 30, 0, 0, 0, 1));
	scene.push_back(obj(18631, 11, 21, 31, 0, 0, 0, 1));
	r = BuildPawn(map, scene, options, name);
	assert(r.numCreated == 1 && r.numCustom == 1);
	assert(has(r, "\nCreateDynamicObject(1002, 10.0000,") && has(r, "\n// CreateDynamicObject(18631,") && has(r, "AddSimpleModel"));

	// Area filter.
	options.useArea = true;
	options.areaX = 300.0f; options.areaY = 300.0f; options.areaRadius = 50.0f;
	r = BuildPawn(map, scene, options, name);
	assert(r.numCreated == 0 && r.numCustom == 0);
	options.useArea = false;

	// Rotations: heading only, then full 3D round trips through open.mp's formulas.
	float rx, ry, rz;
	QuatToSampEuler(0, 0, -0.7071068f, 0.7071068f, &rx, &ry, &rz);
	assert(near(rx, 0) && near(ry, 0) && near(rz, 90));
	const float cases[][3] = { { 30, 0, 0 }, { 10, 20, 30 }, { 0, 45, 90 }, { 300, 15, 200 } };
	for(const auto &c : cases){
		float q[4];
		eulerToQuat(c[0], c[1], c[2], q);
		QuatToSampEuler(q[0], q[1], q[2], q[3], &rx, &ry, &rz);
		assert(near(rx, c[0]) && near(ry, c[1]) && near(rz, c[2]));
	}

	printf("ok\n");
	return 0;
}
'''


class SampExportTests(unittest.TestCase):
    def test_stock_diff_and_pawn(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'gta'
            (root / 'DATA/MAPS').mkdir(parents=True)
            (root / 'MODELS').mkdir()
            (root / 'DATA/GTA.DAT').write_text(GTA_DAT)
            (root / 'DATA/MAPS/TEST.IDE').write_text(TEST_IDE)
            (root / 'DATA/MAPS/TEST.IPL').write_text(TEST_IPL)
            stream = binary_ipl([(300, 300, 1, 0, 0, 0, 1, 1003, 0, -1),
                                 (110, 100, 10, 0, 0, 0, 1, 1000, 0, 0)])
            mission = binary_ipl([(400, 400, 1, 0, 0, 0, 1, 1003, 0, -1)])
            (root / 'MODELS/GTA3.IMG').write_bytes(img_archive([('test_stream0.ipl', stream), ('crack.ipl', mission)]))

            main = Path(tmp) / 'main.cpp'
            main.write_text(FIXTURE)
            exe = Path(tmp) / ('samp_export_test.exe' if os.name == 'nt' else 'samp_export_test')
            cxx = os.environ.get('CXX', 'c++')
            if Path(cxx).stem.lower() == 'cl':
                command = [cxx, '/nologo', '/EHsc', '/std:c++14', '/I', str(EURYOPA), str(main),
                           str(EURYOPA / 'sampexport.cpp'), '/Fe' + str(exe), '/Fo' + tmp + '\\']
            else:
                command = [cxx, '-std=c++11', '-I', str(EURYOPA), str(main),
                           str(EURYOPA / 'sampexport.cpp'), '-o', str(exe)]
            subprocess.run(command, check=True)
            result = subprocess.run([str(exe), str(root)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
