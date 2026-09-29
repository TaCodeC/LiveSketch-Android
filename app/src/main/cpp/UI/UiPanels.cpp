// Paneles que se abren desde las barras: acciones, NDI, pinceles, capas (con el menú de
// la capa activa) y color. En una tableta cuelgan bajo su botón; en un teléfono en
// vertical son hojas que suben desde abajo. Comparten el aspecto: cabecera con el título
// y una línea, filas con el icono atenuado y rótulos de sección pequeños.
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"

#include <SDL3/SDL_platform_defines.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;

namespace {

constexpr const char* kBrushNames[BrushSettings::kTypeCount] = {"Básico", "Texturizado", "Caligráfico", "Acuarela"};
constexpr const char* kBrushNotes[BrushSettings::kTypeCount] = {"Redondo y suave", "Cerdas secas", "Punta cuadrada",
                                                                "Mancha difusa"};

constexpr ImU32 kPalette[12] = {
    IM_COL32(255, 255, 255, 255), IM_COL32(142, 142, 147, 255), IM_COL32(28, 28, 30, 255),
    IM_COL32(255, 69, 58, 255),   IM_COL32(255, 159, 10, 255),  IM_COL32(255, 214, 10, 255),
    IM_COL32(48, 209, 88, 255),   IM_COL32(100, 210, 255, 255), IM_COL32(10, 132, 255, 255),
    IM_COL32(94, 92, 230, 255),   IM_COL32(191, 90, 242, 255),  IM_COL32(255, 55, 95, 255),
};

// Pestañas de Acciones, como las de Procreate.
constexpr const char* kActionTabs[4] = {"Lienzo", "Compartir", "Preferencias", "Ayuda"};

// Ayuda: gestos y atajos de teclado.
struct HelpItem {
    const char* key;
    const char* title;
    const char* detail;   // null: una sola línea
};
constexpr HelpItem kGestures[] = {
    {"2 DEDOS", "Deshacer", "Toca el lienzo con dos dedos"},
    {"3 DEDOS", "Rehacer", "Toca el lienzo con tres dedos"},
    {"PELLIZCA", "Mover y hacer zoom", "Arrastra o pellizca con dos dedos"},
    {"MANTÉN", "Cuentagotas con el dedo", "Deja el dedo quieto al empezar"},
};
constexpr HelpItem kShortcuts[] = {
    {"B", "Pincel", nullptr},
    {"E", "Borrador", nullptr},
    {"[  ]", "Tamaño del pincel", nullptr},
    {"CTRL Z", "Deshacer", nullptr},
    {"CTRL Y", "Rehacer", nullptr},
    {"ALT+CLIC", "Cuentagotas", nullptr},
};
constexpr int kGestureCount = static_cast<int>(sizeof(kGestures) / sizeof(kGestures[0]));
constexpr int kShortcutCount = static_cast<int>(sizeof(kShortcuts) / sizeof(kShortcuts[0]));

// Medidas de la ayuda (pt).
constexpr float kHelpSection = 26.0f;
constexpr float kGestureRow = 46.0f;
constexpr float kShortcutRow = 30.0f;
constexpr float kKeyColumn = 72.0f;
constexpr float kHelpHeight = 8.0f + kHelpSection + kGestureRow * kGestureCount + 6.0f + kHelpSection +
                              kShortcutRow * kShortcutCount + 12.0f + 16.0f + 12.0f;

ImTextureID textureId(GLuint texture) { return static_cast<ImTextureID>(texture); }

void toFloat(ImU32 color, float rgb[3]) {
    rgb[0] = static_cast<float>((color >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    rgb[1] = static_cast<float>((color >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    rgb[2] = static_cast<float>((color >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
}

bool sameColor(const float a[3], const float b[3]) {
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(a[i] - b[i]) > 1.0f / 512.0f) {
            return false;
        }
    }
    return true;
}

// "#RGB", "#RRGGBB" o sin "#".
bool parseHex(const char* text, float rgb[3]) {
    char digits[7] = {};
    int count = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '#' || *p == ' ') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(*p)) || count == 6) {
            return false;
        }
        digits[count++] = *p;
    }
    if (count == 3) {
        const char full[7] = {digits[0], digits[0], digits[1], digits[1], digits[2], digits[2], '\0'};
        std::memcpy(digits, full, sizeof(full));
    } else if (count != 6) {
        return false;
    }
    const unsigned long value = std::strtoul(digits, nullptr, 16);
    rgb[0] = static_cast<float>((value >> 16) & 0xFF) / 255.0f;
    rgb[1] = static_cast<float>((value >> 8) & 0xFF) / 255.0f;
    rgb[2] = static_cast<float>(value & 0xFF) / 255.0f;
    return true;
}

// Resalte de una fila o un botón al pulsarlo (o con el ratón encima).
void pressFeedback(ImDrawList* dl, ImGuiID id, const ImRect& rect, const Press& press, float radius) {
    const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
    if (t > 0.002f) {
        dl->AddRectFilled(rect.Min, rect.Max, ui::withAlpha(th::kPressed, t), radius);
    }
}

// Cabecera de un panel: el título a la izquierda y una línea debajo. Devuelve su centro
// vertical, para lo que va a la derecha.
float panelHeader(ImDrawList* dl, const ImRect& content, float top, const char* title) {
    const float cy = top + pt(th::kHeaderHeight * 0.5f);
    ui::label(dl, Weight::SemiBold, th::kPanelTitle, ImVec2(content.Min.x + pt(16.0f), cy), Align::Left, th::kLabel,
              title);
    ui::separator(dl, content.Min.x, content.Max.x, top + pt(th::kHeaderHeight) - ui::hairline(), th::kRule);
    return cy;
}

// Etiqueta en mayúsculas sobre un fondo suave, con un punto delante si `dot` no es 0.
// `right` es su borde derecho y `cy` su centro.
void tag(ImDrawList* dl, float right, float cy, const char* text, ImU32 fill, ImU32 color, ImU32 dot = 0) {
    constexpr float kPoints = 10.5f;
    constexpr float kTracking = 0.08f;
    const float dotSpace = dot ? pt(6.0f + 6.0f) : 0.0f;
    const float width = pt(8.0f) + dotSpace + ui::trackedWidth(Weight::Bold, kPoints, text, kTracking) + pt(8.0f);
    const ImRect r(ImVec2(right - width, cy - pt(11.0f)), ImVec2(right, cy + pt(11.0f)));
    dl->AddRectFilled(r.Min, r.Max, fill, pt(6.0f));
    float x = r.Min.x + pt(8.0f);
    if (dot) {
        dl->AddCircleFilled(ImVec2(x + pt(3.0f), cy), pt(3.0f), dot, 0);
        x += dotSpace;
    }
    ui::tracked(dl, Weight::Bold, kPoints, ImVec2(x, cy), color, text, kTracking);
}

// Fila de menú: icono atenuado, título y, a la derecha, un valor. `filled`: con fondo
// suave (la acción principal de su grupo). Sin `id` es solo información.
bool menuRow(ImDrawList* dl, const char* id, const ImRect& row, const char* glyph, const char* title,
             const char* value = nullptr, bool filled = false, bool enabled = true, ImU32 color = th::kLabel) {
    const float radius = pt(th::kControlRadius);
    Press press;
    if (id) {
        const ImGuiID gid = ImGui::GetID(id);
        press = ui::pressable(gid, row, enabled);
        if (filled) {
            dl->AddRectFilled(row.Min, row.Max, th::kControl, radius);
        }
        pressFeedback(dl, gid, row, press, radius);
    }
    const bool info = id == nullptr;
    ImU32 textColor = info ? th::kSecondaryLabel : color;
    ImU32 iconColor = info ? th::kSecondaryLabel : (color == th::kLabel ? th::kMutedIcon : color);
    if (!enabled) {
        textColor = ui::withAlpha(textColor, 0.4f);
        iconColor = ui::withAlpha(iconColor, 0.4f);
    }
    const float cy = row.GetCenter().y;
    ui::icon(dl, glyph, ImVec2(row.Min.x + pt(10.0f + 9.0f), cy), 18.0f, iconColor);
    float right = row.Max.x - pt(10.0f);
    if (value) {
        ui::label(dl, Weight::Regular, th::kCallout, ImVec2(right, cy), Align::Right,
                  info ? th::kSecondaryLabel : IM_COL32(235, 235, 245, 128), value);
        right -= ui::measure(Weight::Regular, th::kCallout, value).x + pt(10.0f);
    }
    const float x = row.Min.x + pt(10.0f + 18.0f + 12.0f);
    ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(x, cy), Align::Left, textColor, title,
              std::max(pt(40.0f), right - x));
    return press.clicked;
}

