// Pinceles: la biblioteca por categorías, los ajustes de cada pincel con un trazo de
// muestra, y lo que se guarda de ellos (pinceles.txt): el elegido de cada herramienta, el
// tamaño y la opacidad que tenía cada uno y los ajustes que cambió el usuario.
#include "UI/Ui.h"

#include "UI/Anim.h"
#include "UI/Icons.h"
#include "UI/PanelParts.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;
using ui::parts::pressFeedback;
using ui::parts::textureId;

namespace {

// El archivo se escribe un rato después del último cambio: arrastrar un deslizador no lo
// reescribe a cada paso.
constexpr uint64_t kSaveDelayMs = 1500;
constexpr const char* kToolKeys[2] = {"pintar", "borrar"};

// Medidas (pt).
constexpr float kCategoryWidth = 136.0f;
constexpr float kCategoryRow = 40.0f;
constexpr float kBrushRow = 72.0f;          // caben cuatro sin desplazar
constexpr float kRowGap = 4.0f;
constexpr float kListPad = 8.0f;
constexpr float kSettingsHeight = 560.0f;   // panel con los ajustes (si cabe)
constexpr float kSampleHeight = 76.0f;      // trazo de muestra de los ajustes
constexpr float kSlider = 36.0f;
constexpr float kSliderGap = 6.0f;
constexpr float kSection = 36.0f;           // rótulo de sección
constexpr float kChipWidth = 66.0f;         // fichas de puntas y granos
constexpr float kChipHeight = 74.0f;
constexpr float kChipGap = 6.0f;
constexpr float kSidePad = 12.0f;

// Muestra del panel de ajustes (las de la lista van por índice del pincel).
constexpr int kSettingsSample = 1000;

const char* categoryIcon(BrushCategory category) {
    switch (category) {
    case BrushCategory::Sketching:
        return icon::kPencilLine;
    case BrushCategory::Inking:
        return icon::kPenTool;
    case BrushCategory::Painting:
        return icon::kPaintbrush;
    case BrushCategory::Airbrushing:
        return icon::kSprayCan;
    case BrushCategory::Artistic:
        return icon::kPalette;
    case BrushCategory::Calligraphy:
        return icon::kFeather;
    case BrushCategory::Classic:
        return icon::kSparkles;
    }
    return icon::kBrush;
}

std::string brushesPath() {
    char* folder = SDL_GetPrefPath("TaCodec", "LiveSketch");
    if (!folder) {
        return {};
    }
    std::string path = std::string(folder) + "pinceles.txt";
    SDL_free(folder);
    return path;
}

bool parseNumber(const std::string& text, float& out) {
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool isClassic(BrushTip tip) {
    return tip == BrushTip::Classic0 || tip == BrushTip::Classic1 || tip == BrushTip::Classic2 ||
           tip == BrushTip::Classic3;
}

} // namespace

// -----------------------------------------------------------------------------
// Estado y archivo
// -----------------------------------------------------------------------------

void Ui::initBrushes() {
    const auto library = brushes::library();
    m_brushParams.clear();
    for (const BrushPreset& preset : library) {
        m_brushParams.push_back(preset.params);
    }
    for (auto& memory : m_brushMemory) {
        memory.assign(library.size(), BrushMemory{});
    }
    const std::string_view defaults[2] = {brushes::kDefaultPaint, brushes::kDefaultErase};
    for (int tool = 0; tool < 2; ++tool) {
        const int index = std::max(brushes::indexOf(defaults[tool]), 0);
        m_presets[tool] = {index, m_brushParams[static_cast<size_t>(index)].size,
                           m_brushParams[static_cast<size_t>(index)].opacity};
    }
    loadBrushes();
}

void Ui::loadBrushes() {
    const std::string path = brushesPath();
    if (path.empty()) {
        return;
    }
    std::ifstream in(path);
    if (!in) {
        return;
    }
    // Líneas "clave ...". Lo que no se entiende se salta: el archivo puede venir de otra
    // versión con pinceles o ajustes que esta no tiene.
    int selected[2] = {-1, -1};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string kind;
        std::string tool;
        std::string id;
        words >> kind;
        if (kind == "pincel" || kind == "recuerdo") {
            words >> tool >> id;
            const int t = tool == kToolKeys[0] ? 0 : (tool == kToolKeys[1] ? 1 : -1);
            const int index = brushes::indexOf(id);
            if (t < 0 || index < 0) {
                continue;
            }
            if (kind == "pincel") {
                selected[t] = index;
                continue;
            }
            std::string size;
            std::string opacity;
            words >> size >> opacity;
            BrushMemory memory;
            if (parseNumber(size, memory.size) && parseNumber(opacity, memory.opacity)) {
                memory.size = std::clamp(memory.size, 0.0f, 1.0f);
                memory.opacity = std::clamp(memory.opacity, 0.01f, 1.0f);
                memory.set = true;
                m_brushMemory[t][static_cast<size_t>(index)] = memory;
            }
        } else if (kind == "ajuste") {
            std::string key;
            std::string value;
            words >> id >> key >> value;
            const int index = brushes::indexOf(id);
            if (index >= 0) {
                brushes::applySetting(m_brushParams[static_cast<size_t>(index)], key, value);
            }
        }
    }
    for (BrushParams& params : m_brushParams) {
        brushes::sanitize(params);
    }
    for (int t = 0; t < 2; ++t) {
        ToolPreset& preset = m_presets[t];
        if (selected[t] >= 0) {
            preset.brush = selected[t];
        }
        const BrushMemory& memory = m_brushMemory[t][static_cast<size_t>(preset.brush)];
        const BrushParams& params = m_brushParams[static_cast<size_t>(preset.brush)];
        preset.size = memory.set ? memory.size : params.size;
        preset.opacity = memory.set ? memory.opacity : params.opacity;
    }
}

