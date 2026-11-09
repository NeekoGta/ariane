#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#define WITH_D3D
#include "euryopa.h"
#include "bridge.h"
#include "materials.h"
#include "mapprojects.h"
#include "sampexport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Bridge {

#ifdef _WIN32

// ------------------------------------------------------------------ JSON out

static void
appendEscaped(std::string &out, const char *s)
{
	out += '"';
	for(; *s; s++){
		unsigned char c = (unsigned char)*s;
		if(c == '"' || c == '\\'){
			out += '\\';
			out += (char)c;
		}else if(c == '\n')
			out += "\\n";
		else if(c == '\t')
			out += "\\t";
		else if(c < 0x20 || c >= 0x7F){
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", c);
			out += buf;
		}else
			out += (char)c;
	}
	out += '"';
}

// A JSON object written in order: begin(), fields, end().
struct Json
{
	std::string s;
	std::vector<bool> first;

	void sep(void) { if(!first.back()) s += ','; first.back() = false; }
	void key(const char *k) { if(k){ sep(); appendEscaped(s, k); s += ':'; }else sep(); }
	Json &obj(const char *k = nil) { key(k); s += '{'; first.push_back(true); return *this; }
	Json &arr(const char *k = nil) { key(k); s += '['; first.push_back(true); return *this; }
	Json &endObj(void) { s += '}'; first.pop_back(); return *this; }
	Json &endArr(void) { s += ']'; first.pop_back(); return *this; }
	Json &str(const char *k, const char *v) { key(k); appendEscaped(s, v ? v : ""); return *this; }
	Json &str(const char *k, const std::string &v) { return str(k, v.c_str()); }
	Json &num(const char *k, double v) {
		key(k);
		char buf[64];
		if(!std::isfinite(v)) v = 0.0;
		snprintf(buf, sizeof(buf), "%.4f", v);
		// trim the zeros: 12.5000 -> 12.5, 3.0000 -> 3
		char *dot = strchr(buf, '.');
		if(dot){
			char *e = buf + strlen(buf) - 1;
			while(e > dot && *e == '0') *e-- = '\0';
			if(e == dot) *e = '\0';
		}
		if(strcmp(buf, "-0") == 0) strcpy(buf, "0");
		s += buf;
		return *this;
	}
	Json &integer(const char *k, long long v) { key(k); s += std::to_string(v); return *this; }
	Json &boolean(const char *k, bool v) { key(k); s += v ? "true" : "false"; return *this; }
	Json &vec3(const char *k, float x, float y, float z) {
		arr(k); num(nil, x); num(nil, y); num(nil, z); return endArr();
	}
};

// ------------------------------------------------------------------ requests

struct Request
{
	std::string name;
	std::map<std::string, std::string> args;

	bool has(const char *k) const { return args.count(k) != 0; }
	const char *text(const char *k, const char *def = "") const {
		auto it = args.find(k);
		return it == args.end() ? def : it->second.c_str();
	}
	bool number(const char *k, float *v) const {
		auto it = args.find(k);
		if(it == args.end()) return false;
		char *end;
		double d = strtod(it->second.c_str(), &end);
		if(end == it->second.c_str() || !std::isfinite(d)) return false;
		*v = (float)d;
		return true;
	}
	int integer(const char *k, int def) const {
		auto it = args.find(k);
		if(it == args.end()) return def;
		char *end;
		long v = strtol(it->second.c_str(), &end, 0);
		return end == it->second.c_str() ? def : (int)v;
	}
	bool flag(const char *k, bool def) const {
		auto it = args.find(k);
		if(it == args.end()) return def;
		const std::string &v = it->second;
		return v == "1" || v == "true" || v == "yes";
	}
};

static bool
parseRequest(const std::string &line, Request &req)
{
	size_t start = 0;
	bool firstField = true;
	while(start <= line.size()){
		size_t tab = line.find('\t', start);
		std::string field = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
		if(firstField){
			req.name = field;
			firstField = false;
		}else if(!field.empty()){
			size_t eq = field.find('=');
			if(eq == std::string::npos)
				return false;
			req.args[field.substr(0, eq)] = field.substr(eq + 1);
		}
		if(tab == std::string::npos)
			break;
		start = tab + 1;
	}
	return !req.name.empty();
}

