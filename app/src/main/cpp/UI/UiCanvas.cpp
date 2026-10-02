// Tarjeta de lienzo nuevo, que también es la pantalla de inicio: tamaños por categorías
// (cada ficha con la forma del lienzo a escala), tamaño a medida en px, mm, cm o pulgadas,
// resolución, fondo y nombre, con un resumen en vivo de lo que saldrá. En una tableta o un
// ordenador son dos columnas que se desplazan por separado (los tamaños a la izquierda y
// los ajustes a la derecha) con el resumen y «Crear lienzo» abajo; en un teléfono, una
// hoja a pantalla completa con todo en una columna y el resumen y «Crear lienzo» fijos
// abajo.
//
// Los números se escriben con un teclado numérico propio cuando se tocan con el dedo o el
// lápiz: el navegador de un móvil no saca su teclado para la app, y el de Android no pasa
// a numérico sin cerrarlo y volverlo a abrir. Con el ratón se usa el teclado físico.
#include "UI/Ui.h"

#include "UI/Anim.h"
#include "UI/Icons.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace th = ui::theme;
using canvasspec::PresetCategory;
using canvasspec::SizeProblem;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;
using Field = CanvasForm::Field;
using FormBackground = CanvasForm::Background;

namespace {

#ifdef SDL_PLATFORM_ANDROID
// En Android casi nunca hay teclado físico: los números van siempre con el de la tarjeta.
constexpr bool kKeypadAlways = true;
#else
constexpr bool kKeypadAlways = false;
#endif

// Tamaños que caben en «Mis tamaños».
constexpr int kMaxSavedSizes = 24;
// Lo que puede moverse el dedo en un toque (como en el kit).
constexpr float kTapSlop = 10.0f;
// Opacidad del oscurecido de detrás de los diálogos (el cristal la tiene en cuenta).
constexpr float kDimAlpha = static_cast<float>((th::kDim >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;

// Medidas (pt).
constexpr float kTwoColumns = 640.0f;     // ancho mínimo para las dos columnas
constexpr float kMaxWidth = 900.0f;
constexpr float kMaxHeight = 880.0f;
constexpr float kPad = 24.0f;             // márgenes de la tarjeta
constexpr float kSheetPad = 16.0f;        // márgenes de la hoja del teléfono
constexpr float kGutter = 32.0f;          // entre las columnas, con una línea en medio
constexpr float kSettingsShare = 0.42f;   // parte del ancho para los ajustes
constexpr float kMinSettings = 300.0f;
constexpr float kBodyPad = 18.0f;         // arriba y abajo de lo que se desplaza
constexpr float kTabs = 54.0f;
constexpr float kTabsGap = 14.0f;
constexpr float kTileGap = 8.0f;
constexpr float kMinTile = 128.0f;
constexpr float kEmptySaved = 132.0f;     // «Mis tamaños» sin tamaños
constexpr float kSection = 24.0f;         // rótulo de sección y su hueco
constexpr float kSectionGap = 22.0f;
constexpr float kRowGap = 10.0f;
constexpr float kNameField = 40.0f;
constexpr float kSegment = 36.0f;
constexpr float kNumber = 54.0f;
constexpr float kLock = 40.0f;
constexpr float kChip = 38.0f;
constexpr float kHintGap = 8.0f;
constexpr float kBackgroundChip = 58.0f;
constexpr float kSwatch = 30.0f;
constexpr float kSwatchGap = 8.0f;
constexpr float kHex = 32.0f;
constexpr float kColorOptions = 14.0f + kSwatch * 2.0f + kSwatchGap + 14.0f + kHex;
constexpr float kButton = 46.0f;
constexpr float kSummary = 56.0f;         // alto del resumen (y de su muestra en la tarjeta)
constexpr float kKeyBar = 54.0f;          // barra del teclado: qué se escribe y «Listo»
constexpr float kKey = 54.0f;
constexpr float kKeyGap = 8.0f;
constexpr float kKeypadWidth = 380.0f;

// Iconos de las categorías, en el orden de PresetCategory.
constexpr const char* kCategoryIcons[canvasspec::kPresetCategoryCount] = {
    icon::kVideo, icon::kDevices, icon::kHeart, icon::kFileText, icon::kBookOpen, icon::kStar,
};

// Resoluciones de las fichas; la quinta, «Otra», se escribe.
constexpr float kPpiChoices[4] = {72.0f, 150.0f, 300.0f, 600.0f};

// Colores de fondo para elegir: papel crema (el de por defecto), grises, negro, los verde
// y azul de croma (para recortar el fondo en la mesa de vídeo) y tonos pastel.
constexpr ImU32 kBackgroundSwatches[12] = {
    IM_COL32(243, 237, 226, 255), IM_COL32(209, 209, 214, 255), IM_COL32(142, 142, 147, 255),
    IM_COL32(58, 58, 60, 255),    IM_COL32(28, 28, 30, 255),    IM_COL32(0, 0, 0, 255),
    IM_COL32(0, 177, 64, 255),    IM_COL32(0, 71, 187, 255),    IM_COL32(207, 232, 255, 255),
    IM_COL32(250, 212, 216, 255), IM_COL32(213, 242, 227, 255), IM_COL32(255, 244, 194, 255),
};

std::string prefFile(const char* name) {
    char* folder = SDL_GetPrefPath("TaCodec", "LiveSketch");
    if (!folder) {
        return {};
    }
    std::string path = std::string(folder) + name;
    SDL_free(folder);
    return path;
}

// La unidad en palabras, para la explicación de los ppp.
const char* unitWords(LengthUnit unit) {
    switch (unit) {
    case LengthUnit::Millimeters:
        return "milímetros";
    case LengthUnit::Centimeters:
        return "centímetros";
    case LengthUnit::Inches:
        return "pulgadas";
    default:
        return "píxeles";
    }
}

std::string ppiHint(LengthUnit unit) {
    if (unit == LengthUnit::Pixels) {
        return "En píxeles, los ppp solo cambian el tamaño al imprimir.";
    }
    return std::string("En ") + unitWords(unit) + " se mantiene el tamaño en papel: con más ppp, más píxeles.";
}

// Si dos tamaños en píxeles son el mismo (con uno de margen): 2 tal cual, 1 girado y 0 no.
int sizeMatch(int width, int height, int otherWidth, int otherHeight) {
    auto near = [](int a, int b) { return std::abs(a - b) <= 1; };
    if (near(width, otherWidth) && near(height, otherHeight)) {
        return 2;
    }
    return near(width, otherHeight) && near(height, otherWidth) ? 1 : 0;
}

// Dos tamaños de «Mis tamaños» iguales a la vista (lo que muestra su ficha).
bool sameSaved(const canvasspec::SavedSize& a, const canvasspec::SavedSize& b) {
    return a.unit == b.unit && canvasspec::savedName(a) == canvasspec::savedName(b) &&
           canvasspec::formatPpi(a.ppi) == canvasspec::formatPpi(b.ppi);
}

// Rectángulo de `size` centrado en `center`.
ImRect frameRect(ImVec2 center, ImVec2 size) {
    return ImRect(ImVec2(ui::snap(center.x - size.x * 0.5f), ui::snap(center.y - size.y * 0.5f)),
                  ImVec2(ui::snap(center.x + size.x * 0.5f), ui::snap(center.y + size.y * 0.5f)));
}

// La forma de un lienzo de `w` × `h` píxeles en una caja de `box`: con su proporción y más
// grande cuantos más píxeles tiene (el 4K llena la caja).
ImVec2 shapeSize(float w, float h, ImVec2 box) {
    constexpr float kLargestArea = 3840.0f * 2160.0f;
    w = std::max(w, 1.0f);
    h = std::max(h, 1.0f);
    const float grow = 0.55f + 0.45f * std::sqrt(std::min(w * h / kLargestArea, 1.0f));
    const float fit = std::min(box.x / w, box.y / h) * grow;
    return ImVec2(std::max(w * fit, pt(8.0f)), std::max(h * fit, pt(8.0f)));
}

// Muestra de un fondo: blanco, un color o el damero de lo transparente.
void backgroundFill(ImDrawList* dl, const ImRect& rect, float radius, FormBackground kind, const float color[3]) {
    switch (kind) {
    case FormBackground::White:
        dl->AddRectFilled(rect.Min, rect.Max, IM_COL32_WHITE, radius);
        break;
    case FormBackground::Color:
        dl->AddRectFilled(rect.Min, rect.Max, ui::fromFloat(color), radius);
        break;
    case FormBackground::Transparent:
        ui::checkerboard(dl, rect, radius, std::max(pt(4.0f), ui::hairline()));
        break;
    }
}

// Rótulo de sección: `y` es lo alto de su franja (kSection).
void section(ImDrawList* dl, float left, float right, float y, const char* text) {
    ui::sectionLabel(dl, left, right, y + pt(7.0f), text);
}

// Campo de texto quieto (mientras la tarjeta desaparece), con el aspecto de ui::textField.
void staticField(ImDrawList* dl, const ImRect& rect, const char* text, const char* placeholder) {
    const float radius = pt(8.0f);
    dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(0, 0, 0, 64), radius);
    ui::outline(dl, rect, radius, IM_COL32(255, 255, 255, 31), pt(1.0f));
    const bool empty = text[0] == '\0';
    ui::label(dl, Weight::Regular, th::kBody, ImVec2(rect.Min.x + pt(10.0f), rect.GetCenter().y), Align::Left,
              empty ? th::kTertiaryLabel : th::kLabel, empty ? placeholder : text, rect.GetWidth() - pt(20.0f));
}

// Texto de un número que se escribe, con su cursor o, antes de la primera tecla, resaltado
// (la tecla lo sustituye). `anchor` como en ui::label (una línea centrada en `anchor.y`).
void editText(ImDrawList* dl, float points, ImVec2 anchor, Align align, const std::string& text, bool fresh) {
    const float width = ui::measure(Weight::SemiBold, points, text.c_str()).x;
    float x = anchor.x;
    if (align == Align::Center) {
        x -= width * 0.5f;
    } else if (align == Align::Right) {
        x -= width;
    }
    const float half = ui::fontSize(points) * 0.5f;
    if (fresh && !text.empty()) {
        dl->AddRectFilled(ImVec2(x - pt(3.0f), anchor.y - half), ImVec2(x + width + pt(3.0f), anchor.y + half),
                          ui::withAlpha(th::kAccent, 0.45f), pt(4.0f));
    }
    ui::label(dl, Weight::SemiBold, points, ImVec2(x, anchor.y), Align::Left, th::kLabel, text.c_str());
    if (!fresh) {
        const float caretX = ui::snap(x + width + pt(1.5f));
        dl->AddRectFilled(ImVec2(caretX, anchor.y - half * 0.85f), ImVec2(caretX + pt(2.0f), anchor.y + half * 0.85f),
                          th::kAccent, pt(1.0f));
    }
}

// Tecla del teclado numérico: responde al bajar el dedo, para escribir deprisa.
Press keyPress(ImGuiID id, const ImRect& rect, bool enabled) {
    Press press;
    if (!ImGui::ItemAdd(rect, id, nullptr, enabled ? ImGuiItemFlags_None : ImGuiItemFlags_Disabled)) {
        return press;
    }
    bool hovered = false;
    bool held = false;
    const bool clicked = ImGui::ButtonBehavior(rect, id, &hovered, &held,
                                               ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus);
    if (!enabled) {
        return press;
    }
    press.clicked = clicked;
    press.held = held;
    press.hovered = hovered && ImGui::GetIO().MouseSource != ImGuiMouseSource_TouchScreen;
    return press;
}

// La marca de la app: un cuadrado rojo y "LIVESKETCH". `cy`: su centro vertical.
void brand(ImDrawList* dl, float left, float cy) {
    dl->AddRectFilled(ImVec2(left, cy - pt(4.0f)), ImVec2(left + pt(8.0f), cy + pt(4.0f)), th::kRed, pt(2.0f));
    ui::tracked(dl, Weight::Bold, th::kMicro, ImVec2(left + pt(16.0f), cy), IM_COL32(235, 235, 245, 140), "LIVESKETCH",
                0.12f);
}

bool closeButton(ImDrawList* dl, const ImRect& rect) {
    const Press press = ui::buttonFrame("##close", rect, ui::ButtonStyle::Secondary, true, pt(9.0f));
    ui::icon(dl, icon::kX, rect.GetCenter(), 16.0f, IM_COL32(235, 235, 245, 204));
    return press.clicked;
}

bool createButton(ImDrawList* dl, const ImRect& rect, bool enabled) {
    const float radius = pt(12.0f);
    if (enabled) {
        ui::shadow(dl, rect, radius, pt(24.0f), pt(8.0f), 0.28f, 0, th::kAccent);
    }
    const Press press = ui::buttonFrame("##create", rect, ui::ButtonStyle::Primary, enabled, radius);
    const ImU32 color = ui::withAlpha(th::kLabel, enabled ? 1.0f : 0.6f);
    const float cy = rect.GetCenter().y;
    ui::icon(dl, icon::kArrowRight, ImVec2(rect.Max.x - pt(16.0f + 9.0f), cy), 18.0f, color);
    ui::label(dl, Weight::SemiBold, th::kBody, ImVec2(rect.Min.x + pt(18.0f), cy), Align::Left, color, "Crear lienzo",
              rect.GetWidth() - pt(18.0f + 18.0f + 16.0f + 8.0f));
    return press.clicked && enabled;
}

bool saveSizeButton(ImDrawList* dl, const ImRect& rect, bool withText, bool enabled) {
    const Press press = ui::buttonFrame("##save-size", rect, ui::ButtonStyle::Secondary, enabled, pt(12.0f));
    const ImU32 color = ui::withAlpha(th::kLabel, enabled ? 1.0f : 0.5f);
    const float cy = rect.GetCenter().y;
    if (withText) {
        ui::icon(dl, icon::kBookmarkPlus, ImVec2(rect.Min.x + pt(16.0f + 9.0f), cy), 18.0f, color);
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(rect.Min.x + pt(16.0f + 18.0f + 8.0f), cy), Align::Left,
                  color, "Guardar tamaño", rect.GetWidth() - pt(16.0f + 18.0f + 8.0f + 12.0f));
    } else {
        ui::icon(dl, icon::kBookmarkPlus, rect.GetCenter(), 18.0f, color);
    }
    return press.clicked && enabled;
}

// Alto que pide el teclado numérico: la barra y cuatro filas de teclas.
float keypadHeight() { return pt(kKeyBar + 6.0f + kKey * 4.0f + kKeyGap * 3.0f); }

float saveSizeWidth() {
    return pt(16.0f + 18.0f + 8.0f) + ui::measure(Weight::SemiBold, th::kSubhead, "Guardar tamaño").x + pt(18.0f);
}

} // namespace

