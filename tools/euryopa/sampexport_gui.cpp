#include "euryopa.h"
#include "imgui/imgui.h"
#include "modloader.h"
#include "sampexport.h"
#include "materials.h"
#include "icons.h"

// The SA-MP export window: the stock map is read once (it takes a moment),
// the code is generated on demand.

static SampExport::StockMap gStockMap;
static SampExport::Options gExportOptions;
static SampExport::Result gExportResult;
static bool gExportGenerated;
static char gReferenceRoot[1024];
static char gExportStatus[1100];

static FILE*
openReferenceFile(const std::string &logicalPath)
{
	char root[1024];
	if(gReferenceRoot[0] != '\0')
		snprintf(root, sizeof(root), "%s", gReferenceRoot);
	else if(!GetGameRootDirectory(root, sizeof(root)))
		return nil;

	// Straight from the disk: the modloader overrides are the changes.
	char path[2048];
	if(!BuildPath(path, sizeof(path), root, logicalPath.c_str()))
		return nil;
	return fopen(getPath(path), "rb");
}

static void
loadStockMap(void)
{
	SampExport::LoadStockMap(openReferenceFile, gStockMap);
	if(gStockMap.loaded)
		snprintf(gExportStatus, sizeof(gExportStatus), "Stock map: %d placements (%d text, %d binary IPLs).",
		         (int)gStockMap.insts.size(), gStockMap.numTextFiles, gStockMap.numBinaryFiles);
	else
		snprintf(gExportStatus, sizeof(gExportStatus), "Stock map not read: DATA\\GTA.DAT not found in the reference folder.");
}

static void
generateExport(void)
{
	if(!gStockMap.loaded)
		loadStockMap();
	if(!gStockMap.loaded){
		gExportGenerated = false;
		return;
	}

	std::vector<SampExport::SceneObject> scene;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst->m_isDeleted)
			continue;
		SampExport::SceneObject obj;
		obj.p = { inst->m_objectId,
		          inst->m_translation.x, inst->m_translation.y, inst->m_translation.z,
		          inst->m_rotation.x, inst->m_rotation.y, inst->m_rotation.z, inst->m_rotation.w,
		          inst->m_area };
		if(const std::vector<MaterialOverride> *mats = Materials::Get(inst))
			for(const MaterialOverride &m : *mats)
				obj.materials.push_back({ m.index, m.modelId, m.txd, m.texture, m.color });
		scene.push_back(obj);
	}

	gExportResult = SampExport::BuildPawn(gStockMap, scene, gExportOptions, [](int modelId) {
		ObjectDef *obj = GetObjectDef(modelId);
		return obj ? std::string(obj->m_name) : std::string();
	});
	gExportGenerated = true;
}

static void
saveExport(void)
{
	char root[1024], path[1100];
	if(!GetGameRootDirectory(root, sizeof(root)) ||
	   !BuildPath(path, sizeof(path), root, "ariane_samp_export.pwn")){
		snprintf(gExportStatus, sizeof(gExportStatus), "Could not build the export path.");
		return;
	}
	FILE *f = fopen(path, "wb");
	if(f == nil){
		snprintf(gExportStatus, sizeof(gExportStatus), "Could not write %s", path);
		return;
	}
	fwrite(gExportResult.code.data(), 1, gExportResult.code.size(), f);
	fclose(f);
	snprintf(gExportStatus, sizeof(gExportStatus), "Saved to %s", path);
}

void
uiSampExportWindow(bool *open)
{
	ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_FILE_EXPORT " SA-MP Export", open)){
		ImGui::End();
		return;
	}

	if(!isSA()){
		ImGui::TextDisabled("The SA-MP export needs San Andreas.");
		ImGui::End();
		return;
	}

	ImGui::TextWrapped("Pawn code of the changes against the stock map, read from the game files "
	                   "(modloader ignored): RemoveBuildingForPlayer for the stock objects removed, "
	                   "moved or rotated, and an object for each new placement.");
	if(!ModloaderIsActive())
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
		                   "If you saved into the game files, set a clean GTA SA folder as reference.");

	ImGui::SetNextItemWidth(-160.0f);
	if(ImGui::InputTextWithHint("##reference", "Reference folder (empty: the game folder)",
	                            gReferenceRoot, sizeof(gReferenceRoot)))
		gStockMap.loaded = false;
	ImGui::SameLine();
	if(ImGui::Button("Reload stock map"))
		loadStockMap();

	ImGui::Separator();
	int function = gExportOptions.function;
	ImGui::RadioButton("CreateDynamicObject (Streamer)", &function, SampExport::FUNC_STREAMER);
	ImGui::SameLine();
	ImGui::RadioButton("CreateObject", &function, SampExport::FUNC_NATIVE);
	gExportOptions.function = (SampExport::ObjectFunction)function;
	ImGui::Checkbox("Remove the LODs left without their building", &gExportOptions.removeOrphanLods);
	if(gExportOptions.function == SampExport::FUNC_STREAMER){
		ImGui::SameLine();
		ImGui::Checkbox("Pass the IPL interior", &gExportOptions.useInterior);
	}

	ImGui::Checkbox("Only around a point", &gExportOptions.useArea);
	if(gExportOptions.useArea){
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		ImGui::DragFloat("Radius", &gExportOptions.areaRadius, 1.0f, 1.0f, 6000.0f, "%.0f m");
		ImGui::SameLine();
		if(ImGui::Button("Camera target")){
			gExportOptions.areaX = TheCamera.m_target.x;
			gExportOptions.areaY = TheCamera.m_target.y;
		}
		ImGui::SameLine();
		ImGui::Text("%.1f, %.1f", gExportOptions.areaX, gExportOptions.areaY);
	}

	if(ImGui::Button(ICON_FA_FILE_EXPORT " Generate"))
		generateExport();
	if(gExportGenerated){
		ImGui::SameLine();
		if(ImGui::Button(ICON_FA_COPY " Copy"))
			ImGui::SetClipboardText(gExportResult.code.c_str());
		ImGui::SameLine();
		if(ImGui::Button(ICON_FA_FLOPPY_DISK " Save .pwn"))
			saveExport();
	}
	if(gExportStatus[0] != '\0')
		ImGui::TextDisabled("%s", gExportStatus);

	if(gExportGenerated){
		ImGui::Text("%d removed buildings (%d LODs), %d objects",
		            gExportResult.numRemoved + gExportResult.numLods, gExportResult.numLods,
		            gExportResult.numCreated);
		if(gExportResult.numCustom > 0){
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), ", %d custom models", gExportResult.numCustom);
		}
		ImGui::InputTextMultiline("##pawn", (char*)gExportResult.code.c_str(), gExportResult.code.size() + 1,
		                          ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
	}

	ImGui::End();
}
