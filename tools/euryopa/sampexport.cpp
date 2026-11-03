#include "sampexport.h"

#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace SampExport {

static const int SAMP_FIRST_MODEL = 18631;	// SA-MP's own objects (samp.img)
static const int SAMP_LAST_MODEL = 19999;
static const float MATCH_DISTANCE = 0.02f;
static const float MATCH_ROTATION = 0.9999f;	// |dot| of the quaternions, about 1.6 degrees
static const float GRID_CELL = 8.0f;

static std::string
lower(std::string s)
{
	for(char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

static std::string
trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while(a < b && isspace((unsigned char)s[a])) a++;
	while(b > a && isspace((unsigned char)s[b-1])) b--;
	return s.substr(a, b - a);
}

// "DATA\MAPS\LA\LAe.IPL" -> "lae"
static std::string
baseName(const std::string &path)
{
	size_t slash = path.find_last_of("\\/");
	std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	size_t dot = name.find_last_of('.');
	if(dot != std::string::npos)
		name = name.substr(0, dot);
	return lower(name);
}

static bool
readAll(FILE *f, std::string &out)
{
	char buf[16384];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

static void
forEachLine(const std::string &text, const std::function<void(std::string)> &fn)
{
	size_t start = 0;
	while(start < text.size()){
		size_t end = text.find('\n', start);
		if(end == std::string::npos) end = text.size();
		fn(trim(text.substr(start, end - start)));
		start = end + 1;
	}
}

// ------------------------------------------------------------------ Stock map

struct Loader
{
	const OpenFileFn &open;
	StockMap &map;
	// Text IPL basename -> global index of each of its placement ordinals
	// (-1 for a skipped line): the LOD references of the binary streams.
	std::unordered_map<std::string, std::vector<int>> textOrdinals;

	bool isDefined(int modelId) const
	{
		return map.definedModels.empty() || map.definedModels.count(modelId) != 0;
	}

	void loadDat(const char *path, std::vector<std::string> &ides, std::vector<std::string> &ipls,
	             std::vector<std::string> &imgs)
	{
		FILE *f = open(path);
		if(f == nullptr)
			return;
		std::string text;
		readAll(f, text);
		forEachLine(text, [&](std::string line) {
			if(line.empty() || line[0] == '#')
				return;
			size_t space = line.find_first_of(" \t");
			if(space == std::string::npos)
				return;
			std::string type = lower(line.substr(0, space));
			std::string arg = trim(line.substr(space));
			if(type == "ide") ides.push_back(arg);
			else if(type == "ipl") ipls.push_back(arg);
			else if(type == "img" || type == "cdimage") imgs.push_back(arg);
		});
	}

	void loadIde(const std::string &path)
	{
		FILE *f = open(path);
		if(f == nullptr){
			map.warnings.push_back("Missing IDE: " + path);
			return;
		}
		std::string text;
		readAll(f, text);
		std::string section;
		forEachLine(text, [&](std::string line) {
			if(line.empty() || line[0] == '#')
				return;
			std::string word = lower(line.substr(0, line.find_first_of(" \t,")));
			if(section.empty()){
				section = word;
				return;
			}
			if(word == "end"){
				section.clear();
				return;
			}
			if(section == "objs" || section == "tobj" || section == "anim" || section == "hier"){
				int id;
				if(sscanf(line.c_str(), "%d", &id) == 1)
					map.definedModels.insert(id);
			}
		});
	}

	void loadTextIpl(const std::string &path)
	{
		FILE *f = open(path);
		if(f == nullptr){
			map.warnings.push_back("Missing IPL: " + path);
			return;
		}
		map.numTextFiles++;
		std::string text;
		readAll(f, text);
		size_t first = map.insts.size();
		std::vector<int> &ordinals = textOrdinals[baseName(path)];
		ordinals.clear();
		std::string section;
		forEachLine(text, [&](std::string line) {
			// The game skips the commented lines: they take no ordinal.
			if(line.empty() || line[0] == '#')
				return;
			std::string word = lower(line.substr(0, line.find_first_of(" \t,")));
			if(section.empty()){
				section = word;
				return;
			}
			if(word == "end"){
				section.clear();
				return;
			}
			if(section != "inst")
				return;
			for(char &c : line)
				if(c == ',') c = ' ';
			StockInst inst = {};
			char model[64];
			int n = sscanf(line.c_str(), "%d %63s %d %f %f %f %f %f %f %f %d",
			               &inst.p.modelId, model, &inst.p.interior,
			               &inst.p.x, &inst.p.y, &inst.p.z,
			               &inst.p.qx, &inst.p.qy, &inst.p.qz, &inst.p.qw, &inst.lod);
			if(n != 11 || !isDefined(inst.p.modelId)){
				ordinals.push_back(-1);
				return;
			}
			inst.p.interior &= 0xFF;
			ordinals.push_back((int)map.insts.size());
			map.insts.push_back(inst);
		});
		for(size_t i = first; i < map.insts.size(); i++){
			int lod = map.insts[i].lod;
			map.insts[i].lod = lod >= 0 && lod < (int)ordinals.size() ? ordinals[lod] : -1;
		}
	}

	void loadBinaryIpl(const std::string &entryName, const std::vector<uint8_t> &data)
	{
		uint32_t fourcc, offsetInst;
		uint16_t numInst;
		if(data.size() < 0x4C)
			return;
		memcpy(&fourcc, &data[0], 4);
		memcpy(&numInst, &data[4], 2);
		memcpy(&offsetInst, &data[0x1C], 4);
		if(fourcc != 0x79726E62)	// "bnry"
			return;
		map.numBinaryFiles++;

		// "lae_stream0" references the placements of the text IPL "lae".
		std::string base = baseName(entryName);
		size_t stream = base.find("_stream");
		const std::vector<int> *ordinals = nullptr;
		if(stream != std::string::npos){
			auto it = textOrdinals.find(base.substr(0, stream));
			if(it != textOrdinals.end())
				ordinals = &it->second;
		}

		for(uint32_t i = 0; i < numInst; i++){
			size_t at = offsetInst + (size_t)i * 40;
			if(at + 40 > data.size())
				break;
			float v[7];
			int32_t ints[3];
			memcpy(v, &data[at], 28);
			memcpy(ints, &data[at + 28], 12);
			if(!isDefined(ints[0]))
				continue;
			StockInst inst;
			inst.p = { ints[0], v[0], v[1], v[2], v[3], v[4], v[5], v[6], ints[1] & 0xFF };
			inst.lod = ordinals && ints[2] >= 0 && ints[2] < (int)ordinals->size() ? (*ordinals)[ints[2]] : -1;
			map.insts.push_back(inst);
		}
	}

	void loadImage(const std::string &path, bool required)
	{
		FILE *f = open(path);
		if(f == nullptr){
			if(required)
				map.warnings.push_back("Missing IMG: " + path);
			return;
		}
		char magic[4];
		uint32_t count = 0;
		if(fread(magic, 1, 4, f) != 4 || memcmp(magic, "VER2", 4) != 0 || fread(&count, 4, 1, f) != 1){
			map.warnings.push_back("Not a VER2 IMG archive: " + path);
			fclose(f);
			return;
		}
		struct Entry { uint32_t offset; uint16_t streamSize, archiveSize; char name[24]; };
		std::vector<Entry> entries(count);
		if(count > 0 && fread(&entries[0], sizeof(Entry), count, f) != count){
			map.warnings.push_back("Truncated IMG directory: " + path);
			fclose(f);
			return;
		}
		for(const Entry &e : entries){
			char name[25];
			memcpy(name, e.name, 24);
			name[24] = '\0';
			if(lower(name).find(".ipl") == std::string::npos)
				continue;
			uint32_t sectors = e.streamSize ? e.streamSize : e.archiveSize;
			std::vector<uint8_t> data((size_t)sectors * 2048);
			if(data.empty() || fseek(f, (long)e.offset * 2048, SEEK_SET) != 0 ||
			   fread(&data[0], 1, data.size(), f) != data.size())
				continue;
			loadBinaryIpl(name, data);
		}
		fclose(f);
	}
};

bool
LoadStockMap(const OpenFileFn &open, StockMap &map)
{
	map = StockMap();
	Loader loader = { open, map };
	std::vector<std::string> ides, ipls, imgs;
	loader.loadDat("DATA\\DEFAULT.DAT", ides, ipls, imgs);
	size_t numDefault = ides.size() + ipls.size();
	loader.loadDat("DATA\\GTA.DAT", ides, ipls, imgs);
	if(ides.size() + ipls.size() == numDefault){
		map.warnings.push_back("DATA\\GTA.DAT not found or empty.");
		return false;
	}

	for(const std::string &ide : ides)
		loader.loadIde(ide);
	for(const std::string &ipl : ipls)
		loader.loadTextIpl(ipl);

	// The game always opens these two archives, gta.dat may add others.
	std::unordered_set<std::string> seen;
	const char *stock[] = { "MODELS\\GTA3.IMG", "MODELS\\GTA_INT.IMG" };
	for(int i = 0; i < 2; i++){
		seen.insert(lower(stock[i]));
		loader.loadImage(stock[i], i == 0);
	}
	for(const std::string &img : imgs){
		std::string key = lower(img);
		for(char &c : key)
			if(c == '/') c = '\\';
		if(seen.insert(key).second)
			loader.loadImage(img, true);
	}

	map.loaded = true;
	return true;
}

// ------------------------------------------------------------------ Rotations

static bool
isHeadingOnly(float qx, float qy)
{
	return std::fabs(qx) <= 0.05f && std::fabs(qy) <= 0.05f;
}

// Heading (radians) SA gives to a quaternion with tiny X/Y.
static float
headingOf(float qz, float qw)
{
	float w = qw < -1.0f ? -1.0f : qw > 1.0f ? 1.0f : qw;
	return acosf(w) * (qz < 0.0f ? 2.0f : -2.0f);
}

static float
wrapDegrees(float d)
{
	d = fmodf(d, 360.0f);
	if(d < 0.0f) d += 360.0f;
	if(d >= 359.99995f || d < 0.00005f) d = 0.0f;
	return d;
}

static float
clamp1(float v)
{
	return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

void
QuatToSampEuler(float qx, float qy, float qz, float qw, float *rx, float *ry, float *rz)
{
	const float toDeg = 57.29577951308232f;
	if(isHeadingOnly(qx, qy)){
		*rx = 0.0f;
		*ry = 0.0f;
		*rz = wrapDegrees(headingOf(qz, qw) * toDeg);
		return;
	}

	float len = sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
	if(len > 0.0f){
		qx /= len; qy /= len; qz /= len; qw /= len;
	}

	// The game's quaternion to SA-MP Euler, as open.mp's GTAQuat::ToEuler
	// (formulas from IllidanS4's i_quat).
	const float epsilon = 0.00000202655792236328125f;
	float temp = 2.0f*qy*qz - 2.0f*qx*qw;
	if(temp >= 1.0f - epsilon || -temp >= 1.0f - epsilon){
		*rx = temp > 0.0f ? 90.0f : -90.0f;
		*ry = -atan2f(clamp1(qy), clamp1(qw)) * toDeg;
		*rz = -atan2f(clamp1(qz), clamp1(qw)) * toDeg;
	}else{
		*rx = asinf(clamp1(temp)) * toDeg;
		*ry = -atan2f(clamp1(qx*qz + qy*qw), clamp1(0.5f - qx*qx - qy*qy)) * toDeg;
		*rz = -atan2f(clamp1(qx*qy + qz*qw), clamp1(0.5f - qx*qx - qz*qz)) * toDeg;
	}
	*rx = wrapDegrees(*rx);
	*ry = wrapDegrees(*ry);
	*rz = wrapDegrees(*rz);
}

// The rotation the game shows, as a unit quaternion.
static void
effectiveQuat(const Placement &p, float q[4])
{
	if(isHeadingOnly(p.qx, p.qy)){
		float h = headingOf(p.qz, p.qw);
		q[0] = 0.0f; q[1] = 0.0f; q[2] = -sinf(h*0.5f); q[3] = cosf(h*0.5f);
		return;
	}
	float len = sqrtf(p.qx*p.qx + p.qy*p.qy + p.qz*p.qz + p.qw*p.qw);
	if(len <= 0.0f) len = 1.0f;
	q[0] = p.qx/len; q[1] = p.qy/len; q[2] = p.qz/len; q[3] = p.qw/len;
}

static bool
samePlacement(const Placement &a, const Placement &b)
{
	if(a.modelId != b.modelId ||
	   std::fabs(a.x - b.x) > MATCH_DISTANCE ||
	   std::fabs(a.y - b.y) > MATCH_DISTANCE ||
	   std::fabs(a.z - b.z) > MATCH_DISTANCE)
		return false;
	float qa[4], qb[4];
	effectiveQuat(a, qa);
	effectiveQuat(b, qb);
	float dot = qa[0]*qb[0] + qa[1]*qb[1] + qa[2]*qb[2] + qa[3]*qb[3];
	return std::fabs(dot) >= MATCH_ROTATION;
}

// ------------------------------------------------------------------ Pawn

static int64_t
cellKey(int cx, int cy)
{
	return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cy;
}

static int
cellOf(float v)
{
	return (int)floorf(v / GRID_CELL);
}

static void
appendf(std::string &out, const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	out += buf;
}

static std::string
nameComment(const ModelNameFn &modelName, int modelId)
{
	std::string name = modelName ? modelName(modelId) : std::string();
	return name.empty() ? std::string() : " // " + name;
}

Result
BuildPawn(const StockMap &map, const std::vector<SceneObject> &scene,
          const Options &options, const ModelNameFn &modelName)
{
	Result result;
	const size_t numStock = map.insts.size();

	std::unordered_map<int64_t, std::vector<int>> grid;
	for(size_t i = 0; i < numStock; i++)
		grid[cellKey(cellOf(map.insts[i].p.x), cellOf(map.insts[i].p.y))].push_back((int)i);

	// Each stock placement matches one scene placement at most.
	std::vector<bool> matched(numStock, false);
	std::vector<const SceneObject*> created;
	for(const SceneObject &obj : scene){
		int cx = cellOf(obj.p.x), cy = cellOf(obj.p.y);
		bool found = false;
		for(int dx = -1; dx <= 1 && !found; dx++)
			for(int dy = -1; dy <= 1 && !found; dy++){
				auto it = grid.find(cellKey(cx + dx, cy + dy));
				if(it == grid.end())
					continue;
				for(int index : it->second)
					if(!matched[index] && samePlacement(map.insts[index].p, obj.p)){
						matched[index] = true;
						found = true;
						break;
					}
			}
		if(!found)
			created.push_back(&obj);
	}

	auto inArea = [&](float x, float y) {
		if(!options.useArea)
			return true;
		float dx = x - options.areaX, dy = y - options.areaY;
		return dx*dx + dy*dy <= options.areaRadius*options.areaRadius;
	};

	// A LOD still in the scene is removed once none of its stock children is.
	std::vector<int> keptChildren(numStock, 0);
	for(size_t i = 0; i < numStock; i++)
		if(map.insts[i].lod >= 0 && matched[i])
			keptChildren[map.insts[i].lod]++;

	std::vector<int> removed, lods;
	std::vector<bool> lodListed(numStock, false);
	for(size_t i = 0; i < numStock; i++){
		if(matched[i] || !inArea(map.insts[i].p.x, map.insts[i].p.y))
			continue;
		removed.push_back((int)i);
		int lod = map.insts[i].lod;
		if(options.removeOrphanLods && lod >= 0 && matched[lod] && keptChildren[lod] == 0 && !lodListed[lod]){
			lodListed[lod] = true;
			lods.push_back(lod);
		}
	}

	auto isCustom = [&](int modelId) {
		if(modelId >= SAMP_FIRST_MODEL && modelId <= SAMP_LAST_MODEL)
			return false;
		return !map.definedModels.empty() && map.definedModels.count(modelId) == 0;
	};

	std::vector<const SceneObject*> objects;
	for(const SceneObject *obj : created)
		if(inArea(obj->p.x, obj->p.y))
			objects.push_back(obj);

	result.numRemoved = (int)removed.size();
	result.numLods = (int)lods.size();
	for(const SceneObject *obj : objects)
		if(isCustom(obj->p.modelId))
			result.numCustom++;
	result.numCreated = (int)objects.size() - result.numCustom;

	std::string &out = result.code;
	out += "// Exported from Ariane for SA-MP / open.mp\n";
	appendf(out, "// %d removed buildings (%d LODs), %d objects", result.numRemoved + result.numLods,
	        result.numLods, result.numCreated);
	if(result.numCustom > 0)
		appendf(out, ", %d custom models to declare", result.numCustom);
	out += "\n";
	for(const std::string &warning : map.warnings)
		out += "// Warning: " + warning + "\n";

	if(!removed.empty() || !lods.empty()){
		out += "\n// OnPlayerConnect(playerid)\n";
		auto removeLine = [&](int index, const char *suffix) {
			const Placement &p = map.insts[index].p;
			appendf(out, "RemoveBuildingForPlayer(playerid, %d, %.4f, %.4f, %.4f, %.2f);",
			        p.modelId, p.x, p.y, p.z, options.removeRadius);
			out += nameComment(modelName, p.modelId);
			if(*suffix)
				out += out.back() == ';' ? std::string(" //") + suffix : suffix;
			out += "\n";
		};
		for(int index : removed)
			removeLine(index, "");
		for(int index : lods)
			removeLine(index, " (LOD)");
	}

	if(!objects.empty()){
		out += options.function == FUNC_STREAMER
			? "\n// OnGameModeInit() or OnFilterScriptInit(), Streamer plugin\n"
			: "\n// OnGameModeInit() or OnFilterScriptInit()\n";
		for(const SceneObject *obj : objects){
			const Placement &p = obj->p;
			float rx, ry, rz;
			QuatToSampEuler(p.qx, p.qy, p.qz, p.qw, &rx, &ry, &rz);
			bool custom = isCustom(p.modelId);
			if(custom)
				out += "// ";
			if(options.function == FUNC_STREAMER){
				appendf(out, "CreateDynamicObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f",
				        p.modelId, p.x, p.y, p.z, rx, ry, rz);
				if(options.useInterior && p.interior != 0)
					appendf(out, ", -1, %d", p.interior);
				out += ");";
			}else{
				appendf(out, "CreateObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f);",
				        p.modelId, p.x, p.y, p.z, rx, ry, rz);
			}
			out += nameComment(modelName, p.modelId);
			if(custom)
				out += " (custom model: declare it with AddSimpleModel and use its new id)";
			out += "\n";
		}
	}

	if(removed.empty() && lods.empty() && objects.empty())
		out += "\n// No change from the stock map.\n";
	return result;
}

}