// -----------------------------------------------------------------------------
// Mis tamaños
// -----------------------------------------------------------------------------

void Ui::loadSavedSizes() {
    m_savedSizesLoaded = true;
    m_savedSizes.clear();
    const std::string path = prefFile("tamanos.txt");
    if (path.empty()) {
        return;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line) && static_cast<int>(m_savedSizes.size()) < kMaxSavedSizes) {
        canvasspec::SavedSize size;
        if (canvasspec::fromLine(line, &size)) {
            m_savedSizes.push_back(size);
        }
    }
}

void Ui::saveSavedSizes() const {
    const std::string path = prefFile("tamanos.txt");
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::trunc);
    for (const canvasspec::SavedSize& size : m_savedSizes) {
        out << canvasspec::toLine(size) << '\n';
    }
}

void Ui::saveCanvasSize() {
    m_canvasForm.commit();
    const canvasspec::SavedSize size = m_canvasForm.size();
    m_canvasForm.category = static_cast<int>(PresetCategory::Saved);
    for (const canvasspec::SavedSize& saved : m_savedSizes) {
        if (sameSaved(saved, size)) {
            notify("Ya está en Mis tamaños", Notice::Info, 2000);
            return;
        }
    }
    if (static_cast<int>(m_savedSizes.size()) >= kMaxSavedSizes) {
        notify("Mis tamaños está lleno: quita alguno", Notice::Warning, 2500);
        return;
    }
    // El más reciente, primero.
    m_savedSizes.insert(m_savedSizes.begin(), size);
    saveSavedSizes();
    notify("Guardado en Mis tamaños", Notice::Success, 2000);
}

// -----------------------------------------------------------------------------
// Escribir números
// -----------------------------------------------------------------------------

void Ui::beginCanvasEdit(Field field, bool keypad) {
    m_hexEditing = false;
    m_canvasForm.begin(field);
    m_keypad = keypad;
    m_fieldScroll = true;
}

bool Ui::cancelCanvasEdit() {
    if (m_canvasForm.field() != Field::None) {
        // Escape vuelve a como estaba; atrás (Android) cierra el teclado con lo escrito,
        // como el del sistema.
        if (ImGui::IsKeyPressed(ImGuiKey_AppBack, false)) {
            m_canvasForm.commit();
        } else {
            m_canvasForm.cancel();
        }
        return true;
    }
    if (m_hexEditing || ImGui::GetIO().WantTextInput) {
        // El nombre o el hexadecimal: se deja de escribir con lo escrito.
        m_hexEditing = false;
        ImGui::ClearActiveID();
        return true;
    }
    return false;
}

void Ui::canvasKeys() {
    CanvasForm& form = m_canvasForm;
    for (int i = 0; i < 10; ++i) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + i)) ||
            ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + i))) {
            form.type(static_cast<char>('0' + i));
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Comma) || ImGui::IsKeyPressed(ImGuiKey_Period) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal)) {
        form.type(',');
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        form.erase();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        form.commit();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        // Tabulador: ancho, alto y resolución, en vuelta (con Mayús, hacia atrás).
        constexpr Field kOrder[3] = {Field::Width, Field::Height, Field::Ppi};
        int at = 0;
        while (at < 2 && kOrder[at] != form.field()) {
            ++at;
        }
        const int next = (at + (ImGui::GetIO().KeyShift ? 2 : 1)) % 3;
        beginCanvasEdit(kOrder[next], m_keypad);
    }
}

