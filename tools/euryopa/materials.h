#ifndef MATERIALS_H
#define MATERIALS_H

// SA-MP style retexturing: SetObjectMaterial / SetDynamicObjectMaterial.
//
// An override replaces, on one placed object, the texture of one material of
// its model (by material index) with a texture of another TXD, and/or tints it
// with an ARGB colour. The overrides are not representable in IPL files, so
// they are kept in a side file, keyed by model and position:
//   modloader/Ariane/ariane.materials   (or ariane/materials.txt without modloader)
// Each map of File > Maps keeps its own file, as it lives in modloader/Ariane.

#include <string>
#include <vector>

struct ObjectInst;
namespace rw { struct Texture; }

struct MaterialOverride
{
	int index;		// material index of the object's model
	int modelId;		// model whose TXD holds the texture, -1 = colour only
	std::string txd;	// TXD name ("none" with modelId -1)
	std::string texture;	// texture name ("none" with modelId -1)
	unsigned int color;	// 0xAARRGGBB, 0 = keep the model's colour

	// resolved on first use
	rw::Texture *resolved = nullptr;
	bool resolveTried = false;
};

namespace Materials {

const int MAX_INDEX = 15;	// SA-MP accepts material indices 0 to 15

// Overrides of one object, nil if it has none.
const std::vector<MaterialOverride> *Get(const ObjectInst *inst);
bool HasAny(const ObjectInst *inst);
// Replaces the override of the same index. Returns an error message, empty on success.
std::string Set(ObjectInst *inst, MaterialOverride m);
// index -1 removes all of them. Returns the number removed.
int Clear(ObjectInst *inst, int index);
int CountObjects(void);
bool IsDirty(void);	// changed since the last save or load

// Called around the rendering of one object.
void BeginRender(ObjectInst *inst);
void EndRender(void);

// The materials of a model: texture names in material order ("" if untextured).
std::vector<std::string> ModelMaterials(int modelId);
// The TXD name of a model, "" if none.
std::string ModelTxd(int modelId);
// A model using a TXD (SA-MP finds the TXD through it), -1 if none.
int ModelForTxd(const char *txd);
// The textures of a TXD (loaded on demand). Empty if the TXD is unknown.
std::vector<std::string> TxdTextures(const char *txd);
// The texture an override points to, nil if it cannot be found.
rw::Texture *Resolve(MaterialOverride &m);

// "SetDynamicObjectMaterial(id, 0, 18888, \"txd\", \"tex\", 0xFFFFFFFF);" -> override.
bool ParsePawn(const char *text, MaterialOverride *out);

bool Load(void);	// after the map is loaded
bool Save(void);	// with the IPL files
std::string FilePath(void);

}

// Object Info > Materials panel (materials_gui.cpp).
void uiMaterialsPanel(ObjectInst *inst);

#endif