void Ui::saveBrushes() {
    m_brushSaveAt = 0;
    const std::string path = brushesPath();
    if (path.empty() || m_brushParams.empty()) {
        return;
    }
    // Lo que tiene cada herramienta ahora también se recuerda.
    for (int t = 0; t < 2; ++t) {
        const ToolPreset& preset = m_presets[t];
        m_brushMemory[t][static_cast<size_t>(preset.brush)] = {preset.size, preset.opacity, true};
    }
    std::ostringstream out;
    const auto library = brushes::library();
    for (int t = 0; t < 2; ++t) {
        out << "pincel " << kToolKeys[t] << ' ' << library[static_cast<size_t>(m_presets[t].brush)].id << '\n';
    }
    char number[64];
    for (int t = 0; t < 2; ++t) {
        for (size_t i = 0; i < library.size(); ++i) {
            const BrushMemory& memory = m_brushMemory[t][i];
            if (memory.set) {
                std::snprintf(number, sizeof(number), "%.4f %.4f", static_cast<double>(memory.size),
                              static_cast<double>(memory.opacity));
                out << "recuerdo " << kToolKeys[t] << ' ' << library[i].id << ' ' << number << '\n';
            }
        }
    }
    for (size_t i = 0; i < library.size(); ++i) {
        std::istringstream changes(brushes::describeChanges(library[i].params, m_brushParams[i]));
        std::string change;
        while (std::getline(changes, change)) {
            out << "ajuste " << library[i].id << ' ' << change << '\n';
        }
    }
    std::ofstream file(path, std::ios::trunc);
    file << out.str();
}

void Ui::scheduleBrushSave() { m_brushSaveAt = SDL_GetTicks() + kSaveDelayMs; }

void Ui::saveBrushesIfDue(bool force) {
    if (m_brushSaveAt != 0 && (force || SDL_GetTicks() >= m_brushSaveAt)) {
        saveBrushes();
    }
}