static std::string
errorJson(const std::string &message)
{
	Json j;
	j.first.push_back(true);
	j.s = "{";
	j.boolean("ok", false).str("error", message);
	j.s += "}";
	return j.s;
}

// ------------------------------------------------------------------ server

static std::atomic<bool> gEnabled(true);	// the editor setting
static bool gReady;
static std::atomic<bool> gRunning(false);
static std::atomic<int> gNumCommands(0);
static int gPort = DEFAULT_PORT;
static char gLastError[256];
static std::thread gThread;
static SOCKET gListen = INVALID_SOCKET;
static std::atomic<SOCKET> gClient(INVALID_SOCKET);

static std::mutex gMutex;
static std::condition_variable gCond;
static bool gHasRequest;	// waiting for the main thread
static bool gHasResponse;	// waiting for the server thread
static std::string gRequestLine;
static std::string gResponse;
static bool gStopping;

// Screenshot asked: answered by Capture() at the end of the frame.
static bool gCapturePending;
static int gCaptureWidth;
static int gCaptureFrames;	// frames to draw first: streamed models load, the camera settles

static bool
sendAll(SOCKET s, const std::string &data)
{
	const char *p = data.c_str();
	int left = (int)data.size();
	while(left > 0){
		int n = send(s, p, left, 0);
		if(n <= 0)
			return false;
		p += n;
		left -= n;
	}
	return true;
}

// One request, answered by the main thread (or an error if it is closing).
static std::string
callMainThread(const std::string &line)
{
	std::unique_lock<std::mutex> lock(gMutex);
	gRequestLine = line;
	gHasRequest = true;
	gHasResponse = false;
	if(!gCond.wait_for(lock, std::chrono::seconds(60), [] { return gHasResponse || gStopping; })){
		gHasRequest = false;
		return errorJson("Ariane did not answer within 60 s (loading, or a modal dialog?)");
	}
	if(!gHasResponse){
		gHasRequest = false;
		return errorJson("Ariane is closing the bridge.");
	}
	gHasResponse = false;
	return gResponse;
}

static void
serveClient(SOCKET client)
{
	std::string buffer;
	char chunk[4096];
	for(;;){
		int n = recv(client, chunk, sizeof(chunk), 0);
		if(n <= 0)
			break;
		buffer.append(chunk, n);
		size_t nl;
		while((nl = buffer.find('\n')) != std::string::npos){
			std::string line = buffer.substr(0, nl);
			buffer.erase(0, nl + 1);
			if(!line.empty() && line.back() == '\r')
				line.pop_back();
			if(line.empty())
				continue;
			std::string response = callMainThread(line);
			if(!sendAll(client, response + "\n"))
				return;
		}
		if(buffer.size() > (1 << 20))
			break;	// no line in 1 MB: not a client of ours
	}
}

static void
serverThread(void)
{
	for(;;){
		SOCKET client = accept(gListen, nil, nil);
		if(client == INVALID_SOCKET)
			break;	// listening socket closed: stop
		gClient = client;
		serveClient(client);
		gClient = INVALID_SOCKET;
		closesocket(client);
	}
}