// Fila con un interruptor a la derecha.
bool toggleRow(ImDrawList* dl, const char* id, const ImRect& row, const char* glyph, const char* title, bool* value) {
    const float cy = row.GetCenter().y;
    const ImVec2 size = ui::toggleSize();
    ui::icon(dl, glyph, ImVec2(row.Min.x + pt(10.0f + 9.0f), cy), 18.0f, th::kMutedIcon);
    const float x = row.Min.x + pt(10.0f + 18.0f + 12.0f);
    const float toggleX = row.Max.x - pt(10.0f) - size.x;
    ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(x, cy), Align::Left, th::kLabel, title,
              std::max(pt(40.0f), toggleX - pt(10.0f) - x));
    return ui::toggle(id, ImVec2(toggleX, cy - size.y * 0.5f), value);
}

// Tecla o gesto de la ayuda: texto pequeño en un recuadro de `width` (centrado en `cy`).
void keycap(ImDrawList* dl, float x, float cy, float width, const char* text) {
    constexpr float kTracking = 0.04f;
    const ImRect r(ImVec2(x, cy - pt(13.0f)), ImVec2(x + width, cy + pt(13.0f)));
    dl->AddRectFilled(r.Min, r.Max, th::kControl, pt(7.0f));
    ui::outline(dl, r, pt(7.0f), th::kControlBorder, pt(1.0f));
    const float textWidth = ui::trackedWidth(Weight::Bold, th::kMicro, text, kTracking);
    ui::tracked(dl, Weight::Bold, th::kMicro, ImVec2(r.GetCenter().x - textWidth * 0.5f, cy), IM_COL32(235, 235, 245, 204),
                text, kTracking);
}

} // namespace

// -----------------------------------------------------------------------------
// Marco de los paneles
// -----------------------------------------------------------------------------

bool Ui::beginPanel(PanelFrame& f, Panel panel, const char* name, float x, float width, float contentHeight,
                    float anchorX, bool scrollBody) {
    const Layout& L = m_layout;
    f.open = m_panel == panel;
    f.presence = ui::anim::followFrom(ImHashStr(name), 0.0f, f.open ? 1.0f : 0.0f, f.open ? 16.0f : 22.0f);
    if (!f.open && f.presence <= 0.002f) {
        return false;
    }
    f.sheet = L.narrow;
    if (f.sheet) {
        const float grabber = pt(16.0f);
        const float maxHeight = L.display.y - (L.leftBar.Max.y + pt(12.0f));
        const float height = std::min(contentHeight + grabber + L.safe[2], maxHeight);
        f.rect = ImRect(0.0f, L.display.y - height, L.display.x, L.display.y);
        f.content = ImRect(f.rect.Min.x + L.safe[3], f.rect.Min.y + grabber, f.rect.Max.x - L.safe[1],
                           f.rect.Max.y - L.safe[2]);
        f.anchor = ImVec2(L.display.x * 0.5f, L.display.y);
    } else {
        x = std::clamp(x, L.left, std::max(L.left, L.right - width));
        const float height = std::min(contentHeight, L.bottom - L.popoverTop);
        f.rect = ImRect(x, L.popoverTop, x + width, L.popoverTop + height);
        f.content = f.rect;
        f.anchor = ImVec2(anchorX, L.popoverTop);
    }
    f.scrolls = scrollBody && contentHeight > f.content.GetHeight() + 0.5f;

    ui::beginSurface(name, f.rect, f.open, true);
    f.dl = ImGui::GetWindowDrawList();
    f.mark = ui::mark(f.dl);
    const float radius = pt(f.sheet ? th::kSheetRadius : th::kPanelRadius);
    const ImDrawFlags corners = f.sheet ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
    ui::pushUnclipped(f.dl);
    if (f.sheet) {
        ui::shadow(f.dl, f.rect, radius, pt(36.0f), -pt(8.0f), 0.32f, corners);
    } else {
        ui::shadow(f.dl, f.rect, radius, pt(44.0f), pt(16.0f), 0.3f, corners);
    }
    ui::popUnclipped(f.dl);
    ui::glass(f.dl, f.rect, radius, th::kPanelTint, corners);
    if (f.sheet) {
        const ImVec2 center(f.rect.GetCenter().x, f.rect.Min.y + pt(8.0f));
        f.dl->AddRectFilled(ImVec2(center.x - pt(16.0f), center.y - pt(2.0f)),
                            ImVec2(center.x + pt(16.0f), center.y + pt(2.0f)), IM_COL32(235, 235, 245, 71), pt(2.0f));
    }
    f.scroll = f.scrolls ? ui::beginScroll("##body", f.content, contentHeight) : 0.0f;
    return true;
}

void Ui::endPanel(PanelFrame& f) {
    if (f.scrolls) {
        ui::endScroll();
    }
    const float e = f.presence;
    if (f.sheet) {
        ui::transform(f.mark, f.anchor, 1.0f, ImVec2(0.0f, (1.0f - e) * f.rect.GetHeight()), std::min(1.0f, e * 2.0f));
    } else {
        // Baja un poco desde su botón mientras aparece.
        ui::transform(f.mark, f.anchor, 0.96f + 0.04f * e, ImVec2(0.0f, -pt(8.0f) * (1.0f - e)),
                      std::min(1.0f, e * 1.3f));
    }
    ui::endSurface();
}

void Ui::drawPanels(Canvas& canvas, UiRequests& requests) {
    actionsPanel(canvas, requests);
    ndiPanel(canvas, requests);
    brushesPanel();
    layersPanel(canvas);
    layerMenu(canvas);
    colorPanel(canvas);
}

// -----------------------------------------------------------------------------
// Acciones
// -----------------------------------------------------------------------------