void Ui::selectBrush(Tool tool, int index) {
    const int t = static_cast<int>(tool);
    ToolPreset& preset = m_presets[t];
    if (index < 0 || index >= static_cast<int>(m_brushParams.size()) || index == preset.brush) {
        return;
    }
    m_brushMemory[t][static_cast<size_t>(preset.brush)] = {preset.size, preset.opacity, true};
    const BrushMemory& memory = m_brushMemory[t][static_cast<size_t>(index)];
    const BrushParams& params = m_brushParams[static_cast<size_t>(index)];
    preset.brush = index;
    preset.size = memory.set ? memory.size : params.size;
    preset.opacity = memory.set ? memory.opacity : params.opacity;
    scheduleBrushSave();
}

const BrushParams& Ui::toolBrush(Tool tool) const {
    const int index = m_presets[static_cast<int>(tool)].brush;
    if (index < 0 || index >= static_cast<int>(m_brushParams.size())) {
        return brushes::basic();
    }
    return m_brushParams[static_cast<size_t>(index)];
}

float Ui::toolRadius(Tool tool) const {
    return brushes::radiusFor(toolBrush(tool), m_presets[static_cast<int>(tool)].size);
}

// -----------------------------------------------------------------------------
// Panel
// -----------------------------------------------------------------------------

void Ui::brushesPanel() {
    const Layout& L = m_layout;
    if (m_panel != Panel::Brushes) {
        m_brushPage = false;
        m_brushScroll = true;
    }
    const int toolIndex = static_cast<int>(m_tool);
    const float listContent = th::kHeaderHeight + kListPad * 2.0f + kCategoryRow * kBrushCategoryCount +
                              kRowGap * (kBrushCategoryCount - 1);
    const float target = m_brushPage ? std::max(listContent, kSettingsHeight) : listContent;
    const float content = ui::anim::follow(ImHashStr("##brushes-height"), target, 20.0f, 0.2f);
    const float width = pt(L.compact ? 372.0f : 400.0f);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * (0.5f + toolIndex));
    PanelFrame f;
    if (!beginPanel(f, Panel::Brushes, "##panel-brushes", L.rightBar.Max.x - width, width, pt(content), anchorX,
                    false)) {
        return;
    }
    ImDrawList* dl = f.dl;
    const float top = f.content.Min.y;
    const ImRect body(ImVec2(f.content.Min.x, top + pt(th::kHeaderHeight)), f.content.Max);
    if (m_brushPage) {
        brushSettings(dl, f.content);
    } else {
        const float cy = ui::parts::panelHeader(dl, f.content, top, "Pinceles");
        ui::parts::tag(dl, f.content.Max.x - pt(12.0f), cy, m_tool == Tool::Brush ? "PINCEL" : "BORRADOR",
                       IM_COL32(255, 255, 255, 20), IM_COL32(235, 235, 245, 179));
        brushList(dl, body);
    }
    endPanel(f);
}

