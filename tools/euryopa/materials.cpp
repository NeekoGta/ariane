#include "euryopa.h"
#include "materials.h"

#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>
#include <unordered_map>

namespace Materials {

static std::unordered_map<const ObjectInst*, std::vector<MaterialOverride>> gOverrides;
// Lines of the file whose object was not found: written back as they are.
static std::vector<std::string> gOrphanLines;
static bool gDirty;

struct Swapped
{
	rw::Material *material;
	rw::Texture *texture;
	rw::RGBA color;
};
static std::vector<Swapped> gSwapped;

const std::vector<MaterialOverride>*
Get(const ObjectInst *inst)
{
	auto it = gOverrides.find(inst);
	return it == gOverrides.end() || it->second.empty() ? nil : &it->second;
}

bool
HasAny(const ObjectInst *inst)
{
	return Get(inst) != nil;
}

bool
IsDirty(void)
{
	return gDirty;
}

int
CountObjects(void)
{
	int n = 0;
	for(auto &it : gOverrides)
		if(!it.second.empty() && !it.first->m_isDeleted)
			n++;
	return n;
}

static void
collectMaterials(rw::Atomic *atomic, std::vector<rw::Material*> &out)
{
	if(atomic == nil || atomic->geometry == nil)
		return;
	rw::MaterialList &list = atomic->geometry->matList;
	for(int i = 0; i < list.numMaterials; i++)
		out.push_back(list.materials[i]);
}

// The materials in SA-MP index order: the geometry material list (all the
// atomics of a clump one after the other).
static void
collectMaterials(int type, void *rwObject, std::vector<rw::Material*> &out)
{
	if(rwObject == nil)
		return;
	if(type == ObjectDef::ATOMIC)
		collectMaterials((rw::Atomic*)rwObject, out);
	else if(type == ObjectDef::CLUMP){
		rw::Clump *clump = (rw::Clump*)rwObject;
		FORLIST(lnk, clump->atomics)
			collectMaterials(rw::Atomic::fromClump(lnk), out);
	}
}

static ObjectDef*
loadedObject(int modelId)
{
	ObjectDef *obj = GetObjectDef(modelId);
	if(obj == nil)
		return nil;
	if(!obj->IsLoaded())
		obj->Load();
	return obj->IsLoaded() ? obj : nil;
}

std::vector<std::string>
ModelMaterials(int modelId)
{
	std::vector<std::string> names;
	ObjectDef *obj = loadedObject(modelId);
	if(obj == nil)
		return names;
	std::vector<rw::Material*> mats;
	collectMaterials(obj->m_type, obj->m_type == ObjectDef::ATOMIC ? (void*)obj->m_atomics[0] : (void*)obj->m_clump, mats);
	for(rw::Material *m : mats)
		names.push_back(m && m->texture ? m->texture->name : "");
	return names;
}

std::string
ModelTxd(int modelId)
{
	ObjectDef *obj = GetObjectDef(modelId);
	TxdDef *td = obj ? GetTxdDef(obj->m_txdSlot) : nil;
	return td ? td->name : "";
}

int
ModelForTxd(const char *txd)
{
	int slot = FindTxdSlot(txd);
	if(slot < 0)
		return -1;
	for(int id = 0; id < NUMOBJECTDEFS; id++){
		ObjectDef *obj = GetObjectDef(id);
		if(obj && obj->m_txdSlot == slot)
			return id;
	}
	return -1;
}

static TxdDef*
loadedTxd(const char *name)
{
	int slot = FindTxdSlot(name);
	if(slot < 0)
		return nil;
	if(!IsTxdLoaded(slot))
		LoadTxd(slot);
	TxdDef *td = GetTxdDef(slot);
	return td && td->txd ? td : nil;
}

std::vector<std::string>
TxdTextures(const char *txd)
{
	std::vector<std::string> names;
	TxdDef *td = loadedTxd(txd);
	if(td == nil)
		return names;
	FORLIST(lnk, td->txd->textures){
		rw::Texture *tex = rw::Texture::fromDict(lnk);
		if(tex)
			names.push_back(tex->name);
	}
	return names;
}

static rw::Texture*
findTexture(TxdDef *td, const char *name)
{
	// The TXD then its parents, as the game looks textures up.
	for(int depth = 0; td && depth < 8; depth++){
		if(td->txd)
			FORLIST(lnk, td->txd->textures){
				rw::Texture *tex = rw::Texture::fromDict(lnk);
				if(tex && rw::strcmp_ci(tex->name, name) == 0)
					return tex;
			}
		if(td->parentId < 0)
			break;
		if(!IsTxdLoaded(td->parentId))
			LoadTxd(td->parentId);
		td = GetTxdDef(td->parentId);
	}
	return nil;
}

static bool
isNone(const std::string &s)
{
	return s.empty() || rw::strcmp_ci(s.c_str(), "none") == 0;
}

rw::Texture*
Resolve(MaterialOverride &m)
{
	if(m.resolveTried)
		return m.resolved;
	m.resolveTried = true;
	m.resolved = nil;
	if(m.modelId < 0 || isNone(m.txd) || isNone(m.texture))
		return nil;
	TxdDef *td = loadedTxd(m.txd.c_str());
	if(td)
		m.resolved = findTexture(td, m.texture.c_str());
	return m.resolved;
}

static void
markChanged(ObjectInst *inst)
{
	gDirty = true;
	StampChangeSeq(inst);
}

std::string
Set(ObjectInst *inst, MaterialOverride m)
{
	if(inst == nil || inst->m_isDeleted)
		return "No object.";
	if(m.index < 0 || m.index > MAX_INDEX)
		return "The material index goes from 0 to 15.";
	std::vector<std::string> materials = ModelMaterials(inst->m_objectId);
	if(!materials.empty() && m.index >= (int)materials.size()){
		char buf[128];
		snprintf(buf, sizeof(buf), "This model has %d material(s): index 0 to %d.",
		         (int)materials.size(), (int)materials.size() - 1);
		return buf;
	}
	if(isNone(m.txd) || isNone(m.texture)){
		m.modelId = -1;
		m.txd = "none";
		m.texture = "none";
		if(m.color == 0)
			return "Give a texture or a colour.";
	}else{
		if(FindTxdSlot(m.txd.c_str()) < 0)
			return "Unknown TXD: " + m.txd + ".";
		// SA-MP looks the TXD up through the model: it must be one using it.
		if(m.modelId < 0 || rw::strcmp_ci(ModelTxd(m.modelId).c_str(), m.txd.c_str()) != 0)
			m.modelId = ModelForTxd(m.txd.c_str());
		if(m.modelId < 0)
			return "No model uses the TXD " + m.txd + ".";
		m.resolveTried = false;
		if(Resolve(m) == nil)
			return "No texture " + m.texture + " in " + m.txd + ".";
	}
	std::vector<MaterialOverride> &list = gOverrides[inst];
	for(MaterialOverride &o : list)
		if(o.index == m.index){
			o = m;
			markChanged(inst);
			return "";
		}
	list.push_back(m);
	std::sort(list.begin(), list.end(), [](const MaterialOverride &a, const MaterialOverride &b) {
		return a.index < b.index;
	});
	markChanged(inst);
	return "";
}

int
Clear(ObjectInst *inst, int index)
{
	auto it = gOverrides.find(inst);
	if(it == gOverrides.end())
		return 0;
	int removed = 0;
	std::vector<MaterialOverride> &list = it->second;
	for(int i = (int)list.size() - 1; i >= 0; i--)
		if(index < 0 || list[i].index == index){
			list.erase(list.begin() + i);
			removed++;
		}
	if(list.empty())
		gOverrides.erase(it);
	if(removed)
		markChanged(inst);
	return removed;
}

void
BeginRender(ObjectInst *inst)
{
	gSwapped.clear();
	auto it = gOverrides.find(inst);
	if(it == gOverrides.end() || it->second.empty())
		return;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil)
		return;
	std::vector<rw::Material*> mats;
	collectMaterials(obj->m_type, inst->m_rwObject, mats);
	for(MaterialOverride &m : it->second){
		if(m.index < 0 || m.index >= (int)mats.size() || mats[m.index] == nil)
			continue;
		rw::Material *mat = mats[m.index];
		gSwapped.push_back({ mat, mat->texture, mat->color });
		rw::Texture *tex = Resolve(m);
		if(tex)
			mat->texture = tex;
		if(m.color != 0){
			mat->color.red = (m.color >> 16) & 0xFF;
			mat->color.green = (m.color >> 8) & 0xFF;
			mat->color.blue = m.color & 0xFF;
			mat->color.alpha = (m.color >> 24) & 0xFF;
		}
	}
}