static bool
start(void)
{
	WSADATA wsa;
	if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0){
		snprintf(gLastError, sizeof(gLastError), "WSAStartup failed");
		return false;
	}
	const char *env = getenv("ARIANE_BRIDGE_PORT");
	gPort = env && atoi(env) > 0 ? atoi(env) : DEFAULT_PORT;

	gListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(gListen == INVALID_SOCKET){
		snprintf(gLastError, sizeof(gLastError), "socket() failed (%d)", WSAGetLastError());
		WSACleanup();
		return false;
	}
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)gPort);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	// this PC only
	BOOL exclusive = TRUE;
	setsockopt(gListen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
	if(bind(gListen, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(gListen, 2) != 0){
		snprintf(gLastError, sizeof(gLastError), "Port %d is taken (another Ariane?)", gPort);
		closesocket(gListen);
		gListen = INVALID_SOCKET;
		WSACleanup();
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(gMutex);
		gStopping = false;
		gHasRequest = gHasResponse = false;
	}
	gLastError[0] = '\0';
	gThread = std::thread(serverThread);
	gRunning = true;
	log("bridge: listening on 127.0.0.1:%d\n", gPort);
	return true;
}

static void
stop(void)
{
	if(!gRunning)
		return;
	{
		std::lock_guard<std::mutex> lock(gMutex);
		gStopping = true;
	}
	gCond.notify_all();
	closesocket(gListen);
	gListen = INVALID_SOCKET;
	SOCKET client = gClient.exchange(INVALID_SOCKET);
	if(client != INVALID_SOCKET)
		shutdown(client, SD_BOTH);
	if(gThread.joinable())
		gThread.join();
	WSACleanup();
	gRunning = false;
	gCapturePending = false;
	log("bridge: stopped\n");
}

void
SetEnabled(bool enabled)
{
	gEnabled = enabled;
	if(!gReady)
		return;
	if(enabled && !gRunning)
		start();
	else if(!enabled && gRunning)
		stop();
}

void
Ready(void)
{
	gReady = true;
	SetEnabled(gEnabled);
}

bool IsEnabled(void) { return gEnabled; }
bool IsRunning(void) { return gRunning; }
int Port(void) { return gPort; }
const char *LastError(void) { return gLastError; }
int NumCommands(void) { return gNumCommands; }

void
Shutdown(void)
{
	stop();
}

static void
respond(const std::string &response)
{
	{
		std::lock_guard<std::mutex> lock(gMutex);
		gHasRequest = false;
		gResponse = response;
		gHasResponse = true;
	}
	gCond.notify_all();
}

// ------------------------------------------------------------------ commands

typedef std::string (*CommandFn)(const Request &req, Json &out);

static Json
okJson(void)
{
	Json j;
	j.first.push_back(true);
	j.s = "{";
	j.boolean("ok", true);
	return j;
}

static bool
strstr_ci(const char *text, const char *needle)
{
	size_t n = strlen(needle);
	for(; *text; text++)
		if(rw::strncmp_ci(text, needle, (int)n) == 0)
			return true;
	return n == 0;
}

static void
sampRotation(const rw::Quat &q, float *rx, float *ry, float *rz)
{
	SampExport::QuatToSampEuler(q.x, q.y, q.z, q.w, rx, ry, rz);
}

static void
writeInst(Json &j, ObjectInst *inst, const char *key = nil)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	float rx, ry, rz;
	sampRotation(inst->m_rotation, &rx, &ry, &rz);
	j.obj(key);
	j.integer("id", inst->m_id);
	j.integer("model", inst->m_objectId);
	j.str("name", obj ? obj->m_name : "");
	j.vec3("pos", inst->m_translation.x, inst->m_translation.y, inst->m_translation.z);
	j.vec3("rot", rx, ry, rz);
	if(inst->m_area != 0)
		j.integer("interior", inst->m_area);
	if(inst->m_isBigBuilding)
		j.boolean("lod", true);
	if(inst->m_isAdded)
		j.boolean("added", true);
	if(inst->m_isDirty)
		j.boolean("modified", true);
	if(inst->m_selected)
		j.boolean("selected", true);
	if(!IsInstInIplMapDocument(inst))
		j.boolean("locked", true);
	const std::vector<MaterialOverride> *mats = Materials::Get(inst);
	if(mats){
		j.arr("materials");
		for(const MaterialOverride &m : *mats){
			j.obj();
			j.integer("index", m.index);
			j.integer("model", m.modelId);
			j.str("txd", m.txd);
			j.str("texture", m.texture);
			char col[16];
			snprintf(col, sizeof(col), "0x%08X", m.color);
			j.str("color", col);
			j.endObj();
		}
		j.endArr();
	}
	j.endObj();
}

static bool
readVec(const Request &req, const char *a, const char *b, const char *c, rw::V3d *v)
{
	return req.number(a, &v->x) && req.number(b, &v->y) && req.number(c, &v->z);
}

// "rot" given as SA-MP angles (default) or the Transform panel angles.
static bool
readRotation(const Request &req, rw::Quat *q)
{
	rw::V3d r = { 0.0f, 0.0f, 0.0f };
	bool any = false;
	any |= req.number("rx", &r.x);
	any |= req.number("ry", &r.y);
	any |= req.number("rz", &r.z);
	if(!any)
		return false;
	if(strcmp(req.text("rotation", "samp"), "ariane") == 0)
		*q = MakeObjectRotationDegrees(r);
	else
		SampExport::SampEulerToQuat(r.x, r.y, r.z, &q->x, &q->y, &q->z, &q->w);
	return true;
}