void Ui::actionsPanel(Canvas& canvas, UiRequests& requests) {
    const Layout& L = m_layout;
    const int tab = std::clamp(m_actionsTab, 0, 3);
    const float rowPoints = L.narrow ? 48.0f : 44.0f;
    const float width = pt(332.0f);
    const float panelWidth = L.narrow ? L.display.x - L.safe[1] - L.safe[3] : width;
    constexpr float kStrip = 8.0f + 54.0f + 8.0f;   // pestañas con su margen

#ifdef SDL_PLATFORM_EMSCRIPTEN
    const char* shareNote = "Se descarga con el navegador, a tamaño completo. Si ocultas el fondo, queda transparente.";
#elif defined(SDL_PLATFORM_ANDROID)
    const char* shareNote = "Se guarda en Descargas, a tamaño completo. Si ocultas el fondo, queda transparente.";
#else
    const char* shareNote =
        "Se guarda en la carpeta de descargas, a tamaño completo. Si ocultas el fondo, queda transparente.";
#endif
    const float noteWidth = panelWidth - pt(8.0f + 10.0f) * 2.0f;

    // Alto del contenido de la pestaña elegida; al cambiar de pestaña el panel se anima.
    float body = 0.0f;
    switch (tab) {
    case 0:
        body = pt(8.0f + rowPoints * 3.0f + 2.0f * 2.0f + 8.0f);
        break;
    case 1:
        body = pt(8.0f + rowPoints + 10.0f) +
               ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f), noteWidth, Align::Left, 0,
                             shareNote, 1.4f) +
               pt(12.0f);
        break;
    case 2:
        body = pt(8.0f + rowPoints * 2.0f + 2.0f + 34.0f + 58.0f + 12.0f);
        break;
    default:
        body = pt(kHelpHeight);
        break;
    }
    const float content = ui::anim::follow(ImHashStr("##actions-height"), pt(kStrip) + body, 22.0f, 0.5f);
    const float anchorX = L.leftBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * 0.5f);
    PanelFrame f;
    if (!beginPanel(f, Panel::Actions, "##panel-actions", L.leftBar.Min.x, width, content, anchorX, false)) {
        return;
    }
    ImDrawList* dl = f.dl;
    const float left = f.content.Min.x + pt(8.0f);
    const float right = f.content.Max.x - pt(8.0f);
    float y = f.content.Min.y;

    // Pestañas: icono y nombre; la elegida, en el color de acento.
    {
        const float gap = pt(4.0f);
        const float tabWidth = (right - left - gap * 3.0f) / 4.0f;
        const char* glyphs[4] = {icon::kFrame, icon::kShare, icon::kSliders, icon::kHelp};
        for (int i = 0; i < 4; ++i) {
            const float x0 = left + (tabWidth + gap) * static_cast<float>(i);
            const ImRect r(ImVec2(x0, y + pt(8.0f)), ImVec2(x0 + tabWidth, y + pt(8.0f + 54.0f)));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##tab", r, tab == i, true, false);
            ImGui::PopID();
            const ImU32 color = ui::mix(IM_COL32(235, 235, 245, 179), th::kAccent, c.on);
            ui::icon(dl, glyphs[i], ImVec2(r.GetCenter().x, r.Min.y + pt(19.0f)), 20.0f, color);
            ui::label(dl, Weight::SemiBold, th::kMicro, ImVec2(r.GetCenter().x, r.Max.y - pt(14.0f)), Align::Center,
                      color, kActionTabs[i], tabWidth - pt(4.0f));
            if (c.press.clicked) {
                m_actionsTab = i;
            }
        }
        y += pt(kStrip);
        ui::separator(dl, f.content.Min.x, f.content.Max.x, y - ui::hairline(), th::kRule);
    }

    // Contenido de la pestaña (con su propio desplazamiento si no cabe).
    const ImRect view(ImVec2(f.content.Min.x, y), f.content.Max);
    const bool scrolls = body > view.GetHeight() + 0.5f;
    char scrollId[32];
    std::snprintf(scrollId, sizeof(scrollId), "##actions-body%d", tab);
    const float scroll = scrolls ? ui::beginScroll(scrollId, view, body) : 0.0f;
    y += pt(8.0f) - scroll;
    const float row = pt(rowPoints);
    char size[32];
    std::snprintf(size, sizeof(size), "%d × %d", canvas.width(), canvas.height());
    switch (tab) {
    case 0:
        if (menuRow(dl, "##new", ImRect(left, y, right, y + row), icon::kFilePlus, "Nuevo lienzo…", nullptr, true)) {
            openDialog(Dialog::NewCanvas, &canvas);
        }
        y += row + pt(2.0f);
        if (menuRow(dl, "##fit", ImRect(left, y, right, y + row), icon::kScan, "Centrar lienzo")) {
            requests.fitView = true;
            closePanels();
        }
        y += row + pt(2.0f);
        menuRow(dl, nullptr, ImRect(left, y, right, y + row), icon::kProportions, "Tamaño del lienzo", size);
        break;

    case 1: {
        const bool exporting = m_status.exporting;
        if (menuRow(dl, "##save", ImRect(left, y, right, y + row), icon::kImageDown,
                    exporting ? "Guardando PNG…" : "Guardar PNG", size, true, !exporting)) {
            requests.savePng = true;
            closePanels();
        }
        y += row + pt(10.0f);
        ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(10.0f), y), noteWidth, Align::Left,
                      IM_COL32(235, 235, 245, 128), shareNote, 1.4f);
        break;
    }

    case 2: {
        bool finger = m_prefs.drawWithFinger;
        if (toggleRow(dl, "##finger", ImRect(left, y, right, y + row), icon::kHand, "Dibujar con el dedo", &finger)) {
            m_prefs.drawWithFinger = finger;
            savePrefs();
        }
        y += row + pt(2.0f);
        bool sideRight = m_prefs.sidebarRight;
        if (toggleRow(dl, "##side", ImRect(left, y, right, y + row), icon::kPanelRight, "Barra lateral a la derecha",
                      &sideRight)) {
            m_prefs.sidebarRight = sideRight;
            savePrefs();
        }
        y += row;
        ui::sectionLabel(dl, left + pt(10.0f), right - pt(10.0f), y + pt(19.0f), "TAMAÑO DE LA INTERFAZ");
        y += pt(34.0f);
        // Fichas con una muestra del tamaño del texto.
        const float gap = pt(6.0f);
        const float x0 = left + pt(2.0f);
        const float chipWidth = (right - pt(2.0f) - x0 - gap * 2.0f) / 3.0f;
        const char* names[3] = {"Pequeña", "Normal", "Grande"};
        constexpr float kSample[3] = {13.0f, 16.0f, 19.0f};
        for (int i = 0; i < 3; ++i) {
            const float cx = x0 + (chipWidth + gap) * static_cast<float>(i);
            const ImRect r(ImVec2(cx, y), ImVec2(cx + chipWidth, y + pt(58.0f)));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##uisize", r, m_prefs.size == i);
            ImGui::PopID();
            ui::label(dl, Weight::SemiBold, kSample[i], ImVec2(r.GetCenter().x, r.Min.y + pt(22.0f)), Align::Center,
                      ui::mix(IM_COL32(235, 235, 245, 204), th::kLabel, c.on), "Aa");
            ui::label(dl, Weight::SemiBold, th::kMicro, ImVec2(r.GetCenter().x, r.Max.y - pt(13.0f)), Align::Center,
                      ui::mix(IM_COL32(235, 235, 245, 128), th::kAccentText, c.on), names[i], chipWidth - pt(6.0f));
            if (c.press.clicked && m_prefs.size != i) {
                m_prefs.size = i;
                savePrefs();
            }
        }
        break;
    }

    default: {
        const float x = left + pt(10.0f);
        const float textX = x + pt(kKeyColumn + 12.0f);
        const float textWidth = right - pt(10.0f) - textX;
        ui::sectionLabel(dl, x, right - pt(10.0f), y + pt(kHelpSection * 0.5f), "GESTOS");
        y += pt(kHelpSection);
        for (const HelpItem& item : kGestures) {
            const float cy = y + pt(kGestureRow * 0.5f);
            keycap(dl, x, cy, pt(kKeyColumn), item.key);
            ui::label(dl, Weight::SemiBold, th::kCallout, ImVec2(textX, cy - pt(8.5f)), Align::Left, th::kLabel,
                      item.title, textWidth);
            ui::label(dl, Weight::Regular, 12.5f, ImVec2(textX, cy + pt(9.5f)), Align::Left, IM_COL32(235, 235, 245, 140),
                      item.detail, textWidth);
            y += pt(kGestureRow);
        }
        y += pt(6.0f);
        ui::sectionLabel(dl, x, right - pt(10.0f), y + pt(kHelpSection * 0.5f), "TECLADO");
        y += pt(kHelpSection);
        for (const HelpItem& item : kShortcuts) {
            const float cy = y + pt(kShortcutRow * 0.5f);
            keycap(dl, x, cy, pt(kKeyColumn), item.key);
            ui::label(dl, Weight::Regular, th::kCallout, ImVec2(textX, cy), Align::Left, th::kLabel, item.title,
                      textWidth);
            y += pt(kShortcutRow);
        }
        y += pt(12.0f);
        ui::label(dl, Weight::Regular, th::kMicro, ImVec2((left + right) * 0.5f, y + pt(8.0f)), Align::Center,
                  th::kTertiaryLabel, "LiveSketch 0.3 · Android y web");
        break;
    }
    }
    if (scrolls) {
        ui::endScroll();
    }
    endPanel(f);
}

// -----------------------------------------------------------------------------
// NDI
// -----------------------------------------------------------------------------

