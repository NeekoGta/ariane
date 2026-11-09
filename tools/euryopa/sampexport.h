#ifndef SAMPEXPORT_H
#define SAMPEXPORT_H

// SA-MP / open.mp Pawn export of the map changes.
//
// The changes are computed against the stock San Andreas map read straight from
// the game files (gta.dat, text IPLs, binary stream IPLs in the IMG archives),
// so they survive saves and editor restarts:
//   - a stock placement no longer in the scene  -> RemoveBuildingForPlayer
//   - a scene placement missing from stock map  -> CreateDynamicObject/CreateObject
// A moved or rotated stock object gives both.
//
// The core below has no dependency on librw or on the editor state, so it can
// be tested on its own (tests/test_samp_export.py).

#include <cstdio>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace SampExport {

struct Placement
{
	int modelId;
	float x, y, z;
	float qx, qy, qz, qw;	// IPL quaternion
	int interior;
};

struct StockInst
{
	Placement p;
	int lod;	// index in StockMap::insts, -1 if none
};

struct StockMap
{
	std::vector<StockInst> insts;
	std::unordered_set<int> definedModels;	// models of the stock IDE files
	int numTextFiles = 0;
	int numBinaryFiles = 0;
	std::vector<std::string> warnings;
	bool loaded = false;
};

// Opens a game file by its path relative to the game root (as written in
// gta.dat, for example "DATA\MAPS\LA\LAe.IPL"). Returns nil if missing.
typedef std::function<FILE*(const std::string &logicalPath)> OpenFileFn;

bool LoadStockMap(const OpenFileFn &open, StockMap &map);

struct SceneObject
{
	Placement p;
};

enum ObjectFunction
{
	FUNC_STREAMER = 0,	// CreateDynamicObject (Streamer plugin)
	FUNC_NATIVE		// CreateObject
};

struct Options
{
	ObjectFunction function = FUNC_STREAMER;
	bool removeOrphanLods = true;	// remove the LOD once all its stock children are removed
	bool useInterior = false;	// pass the IPL interior to CreateDynamicObject
	bool useArea = false;		// only the changes within radius of (areaX, areaY)
	float areaX = 0.0f, areaY = 0.0f, areaRadius = 100.0f;
	float removeRadius = 0.25f;
};

typedef std::function<std::string(int modelId)> ModelNameFn;

struct Result
{
	std::string code;
	int numRemoved = 0;
	int numLods = 0;
	int numCreated = 0;
	int numCustom = 0;	// models SA-MP cannot create without AddSimpleModel
};

Result BuildPawn(const StockMap &map, const std::vector<SceneObject> &scene,
                 const Options &options, const ModelNameFn &modelName);

// IPL quaternion to the SA-MP rotation (degrees, [0, 360)), as the game shows it:
// SA's IPL loader treats quaternions with tiny X/Y as heading only.
void QuatToSampEuler(float qx, float qy, float qz, float qw, float *rx, float *ry, float *rz);

// The SA-MP rotation (degrees) as an IPL quaternion, as open.mp's GTAQuat(Vector3).
void SampEulerToQuat(float rx, float ry, float rz, float *qx, float *qy, float *qz, float *qw);

}

// Editor window (sampexport_gui.cpp).
void uiSampExportWindow(bool *open);

#endif
