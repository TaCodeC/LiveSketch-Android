// IMGUIMenu.cpp - corregido con botón NDI, botón Guardar LVSKT y collectLayerData()
#include "IMGUIMenu.h"
#include "../FileManager/FileManager.h"
#include <android/log.h>
#include <GLES3/gl3.h>
#define LOG_TAG "LiveSketch"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))

extern FileManager* gFileManager;
static LayerManager* gLayerManager = nullptr;  // Guardamos puntero para collectLayerData

// Estructura LayerData definida en FileManager.h
// Implementación de collectLayerData: lee cada FBO y genera LayerData
std::vector<LayerData> collectLayerData() {
    std::vector<LayerData> out;
    if (!gLayerManager) return out;

    for (int i = 0; i < (int)gLayerManager->layers.size(); ++i) {
        auto& layer = gLayerManager->layers[i];
        LayerData data;
        data.name = layer->layername;
        data.visible = layer->visible;
        data.width = gLayerManager->layerWidth;
        data.height = gLayerManager->layerHeight;

        // Leer pixeles RGBA desde el FBO
        GLuint fbo = layer->fbo;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        size_t sz = static_cast<size_t>(data.width) * data.height * 4;
        data.fboPixels.resize(sz);
        glReadPixels(0, 0, data.width, data.height, GL_RGBA, GL_UNSIGNED_BYTE, data.fboPixels.data());

        out.push_back(std::move(data));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return out;
}

IMGUIMenu::IMGUIMenu()
        : currentLayerIndex(0),
          currentColorHSV(0.0f, 1.0f, 1.0f, 1.0f),
          brushSize(10.0f),
          eraserActive(false),
          selectedBrushIndex(0)
{
    brushNames = {"Básico","Texturizado","Caligráfico","Acuarela"};
}

int IMGUIMenu::GetCurrentLayer() const { return currentLayerIndex; }
ImVec4 IMGUIMenu::GetCurrentColorHSV() const { return currentColorHSV; }
float IMGUIMenu::GetBrushSize() const { return brushSize; }
bool IMGUIMenu::IsEraserActive() const { return eraserActive; }

MenuAction IMGUIMenu::Show() {
    MenuAction action;
    static ImVec4 lastColor = currentColorHSV;
    static float lastSize   = brushSize;
    static char renameBuf[64] = "";
    static bool ndiActive = false;

    ImGui::SetNextWindowSizeConstraints(ImVec2(300,500), ImVec2(900,3800));
    ImGui::SetNextWindowSize(ImVec2(500,850), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LiveSketch Control")) { ImGui::End(); return action; }

    // Capas
    ImGui::Text("Capas:");
    ImGui::BeginChild("LayersScroll", ImVec2(0,150), true);
    int layerCount = gLayerManager ? (int)gLayerManager->layers.size() : 0;
    for (int i = 0; i < layerCount; ++i) {
        auto& L = gLayerManager->layers[i];
        if (ImGui::Selectable(L->layername.c_str(), currentLayerIndex == i)) {
            currentLayerIndex = i;
            gLayerManager->targetLayer = i;
        }
        ImGui::SameLine();
        bool vis = L->visible;
        if (ImGui::Checkbox(("##vis"+std::to_string(i)).c_str(), &vis)) L->visible = vis;
    }
    ImGui::EndChild();
    if (ImGui::Button("Agregar Capa")) { int p=currentLayerIndex+1; gLayerManager->addLayer(p); action.addedLayer=true; currentLayerIndex=p; }
    ImGui::SameLine();
    if (ImGui::Button("Eliminar Capa") && layerCount>1) { gLayerManager->removeLayer(currentLayerIndex); action.removedLayer=true; action.removedLayerIndex=currentLayerIndex; currentLayerIndex = std::min(currentLayerIndex,(int)gLayerManager->layers.size()-1); }
    ImGui::SameLine(); if (ImGui::Button("Renombrar Capa")) { auto& L=gLayerManager->layers[currentLayerIndex]; strncpy(renameBuf, L->layername.c_str(),64); ImGui::OpenPopup("Renombrar"); }
    if (ImGui::BeginPopupModal("Renombrar",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::InputText("Nuevo nombre",renameBuf,64); if(ImGui::Button("OK")){ auto& L=gLayerManager->layers[currentLayerIndex]; L->layername=renameBuf; action.renamedLayer=true; action.renamedLayerIndex=currentLayerIndex; action.newLayerName=renameBuf; ImGui::CloseCurrentPopup(); } ImGui::SameLine(); if(ImGui::Button("Cancelar")) ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
    ImGui::Separator();

    // NDI
    if (ImGui::Checkbox("NDI", &ndiActive)) LOGI("NDI: %s", ndiActive?"On":"Off"); action.ndiActive=ndiActive;
    ImGui::Separator();

    // Pincel
    ImGui::Text("Tipo de Pincel:"); if(ImGui::BeginCombo("##brush", brushNames[selectedBrushIndex].c_str())){ for(int i=0;i<brushNames.size();++i){ bool sel=i==selectedBrushIndex; if(ImGui::Selectable(brushNames[i].c_str(),sel)){selectedBrushIndex=i; action.brushChanged=true; action.BrushIndex=i;} if(sel) ImGui::SetItemDefaultFocus(); } ImGui::EndCombo(); }
    ImGui::Separator();
    ImGui::Text("Color (HSV):"); if(ImGui::ColorPicker3("##col",(float*)&currentColorHSV,ImGuiColorEditFlags_DisplayHSV)){ if(memcmp(&lastColor,&currentColorHSV,sizeof(ImVec4))){ action.colorChanged=true; action.newColor=currentColorHSV; lastColor=currentColorHSV; }}
    if(ImGui::SliderFloat("Opacidad",&currentColorHSV.w,0,1,"%.2f")){ if(currentColorHSV.w!=lastColor.w){action.colorChanged=true;action.newColor=currentColorHSV;lastColor=currentColorHSV;}}
    if(ImGui::SliderFloat("Grosor",&brushSize,1,200)){ if(brushSize!=lastSize){action.sizeChanged=true;action.newSize=brushSize;lastSize=brushSize;}}
    ImGui::Checkbox("Borrador", &eraserActive); action.eraser=eraserActive;
    ImGui::Separator(); if(ImGui::Button("Guardar PNG")) action.savePNG=true;

    // Botón LVSKT
    ImGui::Separator(); if(ImGui::Button("Guardar proyecto (.lvskt)")){
        auto layers = collectLayerData();
        if(gFileManager) gFileManager->showSaveFileDialog("mi_proyecto", layers,
                                                          [](bool s,const std::string& m){ LOGI("FileManager: %s",m.c_str()); });
    }

    ImGui::End();
    return action;
}

void IMGUIMenu::SetLayerManager(LayerManager* manager) {
    layerManager = manager;
    gLayerManager = manager;
}