void Ui::brushList(ImDrawList* dl, const ImRect& view) {
    const auto library = brushes::library();
    const int toolIndex = static_cast<int>(m_tool);
    const int selected = m_presets[toolIndex].brush;
    if (m_brushScroll) {
        m_brushCategory = static_cast<int>(library[static_cast<size_t>(selected)].category);
    }

    // Categorías, a la izquierda.
    const float columnRight = view.Min.x + pt(kCategoryWidth);
    const ImRect categories(view.Min, ImVec2(columnRight, view.Max.y));
    const float categoriesHeight =
        pt(kListPad * 2.0f + kCategoryRow * kBrushCategoryCount + kRowGap * (kBrushCategoryCount - 1));
    const float categoryScroll = ui::beginScroll("##brush-categories", categories, categoriesHeight);
    for (int c = 0; c < kBrushCategoryCount; ++c) {
        const float y = categories.Min.y + pt(kListPad) - categoryScroll + pt((kCategoryRow + kRowGap) * c);
        const ImRect row(ImVec2(categories.Min.x + pt(8.0f), y), ImVec2(categories.Max.x - pt(4.0f), y + pt(kCategoryRow)));
        ImGui::PushID(c);
        const ImGuiID id = ImGui::GetID("##category");
        const Press press = ui::pressable(id, row);
        ImGui::PopID();
        const bool on = c == m_brushCategory;
        const float t = ui::anim::follow(id + 7u, on ? 1.0f : 0.0f, 20.0f);
        if (t > 0.002f) {
            dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(IM_COL32(255, 255, 255, 26), t), pt(th::kControlRadius));
        }
        pressFeedback(dl, id, row, press, pt(th::kControlRadius));
        const float cy = row.GetCenter().y;
        const ImU32 color = ui::mix(th::kSecondaryLabel, th::kLabel, t);
        ui::icon(dl, categoryIcon(static_cast<BrushCategory>(c)), ImVec2(row.Min.x + pt(10.0f + 9.0f), cy), 17.0f,
                 ui::mix(th::kMutedIcon, th::kLabel, t));
        ui::label(dl, t > 0.5f ? Weight::SemiBold : Weight::Regular, th::kCallout,
                  ImVec2(row.Min.x + pt(10.0f + 18.0f + 10.0f), cy), Align::Left, color,
                  brushes::categoryName(static_cast<BrushCategory>(c)), row.Max.x - row.Min.x - pt(42.0f));
        if (press.clicked) {
            m_brushCategory = c;
        }
    }
    ui::endScroll();
    dl->AddLine(ImVec2(columnRight, view.Min.y + pt(10.0f)), ImVec2(columnRight, view.Max.y - pt(10.0f)), th::kRule,
                ui::hairline());

    // Pinceles de la categoría, con su trazo de muestra.
    int indices[32];
    int count = 0;
    for (size_t i = 0; i < library.size() && count < 32; ++i) {
        if (static_cast<int>(library[i].category) == m_brushCategory) {
            indices[count++] = static_cast<int>(i);
        }
    }
    const ImRect list(ImVec2(columnRight, view.Min.y), view.Max);
    const float listHeight = pt(kListPad * 2.0f + kBrushRow * count + kRowGap * std::max(count - 1, 0));
    if (m_brushScroll) {
        for (int k = 0; k < count; ++k) {
            if (indices[k] == selected) {
                const float rowTop = pt(kListPad + (kBrushRow + kRowGap) * k);
                ui::scrollIntoView("##brush-list", rowTop - pt(kListPad), rowTop + pt(kBrushRow + kListPad),
                                   list.GetHeight());
            }
        }
        m_brushScroll = false;
    }
    const float scroll = ui::beginScroll("##brush-list", list, listHeight);
    const float pixels = m_status.pixelsPerUnit;
    for (int k = 0; k < count; ++k) {
        const int index = indices[k];
        const BrushPreset& preset = library[static_cast<size_t>(index)];
        const BrushParams& params = m_brushParams[static_cast<size_t>(index)];
        const float y = list.Min.y + pt(kListPad) - scroll + pt((kBrushRow + kRowGap) * k);
        const ImRect row(ImVec2(list.Min.x + pt(6.0f), y), ImVec2(list.Max.x - pt(8.0f), y + pt(kBrushRow)));
        if (row.Max.y < list.Min.y || row.Min.y > list.Max.y) {
            continue;
        }
        ImGui::PushID(index);
        const ImGuiID id = ImGui::GetID("##brush");
        const bool on = index == selected;
        // El pincel elegido lleva un botón para sus ajustes.
        const ImRect gear(ImVec2(row.Max.x - pt(8.0f + 32.0f), row.Min.y + pt(4.0f)),
                          ImVec2(row.Max.x - pt(8.0f), row.Min.y + pt(4.0f + 30.0f)));
        Press gearPress;
        if (on) {
            gearPress = ui::pressable("##settings", gear);
        }
        const Press press = ui::pressable(id, row);
        ImGui::PopID();
        const float t = ui::anim::follow(id + 7u, on ? 1.0f : 0.0f, 20.0f);
        if (t > 0.002f) {
            dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(th::kAccent, t), pt(th::kRowRadius));
        }
        pressFeedback(dl, id, row, press, pt(th::kRowRadius));

        const float nameY = row.Min.y + pt(18.0f);
        const float nameRight = row.Max.x - pt(on ? 48.0f : 12.0f);
        const float nameX = row.Min.x + pt(12.0f);
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(nameX, nameY), Align::Left, th::kLabel, preset.name,
                  nameRight - nameX);
        if (!(params == preset.params)) {
            // Tiene ajustes cambiados.
            const float nameWidth = std::min(ui::measure(Weight::SemiBold, th::kSubhead, preset.name).x, nameRight - nameX);
            dl->AddCircleFilled(ImVec2(nameX + nameWidth + pt(8.0f), nameY), pt(3.0f),
                                ui::mix(th::kAccentText, th::kLabel, t), 0);
        }
        if (on) {
            ImGui::PushID(index);
            pressFeedback(dl, ImGui::GetID("##settings"), gear, gearPress, pt(8.0f));
            ImGui::PopID();
            ui::icon(dl, icon::kSliders, gear.GetCenter(), 17.0f, th::kLabel);
        }

        const ImRect sample(ImVec2(row.Min.x + pt(8.0f), row.Min.y + pt(30.0f)),
                            ImVec2(row.Max.x - pt(8.0f), row.Max.y - pt(6.0f)));
        const GLuint texture = m_previews.brushPreview(index, params, static_cast<int>(std::lround(sample.GetWidth() * pixels)),
                                                       static_cast<int>(std::lround(sample.GetHeight() * pixels)));
        if (texture != 0) {
            dl->AddImage(ImTextureRef(textureId(texture)), sample.Min, sample.Max, ImVec2(0.0f, 0.0f),
                         ImVec2(1.0f, 1.0f), ui::mix(IM_COL32(235, 235, 245, 225), IM_COL32_WHITE, t));
        }

        if (gearPress.clicked) {
            m_brushPage = true;
        } else if (press.clicked) {
            // Tocar el que ya está elegido abre sus ajustes.
            if (on) {
                m_brushPage = true;
            } else {
                selectBrush(m_tool, index);
            }
        }
    }
    ui::endScroll();
}