// -----------------------------------------------------------------------------
// Tamaños
// -----------------------------------------------------------------------------

void Ui::presetPixels(const canvasspec::Preset& preset, float ppi, int* width, int* height) const {
    if (preset.width <= 0.0) {
        // Esta pantalla, sin pasar del lado que admite la GPU.
        int w = std::max(1, m_status.screenWidth);
        int h = std::max(1, m_status.screenHeight);
        if (m_status.maxCanvasSize > 0) {
            w = std::min(w, m_status.maxCanvasSize);
            h = std::min(h, m_status.maxCanvasSize);
        }
        *width = w;
        *height = h;
        return;
    }
    // Los de píxeles tienen sus píxeles; los de papel, los que salen a `ppi`.
    const double at = preset.unit == LengthUnit::Pixels ? static_cast<double>(preset.ppi) : static_cast<double>(ppi);
    *width = canvasspec::toPixels(preset.width, preset.unit, at);
    *height = canvasspec::toPixels(preset.height, preset.unit, at);
}

namespace {

int tileColumns(float width) {
    const float gap = pt(kTileGap);
    return std::clamp(static_cast<int>((width + gap) / (pt(kMinTile) + gap)), 2, 4);
}

float tileHeight(bool compact) { return pt(compact ? 92.0f : 104.0f); }

} // namespace

float Ui::canvasPresetsHeight(float width, int category) const {
    const auto kind = static_cast<PresetCategory>(std::clamp(category, 0, canvasspec::kPresetCategoryCount - 1));
    const int count = kind == PresetCategory::Saved ? static_cast<int>(m_savedSizes.size())
                                                    : static_cast<int>(canvasspec::presets(kind).size());
    const float tabs = pt(kTabs + kTabsGap);
    if (count == 0) {
        return tabs + pt(kEmptySaved);
    }
    const int columns = tileColumns(width);
    const int rows = (count + columns - 1) / columns;
    return tabs + tileHeight(m_layout.compact) * static_cast<float>(rows) + pt(kTileGap) * static_cast<float>(rows - 1);
}

void Ui::canvasPresets(ImDrawList* dl, float left, float right, float y) {
    CanvasForm& form = m_canvasForm;
    const float width = right - left;
    form.category = std::clamp(form.category, 0, canvasspec::kPresetCategoryCount - 1);
    const int category = form.category;

    // Pestañas: icono y nombre corto; la elegida, en el color de acento.
    {
        constexpr int kCount = canvasspec::kPresetCategoryCount;
        const float gap = pt(4.0f);
        const float tabWidth = (width - gap * (kCount - 1)) / kCount;
        for (int i = 0; i < kCount; ++i) {
            const float x0 = left + (tabWidth + gap) * static_cast<float>(i);
            const ImRect r(ImVec2(x0, y), ImVec2(x0 + tabWidth, y + pt(kTabs)));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##category", r, category == i, true, false);
            ImGui::PopID();
            const ImU32 color = ui::mix(IM_COL32(235, 235, 245, 179), th::kAccent, c.on);
            ui::icon(dl, kCategoryIcons[i], ImVec2(r.GetCenter().x, r.Min.y + pt(19.0f)), 20.0f, color);
            ui::label(dl, Weight::SemiBold, th::kMicro, ImVec2(r.GetCenter().x, r.Max.y - pt(14.0f)), Align::Center,
                      color, canvasspec::categoryShortName(static_cast<PresetCategory>(i)), tabWidth - pt(4.0f));
            if (c.press.clicked) {
                form.category = i;
            }
        }
        y += pt(kTabs);
        ui::separator(dl, left, right, y + pt(kTabsGap * 0.5f), th::kRule);
        y += pt(kTabsGap);
    }

    // Fichas: la forma del lienzo a escala (con su proporción y más grande cuantos más
    // píxeles tiene), el nombre y las medidas.
    const bool compact = m_layout.compact;
    const int columns = tileColumns(width);
    const float gap = pt(kTileGap);
    const float tileWidth = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float height = tileHeight(compact);
    const int formWidth = form.pixelWidth();
    const int formHeight = form.pixelHeight();
    const int maxSide = m_status.maxCanvasSize;
    auto tileRect = [&](int index) {
        const float x0 = left + (tileWidth + gap) * static_cast<float>(index % columns);
        const float y0 = y + (height + gap) * static_cast<float>(index / columns);
        return ImRect(ImVec2(x0, y0), ImVec2(x0 + tileWidth, y0 + height));
    };
    auto drawTile = [&](const ImRect& r, const ui::Choice& c, bool supported, int w, int h, const char* name,
                        const char* detail) {
        // La elegida enseña la orientación que tiene el lienzo.
        if (c.on > 0.5f && formWidth != formHeight && (formWidth > formHeight) != (w > h)) {
            std::swap(w, h);
        }
        const float alpha = supported ? 1.0f : 0.4f;
        const float boxHeight = pt(compact ? 38.0f : 44.0f);
        const ImVec2 box = compact ? pt(42.0f, 34.0f) : pt(46.0f, 40.0f);
        const float stack = boxHeight + pt(8.0f + 16.0f + 2.0f + 15.0f);   // forma, nombre y medidas
        const float cx = r.GetCenter().x;
        const float top = r.Min.y + (r.GetHeight() - stack) * 0.5f;
        const ImRect shape =
            frameRect(ImVec2(cx, top + boxHeight * 0.5f), shapeSize(static_cast<float>(w), static_cast<float>(h), box));
        if (c.on > 0.002f) {
            dl->AddRectFilled(shape.Min, shape.Max, ui::withAlpha(th::kAccent, 0.22f * c.on), pt(4.0f));
        }
        ui::outline(dl, shape, pt(4.0f), ui::withAlpha(ui::mix(IM_COL32(235, 235, 245, 140), th::kAccent, c.on), alpha),
                    pt(2.0f));
        const float nameCy = top + boxHeight + pt(16.0f);
        ui::label(dl, Weight::SemiBold, 13.0f, ImVec2(cx, nameCy), Align::Center, ui::withAlpha(th::kLabel, alpha), name,
                  r.GetWidth() - pt(12.0f));
        ui::label(dl, Weight::Regular, th::kCaption, ImVec2(cx, nameCy + pt(17.5f)), Align::Center,
                  supported ? ui::mix(IM_COL32(235, 235, 245, 128), th::kAccentText, c.on) : th::kTertiaryLabel, detail,
                  r.GetWidth() - pt(12.0f));
    };

    if (category != static_cast<int>(PresetCategory::Saved)) {
        const auto list = canvasspec::presets(static_cast<PresetCategory>(category));
        // La ficha elegida es la del tamaño del lienzo: la que lo tiene en su orientación o,
        // si no hay, la que lo tiene girado (A4 en horizontal sigue siendo A4). Un papel
        // se compara a los ppp elegidos: sigue siendo el mismo papel a otra resolución.
        std::vector<int> matches(list.size(), 0);
        int best = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            int w = 0;
            int h = 0;
            presetPixels(list[i], form.ppi(), &w, &h);
            matches[i] = sizeMatch(formWidth, formHeight, w, h);
            best = std::max(best, matches[i]);
        }
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const canvasspec::Preset& preset = list[static_cast<size_t>(i)];
            // Lo que crea, a sus ppp.
            int width0 = 0;
            int height0 = 0;
            presetPixels(preset, preset.ppi, &width0, &height0);
            const bool supported = canvasspec::sizeProblem(width0, height0, maxSide) == SizeProblem::None;
            const ImRect r = tileRect(i);
            ImGui::PushID(i);
            const ui::Choice c =
                ui::choice("##preset", r, best > 0 && matches[static_cast<size_t>(i)] == best, supported);
            ImGui::PopID();
            std::string detail;
            if (!supported) {
                detail = "No admitido";
            } else if (preset.width <= 0.0) {
                detail = canvasspec::formatSize(width0, height0, LengthUnit::Pixels);
            } else {
                detail = canvasspec::formatSize(preset.width, preset.height, preset.unit);
            }
            drawTile(r, c, supported, width0, height0, preset.name, detail.c_str());
            if (c.press.clicked) {
                if (preset.width <= 0.0) {
                    form.choose(width0, height0, LengthUnit::Pixels, preset.ppi);
                } else {
                    form.choose(preset.width, preset.height, preset.unit, preset.ppi);
                }
                m_canvasPreset[0] = category;
                m_canvasPreset[1] = i;
            }
        }
        return;
    }

    // Mis tamaños: cada ficha con su botón de quitar.
    if (m_savedSizes.empty()) {
        const float cx = (left + right) * 0.5f;
        ui::icon(dl, icon::kBookmarkPlus, ImVec2(cx, y + pt(34.0f)), 26.0f, th::kTertiaryLabel);
        ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(16.0f), y + pt(62.0f)), width - pt(32.0f),
                      Align::Center, th::kSecondaryLabel,
                      "Guarda aquí los tamaños que más uses: prepara uno y toca «Guardar tamaño».", 1.45f);
        return;
    }
    const canvasspec::SavedSize current = form.size();
    int remove = -1;
    for (int i = 0; i < static_cast<int>(m_savedSizes.size()); ++i) {
        const canvasspec::SavedSize& size = m_savedSizes[static_cast<size_t>(i)];
        const int w = canvasspec::toPixels(size.width, size.unit, size.ppi);
        const int h = canvasspec::toPixels(size.height, size.unit, size.ppi);
        const bool supported = canvasspec::sizeProblem(w, h, maxSide) == SizeProblem::None;
        const ImRect r = tileRect(i);
        const ImRect removeRect(ImVec2(r.Max.x - pt(32.0f), r.Min.y), ImVec2(r.Max.x, r.Min.y + pt(32.0f)));
        ImGui::PushID(i);
        // Quitar va antes que la ficha: se queda con los toques de su esquina.
        const Press removePress = ui::pressable("##remove", removeRect);
        const ui::Choice c = ui::choice("##saved", r, sameSaved(size, current), supported);
        ImGui::PopID();
        const std::string name = canvasspec::savedName(size);
        const std::string detail = supported ? canvasspec::formatPpi(size.ppi) : std::string("No admitido");
        drawTile(r, c, supported, w, h, name.c_str(), detail.c_str());
        const ImVec2 cc(removeRect.Max.x - pt(14.0f), removeRect.Min.y + pt(14.0f));
        const float level = ui::anim::follow(ImGui::GetID("##remove-level") + static_cast<ImGuiID>(i),
                                             removePress.held ? 1.0f : (removePress.hovered ? 0.5f : 0.0f), 24.0f);
        dl->AddCircleFilled(cc, pt(9.0f), ui::mix(IM_COL32(255, 255, 255, 26), IM_COL32(255, 255, 255, 64), level));
        ui::icon(dl, icon::kX, cc, 11.0f, IM_COL32(235, 235, 245, 191));
        if (removePress.clicked) {
            remove = i;
        } else if (c.press.clicked) {
            form.choose(size.width, size.height, size.unit, size.ppi);
            m_canvasPreset[0] = -1;
        }
    }
    if (remove >= 0) {
        m_savedSizes.erase(m_savedSizes.begin() + remove);
        saveSavedSizes();
        notify("Quitado de Mis tamaños", Notice::Info, 1800);
    }
}

