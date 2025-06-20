//
// Created by Fernando Daniel Martinez Cortes on 18/04/25.
//

#ifndef LIVESKETCH_IMGUIMENU_H
#define LIVESKETCH_IMGUIMENU_H
#ifndef ImGuiWindowFlags_ResizeFromAnySide
#define ImGuiWindowFlags_ResizeFromAnySide (1 << 25) // Bit 25 fue usado para este flag en v1.89.4
#endif


// IMGUIMenu.h
#pragma once

#include <vector>
#include <string>
#include "imgui.h"
#include "../layers/LayerManager.h"
struct LayerItem {
    std::string name;
    bool visible;
};
// IMGUIMenu.h
struct MenuAction {
    bool addedLayer = false;
    bool removedLayer = false;
    bool renamedLayer = false;
    bool colorChanged = false;
    bool sizeChanged = false;
    bool eraser = false;
    bool ndiToggled = false;
    bool ndiActive = false;
    bool brushChanged= false;
    ImVec4 newColor = ImVec4(0, 0, 0, 1.0);
    ImVec4 RGBColor = ImVec4(0, 0, 0, 1.0);
    float newSize = 0.0f;
    std::string newLayerName;
    int renamedLayerIndex = -1;
    int removedLayerIndex = -1;
    int BrushIndex = 0;
    bool savePNG = false;

};

class IMGUIMenu {
public:
    IMGUIMenu();

    // Llama a esta función cada frame para mostrar el menú
    MenuAction Show();

    // Acceso al estado
    int GetCurrentLayer() const;
    const std::vector<LayerItem>& GetLayers() const;
    ImVec4 GetCurrentColorHSV() const;
    float GetBrushSize() const;
    bool IsEraserActive() const;
    bool show_demo = true;
    LayerManager* layerManager = nullptr;
    bool eraserActive;
    void SetLayerManager(LayerManager* manager);

private:
    int selectedBrushIndex;
    std::vector<std::string> brushNames;
    // Estado UI
    int currentLayerIndex;
    ImVec4 currentColorHSV;
    float brushSize;
};



#endif //LIVESKETCH_IMGUIMENU_H