static ObjectInst*
instArg(const Request &req, const char *key, std::string *error)
{
	int id = req.integer(key, -1);
	ObjectInst *inst = id >= 0 ? GetInstanceByID(id) : nil;
	if(inst == nil || inst->m_isDeleted){
		*error = "No object with id " + std::string(req.text(key)) + ".";
		return nil;
	}
	return inst;
}

static std::vector<int>
idList(const char *text)
{
	std::vector<int> ids;
	const char *p = text;
	while(*p){
		char *end;
		long v = strtol(p, &end, 10);
		if(end == p){
			p++;
			continue;
		}
		ids.push_back((int)v);
		p = end;
	}
	return ids;
}

static std::string
cmdStatus(const Request &, Json &j)
{
	char root[1024] = "";
	GetGameRootDirectory(root, sizeof(root));
	j.str("game_root", root);
	j.str("map", MapProjects::IsAvailable() ? MapProjects::CurrentName() : "");
	j.vec3("camera", TheCamera.m_position.x, TheCamera.m_position.y, TheCamera.m_position.z);
	j.vec3("target", TheCamera.m_target.x, TheCamera.m_target.y, TheCamera.m_target.z);
	j.num("fov", TheCamera.m_fov);
	int numSelected = 0, numAdded = 0, numModified = 0;
	for(CPtrNode *p = selection.first; p; p = p->next) numSelected++;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted) continue;
		if(inst->m_isAdded) numAdded++;
		if(inst->m_isDirty) numModified++;
	}
	j.integer("selected", numSelected);
	j.integer("added", numAdded);
	j.integer("modified", numModified);
	j.integer("retextured_objects", Materials::CountObjects());
	j.str("materials_file", Materials::FilePath());
	j.integer("interior", currentArea);
	if(IsIplMapDocumentOpen())
		j.str("open_ipl", GetIplMapDocumentLogicalPath());
	return "";
}

static std::string
cmdCamera(const Request &req, Json &j)
{
	rw::V3d pos, target;
	bool hasPos = readVec(req, "x", "y", "z", &pos);
	bool hasTarget = readVec(req, "tx", "ty", "tz", &target);
	float fov;
	if(hasTarget){
		if(!hasPos){
			// keep the view direction and distance, look at the new target
			float dist = req.number("distance", &fov) ? fov : TheCamera.distanceToTarget();
			rw::V3d dir = normalize(sub(TheCamera.m_target, TheCamera.m_position));
			pos = sub(target, scale(dir, dist));
		}
		if(length(sub(target, pos)) < 0.01f)
			return "The camera and its target are at the same place.";
		TheCamera.m_position = pos;
		TheCamera.m_target = target;
	}else if(hasPos){
		TheCamera.setTarget(add(pos, sub(TheCamera.m_target, TheCamera.m_position)));
		TheCamera.m_position = pos;
	}
	if(req.number("fov", &fov))
		TheCamera.m_fov = ClampCameraFov(fov);
	TheCamera.update();
	j.vec3("camera", TheCamera.m_position.x, TheCamera.m_position.y, TheCamera.m_position.z);
	j.vec3("target", TheCamera.m_target.x, TheCamera.m_target.y, TheCamera.m_target.z);
	j.num("fov", TheCamera.m_fov);
	return "";
}

static std::string
cmdObjects(const Request &req, Json &j)
{
	rw::V3d center;
	if(!readVec(req, "x", "y", "z", &center))
		center = TheCamera.m_target;
	float radius = 30.0f;
	req.number("radius", &radius);
	int limit = clamp(req.integer("limit", 100), 1, 2000);
	int model = req.integer("model", -1);
	bool lods = req.flag("lods", false);
	bool onlyAdded = req.flag("added", false);
	const char *nameFilter = req.text("name");

	std::vector<std::pair<float, ObjectInst*>> found;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || (!lods && inst->m_isBigBuilding) || (onlyAdded && !inst->m_isAdded))
			continue;
		if(model >= 0 && inst->m_objectId != model)
			continue;
		if(nameFilter[0]){
			ObjectDef *obj = GetObjectDef(inst->m_objectId);
			if(obj == nil || !strstr_ci(obj->m_name, nameFilter))
				continue;
		}
		float d = length(sub(inst->m_translation, center));
		if(d <= radius)
			found.push_back({ d, inst });
	}
	std::sort(found.begin(), found.end(), [](const std::pair<float, ObjectInst*> &a, const std::pair<float, ObjectInst*> &b) {
		return a.first < b.first;
	});
	j.integer("count", (long long)found.size());
	j.arr("objects");
	for(int i = 0; i < (int)found.size() && i < limit; i++)
		writeInst(j, found[i].second);
	j.endArr();
	return "";
}