// -----------------------------------------------------------------------------
// Ajustes
// -----------------------------------------------------------------------------

float Ui::canvasSettingsHeight(float width, bool colorOptions) const {
    // La explicación de los ppp cambia con la unidad: se cuenta la más larga para que la
    // columna no salte.
    float hint = 0.0f;
    for (int i = 0; i < kLengthUnitCount; ++i) {
        hint = std::max(hint, ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f), width,
                                            Align::Left, 0, ppiHint(static_cast<LengthUnit>(i)).c_str(), 1.4f));
    }
    float points = kSection + kNameField + kSectionGap;                                      // nombre
    points += kSection + kSegment + kRowGap + kNumber + kRowGap + kChip + kSectionGap;   // tamaño
    points += kSection + kChip + kHintGap;                                                // resolución
    points += kSectionGap + kSection + kBackgroundChip;                                   // fondo
    if (colorOptions) {
        points += kColorOptions;
    }
    return pt(points) + hint;
}

void Ui::numberField(ImDrawList* dl, const char* id, const ImRect& rect, Field field, const char* caption,
                     const std::string& value, const char* unit) {
    CanvasForm& form = m_canvasForm;
    const bool editing = form.field() == field;
    const ImGuiID gid = ImGui::GetID(id);
    const Press press = ui::pressable(gid, rect);
    const float radius = pt(8.0f);
    const float edit = ui::anim::follow(gid + 3u, editing ? 1.0f : 0.0f, 20.0f);
    const float touch = ui::anim::follow(gid + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
    dl->AddRectFilled(rect.Min, rect.Max, ui::mix(IM_COL32(0, 0, 0, 64), IM_COL32(255, 255, 255, 20), touch), radius);
    ui::outline(dl, rect, radius, ui::mix(IM_COL32(255, 255, 255, 31), th::kAccent, edit), pt(1.0f + edit * 0.5f));
    const float x = rect.Min.x + pt(12.0f);
    ui::tracked(dl, Weight::Bold, th::kMicro, ImVec2(x, rect.Min.y + pt(15.0f)),
                ui::mix(th::kSectionLabel, th::kAccentText, edit), caption, 0.08f);
    const float cy = rect.Max.y - pt(17.0f);
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(rect.Max.x - pt(12.0f), cy), Align::Right, th::kSecondaryLabel,
              unit);
    if (editing) {
        editText(dl, th::kBody, ImVec2(x, cy), Align::Left, form.text(), form.fresh());
        m_fieldRect = rect;
    } else {
        const float unitWidth = ui::measure(Weight::Regular, th::kFootnote, unit).x;
        ui::label(dl, Weight::SemiBold, th::kBody, ImVec2(x, cy), Align::Left, th::kLabel, value.c_str(),
                  rect.Max.x - pt(12.0f + 6.0f) - unitWidth - x);
    }
    if (press.clicked && !editing) {
        beginCanvasEdit(field, kKeypadAlways || m_status.touchInput);
    }
}