void Ui::ndiPanel(Canvas& canvas, UiRequests& requests) {
    const Layout& L = m_layout;
    const bool available = m_status.ndiAvailable;
    const bool running = m_status.ndiRunning;
    const bool failed = running && !m_status.ndiError.empty();
    const float width = pt(336.0f);
    const float panelWidth = L.narrow ? L.display.x - L.safe[1] - L.safe[3] : width;
    const float textWidth = panelWidth - pt(12.0f + 4.0f) * 2.0f;

#ifdef SDL_PLATFORM_EMSCRIPTEN
    const char* note = available ? "Los receptores ven el lienzo completo, sin el zoom ni la interfaz."
                                 : "Un navegador no puede emitir NDI. Para emitir, usa la app de Android.";
#else
    const char* note = available ? "Los receptores ven el lienzo completo, sin el zoom ni la interfaz."
                                 : "Esta compilación no incluye el SDK de NDI.";
#endif
    const float noteHeight = ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f), textWidth,
                                           Align::Left, 0, note, 1.4f);
    const float errorHeight = failed ? ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f),
                                                     textWidth, Align::Left, 0, m_status.ndiError.c_str(), 1.4f) +
                                           pt(8.0f)
                                     : 0.0f;
    constexpr float kButton = 52.0f;
    constexpr float kStats = 62.0f;
    const float content = pt(th::kHeaderHeight + 12.0f + kButton + 12.0f + kStats + 12.0f) + errorHeight + noteHeight +
                          pt(14.0f);
    const float anchorX = L.narrow ? L.leftBar.Max.x : L.ndiBar.GetCenter().x;
    PanelFrame f;
    if (!beginPanel(f, Panel::Ndi, "##panel-ndi", L.narrow ? L.left : L.ndiBar.Min.x, width, content, anchorX,
                    true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    float y = f.content.Min.y - f.scroll;

    // Cabecera con el estado.
    const float cy = panelHeader(dl, f.content, y, "NDI");
    {
        const ImU32 neutralFill = IM_COL32(255, 255, 255, 20);
        const ImU32 neutralText = IM_COL32(235, 235, 245, 179);
        const ImU32 neutralDot = IM_COL32(235, 235, 245, 115);
        const float tagRight = f.content.Max.x - pt(12.0f);
        if (!available) {
            tag(dl, tagRight, cy, "NO DISPONIBLE", neutralFill, neutralText, neutralDot);
        } else if (failed) {
            tag(dl, tagRight, cy, "ERROR", ui::withAlpha(th::kOrange, 0.2f), th::kOrange, th::kOrange);
        } else if (running) {
            tag(dl, tagRight, cy, "EN VIVO", IM_COL32(255, 69, 58, 51), IM_COL32(255, 105, 97, 255), th::kRed);
        } else {
            tag(dl, tagRight, cy, "APAGADO", neutralFill, neutralText, neutralDot);
        }
    }
    y += pt(th::kHeaderHeight + 12.0f);
    const float left = f.content.Min.x + pt(12.0f);
    const float right = f.content.Max.x - pt(12.0f);

    // Un solo botón grande: empezar a emitir (con el punto de grabar) o detener.
    {
        const ImRect r(left, y, right, y + pt(kButton));
        const Press press = ui::buttonFrame("##ndi-button", r, running ? ui::ButtonStyle::Destructive
                                                                       : ui::ButtonStyle::Secondary,
                                            available, pt(12.0f));
        const char* text = !available ? "Emitir por NDI" : (running ? "Detener la emisión" : "Empezar a emitir");
        const float markSide = pt(18.0f);
        const float gap = pt(10.0f);
        const float textW = ui::measure(Weight::SemiBold, th::kSubhead, text).x;
        const float x0 = r.GetCenter().x - (markSide + gap + textW) * 0.5f;
        const ImVec2 markCenter(x0 + markSide * 0.5f, r.GetCenter().y);
        const float alpha = available ? 1.0f : 0.4f;
        if (running) {
            dl->AddRectFilled(ImVec2(markCenter.x - pt(5.0f), markCenter.y - pt(5.0f)),
                              ImVec2(markCenter.x + pt(5.0f), markCenter.y + pt(5.0f)), IM_COL32_WHITE, pt(2.0f));
        } else {
            dl->AddCircleFilled(markCenter, pt(9.0f), ui::withAlpha(IM_COL32(255, 69, 58, 64), alpha), 0);
            dl->AddCircleFilled(markCenter, pt(5.0f), ui::withAlpha(th::kRed, alpha), 0);
        }
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(x0 + markSide + gap, r.GetCenter().y), Align::Left,
                  ui::withAlpha(th::kLabel, available ? 1.0f : 0.6f), text);
        if (press.clicked) {
            requests.ndi = running ? 0 : 1;
        }
        y += pt(kButton + 12.0f);
    }

    // Datos de la emisión en una tira de tres columnas.
    {
        const ImRect strip(left, y, right, y + pt(kStats));
        dl->AddRectFilled(strip.Min, strip.Max, IM_COL32(255, 255, 255, 13), pt(12.0f));
        ui::outline(dl, strip, pt(12.0f), th::kRule, pt(1.0f));
        char receivers[16];
        std::snprintf(receivers, sizeof(receivers), "%d", m_status.ndiConnections);
        char resolution[32];
        std::snprintf(resolution, sizeof(resolution), "%d×%d", canvas.width(), canvas.height());
        struct Stat {
            const char* title;
            const char* value;
            float points;
        };
        const Stat stats[3] = {
            {"RECEPTORES", running ? receivers : "—", 20.0f},
            {"TAMAÑO", resolution, th::kCallout},
            {"FUENTE", "LiveSketch", th::kCallout},
        };
        const float column = strip.GetWidth() / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float x0 = strip.Min.x + column * static_cast<float>(i);
            if (i > 0) {
                const float lineX = ui::snap(x0);
                dl->AddRectFilled(ImVec2(lineX, strip.Min.y), ImVec2(lineX + ui::hairline(), strip.Max.y), th::kRule);
            }
            ui::tracked(dl, Weight::Bold, 10.0f, ImVec2(x0 + pt(12.0f), strip.Min.y + pt(18.0f)), th::kSectionLabel,
                        stats[i].title, 0.08f);
            ui::label(dl, Weight::SemiBold, stats[i].points, ImVec2(x0 + pt(12.0f), strip.Min.y + pt(41.0f)),
                      Align::Left, th::kLabel, stats[i].value, column - pt(20.0f));
        }
        y += pt(kStats + 12.0f);
    }

    if (failed) {
        y += ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(4.0f), y), textWidth, Align::Left,
                           th::kOrange, m_status.ndiError.c_str(), 1.4f) +
             pt(8.0f);
    }
    ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(4.0f), y), textWidth, Align::Left,
                  IM_COL32(235, 235, 245, 140), note, 1.4f);
    endPanel(f);
}

// -----------------------------------------------------------------------------
// Pinceles
// -----------------------------------------------------------------------------

