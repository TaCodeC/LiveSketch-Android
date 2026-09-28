#include "UI/Menu.h"

#include "Canvas/Canvas.h"

#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

struct Preset {
    const char* label;
    int width;
    int height;
};

// Los mismos presets que la versión anterior. "Full" usa el tamaño de la pantalla.
constexpr Preset kPresets[] = {
    {"640×360", 640, 360},     {"1280×720", 1280, 720},   {"1920×1080", 1920, 1080},
    {"2560×1440", 2560, 1440}, {"3840×2160", 3840, 2160}, {"Full", 0, 0},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

constexpr const char* kBrushNames[BrushSettings::kTypeCount] = {"Básico", "Texturizado", "Caligráfico", "Acuarela"};

constexpr const char* kRenamePopup = "Renombrar";
constexpr const char* kDeletePopup = "Eliminar capa";
constexpr const char* kClearPopup = "Limpiar capa";
constexpr const char* kExitPopup = "Salir";

void centerNextWindow() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

} // namespace

void Menu::notify(std::string text, uint64_t durationMs) {
    m_notice = std::move(text);
    m_noticeUntil = SDL_GetTicks() + durationMs;
}

// -----------------------------------------------------------------------------
// Pantalla de inicio
// -----------------------------------------------------------------------------

void Menu::startup(int screenWidth, int screenHeight, int maxSize, UiRequests& requests) {
    auto fits = [maxSize](const Preset& preset) {
        return preset.width <= maxSize && preset.height <= maxSize;
    };
    if (!fits(kPresets[m_preset])) {
        m_preset = kPresetCount - 1;
    }

    centerNextWindow();
    ImGui::Begin("Selecciona resolución del lienzo", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Elige un preset o Full pantalla:");

    if (ImGui::BeginCombo("Preset", kPresets[m_preset].label)) {
        for (int i = 0; i < kPresetCount; ++i) {
            const bool supported = fits(kPresets[i]);
            ImGui::BeginDisabled(!supported);
            std::string label = kPresets[i].label;
            if (!supported) {
                label += " (la GPU no lo admite)";
            }
            if (ImGui::Selectable(label.c_str(), i == m_preset)) {
                m_preset = i;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button("OK")) {
        const Preset& preset = kPresets[m_preset];
        const bool full = preset.width == 0;
        requests.canvasWidth = full ? std::min(screenWidth, maxSize) : preset.width;
        requests.canvasHeight = full ? std::min(screenHeight, maxSize) : preset.height;
    }
    ImGui::End();
}

// -----------------------------------------------------------------------------
// Panel de control
// -----------------------------------------------------------------------------

void Menu::controls(Canvas& canvas, UiState& state, UiRequests& requests) {
    // Tamaños de la versión anterior, pero sin salirse de la pantalla.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 margin(20.0f, 20.0f);
    ImGui::SetNextWindowPos(margin, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::min(500.0f, display.x - 2.0f * margin.x),
                                    std::min(850.0f, display.y - 2.0f * margin.y)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(300.0f, display.x), std::min(500.0f, display.y)),
                                        ImVec2(900, 3800));
    if (ImGui::Begin("LiveSketch Control")) {
        layerPanel(canvas);
        ImGui::Separator();
        ndiPanel(state, requests);
        ImGui::Separator();
        brushPanel(canvas, state);
        ImGui::Separator();
        ImGui::BeginDisabled(state.exporting);
        if (ImGui::Button("Guardar PNG")) {
            requests.savePng = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Centrar lienzo")) {
            requests.fitView = true;
        }
    }
    ImGui::End();
}

void Menu::layerPanel(Canvas& canvas) {
    LayerStack& layers = canvas.layers();

    ImGui::Text("Capas:");
    // La capa de arriba se lista primero, como en cualquier editor.
    ImGui::BeginChild("LayersScroll", ImVec2(0, 160), ImGuiChildFlags_Borders);
    for (int i = layers.count() - 1; i >= 0; --i) {
        Layer& layer = layers.at(i);
        ImGui::PushID(static_cast<int>(layer.id));

        bool visible = layer.visible;
        if (ImGui::Checkbox("##visible", &visible)) {
            layers.setVisible(i, visible);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(visible ? "Ocultar" : "Mostrar");
        }
        ImGui::SameLine();

        std::string label = layer.name;
        if (layer.opacity < 1.0f) {
            char percent[16];
            std::snprintf(percent, sizeof(percent), " (%d%%)", static_cast<int>(layer.opacity * 100.0f + 0.5f));
            label += percent;
        }
        label += "##select";
        if (ImGui::Selectable(label.c_str(), i == layers.activeIndex())) {
            canvas.selectLayer(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Cada botón vuelve a leer la capa activa: un botón anterior del mismo frame pudo
    // cambiar la pila (combinar borra la capa de arriba).
    auto activeIndex = [&layers] { return layers.activeIndex(); };
    auto activeId = [&layers] { return layers.active().id; };

    if (ImGui::Button("Agregar Capa")) {
        if (layers.count() >= canvas.maxLayers()) {
            notify("Límite de capas alcanzado (" + std::to_string(canvas.maxLayers()) + ")");
        } else if (!canvas.addLayer()) {
            notify("No hay memoria para otra capa");
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(layers.count() <= 1);
    if (ImGui::Button("Eliminar Capa")) {
        m_deleteRequested = true;
        m_dialogLayerId = activeId();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Renombrar Capa")) {
        m_renameRequested = true;
        m_dialogLayerId = activeId();
        std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", layers.active().name.c_str());
    }

    ImGui::BeginDisabled(activeIndex() >= layers.count() - 1);
    if (ImGui::Button("Subir")) {
        canvas.moveLayer(activeIndex(), activeIndex() + 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(activeIndex() <= 0);
    if (ImGui::Button("Bajar")) {
        canvas.moveLayer(activeIndex(), activeIndex() - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Duplicar")) {
        if (layers.count() >= canvas.maxLayers()) {
            notify("Límite de capas alcanzado (" + std::to_string(canvas.maxLayers()) + ")");
        } else if (!canvas.duplicateLayer(activeIndex())) {
            notify("No hay memoria para otra capa");
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canvas.canMergeDown(activeIndex()));
    if (ImGui::Button("Combinar abajo")) {
        canvas.mergeDown(activeIndex());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Limpiar")) {
        m_clearRequested = true;
        m_dialogLayerId = activeId();
    }

    float opacity = layers.active().opacity;
    if (ImGui::SliderFloat("Opacidad de capa", &opacity, 0.0f, 1.0f, "%.2f")) {
        layers.setOpacity(activeIndex(), opacity);
    }
}

void Menu::ndiPanel(const UiState& state, UiRequests& requests) {
    if (!state.ndiAvailable) {
        bool off = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("NDI", &off);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(esta compilación no incluye NDI)");
        return;
    }

    bool on = state.ndiRunning;
    if (ImGui::Checkbox("NDI", &on)) {
        requests.ndi = on ? 1 : 0;
    }
    if (!state.ndiRunning) {
        return;
    }
    ImGui::SameLine();
    if (!state.ndiError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", state.ndiError.c_str());
    } else {
        const int n = state.ndiConnections;
        ImGui::TextDisabled("Emitiendo «LiveSketch» · %d %s", n, n == 1 ? "receptor" : "receptores");
    }
}

void Menu::brushPanel(Canvas& canvas, UiState& state) {
    BrushSettings& brush = canvas.brushSettings();

    ImGui::Text("Tipo de Pincel:");
    const int type = std::clamp(brush.type, 0, BrushSettings::kTypeCount - 1);
    if (ImGui::BeginCombo("##brush", kBrushNames[type])) {
        for (int i = 0; i < BrushSettings::kTypeCount; ++i) {
            const bool selected = i == type;
            if (ImGui::Selectable(kBrushNames[i], selected) && !canvas.setBrushType(i)) {
                notify(std::string("No se pudo cargar el pincel ") + kBrushNames[i]);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();

    ImGui::Text("Color (HSV):");
    ImGui::ColorPicker3("##color", brush.color, ImGuiColorEditFlags_DisplayHSV);
    ImGui::SliderFloat("Opacidad", &brush.opacity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Grosor", &brush.radius, Brush::kMinRadius, Brush::kMaxRadius, "%.0f");
    ImGui::Checkbox("Borrador", &brush.eraser);
    ImGui::Checkbox("Dibujar con el dedo", &state.drawWithFinger);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Si está apagado, los dedos solo mueven y hacen zoom y se dibuja con el lápiz");
    }
}

// -----------------------------------------------------------------------------
// Diálogos y avisos
// -----------------------------------------------------------------------------

void Menu::overlays(Canvas* canvas, UiRequests& requests) {
    LayerStack* layers = canvas ? &canvas->layers() : nullptr;
    const int dialogIndex = layers ? layers->indexOf(m_dialogLayerId) : -1;

    if (m_renameRequested) {
        ImGui::OpenPopup(kRenamePopup);
        m_renameRequested = false;
    }
    if (m_deleteRequested) {
        ImGui::OpenPopup(kDeletePopup);
        m_deleteRequested = false;
    }
    if (m_clearRequested) {
        ImGui::OpenPopup(kClearPopup);
        m_clearRequested = false;
    }
    if (m_exitRequested) {
        ImGui::OpenPopup(kExitPopup);
        m_exitRequested = false;
    }

    centerNextWindow();
    if (ImGui::BeginPopupModal(kRenamePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        const bool enter = ImGui::InputText("Nuevo nombre", m_renameBuffer, sizeof(m_renameBuffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("OK") || enter) {
            if (dialogIndex >= 0) {
                layers->rename(dialogIndex, m_renameBuffer);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    centerNextWindow();
    if (ImGui::BeginPopupModal(kDeletePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (dialogIndex >= 0) {
            ImGui::Text("¿Eliminar «%s»? No se puede deshacer.", layers->at(dialogIndex).name.c_str());
        }
        if (ImGui::Button("Eliminar")) {
            if (dialogIndex >= 0) {
                canvas->removeLayer(dialogIndex);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    centerNextWindow();
    if (ImGui::BeginPopupModal(kClearPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (dialogIndex >= 0) {
            ImGui::Text("¿Borrar todo el contenido de «%s»?", layers->at(dialogIndex).name.c_str());
        }
        if (ImGui::Button("Limpiar")) {
            if (dialogIndex >= 0) {
                canvas->clearLayer(dialogIndex);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    centerNextWindow();
    if (ImGui::BeginPopupModal(kExitPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("¿Salir de LiveSketch?");
        ImGui::TextDisabled("Lo que no hayas guardado como PNG se perderá.");
        if (ImGui::Button("Salir")) {
            requests.quit = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_notice.empty() && SDL_GetTicks() >= m_noticeUntil) {
        m_notice.clear();
    }
    if (!m_notice.empty()) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->GetCenter().x, viewport->WorkPos.y + viewport->WorkSize.y - 24.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        ImGui::Begin("##notice", nullptr, flags);
        ImGui::PushTextWrapPos(std::min(ImGui::GetFontSize() * 40.0f, viewport->WorkSize.x * 0.8f));
        ImGui::TextUnformatted(m_notice.c_str());
        ImGui::PopTextWrapPos();
        ImGui::End();
    }
}