void
EndRender(void)
{
	// The geometry is shared by every placement of the model: put it back.
	for(int i = (int)gSwapped.size() - 1; i >= 0; i--){
		gSwapped[i].material->texture = gSwapped[i].texture;
		gSwapped[i].material->color = gSwapped[i].color;
	}
	gSwapped.clear();
}

// ------------------------------------------------------------------ Pawn

static const char*
skipSeparator(const char *p)
{
	while(*p == ' ' || *p == '\t')
		p++;
	if(*p == ',')
		p++;
	while(*p == ' ' || *p == '\t')
		p++;
	return p;
}

static bool
readQuoted(const char **pp, std::string &out)
{
	const char *p = *pp;
	if(*p != '"')
		return false;
	const char *end = strchr(p + 1, '"');
	if(end == nil)
		return false;
	out.assign(p + 1, end - p - 1);
	*pp = end + 1;
	return true;
}

bool
ParsePawn(const char *text, MaterialOverride *out)
{
	const char *p = text ? strstr(text, "Material(") : nil;
	if(p == nil)
		return false;
	p += strlen("Material(");
	// object id or variable: skipped
	while(*p && *p != ',' && *p != ')')
		p++;
	if(*p != ',')
		return false;
	p = skipSeparator(p);
	char *end;
	MaterialOverride m;
	m.index = (int)strtol(p, &end, 10);
	if(end == p) return false;
	p = skipSeparator(end);
	m.modelId = (int)strtol(p, &end, 10);
	if(end == p) return false;
	p = skipSeparator(end);
	if(!readQuoted(&p, m.txd)) return false;
	p = skipSeparator(p);
	if(!readQuoted(&p, m.texture)) return false;
	p = skipSeparator(p);
	m.color = 0;
	if(*p == '-')
		m.color = (unsigned int)strtol(p, &end, 0);
	else if(*p >= '0' && *p <= '9')
		m.color = (unsigned int)strtoul(p, &end, 0);
	*out = m;
	return true;
}

