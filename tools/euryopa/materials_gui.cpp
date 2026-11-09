#include "euryopa.h"
#include "imgui/imgui.h"
#include "materials.h"

// Object Info > Materials (SA-MP): retexture the selected object, as
// SetObjectMaterial / SetDynamicObjectMaterial would in game.

static const ObjectInst *gEditedInst;
static int gIndex;
static int gSourceModel = -1;
static char gTxd[64];
static char gTexture[64];
static char gFilter[64];
static bool gTint;
static float gColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };	// RGBA for ImGui
static char gLoadedTxd[64];
static std::vector<std::string> gTextures;
static std::string gMessage;
static bool gMessageIsError;

static unsigned int
editedColor(void)
{
	if(!gTint)
		return 0;
	auto byte = [](float v) { return (unsigned int)(clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return byte(gColor[3]) << 24 | byte(gColor[0]) << 16 | byte(gColor[1]) << 8 | byte(gColor[2]);
}

static void
loadFields(const MaterialOverride *m)
{
	gSourceModel = -1;
	gTxd[0] = gTexture[0] = '\0';
	gTint = false;
	if(m == nil)
		return;
	if(m->modelId >= 0){
		gSourceModel = m->modelId;
		snprintf(gTxd, sizeof(gTxd), "%s", m->txd.c_str());
		snprintf(gTexture, sizeof(gTexture), "%s", m->texture.c_str());
	}
	if(m->color != 0){
		gTint = true;
		gColor[0] = ((m->color >> 16) & 0xFF) / 255.0f;
		gColor[1] = ((m->color >> 8) & 0xFF) / 255.0f;
		gColor[2] = (m->color & 0xFF) / 255.0f;
		gColor[3] = ((m->color >> 24) & 0xFF) / 255.0f;
	}
}

static const MaterialOverride*
findOverride(const ObjectInst *inst, int index)
{
	const std::vector<MaterialOverride> *list = Materials::Get(inst);
	if(list)
		for(const MaterialOverride &m : *list)
			if(m.index == index)
				return &m;
	return nil;
}

static void
selectIndex(const ObjectInst *inst, int index)
{
	gIndex = index;
	loadFields(findOverride(inst, index));
	gMessage.clear();
}

static void
setMessage(const std::string &text, bool error)
{
	gMessage = text;
	gMessageIsError = error;
}

static rw::Texture*
previewTexture(void)
{
	if(gTxd[0] == '\0' || gTexture[0] == '\0')
		return nil;
	static MaterialOverride preview;
	if(preview.txd != gTxd || preview.texture != gTexture){
		preview = MaterialOverride();
		preview.index = 0;
		preview.modelId = 0;
		preview.txd = gTxd;
		preview.texture = gTexture;
		preview.color = 0;
	}
	return Materials::Resolve(preview);
}

void
uiMaterialsPanel(ObjectInst *inst)
{
	if(inst != gEditedInst){
		gEditedInst = inst;
		selectIndex(inst, 0);
	}

	std::vector<std::string> materials = Materials::ModelMaterials(inst->m_objectId);
	ImGui::TextDisabled("Retextures this object only, as SetDynamicObjectMaterial.");
	if(materials.empty()){
		ImGui::TextDisabled("The model is not loaded or has no material.");
		return;
	}

	if(ImGui::BeginTable("##materials", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)){
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Model texture");
		ImGui::TableSetupColumn("Override");
		ImGui::TableHeadersRow();
		for(int i = 0; i < (int)materials.size() && i <= Materials::MAX_INDEX; i++){
			const MaterialOverride *m = findOverride(inst, i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			char label[16];
			snprintf(label, sizeof(label), "%d", i);
			if(ImGui::Selectable(label, gIndex == i, ImGuiSelectableFlags_SpanAllColumns))
				selectIndex(inst, i);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(materials[i].empty() ? "(none)" : materials[i].c_str());
			ImGui::TableNextColumn();
			if(m){
				if(m->modelId >= 0)
					ImGui::Text("%s / %s", m->txd.c_str(), m->texture.c_str());
				else
					ImGui::TextUnformatted("colour");
				if(m->color != 0){
					ImGui::SameLine();
					ImGui::TextDisabled("0x%08X", m->color);
				}
			}
		}
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Material");
	ImGui::Text("Index %d", gIndex);

	int model = gSourceModel;
	if(ImGui::InputInt("Texture model", &model, 0, 0)){
		gSourceModel = model;
		std::string txd = Materials::ModelTxd(model);
		if(!txd.empty())
			snprintf(gTxd, sizeof(gTxd), "%s", txd.c_str());
	}
	ImGui::SetItemTooltip("A model whose TXD holds the texture (the modelid of SetObjectMaterial).\n"
	                      "Typing a TXD name below also works: a model using it is found.");
	ImGui::InputText("TXD", gTxd, sizeof(gTxd));

	if(strcmp(gLoadedTxd, gTxd) != 0){
		snprintf(gLoadedTxd, sizeof(gLoadedTxd), "%s", gTxd);
		gTextures = gTxd[0] ? Materials::TxdTextures(gTxd) : std::vector<std::string>();
	}
	if(gTxd[0] && gTextures.empty())
		ImGui::TextDisabled("Unknown or empty TXD.");
	else if(!gTextures.empty()){
		ImGui::InputTextWithHint("##filter", "Filter textures", gFilter, sizeof(gFilter));
		if(ImGui::BeginListBox("Texture", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8))){
			for(const std::string &name : gTextures){
				if(gFilter[0] && strstr(name.c_str(), gFilter) == nil)
					continue;
				if(ImGui::Selectable(name.c_str(), rw::strcmp_ci(name.c_str(), gTexture) == 0))
					snprintf(gTexture, sizeof(gTexture), "%s", name.c_str());
			}
			ImGui::EndListBox();
		}
	}
	rw::Texture *tex = previewTexture();
	if(tex && tex->raster){
		float size = ImGui::GetFontSize() * 6.0f;
		ImGui::Image((void*)(intptr_t)tex, ImVec2(size, size));
		ImGui::SameLine();
		ImGui::TextDisabled("%s\n%dx%d", tex->name, tex->raster->width, tex->raster->height);
	}

	ImGui::Checkbox("Colour", &gTint);
	ImGui::SetItemTooltip("The materialcolor (ARGB). Unchecked: 0, the model's own colour.");
	if(gTint){
		ImGui::SameLine();
		ImGui::ColorEdit4("##colour", gColor, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
	}

	if(ImGui::Button("Apply")){
		MaterialOverride m;
		m.index = gIndex;
		m.modelId = gSourceModel;
		m.txd = gTexture[0] ? gTxd : "none";
		m.texture = gTexture[0] ? gTexture : "none";
		m.color = editedColor();
		std::string error = Materials::Set(inst, m);
		if(error.empty()){
			loadFields(findOverride(inst, gIndex));
			setMessage("Applied. Saved with the map (Ctrl+S).", false);
		}else
			setMessage(error, true);
	}
	ImGui::SameLine();
	if(ImGui::Button("Remove"))
		if(Materials::Clear(inst, gIndex) > 0){
			loadFields(nil);
			setMessage("Removed.", false);
		}
	ImGui::SameLine();
	if(ImGui::Button("Remove all"))
		if(Materials::Clear(inst, -1) > 0){
			loadFields(nil);
			setMessage("All the overrides of this object removed.", false);
		}
	ImGui::SameLine();
	if(ImGui::Button("Paste")){
		MaterialOverride m;
		if(Materials::ParsePawn(ImGui::GetClipboardText(), &m)){
			std::string error = Materials::Set(inst, m);
			if(error.empty()){
				selectIndex(inst, m.index);
				setMessage("Pasted.", false);
			}else
				setMessage(error, true);
		}else
			setMessage("The clipboard holds no SetObjectMaterial / SetDynamicObjectMaterial line.", true);
	}
	ImGui::SetItemTooltip("Applies a SetObjectMaterial / SetDynamicObjectMaterial line of the clipboard.");

	if(!gMessage.empty()){
		if(gMessageIsError)
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", gMessage.c_str());
		else
			ImGui::TextDisabled("%s", gMessage.c_str());
	}
}