void Ui::brushesPanel() {
    const Layout& L = m_layout;
    constexpr float kRow = 66.0f;
    constexpr float kRowGap = 4.0f;
    const float content = th::kHeaderHeight + 8.0f + kRow * BrushSettings::kTypeCount +
                          kRowGap * (BrushSettings::kTypeCount - 1) + 8.0f;
    const float width = pt(340.0f);
    const int toolIndex = static_cast<int>(m_tool);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * (0.5f + toolIndex));
    PanelFrame f;
    if (!beginPanel(f, Panel::Brushes, "##panel-brushes", L.rightBar.Max.x - width, width, pt(content), anchorX,
                    true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    float y = f.content.Min.y - f.scroll;
    const float cy = panelHeader(dl, f.content, y, "Pinceles");
    tag(dl, f.content.Max.x - pt(12.0f), cy, m_tool == Tool::Brush ? "PINCEL" : "BORRADOR", IM_COL32(255, 255, 255, 20),
        IM_COL32(235, 235, 245, 179));
    y += pt(th::kHeaderHeight + 8.0f);
    const float left = f.content.Min.x + pt(8.0f);
    const float right = f.content.Max.x - pt(8.0f);

    ToolPreset& preset = m_presets[toolIndex];
    const float pixels = m_status.pixelsPerUnit;
    for (int i = 0; i < BrushSettings::kTypeCount; ++i) {
        const ImRect row(left, y, right, y + pt(kRow));
        ImGui::PushID(i);
        const ImGuiID id = ImGui::GetID("##row");
        const Press press = ui::pressable(id, row);
        ImGui::PopID();
        const bool selected = preset.type == i;
        const float on = ui::anim::follow(id + 7u, selected ? 1.0f : 0.0f, 20.0f);
        if (on > 0.002f) {
            dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(th::kAccent, on), pt(th::kRowRadius));
        }
        pressFeedback(dl, id, row, press, pt(th::kRowRadius));

        const float cy2 = row.GetCenter().y;
        const float nameWidth = pt(118.0f);
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(row.Min.x + pt(12.0f), cy2 - pt(9.0f)), Align::Left,
                  th::kLabel, kBrushNames[i], nameWidth);
        ui::label(dl, Weight::Regular, th::kCaption, ImVec2(row.Min.x + pt(12.0f), cy2 + pt(10.0f)), Align::Left,
                  ui::mix(IM_COL32(235, 235, 245, 153), IM_COL32(255, 255, 255, 204), on), kBrushNotes[i],
                  nameWidth);

        // Trazo de muestra hecho con el propio pincel.
        const float previewWidth = std::min(pt(176.0f), row.GetWidth() - pt(12.0f + 118.0f + 8.0f + 12.0f));
        const float previewHeight = previewWidth * 44.0f / 176.0f;
        const ImRect preview(ImVec2(row.Max.x - pt(12.0f) - previewWidth, cy2 - previewHeight * 0.5f),
                             ImVec2(row.Max.x - pt(12.0f), cy2 + previewHeight * 0.5f));
        const GLuint texture = m_previews.brushPreview(i, static_cast<int>(std::lround(previewWidth * pixels)),
                                                       static_cast<int>(std::lround(previewHeight * pixels)));
        if (texture != 0) {
            dl->AddImage(ImTextureRef(textureId(texture)), preview.Min, preview.Max, ImVec2(0.0f, 0.0f),
                         ImVec2(1.0f, 1.0f), IM_COL32_WHITE);
        }
        if (press.clicked) {
            preset.type = i;
        }
        y += pt(kRow + kRowGap);
    }
    endPanel(f);
}

// -----------------------------------------------------------------------------
// Capas
// -----------------------------------------------------------------------------

void Ui::layersPanel(Canvas& canvas) {
    const Layout& L = m_layout;
    LayerStack& layers = canvas.layers();
    const int count = layers.count();

    // Miniatura con la proporción del lienzo, en una columna de 64 pt.
    const float aspect = static_cast<float>(std::max(canvas.width(), 1)) / static_cast<float>(std::max(canvas.height(), 1));
    const float thumbWidth = std::min(64.0f, 48.0f * aspect);
    const float thumbHeight = std::max(thumbWidth / aspect, 12.0f);
    const float rowHeight = std::max(thumbHeight, 36.0f) + 12.0f;
    constexpr float kRowGap = 4.0f;
    constexpr float kListPad = 8.0f;
    constexpr float kFooter = 10.0f + 34.0f + 12.0f;   // opacidad con su margen
    const float listHeight = rowHeight * static_cast<float>(count) + kRowGap * static_cast<float>(count - 1);
    const float content = th::kHeaderHeight + kListPad + listHeight + kListPad + kFooter;
    const float width = pt(340.0f);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * 2.5f);
    PanelFrame f;
    if (!beginPanel(f, Panel::Layers, "##panel-layers", L.rightBar.Max.x - width, width, pt(content), anchorX,
                    false)) {
        return;
    }
    m_layersRect = f.rect;
    ImDrawList* dl = f.dl;
    float y = f.content.Min.y;

    // Cabecera: cuántas hay y añadir.
    const float cy = panelHeader(dl, f.content, y, "Capas");
    {
        const ImRect add(ImVec2(f.content.Max.x - pt(10.0f + 32.0f), cy - pt(16.0f)),
                         ImVec2(f.content.Max.x - pt(10.0f), cy + pt(16.0f)));
        const ImGuiID id = ImGui::GetID("##add");
        const bool full = count >= canvas.maxLayers();
        const Press press = ui::pressable(id, add);
        const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
        dl->AddRectFilled(add.Min, add.Max, ui::withAlpha(th::kAccent, 0.18f + 0.14f * t), pt(9.0f));
        ui::icon(dl, icon::kPlus, add.GetCenter(), 18.0f, full ? ui::withAlpha(th::kAccentText, 0.5f) : th::kAccentText);
        char amount[32];
        std::snprintf(amount, sizeof(amount), "%d de %d", count, canvas.maxLayers());
        ui::label(dl, Weight::Regular, th::kCaption, ImVec2(add.Min.x - pt(10.0f), cy), Align::Right,
                  IM_COL32(235, 235, 245, 102), amount);
        if (press.clicked) {
            if (full) {
                notify("Límite de " + std::to_string(canvas.maxLayers()) + " capas alcanzado", Notice::Warning);
            } else if (canvas.addLayer()) {
                m_scrollToLayer = static_cast<int>(layers.active().id);
            } else {
                notify("No hay memoria para otra capa", Notice::Error);
            }
            m_layerMenu = false;
        }
    }
    y += pt(th::kHeaderHeight);

    // Lista, de la capa de arriba a la de abajo.
    const float listBottom = f.content.Max.y - pt(kFooter);
    const ImRect view(ImVec2(f.content.Min.x, y), ImVec2(f.content.Max.x, listBottom));
    const float left = f.content.Min.x + pt(8.0f);
    const float right = f.content.Max.x - pt(8.0f);
    const int rows = layers.count();
    const float rowsHeight = pt(kListPad * 2.0f + rowHeight * static_cast<float>(rows) +
                                kRowGap * static_cast<float>(rows - 1));
    if (m_scrollToLayer >= 0) {
        const int index = layers.indexOf(static_cast<uint32_t>(m_scrollToLayer));
        if (index >= 0) {
            const float top = pt(kListPad + (rowHeight + kRowGap) * static_cast<float>(rows - 1 - index));
            ui::scrollIntoView("##list", top - pt(kListPad), top + pt(rowHeight + kListPad), view.GetHeight());
        }
        m_scrollToLayer = -1;
    }
    const float scroll = ui::beginScroll("##list", view, rowsHeight);
    const float pixels = m_status.pixelsPerUnit;
    const int thumbPixelsW = static_cast<int>(std::lround(pt(thumbWidth) * pixels));
    const int thumbPixelsH = static_cast<int>(std::lround(pt(thumbHeight) * pixels));
    const int checker = std::max(1, static_cast<int>(std::lround(pt(4.0f) * pixels)));
    m_activeRow = ImRect();
    for (int visual = 0; visual < rows; ++visual) {
        const int index = rows - 1 - visual;
        Layer& layer = layers.at(index);
        const float top = y + pt(kListPad) - scroll + pt((rowHeight + kRowGap) * static_cast<float>(visual));
        const ImRect row(ImVec2(left, top), ImVec2(right, top + pt(rowHeight)));
        const bool active = index == layers.activeIndex();
        if (active) {
            m_activeRow = row;
        }
        if (row.Max.y < view.Min.y || row.Min.y > view.Max.y) {
            continue;
        }
        ImGui::PushID(static_cast<int>(layer.id));
        const float rowCy = row.GetCenter().y;
        // Primero los botones de dentro de la fila: se quedan el toque antes que la fila.
        const ImRect visibility(ImVec2(row.Max.x - pt(4.0f + 40.0f), rowCy - pt(20.0f)),
                                ImVec2(row.Max.x - pt(4.0f), rowCy + pt(20.0f)));
        const Press visibilityPress = ui::pressable("##visible", visibility);
        ImRect more;
        Press morePress;
        if (active) {
            more = ImRect(ImVec2(visibility.Min.x - pt(2.0f + 32.0f), rowCy - pt(16.0f)),
                          ImVec2(visibility.Min.x - pt(2.0f), rowCy + pt(16.0f)));
            morePress = ui::pressable("##more", more);
        }
        const ImGuiID rowId = ImGui::GetID("##row");
        const Press rowPress = ui::pressable(rowId, row);

        const float on = ui::anim::follow(rowId + 7u, active ? 1.0f : 0.0f, 20.0f);
        if (on > 0.002f) {
            dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(th::kAccent, on), pt(th::kRowRadius));
        }
        pressFeedback(dl, rowId, row, rowPress, pt(th::kRowRadius));

        const float dim = layer.visible ? 1.0f : 0.5f;
        // Miniatura real de la capa sobre un damero.
        const ImVec2 thumbMin(row.Min.x + pt(6.0f) + pt(64.0f - thumbWidth) * 0.5f, rowCy - pt(thumbHeight) * 0.5f);
        const ImVec2 thumbMax(thumbMin.x + pt(thumbWidth), thumbMin.y + pt(thumbHeight));
        const GLuint texture = m_previews.layerThumbnail(layer, thumbPixelsW, thumbPixelsH, checker);
        const float thumbRadius = pt(6.0f);
        if (texture != 0) {
            dl->AddImageRounded(ImTextureRef(textureId(texture)), thumbMin, thumbMax, ImVec2(0.0f, 0.0f),
                                ImVec2(1.0f, 1.0f), ui::withAlpha(IM_COL32_WHITE, dim), thumbRadius);
        } else {
            dl->AddRectFilled(thumbMin, thumbMax, ui::withAlpha(IM_COL32_WHITE, dim), thumbRadius);
        }
        dl->AddRect(thumbMin, thumbMax, IM_COL32(255, 255, 255, 36), thumbRadius, 0, ui::hairline());

        // Nombre y opacidad (u "Oculta").
        const float textX = row.Min.x + pt(6.0f + 64.0f + 12.0f);
        const float textRight = (active ? more.Min.x : visibility.Min.x) - pt(6.0f);
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(textX, rowCy - pt(8.0f)), Align::Left,
                  ui::withAlpha(th::kLabel, dim), layer.name.c_str(), textRight - textX);
        char detail[32];
        if (layer.visible) {
            std::snprintf(detail, sizeof(detail), "%d %%", static_cast<int>(std::lround(layer.opacity * 100.0f)));
        } else {
            std::snprintf(detail, sizeof(detail), "Oculta");
        }
        ui::label(dl, Weight::Regular, th::kCaption, ImVec2(textX, rowCy + pt(10.0f)), Align::Left,
                  ui::mix(IM_COL32(235, 235, 245, 153), IM_COL32(255, 255, 255, 204), on), detail);

        // Opciones de la capa activa.
        if (active) {
            const float t = ui::anim::follow(ImGui::GetID("##more") + 5u, morePress.held ? 1.0f : 0.0f, 24.0f);
            dl->AddRectFilled(more.Min, more.Max, IM_COL32(255, 255, 255, static_cast<int>(51 + 30 * t)), pt(9.0f));
            ui::icon(dl, icon::kEllipsis, more.GetCenter(), 18.0f, th::kLabel);
        }

        // Casilla de visibilidad.
        const ImVec2 boxCenter = visibility.GetCenter();
        const ImRect box(ImVec2(boxCenter.x - pt(11.0f), boxCenter.y - pt(11.0f)),
                         ImVec2(boxCenter.x + pt(11.0f), boxCenter.y + pt(11.0f)));
        const float shown = ui::anim::follow(ImGui::GetID("##visible") + 5u, layer.visible ? 1.0f : 0.0f, 22.0f);
        const ImU32 fill = active ? IM_COL32_WHITE : th::kAccent;
        const float boxRadius = pt(6.0f);
        if (shown < 0.999f) {
            ui::outline(dl, box, boxRadius,
                        ui::withAlpha(active ? IM_COL32(255, 255, 255, 204) : IM_COL32(255, 255, 255, 102), 1.0f - shown),
                        pt(1.5f));
        }
        if (shown > 0.001f) {
            dl->AddRectFilled(box.Min, box.Max, ui::withAlpha(fill, shown), boxRadius);
            ui::icon(dl, icon::kCheck, boxCenter, 14.0f, ui::withAlpha(active ? th::kAccent : IM_COL32_WHITE, shown));
        }
        if (visibilityPress.held) {
            dl->AddRectFilled(box.Min, box.Max, IM_COL32(255, 255, 255, 40), boxRadius);
        }

        if (visibilityPress.clicked) {
            canvas.setLayerVisible(index, !layer.visible);
        } else if (active && morePress.clicked) {
            m_layerMenu = !m_layerMenu;
        } else if (rowPress.clicked) {
            if (active) {
                m_layerMenu = !m_layerMenu;   // tocar la capa activa abre sus opciones
            } else {
                canvas.selectLayer(index);
                m_layerMenu = false;
            }
        }
        ImGui::PopID();
    }
    ui::endScroll();

    // Opacidad de la capa activa: barra de relleno.
    ui::separator(dl, f.content.Min.x, f.content.Max.x, listBottom, th::kRule);
    const int activeIndex = layers.activeIndex();
    const ImRect bar(ImVec2(f.content.Min.x + pt(12.0f), listBottom + pt(10.0f)),
                     ImVec2(f.content.Max.x - pt(12.0f), listBottom + pt(10.0f + 34.0f)));
    float opacity = layers.active().opacity;
    bool dragging = false;
    if (ui::barSlider("##opacity", bar, &opacity, "Opacidad", &dragging)) {
        canvas.setLayerOpacity(activeIndex, opacity, false);
    }
    if (m_layerOpacityDragging && !dragging) {
        canvas.setLayerOpacity(activeIndex, layers.active().opacity, true);
    }
    m_layerOpacityDragging = dragging;
    endPanel(f);
}