static std::string
cmdSelection(const Request &, Json &j)
{
	j.arr("objects");
	for(CPtrNode *p = selection.first; p; p = p->next)
		writeInst(j, (ObjectInst*)p->item);
	j.endArr();
	return "";
}

static std::string
cmdSelect(const Request &req, Json &j)
{
	if(!req.flag("add", false))
		ClearSelection();
	std::vector<int> ids = idList(req.text("ids"));
	int n = 0;
	ObjectInst *last = nil;
	for(int id : ids){
		ObjectInst *inst = GetInstanceByID(id);
		if(inst && !inst->m_isDeleted){
			inst->Select();
			if(inst->m_selected){
				n++;
				last = inst;
			}
		}
	}
	if(last && req.flag("focus", true))
		last->JumpTo();
	j.integer("selected", n);
	return "";
}

static std::string
cmdFindModels(const Request &req, Json &j)
{
	const char *query = req.text("query");
	int limit = clamp(req.integer("limit", 50), 1, 500);
	j.arr("models");
	int n = 0;
	for(int id = 0; id < NUMOBJECTDEFS && n < limit; id++){
		ObjectDef *obj = GetObjectDef(id);
		if(obj == nil || (query[0] && !strstr_ci(obj->m_name, query)))
			continue;
		if(obj->m_isBigBuilding && !req.flag("lods", false))
			continue;
		j.obj();
		j.integer("model", id);
		j.str("name", obj->m_name);
		TxdDef *td = GetTxdDef(obj->m_txdSlot);
		j.str("txd", td ? td->name : "");
		if(obj->m_colModel){
			CBox &b = obj->m_colModel->boundingBox;
			j.vec3("size", b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
		}
		j.endObj();
		n++;
	}
	j.endArr();
	return "";
}

static std::string
cmdModelInfo(const Request &req, Json &j)
{
	int id = req.integer("model", -1);
	ObjectDef *obj = GetObjectDef(id);
	if(obj == nil && req.has("name"))
		obj = GetObjectDef(req.text("name"), &id);
	if(obj == nil)
		return "Unknown model.";
	j.integer("model", obj->m_id);
	j.str("name", obj->m_name);
	j.str("txd", Materials::ModelTxd(obj->m_id));
	j.num("draw_distance", obj->GetLargestDrawDist());
	int lod = GetLodForObject(obj->m_id);
	if(lod >= 0)
		j.integer("lod_model", lod);
	if(obj->m_colModel){
		CBox &b = obj->m_colModel->boundingBox;
		j.vec3("bbox_min", b.min.x, b.min.y, b.min.z);
		j.vec3("bbox_max", b.max.x, b.max.y, b.max.z);
		j.vec3("size", b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z);
		j.num("radius", obj->m_colModel->boundingSphere.radius);
	}
	std::vector<std::string> mats = Materials::ModelMaterials(obj->m_id);
	j.arr("materials");
	for(const std::string &m : mats)
		j.str(nil, m);
	j.endArr();
	return "";
}

static std::string
cmdPlace(const Request &req, Json &j)
{
	int model = req.integer("model", -1);
	ObjectDef *obj = GetObjectDef(model);
	if(obj == nil)
		return "Unknown model " + std::string(req.text("model")) + ".";
	rw::V3d pos;
	if(!readVec(req, "x", "y", "z", &pos))
		return "x, y and z are required.";
	rw::Quat rot = { 0.0f, 0.0f, 0.0f, 1.0f };
	readRotation(req, &rot);

	int previous = GetSpawnObjectId();
	SetSpawnObjectId(model);
	ClearSelection();
	ObjectInst *insts[4];
	int n = SpawnPlaceObjectNoUndo(pos, &rot, insts, 4);
	SetSpawnObjectId(previous);
	if(n == 0)
		return "The object could not be placed (is the opened IPL read-only?).";
	UndoRecordPaste(insts, n);
	ObjectInst *hd = insts[n - 1];
	if(req.flag("focus", false))
		hd->JumpTo();
	writeInst(j, hd, "object");
	return "";
}

static std::string
cmdTransform(const Request &req, Json &j)
{
	std::string error;
	ObjectInst *inst = instArg(req, "id", &error);
	if(inst == nil)
		return error;
	rw::V3d pos = inst->m_translation;
	rw::Quat rot = inst->m_rotation;
	bool any = false;
	float v;
	if(req.number("x", &v)){ pos.x = v; any = true; }
	if(req.number("y", &v)){ pos.y = v; any = true; }
	if(req.number("z", &v)){ pos.z = v; any = true; }
	if(req.number("dx", &v)){ pos.x += v; any = true; }
	if(req.number("dy", &v)){ pos.y += v; any = true; }
	if(req.number("dz", &v)){ pos.z += v; any = true; }
	if(req.has("rx") || req.has("ry") || req.has("rz")){
		// a missing angle keeps its current SA-MP value
		float rx, ry, rz;
		sampRotation(inst->m_rotation, &rx, &ry, &rz);
		Request full = req;
		if(!req.has("rx")) full.args["rx"] = std::to_string(rx);
		if(!req.has("ry")) full.args["ry"] = std::to_string(ry);
		if(!req.has("rz")) full.args["rz"] = std::to_string(rz);
		readRotation(full, &rot);
		any = true;
	}
	if(!any)
		return "Nothing to change: give x/y/z, dx/dy/dz or rx/ry/rz.";
	std::vector<UndoTransform> transforms;
	if(!CaptureObjectTransformTargets(inst, false, transforms))
		return "This object cannot be edited (reference of an opened IPL?).";
	PreviewObjectTransformTargets(inst, transforms, pos, rot, UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT);
	CommitObjectTransformTargets(transforms);
	writeInst(j, inst, "object");
	return "";
}

static std::string
cmdDelete(const Request &req, Json &j)
{
	std::vector<int> ids = idList(req.text("ids"));
	ClearSelection();
	int n = 0;
	for(int id : ids){
		ObjectInst *inst = GetInstanceByID(id);
		if(inst && !inst->m_isDeleted){
			inst->Select();
			if(inst->m_selected)
				n++;
		}
	}
	if(n == 0)
		return "None of these objects can be deleted.";
	DeleteSelected();
	j.integer("deleted", n);
	return "";
}

static std::string
cmdUndo(const Request &req, Json &j)
{
	int times = clamp(req.integer("times", 1), 1, 100);
	bool redo = req.flag("redo", false);
	for(int i = 0; i < times; i++)
		redo ? Redo() : Undo();
	j.integer("done", times);
	return "";
}

static std::string
cmdGround(const Request &req, Json &j)
{
	rw::V3d pos;
	if(!req.number("x", &pos.x) || !req.number("y", &pos.y))
		return "x and y are required.";
	if(!req.number("z", &pos.z))
		pos.z = 0.0f;
	rw::V3d hit, normal;
	// From above z (+5000 inside): pass a z under a roof to get the floor below it.
	bool below = req.has("z");
	rw::V3d start = pos;
	if(below)
		start.z -= 5000.0f;
	if(!GetGroundPlacementSurface(start, &hit, &normal, true))
		return "No ground under this point.";
	if(below && hit.z > pos.z + 0.01f)
		return "No ground under this point.";
	j.num("z", hit.z);
	j.vec3("normal", normal.x, normal.y, normal.z);
	return "";
}

static std::string
cmdSave(const Request &, Json &j)
{
	if(!SaveMapFromBridge())
		return "The save failed or was refused (see the toast in Ariane).";
	j.str("materials_file", Materials::FilePath());
	return "";
}

static std::string
cmdMaterialSet(const Request &req, Json &j)
{
	std::string error;
	ObjectInst *inst = instArg(req, "id", &error);
	if(inst == nil)
		return error;
	MaterialOverride m;
	m.index = req.integer("index", -1);
	m.modelId = req.integer("model", -1);
	m.txd = req.text("txd", "none");
	m.texture = req.text("texture", "none");
	m.color = (unsigned int)strtoul(req.text("color", "0"), nil, 0);
	error = Materials::Set(inst, m);
	if(!error.empty())
		return error;
	writeInst(j, inst, "object");
	return "";
}

static std::string
cmdMaterialClear(const Request &req, Json &j)
{
	std::string error;
	ObjectInst *inst = instArg(req, "id", &error);
	if(inst == nil)
		return error;
	j.integer("removed", Materials::Clear(inst, req.integer("index", -1)));
	return "";
}

static std::string
cmdTextures(const Request &req, Json &j)
{
	std::string txd = req.text("txd");
	if(txd.empty() && req.has("model"))
		txd = Materials::ModelTxd(req.integer("model", -1));
	if(txd.empty())
		return "Give txd or model.";
	std::vector<std::string> names = Materials::TxdTextures(txd.c_str());
	if(names.empty())
		return "Unknown or empty TXD " + txd + ".";
	const char *filter = req.text("filter");
	j.str("txd", txd);
	j.integer("model", Materials::ModelForTxd(txd.c_str()));
	j.arr("textures");
	for(const std::string &n : names)
		if(!filter[0] || strstr_ci(n.c_str(), filter))
			j.str(nil, n);
	j.endArr();
	return "";
}

static std::string
cmdScreenshot(const Request &req, Json &)
{
	gCaptureWidth = clamp(req.integer("width", 1280), 64, 4096);
	gCaptureFrames = clamp(req.integer("frames", 3), 1, 120);
	gCapturePending = true;
	return "";	// answered by Capture()
}

struct Command
{
	const char *name;
	CommandFn fn;
};

static const Command COMMANDS[] = {
	{ "status", cmdStatus },
	{ "camera", cmdCamera },
	{ "objects", cmdObjects },
	{ "selection", cmdSelection },
	{ "select", cmdSelect },
	{ "find_models", cmdFindModels },
	{ "model_info", cmdModelInfo },
	{ "place", cmdPlace },
	{ "transform", cmdTransform },
	{ "delete", cmdDelete },
	{ "undo", cmdUndo },
	{ "ground", cmdGround },
	{ "save", cmdSave },
	{ "material_set", cmdMaterialSet },
	{ "material_clear", cmdMaterialClear },
	{ "textures", cmdTextures },
	{ "screenshot", cmdScreenshot },
};

void
Process(void)
{
	if(!gRunning || gCapturePending)
		return;
	std::string line;
	{
		std::lock_guard<std::mutex> lock(gMutex);
		if(!gHasRequest || gHasResponse)
			return;
		line = gRequestLine;
	}
	gNumCommands++;
	Request req;
	if(!parseRequest(line, req)){
		respond(errorJson("Malformed request."));
		return;
	}
	for(const Command &c : COMMANDS)
		if(req.name == c.name){
			Json j = okJson();
			std::string error = c.fn(req, j);
			if(!error.empty())
				respond(errorJson(error));
			else if(!gCapturePending){
				j.s += "}";
				respond(j.s);
			}
			return;
		}
	respond(errorJson("Unknown command " + req.name + "."));
}

// ------------------------------------------------------------------ screenshot

static bool
writeBmp(const char *path, const std::vector<uint8> &rgb, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if(f == nil)
		return false;
	int row = (w*3 + 3) & ~3;
	uint32 size = 54 + row*h;
	uint8 header[54] = { 'B', 'M' };
	auto put32 = [&](int at, uint32 v) { memcpy(header + at, &v, 4); };
	auto put16 = [&](int at, uint16 v) { memcpy(header + at, &v, 2); };
	put32(2, size);
	put32(10, 54);
	put32(14, 40);
	put32(18, w);
	put32(22, h);	// bottom-up
	put16(26, 1);
	put16(28, 24);
	put32(34, row*h);
	fwrite(header, 1, 54, f);
	std::vector<uint8> line(row, 0);
	for(int y = h - 1; y >= 0; y--){
		for(int x = 0; x < w; x++){
			const uint8 *src = &rgb[(y*w + x)*3];
			line[x*3 + 0] = src[2];
			line[x*3 + 1] = src[1];
			line[x*3 + 2] = src[0];
		}
		fwrite(line.data(), 1, row, f);
	}
	fclose(f);
	return true;
}

#ifdef RW_D3D9
// The back buffer as RGB, scaled down to maxWidth (box filter).
static std::string
readBackBuffer(int maxWidth, std::vector<uint8> &rgb, int *outW, int *outH)
{
	IDirect3DDevice9 *dev = rw::d3d::d3ddevice;
	if(dev == nil)
		return "No Direct3D device.";
	IDirect3DSurface9 *target = nil, *resolved = nil, *sys = nil;
	if(FAILED(dev->GetRenderTarget(0, &target)))
		return "GetRenderTarget failed.";
	D3DSURFACE_DESC desc;
	target->GetDesc(&desc);
	std::string error;
	if(desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8){
		target->Release();
		return "Unsupported back buffer format.";
	}
	resolved = target;
	if(desc.MultiSampleType != D3DMULTISAMPLE_NONE){
		if(FAILED(dev->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0,
		                                  FALSE, &resolved, nil)) ||
		   FAILED(dev->StretchRect(target, nil, resolved, nil, D3DTEXF_NONE))){
			if(resolved && resolved != target) resolved->Release();
			target->Release();
			return "Could not resolve the antialiased back buffer.";
		}
	}
	if(FAILED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nil)) ||
	   FAILED(dev->GetRenderTargetData(resolved, sys)))
		error = "Could not read the back buffer.";
	D3DLOCKED_RECT lr;
	if(error.empty() && FAILED(sys->LockRect(&lr, nil, D3DLOCK_READONLY)))
		error = "Could not lock the copy.";
	if(error.empty()){
		int sw = desc.Width, sh = desc.Height;
		int dw = min(sw, maxWidth);
		int dh = max(1, (int)((float)sh * dw / sw + 0.5f));
		rgb.assign(dw*dh*3, 0);
		for(int y = 0; y < dh; y++){
			int y0 = y*sh/dh, y1 = max(y0 + 1, (y + 1)*sh/dh);
			for(int x = 0; x < dw; x++){
				int x0 = x*sw/dw, x1 = max(x0 + 1, (x + 1)*sw/dw);
				uint32 r = 0, g = 0, b = 0, n = 0;
				for(int sy = y0; sy < y1; sy++){
					const uint8 *p = (const uint8*)lr.pBits + sy*lr.Pitch + x0*4;
					for(int sx = x0; sx < x1; sx++, p += 4){
						b += p[0]; g += p[1]; r += p[2]; n++;
					}
				}
				uint8 *d = &rgb[(y*dw + x)*3];
				d[0] = (uint8)(r/n); d[1] = (uint8)(g/n); d[2] = (uint8)(b/n);
			}
		}
		*outW = dw;
		*outH = dh;
		sys->UnlockRect();
	}
	if(sys) sys->Release();
	if(resolved != target) resolved->Release();
	target->Release();
	return error;
}
#endif