void Ui::canvasSettings(ImDrawList* dl, float left, float right, float y, bool interactive) {
    CanvasForm& form = m_canvasForm;
    const float width = right - left;

    // Nombre del proyecto.
    section(dl, left, right, y, "NOMBRE");
    y += pt(kSection);
    {
        const ImRect field(ImVec2(left, y), ImVec2(right, y + pt(kNameField)));
        if (interactive) {
            ui::textField("##canvas-name", field, form.name, sizeof(form.name), false, "Sin título");
        } else {
            staticField(dl, field, form.name, "Sin título");
        }
    }
    y += pt(kNameField + kSectionGap);

    // Tamaño: la unidad, el ancho y el alto con el candado de la proporción, y la
    // orientación.
    section(dl, left, right, y, "TAMAÑO");
    y += pt(kSection);
    {
        const ImRect bar(ImVec2(left, y), ImVec2(right, y + pt(kSegment)));
        const float inset = pt(3.0f);
        const float radius = pt(th::kControlRadius) + inset;
        dl->AddRectFilled(bar.Min, bar.Max, IM_COL32(0, 0, 0, 64), radius);
        ui::outline(dl, bar, radius, th::kControlBorder, pt(1.0f));
        const float segmentWidth = (width - inset * 2.0f) / static_cast<float>(kLengthUnitCount);
        for (int i = 0; i < kLengthUnitCount; ++i) {
            const auto unit = static_cast<LengthUnit>(i);
            const float x0 = left + inset + segmentWidth * static_cast<float>(i);
            const ImRect r(ImVec2(x0, bar.Min.y + inset), ImVec2(x0 + segmentWidth, bar.Max.y - inset));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##unit", r, form.unit() == unit, true, false);
            ImGui::PopID();
            ui::label(dl, Weight::SemiBold, th::kFootnote, r.GetCenter(), Align::Center,
                      ui::mix(IM_COL32(235, 235, 245, 166), th::kAccentText, c.on), canvasspec::unitSymbol(unit));
            if (c.press.clicked && form.unit() != unit) {
                form.setUnit(unit);
            }
        }
    }
    y += pt(kSegment + kRowGap);
    {
        const float gap = pt(8.0f);
        const float fieldWidth = (width - pt(kLock) - gap * 2.0f) * 0.5f;
        const ImRect widthRect(ImVec2(left, y), ImVec2(left + fieldWidth, y + pt(kNumber)));
        const ImRect lockRect(ImVec2(widthRect.Max.x + gap, y), ImVec2(widthRect.Max.x + gap + pt(kLock), y + pt(kNumber)));
        const ImRect heightRect(ImVec2(lockRect.Max.x + gap, y), ImVec2(right, y + pt(kNumber)));
        const char* unit = canvasspec::unitSymbol(form.unit());
        const int decimals = canvasspec::unitDecimals(form.unit());
        numberField(dl, "##width", widthRect, Field::Width, "ANCHO", canvasspec::formatNumber(form.width(), decimals),
                    unit);
        // El candado: la otra medida sigue la proporción.
        const ui::Choice lock = ui::choice("##lock", lockRect, form.locked());
        ui::icon(dl, form.locked() ? icon::kLink : icon::kUnlink, lockRect.GetCenter(), 18.0f,
                 ui::mix(th::kMutedIcon, th::kAccentText, lock.on));
        if (lock.press.clicked) {
            form.setLocked(!form.locked());
            notify(form.locked() ? "Proporción bloqueada" : "Proporción libre", Notice::Info, 1400);
        }
        numberField(dl, "##height", heightRect, Field::Height, "ALTO",
                    canvasspec::formatNumber(form.height(), decimals), unit);
    }
    y += pt(kNumber + kRowGap);
    {
        // Vertical u horizontal (un cuadrado no tiene a cuál girar).
        const int w = form.pixelWidth();
        const int h = form.pixelHeight();
        const int shown = w == h ? -1 : (w < h ? 0 : 1);
        const float gap = pt(8.0f);
        const float chipWidth = (width - gap) * 0.5f;
        const char* labels[2] = {"Vertical", "Horizontal"};
        for (int i = 0; i < 2; ++i) {
            const float x0 = left + (chipWidth + gap) * static_cast<float>(i);
            const ImRect chip(ImVec2(x0, y), ImVec2(x0 + chipWidth, y + pt(kChip)));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##orientation", chip, shown == i, shown >= 0);
            ImGui::PopID();
            const ImU32 color =
                ui::withAlpha(ui::mix(IM_COL32(235, 235, 245, 191), th::kLabel, c.on), shown >= 0 ? 1.0f : 0.4f);
            const ImVec2 glyph = i == 0 ? pt(12.0f, 17.0f) : pt(17.0f, 12.0f);
            const float textWidth = ui::measure(Weight::SemiBold, th::kFootnote, labels[i]).x;
            const float groupX = chip.GetCenter().x - (glyph.x + pt(9.0f) + textWidth) * 0.5f;
            const float cy = chip.GetCenter().y;
            ui::outline(dl, frameRect(ImVec2(groupX + glyph.x * 0.5f, cy), glyph), pt(3.0f),
                        ui::mix(color, th::kAccent, c.on), pt(2.0f));
            ui::label(dl, Weight::SemiBold, th::kFootnote, ImVec2(groupX + glyph.x + pt(9.0f), cy), Align::Left, color,
                      labels[i]);
            if (c.press.clicked && shown >= 0 && shown != i) {
                form.swap();
            }
        }
    }
    y += pt(kChip + kSectionGap);

    // Resolución: las habituales o la que se escriba.
    section(dl, left, right, y, "RESOLUCIÓN (PPP)");
    y += pt(kSection);
    {
        const bool editing = form.field() == Field::Ppi;
        int matched = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(form.ppi() - kPpiChoices[i]) < 0.05f) {
                matched = i;
            }
        }
        const float gap = pt(6.0f);
        const float chipWidth = (width - gap * 4.0f) / 5.0f;
        for (int i = 0; i < 5; ++i) {
            const float x0 = left + (chipWidth + gap) * static_cast<float>(i);
            const ImRect chip(ImVec2(x0, y), ImVec2(x0 + chipWidth, y + pt(kChip)));
            const bool other = i == 4;
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##ppi", chip, other ? (matched < 0 || editing) : (matched == i && !editing));
            ImGui::PopID();
            const ImU32 color = ui::mix(IM_COL32(235, 235, 245, 191), th::kLabel, c.on);
            if (!other) {
                char text[8];
                std::snprintf(text, sizeof(text), "%d", static_cast<int>(kPpiChoices[i]));
                ui::label(dl, Weight::SemiBold, th::kCallout, chip.GetCenter(), Align::Center, color, text);
                if (c.press.clicked) {
                    form.commit();
                    form.setPpi(kPpiChoices[i]);
                }
                continue;
            }
            // «Otra»: se escribe ahí mismo; si los ppp no son de los de antes, los muestra.
            if (editing) {
                ui::outline(dl, chip, pt(th::kControlRadius), th::kAccent, pt(1.5f));
                editText(dl, th::kCallout, chip.GetCenter(), Align::Center, form.text(), form.fresh());
                m_fieldRect = chip;
            } else {
                const std::string value = matched < 0 ? canvasspec::formatNumber(form.ppi(), 1) : std::string("Otra");
                ui::label(dl, Weight::SemiBold, th::kCallout, chip.GetCenter(), Align::Center, color, value.c_str(),
                          chipWidth - pt(8.0f));
            }
            if (c.press.clicked && !editing) {
                beginCanvasEdit(Field::Ppi, kKeypadAlways || m_status.touchInput);
            }
        }
    }
    y += pt(kChip + kHintGap);
    {
        const std::string hint = ppiHint(form.unit());
        ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left, y), width, Align::Left, th::kSecondaryLabel,
                      hint.c_str(), 1.4f);
        float tallest = 0.0f;
        for (int i = 0; i < kLengthUnitCount; ++i) {
            tallest = std::max(tallest, ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f), width,
                                                      Align::Left, 0, ppiHint(static_cast<LengthUnit>(i)).c_str(), 1.4f));
        }
        y += tallest;
    }
    y += pt(kSectionGap);

    // Fondo: blanco, un color o transparente (en Capas se puede cambiar después).
    section(dl, left, right, y, "FONDO");
    y += pt(kSection);
    {
        const char* labels[3] = {"Blanco", "Color", "Transparente"};
        const float gap = pt(8.0f);
        const float chipWidth = (width - gap * 2.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const auto kind = static_cast<FormBackground>(i);
            const float x0 = left + (chipWidth + gap) * static_cast<float>(i);
            const ImRect chip(ImVec2(x0, y), ImVec2(x0 + chipWidth, y + pt(kBackgroundChip)));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##background", chip, form.background == kind);
            ImGui::PopID();
            const ImVec2 center(chip.GetCenter().x, chip.Min.y + pt(21.0f));
            const ImRect sample(ImVec2(center.x - pt(11.0f), center.y - pt(11.0f)),
                                ImVec2(center.x + pt(11.0f), center.y + pt(11.0f)));
            backgroundFill(dl, sample, pt(5.0f), kind, form.color);
            ui::outline(dl, sample, pt(5.0f), IM_COL32(255, 255, 255, 46), ui::hairline());
            ui::label(dl, Weight::SemiBold, th::kMicro, ImVec2(chip.GetCenter().x, chip.Max.y - pt(13.0f)),
                      Align::Center, ui::mix(IM_COL32(235, 235, 245, 166), th::kAccentText, c.on), labels[i],
                      chipWidth - pt(6.0f));
            if (c.press.clicked) {
                form.background = kind;
                if (kind != FormBackground::Color) {
                    m_hexEditing = false;
                }
            }
        }
    }
    y += pt(kBackgroundChip);
    if (form.background != FormBackground::Color) {
        return;
    }

    // El color: muestras y el hexadecimal.
    y += pt(14.0f);
    {
        const float gap = pt(kSwatchGap);
        const float cell = (width - gap * 5.0f) / 6.0f;
        for (int i = 0; i < 12; ++i) {
            const float x0 = left + (cell + gap) * static_cast<float>(i % 6);
            const float y0 = y + (pt(kSwatch) + gap) * static_cast<float>(i / 6);
            float rgb[3];
            ui::toFloat(kBackgroundSwatches[i], rgb);
            ImGui::PushID(i);
            if (ui::swatch("##swatch", ImRect(ImVec2(x0, y0), ImVec2(x0 + cell, y0 + pt(kSwatch))), kBackgroundSwatches[i],
                           ui::sameColor(rgb, form.color))) {
                std::copy(rgb, rgb + 3, form.color);
                m_hexEditing = false;
            }
            ImGui::PopID();
        }
    }
    y += pt(kSwatch * 2.0f + kSwatchGap + 14.0f);
    {
        char hex[16];
        ui::formatHex(form.color, hex, sizeof(hex));
        const float fieldWidth = pt(112.0f);
        const ImRect field(ImVec2(right - fieldWidth, y), ImVec2(right, y + pt(kHex)));
        ui::sectionLabel(dl, left, field.Min.x - pt(10.0f), field.GetCenter().y, "HEX");
        if (m_hexEditing && interactive) {
            const bool enter = ui::textField("##canvas-hex", field, m_hexBuffer, sizeof(m_hexBuffer),
                                             m_hexFocusFrames == 2, "#RRGGBB");
            const bool active = ImGui::IsItemActive();
            if (m_hexFocusFrames > 0) {
                --m_hexFocusFrames;
            }
            if (enter || (!active && m_hexFocusFrames == 0)) {
                float rgb[3];
                if (ui::parseHex(m_hexBuffer, rgb)) {
                    std::copy(rgb, rgb + 3, form.color);
                } else if (enter) {
                    notify("Escribe el color como #RRGGBB", Notice::Warning, 2500);
                }
                m_hexEditing = false;
            }
        } else {
            const ImGuiID id = ImGui::GetID("##canvas-hex-value");
            const Press press = ui::pressable(id, field);
            const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
            const float radius = pt(8.0f);
            dl->AddRectFilled(field.Min, field.Max, ui::mix(IM_COL32(0, 0, 0, 64), IM_COL32(255, 255, 255, 26), t),
                              radius);
            ui::outline(dl, field, radius, IM_COL32(255, 255, 255, 31), pt(1.0f));
            ui::label(dl, Weight::SemiBold, th::kCallout, field.GetCenter(), Align::Center, th::kLabel, hex);
            if (press.clicked) {
                m_canvasForm.commit();
                m_hexEditing = true;
                m_hexFocusFrames = 2;
                std::snprintf(m_hexBuffer, sizeof(m_hexBuffer), "%s", hex);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Resumen
// -----------------------------------------------------------------------------

Ui::CanvasSummary Ui::canvasSummary() const {
    const CanvasForm& form = m_canvasForm;
    const int width = form.pixelWidth();
    const int height = form.pixelHeight();
    CanvasSummary summary;
    char text[160];
    std::snprintf(text, sizeof(text), "%d × %d px", width, height);
    summary.size = text;

    // Qué tamaño es: el que se eligió si aún lo es, el primero que coincida (mejor en su
    // orientación que girado), un papel conocido o uno a medida.
    auto match = [&](const canvasspec::Preset& preset) {
        int w = 0;
        int h = 0;
        presetPixels(preset, form.ppi(), &w, &h);
        return sizeMatch(width, height, w, h);
    };
    const char* name = nullptr;
    if (m_canvasPreset[0] >= 0) {
        const auto list = canvasspec::presets(static_cast<PresetCategory>(m_canvasPreset[0]));
        if (m_canvasPreset[1] >= 0 && m_canvasPreset[1] < static_cast<int>(list.size()) &&
            match(list[static_cast<size_t>(m_canvasPreset[1])]) > 0) {
            name = list[static_cast<size_t>(m_canvasPreset[1])].name;
        }
    }
    for (int wanted = 2; !name && wanted > 0; --wanted) {
        for (int c = 0; !name && c < static_cast<int>(PresetCategory::Saved); ++c) {
            for (const canvasspec::Preset& preset : canvasspec::presets(static_cast<PresetCategory>(c))) {
                if (match(preset) == wanted) {
                    name = preset.name;
                    break;
                }
            }
        }
    }
    if (!name) {
        name = canvasspec::paperName(width, height, form.ppi());
    }
    const char* orientation = canvasspec::orientationName(width, height);
    std::string detail;
    if (name) {
        // "A4 vertical", pero "Cuadrado" o "Full HD vertical" sin repetirlo.
        auto lower = [](std::string s) {
            for (char& ch : s) {
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
            }
            return s;
        };
        const std::string word = lower(orientation);
        detail = name;
        if (lower(detail).find(word) == std::string::npos) {
            detail += " " + word;
        }
    } else {
        detail = std::string(orientation) + " a medida";
    }
    // Medido en papel, lo que mide a esos ppp; en píxeles, solo los ppp (el tamaño al
    // imprimir no suele importar para vídeo o pantalla).
    if (form.unit() == LengthUnit::Pixels) {
        detail += " · " + canvasspec::formatPpi(form.ppi());
    } else {
        detail += " · " + canvasspec::formatSize(form.width(), form.height(), form.unit()) + " a " +
                  canvasspec::formatPpi(form.ppi());
    }
    summary.detail = detail;

    // Las capas que caben o por qué no se puede crear.
    switch (canvasspec::sizeProblem(width, height, m_status.maxCanvasSize)) {
    case SizeProblem::TooSmall:
        std::snprintf(text, sizeof(text), "Cada lado necesita al menos %d px", canvasspec::kMinSide);
        summary.note = text;
        summary.noteColor = th::kRed;
        summary.canCreate = false;
        break;
    case SizeProblem::TooWide:
        std::snprintf(text, sizeof(text), "Este dispositivo admite como mucho %d px por lado", m_status.maxCanvasSize);
        summary.note = text;
        summary.noteColor = th::kRed;
        summary.canCreate = false;
        break;
    case SizeProblem::TooManyPixels:
        summary.note = "Demasiado grande: como mucho " +
                       canvasspec::formatNumber(static_cast<double>(canvasspec::kMaxPixels) / 1.0e6, 0) +
                       " megapíxeles";
        summary.noteColor = th::kRed;
        summary.canCreate = false;
        break;
    case SizeProblem::None: {
        const int layers = canvasspec::layerLimit(width, height);
        const std::string bytes = canvasspec::formatBytes(canvasspec::layerBytes(width, height));
        if (layers < 10) {
            summary.note = "A este tamaño solo caben " + std::to_string(layers) + " capas · " + bytes + " cada una";
            summary.noteColor = th::kOrange;
        } else {
            summary.note = "Hasta " + std::to_string(layers) + " capas · " + bytes + " cada una";
        }
        break;
    }
    }
    return summary;
}

void Ui::canvasSummaryBlock(ImDrawList* dl, const CanvasSummary& summary, float left, float right, float top,
                            float preview) {
    const CanvasForm& form = m_canvasForm;
    // La forma del lienzo con su fondo.
    const ImRect box(ImVec2(left, top), ImVec2(left + preview, top + preview));
    dl->AddRectFilled(box.Min, box.Max, IM_COL32(0, 0, 0, 64), pt(10.0f));
    ui::outline(dl, box, pt(10.0f), IM_COL32(255, 255, 255, 20), ui::hairline());
    const float w = static_cast<float>(std::max(form.pixelWidth(), 1));
    const float h = static_cast<float>(std::max(form.pixelHeight(), 1));
    const float inner = preview - pt(16.0f);
    const float fit = std::min(inner / w, inner / h);
    const ImRect shape = frameRect(box.GetCenter(), ImVec2(std::max(w * fit, pt(4.0f)), std::max(h * fit, pt(4.0f))));
    backgroundFill(dl, shape, pt(2.0f), form.background, form.color);
    ui::outline(dl, shape, pt(2.0f), IM_COL32(255, 255, 255, 64), ui::hairline());

    // Tres líneas: píxeles, qué tamaño es y las capas (o el aviso).
    const float x = box.Max.x + pt(12.0f);
    const float width = std::max(right - x, pt(40.0f));
    const float cy = box.GetCenter().y;
    ui::label(dl, Weight::SemiBold, th::kBody, ImVec2(x, cy - pt(18.0f)), Align::Left, th::kLabel, summary.size.c_str(),
              width);
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(x, cy + pt(1.0f)), Align::Left, th::kSecondaryLabel,
              summary.detail.c_str(), width);
    if (summary.noteColor != 0) {
        const ImVec2 iconCenter(x + pt(7.0f), cy + pt(19.0f));
        ui::icon(dl, icon::kTriangleAlert, iconCenter, 13.0f, summary.noteColor);
        ui::label(dl, Weight::SemiBold, th::kFootnote, ImVec2(x + pt(19.0f), cy + pt(19.0f)), Align::Left,
                  summary.noteColor, summary.note.c_str(), width - pt(19.0f));
    } else {
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(x, cy + pt(19.0f)), Align::Left, th::kTertiaryLabel,
                  summary.note.c_str(), width);
    }
}

// -----------------------------------------------------------------------------
// Teclado numérico
// -----------------------------------------------------------------------------

void Ui::canvasKeypad(ImDrawList* dl, const ImRect& area) {
    CanvasForm& form = m_canvasForm;
    m_keypadRect = area;
    ImGui::PushID("##keypad");

    // Barra: qué se escribe con su valor, y «Listo».
    const float barCy = area.Min.y + pt(kKeyBar * 0.5f);
    const char* caption = "Resolución";
    const char* unit = "ppp";
    if (form.field() == Field::Width) {
        caption = "Ancho";
        unit = canvasspec::unitSymbol(form.unit());
    } else if (form.field() == Field::Height) {
        caption = "Alto";
        unit = canvasspec::unitSymbol(form.unit());
    }
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(area.Min.x, barCy - pt(10.0f)), Align::Left, th::kSecondaryLabel,
              caption);
    {
        const float valueCy = barCy + pt(10.0f);
        editText(dl, th::kHeadline, ImVec2(area.Min.x, valueCy), Align::Left, form.text(), form.fresh());
        const float textWidth = ui::measure(Weight::SemiBold, th::kHeadline, form.text().c_str()).x;
        ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(area.Min.x + textWidth + pt(9.0f), valueCy), Align::Left,
                  th::kSecondaryLabel, unit);
    }
    const ImRect done(ImVec2(area.Max.x - pt(92.0f), barCy - pt(19.0f)), ImVec2(area.Max.x, barCy + pt(19.0f)));
    if (ui::button("##done", done, "Listo", ui::ButtonStyle::Primary)) {
        form.commit();
    }

    // Teclas: 3 × 4, o 6 × 2 si no hay alto (un teléfono en horizontal).
    const ImRect keys(ImVec2(area.Min.x, area.Min.y + pt(kKeyBar + 6.0f)), area.Max);
    const float gap = pt(kKeyGap);
    const bool flat = keys.GetHeight() < pt(40.0f) * 4.0f + gap * 3.0f;
    const int columns = flat ? 6 : 3;
    const int rows = 12 / columns;
    const float keyWidth = (keys.GetWidth() - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float keyHeight =
        std::min(pt(kKey), (keys.GetHeight() - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows));
    // Cada tecla: un dígito, la coma o '\b' (borrar).
    static constexpr char kKeys[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', ',', '0', '\b'};
    static constexpr char kFlatKeys[12] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',', '\b'};
    const double now = ImGui::GetTime();
    for (int i = 0; i < 12; ++i) {
        const char key = (flat ? kFlatKeys : kKeys)[i];
        const bool erase = key == '\b';
        const bool enabled = key != ',' || form.acceptsComma();
        const float x0 = keys.Min.x + (keyWidth + gap) * static_cast<float>(i % columns);
        const float y0 = keys.Min.y + (keyHeight + gap) * static_cast<float>(i / columns);
        const ImRect r(ImVec2(x0, y0), ImVec2(x0 + keyWidth, y0 + keyHeight));
        const ImGuiID id = ImGui::GetID(static_cast<int>(key));
        const Press press = keyPress(id, r, enabled);
        const float level = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.4f : 0.0f), 30.0f);
        const float alpha = enabled ? 1.0f : 0.35f;
        const float radius = pt(12.0f);
        dl->AddRectFilled(r.Min, r.Max,
                          ui::withAlpha(ui::mix(IM_COL32(255, 255, 255, erase ? 10 : 20), IM_COL32(255, 255, 255, 56), level),
                                        alpha),
                          radius);
        if (erase) {
            ui::icon(dl, icon::kBackspace, r.GetCenter(), 22.0f, ui::withAlpha(th::kLabel, alpha));
        } else {
            const char text[2] = {key, '\0'};
            ui::label(dl, Weight::Regular, 24.0f, r.GetCenter(), Align::Center, ui::withAlpha(th::kLabel, alpha), text);
        }
        if (press.clicked) {
            if (erase) {
                form.erase();
                m_eraseRepeat = now + 0.45;
            } else {
                form.type(key);
            }
        } else if (erase && press.held) {
            // Mantenido, sigue borrando.
            if (now >= m_eraseRepeat) {
                form.erase();
                m_eraseRepeat = now + 0.07;
            }
            ui::anim::keepAlive();
        }
    }
    ImGui::PopID();
}