// ------------------------------------------------------------------ file

static bool
isDirectory(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

std::string
FilePath(void)
{
	char root[1024], path[1024];
	if(GetGameRootDirectory(root, sizeof(root))){
		std::string modloader = std::string(root) + "/modloader";
		if(isDirectory(modloader))
			return modloader + "/Ariane/ariane.materials";
	}
	if(GetArianeDataPath(path, sizeof(path), "materials.txt"))
		return path;
	return "";
}

static const char *HEADER =
	"# Ariane SA-MP material overrides (SetObjectMaterial / SetDynamicObjectMaterial)\n"
	"# model x y z | index texture_model txd texture colour(ARGB)\n";

static std::string
lineFor(const ObjectInst *inst, const MaterialOverride &m)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%d %.4f %.4f %.4f %d %d %s %s 0x%08X",
	         inst->m_objectId, inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
	         m.index, m.modelId, isNone(m.txd) ? "none" : m.txd.c_str(),
	         isNone(m.texture) ? "none" : m.texture.c_str(), m.color);
	return buf;
}

bool
Save(void)
{
	std::string path = FilePath();
	if(path.empty())
		return false;
	std::vector<std::string> lines = gOrphanLines;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst->m_isDeleted)
			continue;
		const std::vector<MaterialOverride> *list = Get(inst);
		if(list)
			for(const MaterialOverride &m : *list)
				lines.push_back(lineFor(inst, m));
	}
	if(lines.empty()){
		remove(path.c_str());
		gDirty = false;
		return true;
	}
	EnsureParentDirectoriesForPath(path.c_str());
	FILE *f = fopen(path.c_str(), "wb");
	if(f == nil)
		return false;
	fputs(HEADER, f);
	for(const std::string &line : lines)
		fprintf(f, "%s\n", line.c_str());
	fclose(f);
	gDirty = false;
	return true;
}

bool
Load(void)
{
	gOverrides.clear();
	gOrphanLines.clear();
	gDirty = false;
	std::string path = FilePath();
	FILE *f = path.empty() ? nil : fopen(path.c_str(), "rb");
	if(f == nil)
		return false;

	std::unordered_multimap<int, ObjectInst*> byModel;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst && !inst->m_isDeleted)
			byModel.insert({ inst->m_objectId, inst });
	}

	char line[1024];
	int loaded = 0;
	while(fgets(line, sizeof(line), f)){
		char *nl = strpbrk(line, "\r\n");
		if(nl) *nl = '\0';
		const char *s = line;
		while(*s == ' ' || *s == '\t') s++;
		if(*s == '\0' || *s == '#')
			continue;
		int model;
		float x, y, z;
		char txd[64], tex[64], color[32];
		MaterialOverride m;
		if(sscanf(s, "%d %f %f %f %d %d %63s %63s %31s", &model, &x, &y, &z,
		          &m.index, &m.modelId, txd, tex, color) != 9){
			log("materials: unreadable line \"%s\"\n", s);
			continue;
		}
		m.txd = txd;
		m.texture = tex;
		m.color = (unsigned int)strtoul(color, nil, 0);

		ObjectInst *found = nil;
		float best = 0.05f;
		auto range = byModel.equal_range(model);
		for(auto it = range.first; it != range.second; ++it){
			rw::V3d d = sub(it->second->m_translation, rw::V3d{ x, y, z });
			float dist = length(d);
			if(dist <= best){
				best = dist;
				found = it->second;
			}
		}
		if(found == nil){
			gOrphanLines.push_back(s);
			continue;
		}
		std::vector<MaterialOverride> &list = gOverrides[found];
		bool replaced = false;
		for(MaterialOverride &o : list)
			if(o.index == m.index){
				o = m;
				replaced = true;
			}
		if(!replaced)
			list.push_back(m);
		loaded++;
	}
	fclose(f);
	for(auto &it : gOverrides)
		std::sort(it.second.begin(), it.second.end(), [](const MaterialOverride &a, const MaterialOverride &b) {
			return a.index < b.index;
		});
	log("materials: %d override(s) loaded from %s, %d without their object\n",
	    loaded, path.c_str(), (int)gOrphanLines.size());
	return true;
}

}
