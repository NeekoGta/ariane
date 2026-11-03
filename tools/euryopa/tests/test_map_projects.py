"""Compile the map folders switching against a temporary game folder."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
EURYOPA = ROOT / 'tools/euryopa'

# Stands in for euryopa.h: only the path helpers mapprojects.cpp uses.
STUB_HEADER = r'''
#pragma once
#include <cstdio>
#include <cstring>
#include <string>
#define nil nullptr
extern std::string gGameRoot, gDataDir;
inline bool GetGameRootDirectory(char *dir, size_t size) { snprintf(dir, size, "%s", gGameRoot.c_str()); return true; }
inline bool GetArianeDataDirectory(char *dir, size_t size) { snprintf(dir, size, "%s", gDataDir.c_str()); return true; }
inline bool BuildPath(char *dst, size_t size, const char *dir, const char *name) { return snprintf(dst, size, "%s/%s", dir, name) < (int)size; }
inline bool GetArianeDataPath(char *dst, size_t size, const char *name) { return BuildPath(dst, size, gDataDir.c_str(), name); }
bool EnsureParentDirectoriesForPath(const char *path);
'''

FIXTURE = r'''
#include "mapprojects.h"
#include "euryopa.h"
#include <cassert>
#include <fstream>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif
using namespace MapProjects;

std::string gGameRoot, gDataDir;

bool EnsureParentDirectoriesForPath(const char *path)
{
	std::string parent = path;
	parent = parent.substr(0, parent.find_last_of('/'));
	MKDIR(parent.c_str());
	return true;
}

static void writeFile(const std::string &path, const char *text) { std::ofstream(path) << text; }
static std::string readFile(const std::string &path)
{
	std::ifstream f(path);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv)
{
	std::string tmp = argv[1];
	gGameRoot = tmp + "/game";
	gDataDir = gGameRoot + "/ariane";
	std::string open = gGameRoot + "/modloader/Ariane";
	std::string error;

	MKDIR(gGameRoot.c_str());
	MKDIR(gDataDir.c_str());
	assert(!IsAvailable());	// no modloader folder
	MKDIR((gGameRoot + "/modloader").c_str());
	assert(IsAvailable());

	assert(IsValidName("Hopital LS-2_b") && !IsValidName("") && !IsValidName("a/b") && !IsValidName(" a") && !IsValidName(".."));

	// Edits saved before maps existed: unnamed, a name is required to store them.
	MKDIR(open.c_str());
	writeFile(open + "/lan.ipl", "hospital");
	assert(CurrentName().empty() && HasOpenMapFiles());
	assert(!Switch("", nullptr, "Garage", error));
	assert(HasOpenMapFiles() && StoredMaps().empty());

	// New map: the open one is stored, nothing is open, the new name is current.
	assert(Switch("Hopital", nullptr, "Garage", error));
	assert(!HasOpenMapFiles() && CurrentName() == "Garage");
	assert(StoredMaps().size() == 1 && StoredMaps()[0] == "Hopital");
	assert(readFile(gDataDir + "/maps/Hopital/lan.ipl") == "hospital");

	// The new map saved, then the stored one opened again.
	MKDIR(open.c_str());
	writeFile(open + "/garage.ipl", "garage");
	assert(!Switch("", nullptr, "hopital", error));	// name taken (case-insensitive)
	assert(Switch("", "Hopital", nullptr, error));
	assert(CurrentName() == "Hopital" && readFile(open + "/lan.ipl") == "hospital");
	assert(StoredMaps().size() == 1 && StoredMaps()[0] == "Garage");
	assert(readFile(gDataDir + "/maps/Garage/garage.ipl") == "garage");

	// Rename, missing map, back to the garage.
	assert(!RenameCurrent("Garage", error));
	assert(RenameCurrent("Hopital LS", error) && CurrentName() == "Hopital LS");
	assert(!Switch("", "Nothing", nullptr, error) && CurrentName() == "Hopital LS" && HasOpenMapFiles());
	assert(Switch("", "Garage", nullptr, error));
	assert(readFile(open + "/garage.ipl") == "garage" && StoredMaps()[0] == "Hopital LS");

	printf("ok\n");
	return 0;
}
'''


class MapProjectsTests(unittest.TestCase):
    def test_switching_map_folders(self):
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / 'stub'
            stub.mkdir()
            (stub / 'euryopa.h').write_text(STUB_HEADER)
            main = Path(tmp) / 'main.cpp'
            main.write_text(FIXTURE)
            exe = Path(tmp) / ('map_projects_test.exe' if os.name == 'nt' else 'map_projects_test')
            cxx = os.environ.get('CXX', 'c++')
            # A copy next to the stub: a quoted include looks in its own folder first.
            for name in ('mapprojects.cpp', 'mapprojects.h'):
                (stub / name).write_bytes((EURYOPA / name).read_bytes())
            sources = [str(main), str(stub / 'mapprojects.cpp')]
            if Path(cxx).stem.lower() == 'cl':
                command = [cxx, '/nologo', '/EHsc', '/std:c++14', '/I', str(stub)] + sources + \
                          ['/Fe' + str(exe), '/Fo' + tmp + '\\']
            else:
                command = [cxx, '-std=c++11', '-I', str(stub)] + sources + ['-o', str(exe)]
            subprocess.run(command, check=True)
            work = Path(tmp) / 'work'
            work.mkdir()
            result = subprocess.run([str(exe), str(work)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