void
Capture(void)
{
	if(!gCapturePending)
		return;
	if(--gCaptureFrames > 0)
		return;
	gCapturePending = false;
#ifdef RW_D3D9
	std::vector<uint8> rgb;
	int w = 0, h = 0;
	std::string error = readBackBuffer(gCaptureWidth, rgb, &w, &h);
	if(!error.empty()){
		respond(errorJson(error));
		return;
	}
	char path[1024];
	if(!GetArianeDataPath(path, sizeof(path), "claude_capture.bmp") ||
	   !EnsureParentDirectoriesForPath(path) || !writeBmp(path, rgb, w, h)){
		respond(errorJson("Could not write the capture file."));
		return;
	}
	Json j = okJson();
	j.str("path", path);
	j.integer("width", w);
	j.integer("height", h);
	j.vec3("camera", TheCamera.m_position.x, TheCamera.m_position.y, TheCamera.m_position.z);
	j.vec3("target", TheCamera.m_target.x, TheCamera.m_target.y, TheCamera.m_target.z);
	j.s += "}";
	respond(j.s);
#else
	respond(errorJson("Screenshots need the Direct3D 9 build."));
#endif
}

#else	// !_WIN32

void SetEnabled(bool) {}
void Ready(void) {}
bool IsEnabled(void) { return false; }
bool IsRunning(void) { return false; }
int Port(void) { return DEFAULT_PORT; }
const char *LastError(void) { return "Windows only"; }
int NumCommands(void) { return 0; }
void Process(void) {}
void Capture(void) {}
void Shutdown(void) {}

#endif

}