// -----------------------------------------------------------------------------
// La tarjeta
// -----------------------------------------------------------------------------

void Ui::submitCanvas(UiRequests& requests, bool modal) {
    CanvasForm& form = m_canvasForm;
    form.commit();
    m_hexEditing = false;
    const CanvasSpec spec = form.spec();
    if (canvasspec::sizeProblem(spec.width, spec.height, m_status.maxCanvasSize) != SizeProblem::None) {
        return;
    }
    requests.createCanvas = true;
    requests.canvas = spec;
    savePrefs();   // el próximo lienzo empieza como este
    if (modal) {
        m_dialog = Dialog::None;
    }
}

void Ui::newCanvasCard(bool modal, float presence, bool interactive, UiRequests& requests) {
    if (!m_savedSizesLoaded) {
        loadSavedSizes();
    }
    const Layout& L = m_layout;
    const ImGuiIO& io = ImGui::GetIO();
    CanvasForm& form = m_canvasForm;

    // Escribiendo un número: un toque fuera del campo y del teclado (sin arrastrar, que
    // es desplazar) lo deja escrito; si no, el teclado físico.
    const bool wasEditing = form.field() != Field::None;
    if (interactive && wasEditing) {
        const float slop = pt(kTapSlop);
        const bool tap = ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] <= slop * slop;
        if (tap && !m_fieldRect.Contains(io.MouseClickedPos[0]) && !m_keypadRect.Contains(io.MouseClickedPos[0])) {
            form.commit();
        } else {
            canvasKeys();
        }
    }
    m_fieldRect = ImRect();
    m_keypadRect = ImRect();
    if (form.field() == Field::None) {
        m_fieldScroll = false;
    }
    const bool keypad = form.field() != Field::None && m_keypad;
    const CanvasSummary summary = canvasSummary();
    const int pixelWidth = form.pixelWidth();
    const int pixelHeight = form.pixelHeight();
    const bool canSave = canvasspec::sizeProblem(pixelWidth, pixelHeight, 0) == SizeProblem::None;
    // Intro crea el lienzo (si no se estaba escribiendo: entonces solo deja el número).
    bool create = interactive && !wasEditing && !io.WantTextInput && summary.canCreate &&
                  (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    bool close = false;
    bool save = false;

    const char* description =
        modal ? "El dibujo actual se descartará: guárdalo antes como PNG si quieres conservarlo."
              : "NDI y el PNG usan el lienzo entero a este tamaño, sin importar el zoom.";
    const char* surface = modal ? "##new-canvas" : "##start-card";
    const ImU32 tint = modal ? IM_COL32(24, 24, 28, 232) : IM_COL32(24, 24, 28, 176);
    const float e = ui::anim::easeOutCubic(std::clamp(presence, 0.0f, 1.0f));
    const bool sheet = L.narrow || L.right - L.left < pt(kTwoColumns);

    if (sheet) {
        // Teléfono: hoja a pantalla completa. Arriba, fijos, la marca, el título y cerrar;
        // en medio, todo en una columna que se desplaza; abajo, fijos, el resumen y los
        // botones (o el teclado mientras se escribe un número).
        const ImRect rect(ImVec2(0.0f, 0.0f), L.display);
        ui::beginSurface(surface, rect, interactive, true);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ui::DrawMark mark = ui::mark(dl);
        ui::glass(dl, rect, 0.0f, tint, 0, modal ? kDimAlpha * presence : 0.0f);
        const float padX = pt(kSheetPad);
        const float left = L.safe[3] + padX;
        const float right = L.display.x - L.safe[1] - padX;
        const float width = right - left;

        const float titleHeight = ui::fontSize(th::kTitle);
        const float headerTop = L.safe[0] + pt(14.0f);
        brand(dl, left, headerTop + pt(7.0f));
        const float titleCy = headerTop + pt(14.0f + 6.0f) + titleHeight * 0.5f;
        const float closeSize = pt(34.0f);
        ui::label(dl, Weight::Bold, th::kTitle, ImVec2(left, titleCy), Align::Left, th::kLabel, "Nuevo lienzo",
                  width - (modal ? closeSize + pt(12.0f) : 0.0f));
        if (modal && closeButton(dl, ImRect(ImVec2(right - closeSize, titleCy - closeSize * 0.5f),
                                            ImVec2(right, titleCy + closeSize * 0.5f)))) {
            close = true;
        }
        const float headerBottom = titleCy + titleHeight * 0.5f + pt(14.0f);
        ui::separator(dl, rect.Min.x, rect.Max.x, headerBottom, th::kRule);

        const float footerHeight =
            keypad ? keypadHeight() + pt(10.0f + 8.0f) : pt(12.0f + kSummary + 12.0f + kButton + 12.0f);
        const float footerTop = L.display.y - L.safe[2] - footerHeight;

        // Cuerpo.
        const ImRect view(ImVec2(rect.Min.x, headerBottom + ui::hairline()), ImVec2(rect.Max.x, footerTop));
        const float descriptionHeight = ui::paragraph(nullptr, Weight::Regular, th::kCallout, ImVec2(0.0f, 0.0f), width,
                                                      Align::Left, 0, description, 1.45f);
        const float presetsHeight = canvasPresetsHeight(width, form.category);
        const float content = pt(kBodyPad) + descriptionHeight + pt(16.0f) + presetsHeight + pt(kSectionGap + 6.0f) +
                              canvasSettingsHeight(width, form.background == FormBackground::Color) + pt(kBodyPad);
        const bool scrolls = content > view.GetHeight() + 0.5f;
        const float scroll = scrolls ? ui::beginScroll("##sheet", view, content) : 0.0f;
        float y = view.Min.y + pt(kBodyPad) - scroll;
        ui::paragraph(dl, Weight::Regular, th::kCallout, ImVec2(left, y), width, Align::Left,
                      IM_COL32(235, 235, 245, 153), description, 1.45f);
        y += descriptionHeight + pt(16.0f);
        canvasPresets(dl, left, right, y);
        y += presetsHeight + pt(kSectionGap + 6.0f);
        canvasSettings(dl, left, right, y, interactive);
        if (scrolls) {
            ui::endScroll();
        }
        // Al empezar a escribir (con el teclado en pantalla, encima de él), que se vea el campo.
        // Si el teclado aparece con este toque, se espera al frame en el que ya ocupa su sitio.
        const bool keypadPlaced = keypad == (form.field() != Field::None && m_keypad);
        if (m_fieldScroll && keypadPlaced && m_fieldRect.GetWidth() > 0.0f) {
            if (scrolls) {
                const float origin = view.Min.y - scroll;
                ui::scrollIntoView("##sheet", m_fieldRect.Min.y - origin - pt(16.0f),
                                   m_fieldRect.Max.y - origin + pt(16.0f), view.GetHeight());
            }
            m_fieldScroll = false;
        }

        // Pie.
        dl->AddRectFilled(ImVec2(rect.Min.x, footerTop), ImVec2(rect.Max.x, rect.Max.y), IM_COL32(16, 16, 19, 140));
        ui::separator(dl, rect.Min.x, rect.Max.x, footerTop, th::kRule);
        if (keypad) {
            canvasKeypad(dl, ImRect(ImVec2(left, footerTop + pt(10.0f)), ImVec2(right, footerTop + footerHeight - pt(8.0f))));
        } else {
            float top = footerTop + pt(12.0f);
            canvasSummaryBlock(dl, summary, left, right, top, pt(kSummary));
            top += pt(kSummary + 12.0f);
            const ImRect saveRect(ImVec2(left, top), ImVec2(left + pt(kButton + 4.0f), top + pt(kButton)));
            save = saveSizeButton(dl, saveRect, false, canSave);
            if (createButton(dl, ImRect(ImVec2(saveRect.Max.x + pt(10.0f), top), ImVec2(right, top + pt(kButton))),
                             summary.canCreate)) {
                create = true;
            }
        }
        // Sube un poco mientras aparece.
        ui::transform(mark, ImVec2(L.display.x * 0.5f, L.display.y), 1.0f, ImVec2(0.0f, pt(40.0f) * (1.0f - e)),
                      std::min(1.0f, presence * 1.5f));
        ui::endSurface();
    } else {
        // Tableta u ordenador: dos columnas. A la izquierda los tamaños (o el teclado mientras
        // se escribe un número con él); a la derecha los ajustes; abajo, el resumen y los
        // botones.
        const float width = std::min(pt(kMaxWidth), L.right - L.left);
        const float pad = pt(kPad);
        const float inner = width - pad * 2.0f;
        const float gutter = pt(kGutter);
        const float settingsWidth = std::max(pt(kMinSettings), (inner - gutter) * kSettingsShare);
        const float presetsWidth = inner - gutter - settingsWidth;
        const float closeSize = pt(32.0f);
        const float headerWidth = inner - (modal ? closeSize + pt(12.0f) : 0.0f);
        // Con poco alto (un teléfono en horizontal), la cabecera va en dos líneas cortas.
        const bool shortHeader = L.bottom - L.top < pt(560.0f);
        const float titleHeight = ui::fontSize(shortHeader ? th::kHeadline : th::kTitle);
        const float descriptionHeight =
            shortHeader ? ui::fontSize(th::kFootnote)
                        : ui::paragraph(nullptr, Weight::Regular, th::kCallout, ImVec2(0.0f, 0.0f), headerWidth,
                                        Align::Left, 0, description, 1.45f);
        const float headerHeight = shortHeader ? pt(14.0f) + titleHeight + pt(2.0f) + descriptionHeight + pt(12.0f)
                                               : pad + pt(14.0f + 6.0f) + titleHeight + pt(6.0f) + descriptionHeight +
                                                     pt(18.0f);
        const float footerHeight = pt(14.0f + kSummary + 14.0f);
        float presetsNeed = 0.0f;
        for (int i = 0; i < canvasspec::kPresetCategoryCount; ++i) {
            presetsNeed = std::max(presetsNeed, canvasPresetsHeight(presetsWidth, i));
        }
        const float settingsNeed = canvasSettingsHeight(settingsWidth, true);
        const float bodyNeed = std::max(presetsNeed, settingsNeed) + pt(kBodyPad * 2.0f);
        const float height = std::min({L.bottom - L.top, headerHeight + bodyNeed + footerHeight, pt(kMaxHeight)});
        const float x = std::round((L.left + L.right - width) * 0.5f);
        const float y = std::round((L.top + L.bottom - height) * 0.5f);
        const ImRect rect(x, y, x + width, y + height);
        const float radius = pt(th::kDialogRadius);

        ui::beginSurface(surface, rect, interactive, true);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ui::DrawMark mark = ui::mark(dl);
        ui::pushUnclipped(dl);
        ui::shadow(dl, rect, radius, pt(70.0f), pt(26.0f), 0.5f);
        ui::popUnclipped(dl);
        ui::glass(dl, rect, radius, tint, 0, modal ? kDimAlpha * presence : 0.0f);
        const float left = rect.Min.x + pad;
        const float right = rect.Max.x - pad;

        // Cabecera: la marca, el título, la explicación y (desde Acciones) cerrar.
        if (shortHeader) {
            const float top = rect.Min.y + pt(14.0f);
            ui::label(dl, Weight::Bold, th::kHeadline, ImVec2(left, top + titleHeight * 0.5f), Align::Left, th::kLabel,
                      "Nuevo lienzo", headerWidth);
            ui::label(dl, Weight::Regular, th::kFootnote,
                      ImVec2(left, top + titleHeight + pt(2.0f) + descriptionHeight * 0.5f), Align::Left,
                      IM_COL32(235, 235, 245, 153), description, headerWidth);
            if (modal && closeButton(dl, ImRect(ImVec2(right - closeSize, top), ImVec2(right, top + closeSize)))) {
                close = true;
            }
        } else {
            float top = rect.Min.y + pad;
            const float headerTop = top;
            brand(dl, left, top + pt(7.0f));
            top += pt(14.0f + 6.0f);
            ui::label(dl, Weight::Bold, th::kTitle, ImVec2(left, top + titleHeight * 0.5f), Align::Left, th::kLabel,
                      "Nuevo lienzo", headerWidth);
            top += titleHeight + pt(6.0f);
            ui::paragraph(dl, Weight::Regular, th::kCallout, ImVec2(left, top), headerWidth, Align::Left,
                          IM_COL32(235, 235, 245, 153), description, 1.45f);
            if (modal &&
                closeButton(dl, ImRect(ImVec2(right - closeSize, headerTop), ImVec2(right, headerTop + closeSize)))) {
                close = true;
            }
        }
        const float bodyTop = rect.Min.y + headerHeight;
        const float bodyBottom = rect.Max.y - footerHeight;
        ui::separator(dl, rect.Min.x, rect.Max.x, bodyTop, th::kRule);

        // Columnas.
        const float split = left + presetsWidth + gutter * 0.5f;
        const ImRect presetsView(ImVec2(rect.Min.x, bodyTop + ui::hairline()), ImVec2(split, bodyBottom));
        const ImRect settingsView(ImVec2(split + ui::hairline(), bodyTop + ui::hairline()), ImVec2(rect.Max.x, bodyBottom));
        if (keypad) {
            const float keypadWidth = std::min(presetsWidth, pt(kKeypadWidth));
            const float kx = left + (presetsWidth - keypadWidth) * 0.5f;
            const float top = bodyTop + pt(kBodyPad);
            const float kh = std::min(keypadHeight(), bodyBottom - pt(kBodyPad) - top);
            canvasKeypad(dl, ImRect(ImVec2(kx, top), ImVec2(kx + keypadWidth, top + kh)));
        } else {
            const float content = canvasPresetsHeight(presetsWidth, form.category) + pt(kBodyPad * 2.0f);
            const bool scrolls = content > presetsView.GetHeight() + 0.5f;
            const float scroll = scrolls ? ui::beginScroll("##presets", presetsView, content) : 0.0f;
            canvasPresets(dl, left, left + presetsWidth, bodyTop + pt(kBodyPad) - scroll);
            if (scrolls) {
                ui::endScroll();
            }
        }
        dl->AddRectFilled(ImVec2(split, bodyTop + pt(16.0f)), ImVec2(split + ui::hairline(), bodyBottom - pt(16.0f)),
                          th::kRule);
        {
            const float content =
                canvasSettingsHeight(settingsWidth, form.background == FormBackground::Color) + pt(kBodyPad * 2.0f);
            const bool scrolls = content > settingsView.GetHeight() + 0.5f;
            const float scroll = scrolls ? ui::beginScroll("##settings", settingsView, content) : 0.0f;
            canvasSettings(dl, right - settingsWidth, right, bodyTop + pt(kBodyPad) - scroll, interactive);
            if (scrolls) {
                ui::endScroll();
            }
            // Que se vea el campo que se empieza a escribir.
            if (m_fieldScroll && m_fieldRect.GetWidth() > 0.0f) {
                if (scrolls) {
                    const float origin = settingsView.Min.y - scroll;
                    ui::scrollIntoView("##settings", m_fieldRect.Min.y - origin - pt(16.0f),
                                       m_fieldRect.Max.y - origin + pt(16.0f), settingsView.GetHeight());
                }
                m_fieldScroll = false;
            }
        }

        // Pie: el resumen y, a la derecha, guardar el tamaño y crear.
        ui::separator(dl, rect.Min.x, rect.Max.x, bodyBottom, th::kRule);
        const float rowTop = bodyBottom + pt(14.0f);
        const float buttonTop = rowTop + pt((kSummary - kButton) * 0.5f);
        const ImRect createRect(ImVec2(right - pt(196.0f), buttonTop), ImVec2(right, buttonTop + pt(kButton)));
        const bool saveText = createRect.Min.x - left - saveSizeWidth() > pt(360.0f);
        const float saveWidth = saveText ? saveSizeWidth() : pt(kButton + 4.0f);
        const ImRect saveRect(ImVec2(createRect.Min.x - pt(10.0f) - saveWidth, buttonTop),
                              ImVec2(createRect.Min.x - pt(10.0f), buttonTop + pt(kButton)));
        canvasSummaryBlock(dl, summary, left, saveRect.Min.x - pt(18.0f), rowTop, pt(kSummary));
        save = saveSizeButton(dl, saveRect, saveText, canSave);
        if (createButton(dl, createRect, summary.canCreate)) {
            create = true;
        }

        // Aparece subiendo un poco; al cerrarse baja mientras se desvanece.
        ui::transform(mark, rect.GetCenter(), 0.97f + 0.03f * e, ImVec2(0.0f, pt(16.0f) * (1.0f - e)),
                      std::min(1.0f, presence * 1.4f));
        ui::endSurface();
    }

    if (!interactive) {
        return;
    }
    if (save) {
        saveCanvasSize();
    }
    if (create) {
        submitCanvas(requests, modal);
    } else if (close) {
        m_dialog = Dialog::None;
    }
}