void Ui::layerMenu(Canvas& canvas) {
    const bool open = m_layerMenu && m_panel == Panel::Layers;
    const ImGuiID menuId = ImHashStr("##layer-menu");
    const float p = ui::anim::followFrom(menuId, 0.0f, open ? 1.0f : 0.0f, open ? 20.0f : 26.0f);
    if (!open && p <= 0.002f) {
        return;
    }
    const Layout& L = m_layout;
    LayerStack& layers = canvas.layers();
    const int index = layers.activeIndex();
    const float width = pt(284.0f);
    const float pad = pt(8.0f);
    const float divider = pt(6.0f + 1.0f + 6.0f);
    // Fichas y filas algo más bajas si no cabe bajo las barras (un teléfono en horizontal).
    float tile = pt(60.0f);
    float item = pt(44.0f);
    const float available = L.bottom - L.popoverTop - pad * 2.0f - divider;
    if (tile + item * 3.0f > available) {
        const float k = std::max(available / (tile + item * 3.0f), 0.75f);
        tile *= k;
        item *= k;
    }
    const float height = pad * 2.0f + divider + tile + item * 3.0f;
    float x = 0.0f;
    float y = 0.0f;
    ImVec2 anchor;
    if (L.narrow) {
        x = L.right - width;
        y = std::max(L.leftBar.Max.y + pt(8.0f), m_layersRect.Min.y - height - pt(8.0f));
        anchor = ImVec2(x + width, y + height);
    } else {
        x = std::max(L.left, m_layersRect.Min.x - pt(12.0f) - width);
        const float rowTop = m_activeRow.GetWidth() > 0.0f ? m_activeRow.Min.y - pad : m_layersRect.Min.y;
        // Si ni así cabe bajo las barras, sube sobre ellas: va por encima de todo.
        y = std::clamp(rowTop, L.popoverTop, std::max(L.popoverTop, L.bottom - height));
        y = std::max(L.top, std::min(y, L.bottom - height));
        anchor = ImVec2(x + width, m_activeRow.GetWidth() > 0.0f ? m_activeRow.GetCenter().y : y);
    }
    const ImRect rect(x, y, x + width, y + height);
    const float radius = pt(th::kPanelRadius);

    ui::beginSurface("##layer-menu", rect, open, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(44.0f), pt(16.0f), 0.3f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, IM_COL32(24, 24, 28, 224));

    // Acciones: 0 renombrar, 1 duplicar, 2 combinar, 3 subir, 4 bajar, 5 limpiar, 6 eliminar.
    struct Item {
        int action;
        const char* label;
        const char* glyph;
        bool enabled;
    };
    const Item tiles[4] = {
        {0, "Renombrar", icon::kPencil, true},
        {1, "Duplicar", icon::kCopy, layers.count() < canvas.maxLayers()},
        {3, "Subir", icon::kArrowUp, index < layers.count() - 1},
        {4, "Bajar", icon::kArrowDown, index > 0},
    };
    const Item rows[3] = {
        {2, "Combinar con la de abajo", icon::kMerge, canvas.canMergeDown(index)},
        {5, "Limpiar capa", icon::kBrushCleaning, true},
        {6, "Eliminar capa", icon::kTrash, layers.count() > 1},
    };
    int chosen = -1;

    // Arriba, las cuatro más usadas como fichas con icono.
    const float gap = pt(6.0f);
    const float tileWidth = (width - pad * 2.0f - gap * 3.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const float x0 = rect.Min.x + pad + (tileWidth + gap) * static_cast<float>(i);
        const ImRect r(ImVec2(x0, rect.Min.y + pad), ImVec2(x0 + tileWidth, rect.Min.y + pad + tile));
        ImGui::PushID(i);
        const ImGuiID id = ImGui::GetID("##tile");
        const Press press = ui::pressable(id, r, tiles[i].enabled);
        ImGui::PopID();
        dl->AddRectFilled(r.Min, r.Max, th::kControl, pt(th::kControlRadius));
        pressFeedback(dl, id, r, press, pt(th::kControlRadius));
        const ImU32 color = tiles[i].enabled ? th::kLabel : ui::withAlpha(th::kLabel, 0.3f);
        const float middle = r.GetCenter().y;
        ui::icon(dl, tiles[i].glyph, ImVec2(r.GetCenter().x, middle - tile * 0.14f), 18.0f, color);
        ui::label(dl, Weight::Regular, th::kMicro, ImVec2(r.GetCenter().x, middle + tile * 0.22f), Align::Center, color,
                  tiles[i].label, tileWidth - pt(4.0f));
        if (press.clicked) {
            chosen = tiles[i].action;
        }
    }
    float top = rect.Min.y + pad + tile + pt(6.0f);
    ui::separator(dl, rect.Min.x + pad + pt(4.0f), rect.Max.x - pad - pt(4.0f), top, th::kRule);
    top += pt(1.0f + 6.0f);

    // Debajo, el resto en filas; eliminar, en rojo.
    for (int i = 0; i < 3; ++i) {
        const ImRect r(ImVec2(rect.Min.x + pad, top), ImVec2(rect.Max.x - pad, top + item));
        ImGui::PushID(10 + i);
        if (menuRow(dl, "##row", r, rows[i].glyph, rows[i].label, nullptr, false, rows[i].enabled,
                    rows[i].action == 6 ? th::kRed : th::kLabel)) {
            chosen = rows[i].action;
        }
        ImGui::PopID();
        top += item;
    }
    ui::transform(mark, anchor, 0.92f + 0.08f * p, ImVec2(0.0f, 0.0f), std::min(1.0f, p * 1.3f));
    ui::endSurface();

    switch (chosen) {
    case 0:
        openDialog(Dialog::RenameLayer, &canvas);
        break;
    case 1:
        if (!canvas.duplicateLayer(index)) {
            notify("No hay memoria para otra capa", Notice::Error);
        } else {
            m_scrollToLayer = static_cast<int>(layers.active().id);
        }
        break;
    case 2:
        canvas.mergeDown(index);
        break;
    case 3:
        canvas.moveLayer(index, index + 1);
        break;
    case 4:
        canvas.moveLayer(index, index - 1);
        break;
    case 5:
        canvas.clearLayer(index);
        break;
    case 6:
        openDialog(Dialog::DeleteLayer, &canvas);
        break;
    default:
        break;
    }
    // Subir y bajar dejan el menú abierto para mover varias posiciones seguidas.
    if (chosen >= 0 && chosen != 3 && chosen != 4) {
        m_layerMenu = false;
    }
}