void Ui::brushSettings(ImDrawList* dl, const ImRect& content) {
    const auto library = brushes::library();
    const int toolIndex = static_cast<int>(m_tool);
    const int index = m_presets[toolIndex].brush;
    const BrushPreset& preset = library[static_cast<size_t>(index)];
    BrushParams& params = m_brushParams[static_cast<size_t>(index)];
    const BrushParams before = params;

    // Cabecera: volver, el nombre y restablecer (con un segundo toque para confirmar).
    const float top = content.Min.y;
    bool back = false;
    float titleRight = 0.0f;
    const bool modified = !(params == preset.params);
    const ImGuiID resetId = ImHashStr("##brush-reset");
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const double armedAt = storage->GetFloat(resetId, -100.0f);
    const bool armed = ImGui::GetTime() - armedAt < 3.0;
    const char* resetText = armed ? "¿Restablecer?" : "Restablecer";
    const float resetWidth = ui::measure(Weight::SemiBold, th::kFootnote, resetText).x + pt(20.0f);
    const float cy = top + pt(th::kHeaderHeight * 0.5f);
    const ImRect reset(ImVec2(content.Max.x - pt(10.0f) - resetWidth, cy - pt(15.0f)),
                       ImVec2(content.Max.x - pt(10.0f), cy + pt(15.0f)));
    ui::parts::backHeader(dl, ImRect(content.Min, ImVec2(reset.Min.x - pt(6.0f), content.Max.y)), top, "##brush-back",
                          "", &back, &titleRight);
    // El título va aparte para cortarlo antes del botón de restablecer.
    ui::label(dl, Weight::SemiBold, th::kPanelTitle, ImVec2(titleRight, cy), Align::Left, th::kLabel, preset.name,
              std::max(pt(40.0f), reset.Min.x - pt(8.0f) - titleRight));
    ui::separator(dl, content.Min.x, content.Max.x, top + pt(th::kHeaderHeight) - ui::hairline(), th::kRule);
    {
        const Press press = ui::pressable(resetId, reset, modified);
        pressFeedback(dl, resetId, reset, press, pt(8.0f));
        const ImU32 color = !modified ? th::kTertiaryLabel : (armed ? th::kRed : th::kAccentText);
        ui::label(dl, Weight::SemiBold, th::kFootnote, reset.GetCenter(), Align::Center, color, resetText);
        if (armed) {
            ui::anim::keepAlive();
        }
        if (press.clicked) {
            if (armed) {
                params = preset.params;
                storage->SetFloat(resetId, -100.0f);
            } else {
                storage->SetFloat(resetId, static_cast<float>(ImGui::GetTime()));
            }
        }
    }
    if (back) {
        m_brushPage = false;
        return;
    }

    // Trazo de muestra, fijo encima de los ajustes.
    const float pixels = m_status.pixelsPerUnit;
    float y = top + pt(th::kHeaderHeight + 8.0f);
    const ImRect sampleBox(ImVec2(content.Min.x + pt(kSidePad), y), ImVec2(content.Max.x - pt(kSidePad), y + pt(kSampleHeight)));
    dl->AddRectFilled(sampleBox.Min, sampleBox.Max, IM_COL32(0, 0, 0, 56), pt(th::kControlRadius));
    ui::outline(dl, sampleBox, pt(th::kControlRadius), th::kControlBorder, ui::hairline());
    const ImRect sample(ImVec2(sampleBox.Min.x + pt(10.0f), sampleBox.Min.y + pt(8.0f)),
                        ImVec2(sampleBox.Max.x - pt(10.0f), sampleBox.Max.y - pt(8.0f)));
    const GLuint texture = m_previews.brushPreview(kSettingsSample, params,
                                                   static_cast<int>(std::lround(sample.GetWidth() * pixels)),
                                                   static_cast<int>(std::lround(sample.GetHeight() * pixels)));
    if (texture != 0) {
        dl->AddImage(ImTextureRef(textureId(texture)), sample.Min, sample.Max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     IM_COL32_WHITE);
    }
    y = sampleBox.Max.y + pt(8.0f);
    ui::separator(dl, content.Min.x, content.Max.x, y, th::kRule);

    // Ajustes, en una lista que se desplaza.
    const ImRect view(ImVec2(content.Min.x, y), content.Max);
    const ImGuiID heightId = ImHashStr("##brush-settings-height");
    const float contentHeight = storage->GetFloat(heightId, pt(1200.0f));
    const float scroll = ui::beginScroll("##brush-settings", view, contentHeight);
    const float left = view.Min.x + pt(kSidePad);
    const float right = view.Max.x - pt(kSidePad);
    const float start = view.Min.y - scroll;
    y = start + pt(4.0f);
    auto visible = [&](float y0, float y1) { return y1 >= view.Min.y && y0 <= view.Max.y; };

    auto section = [&](const char* title) {
        const float labelY = y + pt(kSection * 0.5f + 2.0f);
        if (visible(y, y + pt(kSection))) {
            ui::sectionLabel(dl, left + pt(4.0f), right - pt(4.0f), labelY, title);
        }
        y += pt(kSection);
    };
    auto slider = [&](const char* key, bool enabled = true) {
        const brushes::ParamInfo* info = brushes::findParam(key);
        if (!info) {
            return;
        }
        const ImRect r(ImVec2(left, y), ImVec2(right, y + pt(kSlider)));
        if (visible(r.Min.y, r.Max.y)) {
            float& value = params.*(info->member);
            float t = brushes::toSlider(*info, value);
            const std::string text = brushes::formatValue(*info, value);
            if (ui::paramSlider(key, r, &t, info->label, text.c_str(), enabled)) {
                value = brushes::fromSlider(*info, t);
            }
        }
        y += pt(kSlider + kSliderGap);
    };
    // Fichas en una rejilla de columnas iguales. `draw` pinta el contenido de cada una.
    auto chips = [&](const char* id, int count, int current, auto&& draw) -> int {
        const float width = right - left;
        const int columns = std::max(3, static_cast<int>((width + pt(kChipGap)) / pt(kChipWidth + kChipGap)));
        const float chipWidth = (width - pt(kChipGap) * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const int rows = (count + columns - 1) / columns;
        int picked = -1;
        for (int i = 0; i < count; ++i) {
            const int col = i % columns;
            const int row = i / columns;
            const float x0 = left + (chipWidth + pt(kChipGap)) * static_cast<float>(col);
            const float y0 = y + pt(kChipHeight + kChipGap) * static_cast<float>(row);
            const ImRect r(ImVec2(x0, y0), ImVec2(x0 + chipWidth, y0 + pt(kChipHeight)));
            if (!visible(r.Min.y, r.Max.y)) {
                continue;
            }
            ImGui::PushID(id);
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##chip", r, i == current);
            ImGui::PopID();
            ImGui::PopID();
            draw(i, r, c.on);
            if (c.press.clicked) {
                picked = i;
            }
        }
        y += pt(kChipHeight + kChipGap) * static_cast<float>(rows) + pt(kSliderGap - kChipGap);
        return picked;
    };
    auto chipName = [&](const ImRect& r, const char* name, float on) {
        ui::label(dl, Weight::Regular, th::kMicro, ImVec2(r.GetCenter().x, r.Max.y - pt(12.0f)), Align::Center,
                  ui::mix(th::kSecondaryLabel, th::kLabel, on), name, r.GetWidth() - pt(6.0f));
    };

    section("TRAZO");
    slider("espaciado");
    slider("dispersion");
    slider("sellos");
    slider("estabilizacion");

    section("AFINADO");
    slider("afinado-inicio");
    slider("afinado-final");
    slider("afinado-punta", params.taperStart > 0.0f || params.taperEnd > 0.0f);

    section("FORMA");
    const int tip = chips("##tips", kBrushTipCount, static_cast<int>(params.tip), [&](int i, const ImRect& r, float on) {
        const BrushTip value = static_cast<BrushTip>(i);
        const float side = std::min(r.GetWidth() - pt(16.0f), r.GetHeight() - pt(30.0f));
        const ImVec2 center(r.GetCenter().x, r.Min.y + pt(8.0f) + side * 0.5f);
        const GLuint image = m_previews.tipTexture(value);
        if (image != 0) {
            // Las clásicas se pintan invertidas: aquí se ven como en el lienzo.
            const bool flip = isClassic(value);
            dl->AddImage(ImTextureRef(textureId(image)), ImVec2(center.x - side * 0.5f, center.y - side * 0.5f),
                         ImVec2(center.x + side * 0.5f, center.y + side * 0.5f), ImVec2(0.0f, flip ? 1.0f : 0.0f),
                         ImVec2(1.0f, flip ? 0.0f : 1.0f), ui::mix(IM_COL32(235, 235, 245, 210), IM_COL32_WHITE, on));
        }
        chipName(r, brushes::tipName(value), on);
    });
    if (tip >= 0) {
        params.tip = static_cast<BrushTip>(tip);
    }
    slider("dureza", params.tip == BrushTip::Round);
    slider("redondez");
    slider("angulo");
    {
        const ImRect row(ImVec2(left - pt(4.0f), y), ImVec2(right + pt(4.0f), y + pt(40.0f)));
        if (visible(row.Min.y, row.Max.y)) {
            ui::parts::toggleRow(dl, "##follow", row, icon::kRotateCcw, "Gira con el trazo", &params.followStroke);
        }
        y += pt(40.0f + kSliderGap);
    }
    slider("giro");

    section("GRANO DEL PAPEL");
    const int grain = chips("##grains", kBrushGrainCount, static_cast<int>(params.grain), [&](int i, const ImRect& r, float on) {
        const BrushGrain value = static_cast<BrushGrain>(i);
        const float side = std::min(r.GetWidth() - pt(16.0f), r.GetHeight() - pt(30.0f));
        const ImRect image(ImVec2(r.GetCenter().x - side * 0.5f, r.Min.y + pt(8.0f)),
                           ImVec2(r.GetCenter().x + side * 0.5f, r.Min.y + pt(8.0f) + side));
        const GLuint texture = value == BrushGrain::None ? 0 : m_previews.grainTexture(value);
        if (texture != 0) {
            // Un trozo de la textura, más grande que en el lienzo para que se aprecie.
            dl->AddImageRounded(ImTextureRef(textureId(texture)), image.Min, image.Max, ImVec2(0.0f, 0.0f),
                                ImVec2(0.3f, 0.3f), ui::mix(IM_COL32(235, 235, 245, 210), IM_COL32_WHITE, on),
                                pt(6.0f));
        } else {
            dl->AddRectFilled(image.Min, image.Max, ui::mix(IM_COL32(235, 235, 245, 150), IM_COL32_WHITE, on), pt(6.0f));
        }
        chipName(r, brushes::grainName(value), on);
    });
    if (grain >= 0) {
        params.grain = static_cast<BrushGrain>(grain);
        if (params.grain != BrushGrain::None && params.grainDepth <= 0.0f) {
            params.grainDepth = 0.5f;   // que se note al elegirlo
        }
    }
    slider("grano-escala", params.grain != BrushGrain::None);
    slider("grano-intensidad", params.grain != BrushGrain::None);

    section("PRESIÓN Y VARIACIÓN");
    slider("presion-tamano");
    slider("presion-opacidad");
    slider("variacion-tamano");
    slider("variacion-opacidad");

    section("PINTURA");
    {
        const float gap = pt(kChipGap);
        const float half = (right - left - gap) * 0.5f;
        const float height = pt(52.0f);
        const struct {
            BrushBuildUp mode;
            const char* title;
            const char* detail;
        } modes[2] = {{BrushBuildUp::Glaze, "Acumula", "Repasar oscurece"},
                      {BrushBuildUp::Uniform, "Uniforme", "Como la tinta"}};
        for (int i = 0; i < 2; ++i) {
            const float x0 = left + (half + gap) * static_cast<float>(i);
            const ImRect r(ImVec2(x0, y), ImVec2(x0 + half, y + height));
            if (!visible(r.Min.y, r.Max.y)) {
                continue;
            }
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##buildup", r, params.buildUp == modes[i].mode);
            ImGui::PopID();
            const float cyTitle = r.Min.y + pt(18.0f);
            ui::label(dl, Weight::SemiBold, th::kFootnote, ImVec2(r.Min.x + pt(12.0f), cyTitle), Align::Left,
                      th::kLabel, modes[i].title, r.GetWidth() - pt(20.0f));
            ui::label(dl, Weight::Regular, th::kCaption, ImVec2(r.Min.x + pt(12.0f), cyTitle + pt(17.0f)),
                      Align::Left, ui::mix(th::kSecondaryLabel, th::kLabel, c.on * 0.6f), modes[i].detail,
                      r.GetWidth() - pt(20.0f));
            if (c.press.clicked) {
                params.buildUp = modes[i].mode;
            }
        }
        y += height + pt(kSliderGap + 4.0f);
    }
    slider("flujo");

    section("TAMAÑO");
    // El mínimo no pasa del máximo: al mover uno, empuja al otro.
    slider("tamano-maximo");
    params.minRadius = std::min(params.minRadius, params.maxRadius);
    slider("tamano-minimo");
    params.maxRadius = std::max(params.maxRadius, params.minRadius);
    y += pt(8.0f);

    storage->SetFloat(heightId, y - start);
    ui::endScroll();

    if (!(params == before)) {
        scheduleBrushSave();
    }
}