// -----------------------------------------------------------------------------
// Color
// -----------------------------------------------------------------------------

void Ui::syncHsv(const float rgb[3]) {
    if (sameColor(rgb, m_hsvSource)) {
        return;
    }
    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    ImGui::ColorConvertRGBtoHSV(rgb[0], rgb[1], rgb[2], h, s, v);
    // En los grises el tono no existe y en el negro tampoco la saturación: se conservan
    // los que había para que la rueda no salte.
    if (s <= 0.0001f || v <= 0.0001f) {
        h = m_hsv[0];
    }
    if (v <= 0.0001f) {
        s = m_hsv[1];
    }
    m_hsv[0] = h;
    m_hsv[1] = s;
    m_hsv[2] = v;
    std::copy(rgb, rgb + 3, m_hsvSource);
}

void Ui::applyHsv(Canvas& canvas) {
    float rgb[3];
    ImGui::ColorConvertHSVtoRGB(m_hsv[0], m_hsv[1], m_hsv[2], rgb[0], rgb[1], rgb[2]);
    setColor(canvas, rgb);
    std::copy(rgb, rgb + 3, m_hsvSource);
}

void Ui::colorPanel(Canvas& canvas) {
    const Layout& L = m_layout;
    constexpr float kPadTop = 14.0f;
    constexpr float kWheel = 236.0f;
    constexpr float kHex = 32.0f;
    constexpr float kSection = 14.0f + 10.0f;   // rótulo y hueco
    constexpr float kSwatch = 32.0f;
    constexpr float kSwatchGap = 8.0f;
    constexpr float kSwatches = kHex + 14.0f + kSection + kSwatch + 14.0f + kSection + kSwatch * 2.0f + kSwatchGap;
    constexpr float kSwatchColumn = 244.0f;
    constexpr float kColumnGap = 24.0f;
    constexpr float kPadX = 16.0f;
    constexpr float kPadBottom = 16.0f;
    // Si no cabe de alto (un teléfono en horizontal), la rueda va a la izquierda y el
    // resto a su derecha, y la rueda encoge un poco si hace falta.
    const float available = (L.bottom - L.popoverTop) / ui::scale();
    const float tall = th::kHeaderHeight + kPadTop + kWheel + 14.0f + kSwatches + kPadBottom;
    const bool wide = !L.narrow && tall > available &&
                      L.right - L.left >= pt(kPadX * 2.0f + kWheel + kColumnGap + kSwatchColumn);
    const float wheel =
        wide ? std::clamp(available - th::kHeaderHeight - kPadTop - kPadBottom, 180.0f, kWheel) : kWheel;
    const float content = wide ? th::kHeaderHeight + kPadTop + std::max(wheel, kSwatches) + kPadBottom : tall;
    const float width = pt(wide ? kPadX * 2.0f + wheel + kColumnGap + kSwatchColumn : 300.0f);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * 3.5f);
    PanelFrame f;
    if (!beginPanel(f, Panel::Color, "##panel-color", L.rightBar.Max.x - width, width, pt(content), anchorX, true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    const ImGuiIO& io = ImGui::GetIO();
    float* color = canvas.brushSettings().color;
    syncHsv(color);
    float y = f.content.Min.y - f.scroll;

    // Cabecera con el color actual | el anterior (tocar el anterior lo recupera).
    const float cy = panelHeader(dl, f.content, y, "Color");
    {
        const ImRect pill(ImVec2(f.content.Max.x - pt(12.0f + 56.0f), cy - pt(13.0f)),
                          ImVec2(f.content.Max.x - pt(12.0f), cy + pt(13.0f)));
        const float mid = pill.GetCenter().x;
        const float radius = pt(8.0f);
        dl->AddRectFilled(pill.Min, ImVec2(mid, pill.Max.y), ui::fromFloat(color), radius, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(mid, pill.Min.y), pill.Max, ui::fromFloat(m_previousColor), radius,
                          ImDrawFlags_RoundCornersRight);
        ui::outline(dl, pill, radius, IM_COL32(255, 255, 255, 46), ui::hairline());
        const Press previous = ui::pressable("##previous", ImRect(ImVec2(mid, pill.Min.y), pill.Max));
        if (previous.clicked) {
            setColor(canvas, m_previousColor);
            syncHsv(color);
        }
    }
    y += pt(th::kHeaderHeight + kPadTop);
    float left = f.content.Min.x + pt(kPadX);
    const float right = f.content.Max.x - pt(kPadX);

    // Rueda de tono con el cuadro de saturación (horizontal) y brillo (vertical).
    const ImVec2 center(wide ? left + pt(wheel * 0.5f) : (left + right) * 0.5f, y + pt(wheel * 0.5f));
    const float outer = pt(wheel * 0.5f);
    const float inner = outer * (93.0f / 118.0f);
    const float half = outer * (62.0f / 118.0f);
    const ImRect square(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half));
    bool hovered = false;
    bool held = false;
    const ImGuiID squareId = ImGui::GetID("##sv");
    ImGui::ItemAdd(square, squareId);
    ImGui::ButtonBehavior(square, squareId, &hovered, &held, ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus);
    if (held) {
        m_hsv[1] = std::clamp((io.MousePos.x - square.Min.x) / square.GetWidth(), 0.0f, 1.0f);
        m_hsv[2] = 1.0f - std::clamp((io.MousePos.y - square.Min.y) / square.GetHeight(), 0.0f, 1.0f);
        applyHsv(canvas);
    }
    const ImGuiID ringId = ImGui::GetID("##hue");
    const ImRect ringBox(ImVec2(center.x - outer, center.y - outer), ImVec2(center.x + outer, center.y + outer));
    ImGui::ItemAdd(ringBox, ringId);
    const float dx = io.MousePos.x - center.x;
    const float dy = io.MousePos.y - center.y;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const bool onRing = distance >= inner - pt(8.0f) && distance <= outer + pt(8.0f);
    bool ringHeld = false;
    if (ImGui::GetActiveID() == ringId || (onRing && !square.Contains(io.MousePos))) {
        ImGui::ButtonBehavior(ringBox, ringId, &hovered, &ringHeld,
                              ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus);
    }
    if (ringHeld && distance > 1.0f) {
        float hue = std::atan2(dx, -dy) / (2.0f * IM_PI);
        if (hue < 0.0f) {
            hue += 1.0f;
        }
        m_hsv[0] = hue;
        applyHsv(canvas);
    }

    ui::hueRing(dl, center, inner, outer);
    float hueRgb[3];
    ImGui::ColorConvertHSVtoRGB(m_hsv[0], 1.0f, 1.0f, hueRgb[0], hueRgb[1], hueRgb[2]);
    const ImU32 hueColor = ui::fromFloat(hueRgb);
    const float squareRadius = pt(10.0f);
    ui::linearGradient(dl, square, squareRadius, ImVec2(square.Min.x, center.y), ImVec2(square.Max.x, center.y),
                       IM_COL32_WHITE, hueColor);
    ui::linearGradient(dl, square, squareRadius, ImVec2(center.x, square.Min.y), ImVec2(center.x, square.Max.y),
                       IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 255));
    // Tiradores.
    auto knob = [&](ImVec2 at, float radius, ImU32 fill) {
        const ImRect bounds(ImVec2(at.x - radius, at.y - radius), ImVec2(at.x + radius, at.y + radius));
        ui::shadow(dl, bounds, radius, pt(6.0f), pt(1.5f), 0.4f);
        dl->AddCircleFilled(at, radius, IM_COL32_WHITE, 0);
        dl->AddCircleFilled(at, radius - pt(3.0f), fill, 0);
    };
    const float angle = m_hsv[0] * 2.0f * IM_PI;
    const float middle = (inner + outer) * 0.5f;
    knob(ImVec2(center.x + std::sin(angle) * middle, center.y - std::cos(angle) * middle), pt(13.0f), hueColor);
    knob(ImVec2(square.Min.x + m_hsv[1] * square.GetWidth(), square.Min.y + (1.0f - m_hsv[2]) * square.GetHeight()),
         pt(11.0f), ui::fromFloat(color));
    if (wide) {
        left += pt(wheel + kColumnGap);
    } else {
        y += pt(wheel + 14.0f);
    }

    // Hexadecimal (se puede escribir).
    {
        const ImRect row(ImVec2(left, y), ImVec2(right, y + pt(kHex)));
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(std::lround(color[0] * 255.0f)),
                      static_cast<int>(std::lround(color[1] * 255.0f)), static_cast<int>(std::lround(color[2] * 255.0f)));
        const float fieldWidth = pt(112.0f);
        const ImRect field(ImVec2(right - fieldWidth, row.Min.y), row.Max);
        ui::sectionLabel(dl, left, field.Min.x - pt(10.0f), row.GetCenter().y, "HEX");
        if (m_hexEditing) {
            const bool enter = ui::textField("##hex-field", field, m_hexBuffer, sizeof(m_hexBuffer),
                                             m_hexFocusFrames == 2, "#RRGGBB");
            const bool active = ImGui::IsItemActive();
            if (m_hexFocusFrames > 0) {
                --m_hexFocusFrames;
            }
            if (enter || (!active && m_hexFocusFrames == 0)) {
                float rgb[3];
                if (parseHex(m_hexBuffer, rgb)) {
                    setColor(canvas, rgb);
                    syncHsv(color);
                } else if (enter) {
                    notify("Escribe el color como #RRGGBB", Notice::Warning, 2500);
                }
                m_hexEditing = false;
            }
        } else {
            const ImGuiID id = ImGui::GetID("##hex");
            const Press press = ui::pressable(id, field);
            const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
            const float radius = pt(8.0f);
            dl->AddRectFilled(field.Min, field.Max, ui::mix(IM_COL32(0, 0, 0, 64), IM_COL32(255, 255, 255, 26), t),
                              radius);
            ui::outline(dl, field, radius, IM_COL32(255, 255, 255, 31), pt(1.0f));
            ui::label(dl, Weight::SemiBold, th::kCallout, field.GetCenter(), Align::Center, th::kLabel, hex);
            if (press.clicked) {
                m_hexEditing = true;
                m_hexFocusFrames = 2;
                std::snprintf(m_hexBuffer, sizeof(m_hexBuffer), "%s", hex);
            }
        }
    }
    y += pt(kHex + 14.0f);

    // Recientes y paleta: muestras cuadradas en seis columnas.
    const float gap = pt(kSwatchGap);
    const float cell = (right - left - gap * 5.0f) / 6.0f;
    auto cellRect = [&](int column, float top) {
        const float x0 = left + (cell + gap) * static_cast<float>(column);
        return ImRect(ImVec2(x0, top), ImVec2(x0 + cell, top + pt(kSwatch)));
    };
    ui::sectionLabel(dl, left, right, y + pt(7.0f), "RECIENTES");
    y += pt(kSection);
    if (m_recentCount == 0) {
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left, y + pt(kSwatch * 0.5f)), Align::Left,
                  th::kTertiaryLabel, "Aparecerán los colores que uses", right - left);
    }
    for (int i = 0; i < m_recentCount; ++i) {
        ImGui::PushID(i);
        if (ui::swatch("##recent", cellRect(i, y), ui::fromFloat(m_recent[i]), sameColor(m_recent[i], color))) {
            setColor(canvas, m_recent[i]);
            syncHsv(color);
        }
        ImGui::PopID();
    }
    y += pt(kSwatch + 14.0f);
    ui::sectionLabel(dl, left, right, y + pt(7.0f), "PALETA");
    y += pt(kSection);
    for (int i = 0; i < 12; ++i) {
        float rgb[3];
        toFloat(kPalette[i], rgb);
        ImGui::PushID(100 + i);
        if (ui::swatch("##palette", cellRect(i % 6, y + (pt(kSwatch) + gap) * static_cast<float>(i / 6)), kPalette[i],
                       sameColor(rgb, color))) {
            setColor(canvas, rgb);
            syncHsv(color);
        }
        ImGui::PopID();
    }
    endPanel(f);
}
