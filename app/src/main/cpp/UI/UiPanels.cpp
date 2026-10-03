// Paneles que se abren desde las barras: acciones, NDI, pinceles, capas (con el menú de
// la capa activa) y color. En una tableta cuelgan bajo su botón; en un teléfono en
// vertical son hojas que suben desde abajo. Comparten el aspecto: cabecera con el título
// y una línea, filas con el icono atenuado y rótulos de sección pequeños.
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"
#include "UI/PanelParts.h"

#include <SDL3/SDL_platform_defines.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;
using ui::parts::panelHeader;
using ui::parts::pressFeedback;
using ui::parts::tag;
using ui::parts::textureId;
using ui::parts::toggleRow;

namespace {

constexpr ImU32 kPalette[12] = {
    IM_COL32(255, 255, 255, 255), IM_COL32(142, 142, 147, 255), IM_COL32(28, 28, 30, 255),
    IM_COL32(255, 69, 58, 255),   IM_COL32(255, 159, 10, 255),  IM_COL32(255, 214, 10, 255),
    IM_COL32(48, 209, 88, 255),   IM_COL32(100, 210, 255, 255), IM_COL32(10, 132, 255, 255),
    IM_COL32(94, 92, 230, 255),   IM_COL32(191, 90, 242, 255),  IM_COL32(255, 55, 95, 255),
};

// Modos de fusión, en el orden de BlendMode: nombre en la lista y nombre corto (el pie
// del panel y las filas de las capas).
struct BlendName {
    const char* name;
    const char* shortName;
};
constexpr BlendName kBlendNames[kBlendModeCount] = {
    {"Normal", "Normal"},
    {"Multiplicar", "Multiplicar"},
    {"Oscurecer", "Oscurecer"},
    {"Subexposición de color", "Subexp. color"},
    {"Subexposición lineal", "Subexp. lineal"},
    {"Color más oscuro", "Más oscuro"},
    {"Aclarar", "Aclarar"},
    {"Trama", "Trama"},
    {"Sobreexposición de color", "Sobreexp. color"},
    {"Añadir", "Añadir"},
    {"Color más claro", "Más claro"},
    {"Superponer", "Superponer"},
    {"Luz suave", "Luz suave"},
    {"Luz fuerte", "Luz fuerte"},
    {"Luz intensa", "Luz intensa"},
    {"Luz lineal", "Luz lineal"},
    {"Luz focal", "Luz focal"},
    {"Mezcla definida", "Mezcla definida"},
    {"Diferencia", "Diferencia"},
    {"Exclusión", "Exclusión"},
    {"Restar", "Restar"},
    {"Dividir", "Dividir"},
    {"Tono", "Tono"},
    {"Saturación", "Saturación"},
    {"Color", "Color"},
    {"Luminosidad", "Luminosidad"},
};

// Rótulo del grupo de modos que empieza en `mode` (null si no empieza ninguno).
const char* blendGroup(BlendMode mode) {
    switch (mode) {
    case BlendMode::Multiply:
        return "OSCURECEN";
    case BlendMode::Lighten:
        return "ACLARAN";
    case BlendMode::Overlay:
        return "CONTRASTE";
    case BlendMode::Difference:
        return "DIFERENCIA";
    case BlendMode::Hue:
        return "COLOR Y LUZ";
    default:
        return nullptr;
    }
}

// Medidas de la lista de modos (pt).
constexpr float kBlendRow = 38.0f;
constexpr float kBlendSection = 30.0f;
constexpr float kBlendPad = 6.0f;

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
    {"GIRA", "Girar la vista", "Gira dos dedos sobre el lienzo"},
    {"MANTÉN", "Cuentagotas con el dedo", "Deja el dedo quieto al empezar"},
    {"ESPERA", "Forma rápida", "Quédate quieto al acabar un trazo"},
};
constexpr HelpItem kShortcuts[] = {
    {"B", "Pincel", nullptr},
    {"D", "Difuminar", nullptr},
    {"E", "Borrador", nullptr},
    {"S", "Selección", nullptr},
    {"V", "Transformar", nullptr},
    {"[  ]", "Tamaño del pincel", nullptr},
    {"4  6", "Girar la vista", nullptr},
    {"5", "Enderezar la vista", nullptr},
    {"M", "Voltear la vista", nullptr},
    {"MAYÚS", "Forma rápida perfecta", nullptr},
    {"CTRL Z", "Deshacer", nullptr},
    {"CTRL Y", "Rehacer", nullptr},
    {"CTRL C / X", "Copiar o cortar", nullptr},
    {"CTRL V", "Pegar en una capa nueva", nullptr},
    {"CTRL J", "Duplicar lo seleccionado", nullptr},
    {"CTRL D", "Quitar la selección", nullptr},
    {"INTRO", "Cerrar el lazo o aplicar", nullptr},
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

// Fila de menú: icono atenuado, título y, a la derecha, un valor. `filled`: con fondo
// suave (la acción principal de su grupo). Sin `id` es solo información. `chevron`: abre
// una página del panel (lleva una flecha a la derecha).
bool menuRow(ImDrawList* dl, const char* id, const ImRect& row, const char* glyph, const char* title,
             const char* value = nullptr, bool filled = false, bool enabled = true, ImU32 color = th::kLabel,
             bool chevron = false) {
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
    if (chevron) {
        ui::icon(dl, icon::kChevronRight, ImVec2(right - pt(8.0f), cy), 16.0f, IM_COL32(235, 235, 245, 102));
        right -= pt(16.0f + 8.0f);
    }
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

// Medidas del selector de color (pt).
constexpr float kPickerPadTop = 14.0f;
constexpr float kPickerWheel = 236.0f;
constexpr float kPickerHex = 32.0f;
constexpr float kPickerSection = 14.0f + 10.0f;   // rótulo y hueco
constexpr float kPickerSwatch = 32.0f;
constexpr float kPickerSwatchGap = 8.0f;
// Lo que va debajo de la rueda (o a su derecha): hexadecimal, recientes y paleta.
constexpr float kPickerSwatches = kPickerHex + 14.0f + kPickerSection + kPickerSwatch + 14.0f + kPickerSection +
                                  kPickerSwatch * 2.0f + kPickerSwatchGap;
constexpr float kPickerSwatchColumn = 244.0f;
constexpr float kPickerColumnGap = 24.0f;
constexpr float kPickerPadX = 16.0f;
constexpr float kPickerPadBottom = 16.0f;
// Alto del selector con la rueda encima de lo demás.
constexpr float kPickerTall = kPickerPadTop + kPickerWheel + 14.0f + kPickerSwatches + kPickerPadBottom;

// Color actual | el anterior en una píldora a la derecha de una cabecera. Devuelve si se
// tocó el anterior.
bool colorPill(ImDrawList* dl, float right, float cy, const float current[3], const float previous[3]) {
    const ImRect pill(ImVec2(right - pt(56.0f), cy - pt(13.0f)), ImVec2(right, cy + pt(13.0f)));
    const float mid = pill.GetCenter().x;
    const float radius = pt(8.0f);
    dl->AddRectFilled(pill.Min, ImVec2(mid, pill.Max.y), ui::fromFloat(current), radius, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(mid, pill.Min.y), pill.Max, ui::fromFloat(previous), radius, ImDrawFlags_RoundCornersRight);
    ui::outline(dl, pill, radius, IM_COL32(255, 255, 255, 46), ui::hairline());
    return ui::pressable("##previous", ImRect(ImVec2(mid, pill.Min.y), pill.Max)).clicked;
}


// Casilla de ver u ocultar de una fila de capa. `onAccent`: la fila está elegida (fondo de
// acento), así que la casilla va en blanco.
void visibilityBox(ImDrawList* dl, ImGuiID animId, const ImRect& area, bool checked, bool onAccent, const Press& press) {
    const ImVec2 center = area.GetCenter();
    const ImRect box(ImVec2(center.x - pt(11.0f), center.y - pt(11.0f)), ImVec2(center.x + pt(11.0f), center.y + pt(11.0f)));
    const float visible = ui::anim::follow(animId, checked ? 1.0f : 0.0f, 22.0f);
    const ImU32 fill = onAccent ? IM_COL32_WHITE : th::kAccent;
    const float radius = pt(6.0f);
    if (visible < 0.999f) {
        ui::outline(dl, box, radius,
                    ui::withAlpha(onAccent ? IM_COL32(255, 255, 255, 204) : IM_COL32(255, 255, 255, 102), 1.0f - visible),
                    pt(1.5f));
    }
    if (visible > 0.001f) {
        dl->AddRectFilled(box.Min, box.Max, ui::withAlpha(fill, visible), radius);
        ui::icon(dl, icon::kCheck, center, 14.0f, ui::withAlpha(onAccent ? th::kAccent : IM_COL32_WHITE, visible));
    }
    if (press.held) {
        dl->AddRectFilled(box.Min, box.Max, IM_COL32(255, 255, 255, 40), radius);
    }
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
    featherPanel(canvas);
    modifyPanel(canvas);
    adjustPanel(canvas);
}

// -----------------------------------------------------------------------------
// Acciones
// -----------------------------------------------------------------------------

void Ui::actionsPanel(Canvas& canvas, UiRequests& requests) {
    const Layout& L = m_layout;
    const bool properties = m_actionsPage == ActionsPage::Properties;
    // Al cerrarse el panel, los ppp que se escribían en las propiedades se quedan.
    if (properties && m_panel != Panel::Actions) {
        stopPropertiesEdit(canvas, true);
    }
    const int tab = std::clamp(m_actionsTab, 0, 3);
    const float rowPoints = L.narrow ? 48.0f : 44.0f;
    const float width = pt(332.0f);
    const float panelWidth = L.narrow ? L.display.x - L.safe[1] - L.safe[3] : width;
    constexpr float kStrip = 8.0f + 54.0f + 8.0f;   // pestañas con su margen

#ifdef SDL_PLATFORM_EMSCRIPTEN
    const char* shareNote = "Se descarga con el navegador con el nombre del lienzo, a tamaño completo y con sus ppp "
                            "para imprimir. Si ocultas el color de fondo (en Capas), queda transparente.";
#elif defined(SDL_PLATFORM_ANDROID)
    const char* shareNote = "Se guarda en Descargas con el nombre del lienzo, a tamaño completo y con sus ppp para "
                            "imprimir. Si ocultas el color de fondo (en Capas), queda transparente.";
#else
    const char* shareNote = "Se guarda en la carpeta de descargas con el nombre del lienzo, a tamaño completo y con "
                            "sus ppp para imprimir. Si ocultas el color de fondo (en Capas), queda transparente.";
#endif
    const float noteWidth = panelWidth - pt(8.0f + 10.0f) * 2.0f;

    // Alto del contenido de la pestaña elegida (o de las propiedades del lienzo); al
    // cambiar, el panel se anima.
    float body = 0.0f;
    switch (properties ? -1 : tab) {
    case -1:
        body = propertiesHeight(canvas, panelWidth);
        break;
    case 0:
        body = pt(8.0f + rowPoints * 6.0f + 2.0f * 5.0f + 8.0f);
        break;
    case 1:
        body = pt(8.0f + rowPoints + 10.0f) +
               ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f), noteWidth, Align::Left, 0,
                             shareNote, 1.4f) +
               pt(12.0f);
        break;
    case 2:
        body = pt(8.0f + rowPoints * 4.0f + 2.0f * 3.0f + 34.0f + 58.0f + 6.0f) +
               penSettingsHeight(panelWidth - pt(16.0f)) + pt(6.0f);
        break;
    default:
        body = pt(kHelpHeight);
        break;
    }
    const float header = properties ? pt(th::kHeaderHeight) : pt(kStrip);
    const float content = ui::anim::follow(ImHashStr("##actions-height"), header + body, 22.0f, 0.5f);
    const float anchorX = L.leftBar.Min.x + pt(th::kBarPadding) + L.barButton * 0.5f;
    PanelFrame f;
    if (!beginPanel(f, Panel::Actions, "##panel-actions", L.leftBar.Min.x, width, content, anchorX, false)) {
        // Cerrado del todo: la próxima vez se abre en las pestañas.
        m_actionsPage = ActionsPage::Main;
        return;
    }
    ImDrawList* dl = f.dl;
    const float left = f.content.Min.x + pt(8.0f);
    const float right = f.content.Max.x - pt(8.0f);
    float y = f.content.Min.y;

    // Propiedades del lienzo: volver a las pestañas y la página.
    if (properties) {
        bool back = false;
        ui::parts::backHeader(dl, f.content, y, "##properties-back", "Propiedades del lienzo", &back);
        propertiesPage(canvas, dl, ImRect(ImVec2(f.content.Min.x, y + pt(th::kHeaderHeight)), f.content.Max), f.open);
        if (back) {
            stopPropertiesEdit(canvas, true);
            m_actionsPage = ActionsPage::Main;
        }
        endPanel(f);
        return;
    }

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
        {
            bool flipped = m_status.viewFlipped;
            if (toggleRow(dl, "##flip", ImRect(left, y, right, y + row), icon::kFlipView, "Voltear la vista",
                          &flipped)) {
                requests.flipView = true;
            }
        }
        y += row + pt(2.0f);
        {
            DrawingGuide guide = canvas.guide();
            if (toggleRow(dl, "##guide", ImRect(left, y, right, y + row), icon::kGrid, "Guía de dibujo",
                          &guide.enabled)) {
                canvas.setGuide(guide);
            }
        }
        y += row + pt(2.0f);
        if (menuRow(dl, "##edit-guide", ImRect(left, y, right, y + row), icon::kRuler, "Editar guía de dibujo…")) {
            startGuide(canvas);
        }
        y += row + pt(2.0f);
        if (menuRow(dl, "##properties", ImRect(left, y, right, y + row), icon::kInfo, "Propiedades", size, false, true,
                    th::kLabel, true)) {
            openProperties(canvas);
        }
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
        y += row + pt(2.0f);
        bool rotate = m_prefs.rotateWithFingers;
        if (toggleRow(dl, "##rotate", ImRect(left, y, right, y + row), icon::kRotateView, "Girar con dos dedos",
                      &rotate)) {
            m_prefs.rotateWithFingers = rotate;
            savePrefs();
        }
        y += row + pt(2.0f);
        bool shapes = m_prefs.quickShape;
        if (toggleRow(dl, "##shapes", ImRect(left, y, right, y + row), icon::kShapes, "Forma rápida", &shapes)) {
            m_prefs.quickShape = shapes;
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
        y += pt(58.0f + 6.0f);
        penSettings(dl, left, right, y);
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
    const char* note = available ? "Los receptores ven el lienzo completo, sin el zoom ni la interfaz. Si ocultas el "
                                   "color de fondo (en Capas), les llega con transparencia."
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
// Capas
// -----------------------------------------------------------------------------

void Ui::layersPanel(Canvas& canvas) {
    const Layout& L = m_layout;
    LayerStack& layers = canvas.layers();
    const int count = layers.count();

    // Los modos que se prueban en la lista de fusión son un solo paso de deshacer: se
    // guarda al salir de ella (volver, cerrar el panel o el botón atrás).
    if (m_panel != Panel::Layers) {
        m_blendPage = false;
        m_backgroundPage = false;
    }
    if (m_blendEditing && !m_blendPage) {
        canvas.finishLayerEdit();
        m_blendEditing = false;
    }
    // Lo mismo con los colores de fondo que se prueban en su página.
    if (m_backgroundEditing && !m_backgroundPage) {
        canvas.finishBackgroundEdit();
        m_backgroundEditing = false;
    }

    // Miniatura con la proporción del lienzo, en una columna de 64 pt.
    const float aspect = static_cast<float>(std::max(canvas.width(), 1)) / static_cast<float>(std::max(canvas.height(), 1));
    const float thumbWidth = std::min(64.0f, 48.0f * aspect);
    const float thumbHeight = std::max(thumbWidth / aspect, 12.0f);
    const float rowHeight = std::max(thumbHeight, 36.0f) + 12.0f;
    constexpr float kRowGap = 4.0f;
    constexpr float kListPad = 8.0f;
    constexpr float kFooter = 10.0f + 34.0f + 12.0f;   // opacidad y fusión con su margen
    // Las capas y, al final, el color de fondo.
    const float listHeight = rowHeight * static_cast<float>(count + 1) + kRowGap * static_cast<float>(count);
    const float listContent = th::kHeaderHeight + kListPad + listHeight + kListPad + kFooter;
    // La lista de modos pide más alto que unas pocas capas y el color de fondo, el del
    // selector (y se desplazan si no caben). El panel cambia de alto con una animación.
    float target = listContent;
    if (m_blendPage) {
        target = std::max(listContent, th::kHeaderHeight + 400.0f + kFooter);
    } else if (m_backgroundPage) {
        target = th::kHeaderHeight + kPickerTall + kFooter;
    }
    const float content = ui::anim::follow(ImHashStr("##layers-height"), target, 20.0f, 0.2f);
    const float width = pt(340.0f);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding) + L.barButton * 2.5f;
    PanelFrame f;
    if (!beginPanel(f, Panel::Layers, "##panel-layers", L.rightBar.Max.x - width, width, pt(content), anchorX,
                    false)) {
        return;
    }
    m_layersRect = f.rect;
    m_activeRow = ImRect();
    ImDrawList* dl = f.dl;
    float y = f.content.Min.y;

    if (m_blendPage) {
        // Cabecera de la lista de modos: volver, el título y la capa a la que se aplica.
        const float cy = y + pt(th::kHeaderHeight * 0.5f);
        const ImRect back(ImVec2(f.content.Min.x + pt(8.0f), cy - pt(16.0f)),
                          ImVec2(f.content.Min.x + pt(8.0f + 32.0f), cy + pt(16.0f)));
        const ImGuiID backId = ImGui::GetID("##blend-back");
        const Press backPress = ui::pressable(backId, back);
        pressFeedback(dl, backId, back, backPress, pt(9.0f));
        ui::icon(dl, icon::kChevronLeft, back.GetCenter(), 20.0f, th::kLabel);
        const float titleX = back.Max.x + pt(4.0f);
        ui::label(dl, Weight::SemiBold, th::kPanelTitle, ImVec2(titleX, cy), Align::Left, th::kLabel, "Fusión");
        const float nameLeft = titleX + ui::measure(Weight::SemiBold, th::kPanelTitle, "Fusión").x + pt(16.0f);
        const float nameRight = f.content.Max.x - pt(16.0f);
        if (nameRight > nameLeft) {
            ui::label(dl, Weight::Regular, th::kCaption, ImVec2(nameRight, cy), Align::Right,
                      IM_COL32(235, 235, 245, 102), layers.active().name.c_str(), nameRight - nameLeft);
        }
        ui::separator(dl, f.content.Min.x, f.content.Max.x, y + pt(th::kHeaderHeight) - ui::hairline(), th::kRule);
        if (backPress.clicked) {
            m_blendPage = false;
        }
    } else if (m_backgroundPage) {
        // Cabecera del color de fondo: volver y el color actual | el de antes de abrirla.
        bool back = false;
        const float cy = ui::parts::backHeader(dl, f.content, y, "##background-back", "Color de fondo", &back);
        if (colorPill(dl, f.content.Max.x - pt(12.0f), cy, canvas.background().color, m_previousBackground)) {
            setTargetColor(canvas, ColorTarget::Background, m_previousBackground);
        }
        if (back) {
            m_backgroundPage = false;
        }
    } else {
        // Cabecera: cuántas hay y añadir.
        const float cy = panelHeader(dl, f.content, y, "Capas");
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

    const float listBottom = f.content.Max.y - pt(kFooter);
    const ImRect view(ImVec2(f.content.Min.x, y), ImVec2(f.content.Max.x, listBottom));
    if (m_blendPage) {
        blendList(canvas, dl, view);
    } else if (m_backgroundPage) {
        const float body = pt(kPickerTall);
        const bool scrolls = body > view.GetHeight() + 0.5f;
        const float scroll = scrolls ? ui::beginScroll("##background-body", view, body) : 0.0f;
        colorPicker(canvas, dl, ColorTarget::Background, view.Min.x + pt(kPickerPadX), view.Max.x - pt(kPickerPadX),
                    view.Min.y - scroll, kPickerWheel, false);
        if (scrolls) {
            ui::endScroll();
        }
    } else {
        // Lista, de la capa de arriba a la de abajo, y el color de fondo.
        const float left = f.content.Min.x + pt(8.0f);
        const float right = f.content.Max.x - pt(8.0f);
        const int rows = layers.count();
        const float rowsHeight = pt(kListPad * 2.0f + rowHeight * static_cast<float>(rows + 1) +
                                    kRowGap * static_cast<float>(rows));
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

            // Una capa con máscara de recorte va sangrada, con una flecha hacia su base.
            const bool shown = canvas.layerShown(index);
            const float dim = shown ? 1.0f : 0.5f;
            const ImU32 detailColor = ui::mix(IM_COL32(235, 235, 245, 153), IM_COL32(255, 255, 255, 204), on);
            const float indent = layers.clipBase(index) >= 0 ? pt(18.0f) : 0.0f;
            // Miniatura real de la capa sobre un damero.
            const ImVec2 thumbMin(row.Min.x + pt(6.0f) + indent + pt(64.0f - thumbWidth) * 0.5f,
                                  rowCy - pt(thumbHeight) * 0.5f);
            const ImVec2 thumbMax(thumbMin.x + pt(thumbWidth), thumbMin.y + pt(thumbHeight));
            if (indent > 0.0f) {
                // Pegada a la miniatura, que en lienzos verticales no llena su hueco.
                ui::icon(dl, icon::kClip, ImVec2(thumbMin.x - indent * 0.5f, rowCy), 15.0f,
                         ui::withAlpha(detailColor, dim));
            }
            const GLuint texture = m_previews.layerThumbnail(layer, thumbPixelsW, thumbPixelsH, checker);
            const float thumbRadius = pt(6.0f);
            if (texture != 0) {
                dl->AddImageRounded(ImTextureRef(textureId(texture)), thumbMin, thumbMax, ImVec2(0.0f, 0.0f),
                                    ImVec2(1.0f, 1.0f), ui::withAlpha(IM_COL32_WHITE, dim), thumbRadius);
            } else {
                dl->AddRectFilled(thumbMin, thumbMax, ui::withAlpha(IM_COL32_WHITE, dim), thumbRadius);
            }
            dl->AddRect(thumbMin, thumbMax, IM_COL32(255, 255, 255, 36), thumbRadius, 0, ui::hairline());

            // Nombre y, debajo, el modo de fusión y la opacidad (u "Oculta"), con iconos del
            // bloqueo alfa y de la referencia.
            const float textX = row.Min.x + pt(6.0f + 64.0f + 12.0f) + indent;
            const float textRight = (active ? more.Min.x : visibility.Min.x) - pt(6.0f);
            ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(textX, rowCy - pt(8.0f)), Align::Left,
                      ui::withAlpha(th::kLabel, dim), layer.name.c_str(), textRight - textX);
            char detail[64];
            const int percent = static_cast<int>(std::lround(layer.opacity * 100.0f));
            if (!layer.visible) {
                std::snprintf(detail, sizeof(detail), "Oculta");
            } else if (!shown) {
                std::snprintf(detail, sizeof(detail), "Base oculta");
            } else if (layer.blend != BlendMode::Normal) {
                std::snprintf(detail, sizeof(detail), "%s · %d %%", kBlendNames[static_cast<int>(layer.blend)].shortName,
                              percent);
            } else {
                std::snprintf(detail, sizeof(detail), "%d %%", percent);
            }
            const char* badges[2] = {};
            int badgeCount = 0;
            if (layer.alphaLock) {
                badges[badgeCount++] = icon::kLock;
            }
            if (layer.reference) {
                badges[badgeCount++] = icon::kBookmark;
            }
            const float badgeSpace = pt(16.0f) * static_cast<float>(badgeCount);
            const float detailY = rowCy + pt(10.0f);
            const std::string fitted = ui::fitText(Weight::Regular, th::kCaption, detail,
                                                   std::max(pt(20.0f), textRight - textX - badgeSpace));
            ui::label(dl, Weight::Regular, th::kCaption, ImVec2(textX, detailY), Align::Left, detailColor,
                      fitted.c_str());
            float badgeX = textX + ui::measure(Weight::Regular, th::kCaption, fitted.c_str()).x + pt(4.0f + 7.0f);
            for (int b = 0; b < badgeCount; ++b) {
                ui::icon(dl, badges[b], ImVec2(badgeX, detailY), 12.0f, detailColor);
                badgeX += pt(16.0f);
            }

            // Opciones de la capa activa.
            if (active) {
                const float t = ui::anim::follow(ImGui::GetID("##more") + 5u, morePress.held ? 1.0f : 0.0f, 24.0f);
                dl->AddRectFilled(more.Min, more.Max, IM_COL32(255, 255, 255, static_cast<int>(51 + 30 * t)), pt(9.0f));
                ui::icon(dl, icon::kEllipsis, more.GetCenter(), 18.0f, th::kLabel);
            }

            // Casilla de visibilidad.
            visibilityBox(dl, ImGui::GetID("##visible") + 5u, visibility, layer.visible, active, visibilityPress);

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
        const float backgroundTop = y + pt(kListPad) - scroll + pt((rowHeight + kRowGap) * static_cast<float>(rows));
        const ImRect backgroundRect(ImVec2(left, backgroundTop), ImVec2(right, backgroundTop + pt(rowHeight)));
        if (backgroundRect.Max.y >= view.Min.y && backgroundRect.Min.y <= view.Max.y) {
            backgroundRow(canvas, dl, backgroundRect, thumbWidth, thumbHeight);
        }
        ui::endScroll();
    }

    // Pie del color de fondo: dejarlo transparente.
    ui::separator(dl, f.content.Min.x, f.content.Max.x, listBottom, th::kRule);
    if (m_backgroundPage) {
        const ImRect row(ImVec2(f.content.Min.x + pt(8.0f), listBottom + pt(6.0f)),
                         ImVec2(f.content.Max.x - pt(8.0f), listBottom + pt(6.0f + 44.0f)));
        bool transparent = !canvas.background().visible;
        if (toggleRow(dl, "##transparent", row, icon::kEyeOff, "Fondo transparente", &transparent)) {
            CanvasBackground background = canvas.background();
            background.visible = !transparent;
            canvas.setBackground(background, false);
            m_backgroundEditing = true;
        }
        endPanel(f);
        return;
    }

    // Pie: opacidad de la capa activa (barra de relleno) y su modo de fusión, que abre la
    // lista de modos.
    const int activeIndex = layers.activeIndex();
    const float footerTop = listBottom + pt(10.0f);
    const float chipWidth = pt(124.0f);
    const ImRect bar(ImVec2(f.content.Min.x + pt(12.0f), footerTop),
                     ImVec2(f.content.Max.x - pt(12.0f + 8.0f) - chipWidth, footerTop + pt(34.0f)));
    float opacity = layers.active().opacity;
    bool dragging = false;
    if (ui::barSlider("##opacity", bar, &opacity, "Opacidad", &dragging)) {
        canvas.setLayerOpacity(activeIndex, opacity, false);
    }
    if (m_layerOpacityDragging && !dragging) {
        canvas.setLayerOpacity(activeIndex, layers.active().opacity, true);
    }
    m_layerOpacityDragging = dragging;

    const ImRect chip(ImVec2(bar.Max.x + pt(8.0f), footerTop), ImVec2(f.content.Max.x - pt(12.0f), footerTop + pt(34.0f)));
    const ui::Choice blend = ui::choice("##blend", chip, m_blendPage);
    const float chipCy = chip.GetCenter().y;
    ui::icon(dl, icon::kBlend, ImVec2(chip.Min.x + pt(10.0f + 8.0f), chipCy), 16.0f,
             ui::mix(th::kMutedIcon, th::kAccentText, blend.on));
    const float chipText = chip.Min.x + pt(10.0f + 16.0f + 8.0f);
    ui::label(dl, Weight::SemiBold, th::kFootnote, ImVec2(chipText, chipCy), Align::Left,
              ui::mix(th::kLabel, th::kAccentText, blend.on),
              kBlendNames[static_cast<int>(layers.active().blend)].shortName, chip.Max.x - pt(10.0f) - chipText);
    if (blend.press.clicked) {
        m_blendPage = !m_blendPage;
        m_blendScroll = m_blendPage;
        m_layerMenu = false;
    }
    endPanel(f);
}

void Ui::backgroundRow(Canvas& canvas, ImDrawList* dl, const ImRect& row, float thumbWidth, float thumbHeight) {
    const CanvasBackground& background = canvas.background();
    ImGui::PushID("##background-row");
    const float rowCy = row.GetCenter().y;
    const ImRect visibility(ImVec2(row.Max.x - pt(4.0f + 40.0f), rowCy - pt(20.0f)),
                            ImVec2(row.Max.x - pt(4.0f), rowCy + pt(20.0f)));
    const Press visibilityPress = ui::pressable("##visible", visibility);
    const ImGuiID rowId = ImGui::GetID("##row");
    const Press rowPress = ui::pressable(rowId, row);
    pressFeedback(dl, rowId, row, rowPress, pt(th::kRowRadius));

    // La muestra con la forma del lienzo, como las miniaturas de las capas; oculto, el
    // damero de lo transparente.
    const ImVec2 thumbMin(row.Min.x + pt(6.0f) + pt(64.0f - thumbWidth) * 0.5f, rowCy - pt(thumbHeight) * 0.5f);
    const ImRect thumb(thumbMin, ImVec2(thumbMin.x + pt(thumbWidth), thumbMin.y + pt(thumbHeight)));
    const float thumbRadius = pt(6.0f);
    if (background.visible) {
        dl->AddRectFilled(thumb.Min, thumb.Max, ui::fromFloat(background.color), thumbRadius);
    } else {
        ui::checkerboard(dl, thumb, thumbRadius, std::max(pt(4.0f), ui::hairline()));
    }
    dl->AddRect(thumb.Min, thumb.Max, IM_COL32(255, 255, 255, 36), thumbRadius, 0, ui::hairline());

    const float dim = background.visible ? 1.0f : 0.5f;
    const float textX = row.Min.x + pt(6.0f + 64.0f + 12.0f);
    const float textRight = visibility.Min.x - pt(6.0f);
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(textX, rowCy - pt(8.0f)), Align::Left,
              ui::withAlpha(th::kLabel, dim), "Color de fondo", textRight - textX);
    char detail[32];
    if (background.visible) {
        ui::formatHex(background.color, detail, sizeof(detail));
    } else {
        std::snprintf(detail, sizeof(detail), "Transparente");
    }
    ui::label(dl, Weight::Regular, th::kCaption, ImVec2(textX, rowCy + pt(10.0f)), Align::Left,
              IM_COL32(235, 235, 245, 153), detail, textRight - textX);
    visibilityBox(dl, ImGui::GetID("##visible") + 5u, visibility, background.visible, false, visibilityPress);

    if (visibilityPress.clicked) {
        CanvasBackground next = background;
        next.visible = !next.visible;
        canvas.setBackground(next);
    } else if (rowPress.clicked) {
        // Su página: el selector de color (cada color que se prueba se ve en el lienzo).
        m_backgroundPage = true;
        m_layerMenu = false;
        m_hexEditing = false;
        std::copy(background.color, background.color + 3, m_previousBackground);
    }
    ImGui::PopID();
}

void Ui::blendList(Canvas& canvas, ImDrawList* dl, const ImRect& view) {
    const int index = canvas.layers().activeIndex();
    const BlendMode current = canvas.layers().active().blend;
    const float left = view.Min.x + pt(8.0f);
    const float right = view.Max.x - pt(8.0f);

    // Posición de cada modo en la lista (los grupos llevan un rótulo delante).
    float tops[kBlendModeCount];
    float height = kBlendPad;
    for (int i = 0; i < kBlendModeCount; ++i) {
        if (blendGroup(static_cast<BlendMode>(i))) {
            height += kBlendSection;
        }
        tops[i] = height;
        height += kBlendRow;
    }
    height += kBlendPad;
    if (m_blendScroll) {
        const float top = tops[static_cast<int>(current)];
        ui::scrollIntoView("##blend", pt(top - kBlendSection), pt(top + kBlendRow + kBlendPad), view.GetHeight());
        m_blendScroll = false;
    }

    const float scroll = ui::beginScroll("##blend", view, pt(height));
    for (int i = 0; i < kBlendModeCount; ++i) {
        const BlendMode mode = static_cast<BlendMode>(i);
        const float top = view.Min.y - scroll + pt(tops[i]);
        if (const char* group = blendGroup(mode)) {
            const float labelY = top - pt(kBlendSection * 0.5f - 2.0f);
            if (labelY > view.Min.y - pt(kBlendSection) && labelY < view.Max.y + pt(kBlendSection)) {
                ui::sectionLabel(dl, left + pt(12.0f), right - pt(12.0f), labelY, group);
            }
        }
        const ImRect row(ImVec2(left, top), ImVec2(right, top + pt(kBlendRow)));
        if (row.Max.y < view.Min.y || row.Min.y > view.Max.y) {
            continue;
        }
        ImGui::PushID(i);
        const ImGuiID id = ImGui::GetID("##mode");
        const Press press = ui::pressable(id, row);
        ImGui::PopID();
        const bool selected = mode == current;
        const float on = ui::anim::follow(id + 7u, selected ? 1.0f : 0.0f, 20.0f);
        if (on > 0.002f) {
            dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(th::kAccent, on), pt(th::kControlRadius));
        }
        pressFeedback(dl, id, row, press, pt(th::kControlRadius));
        const float cy = row.GetCenter().y;
        ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(row.Min.x + pt(12.0f), cy), Align::Left, th::kLabel,
                  kBlendNames[i].name, row.GetWidth() - pt(12.0f + 36.0f));
        if (on > 0.002f) {
            ui::icon(dl, icon::kCheck, ImVec2(row.Max.x - pt(12.0f + 8.0f), cy), 16.0f, ui::withAlpha(th::kLabel, on));
        }
        // Cada modo se ve en el lienzo en cuanto se toca.
        if (press.clicked && !selected) {
            canvas.setLayerBlend(index, mode, false);
            m_blendEditing = true;
        }
    }
    ui::endScroll();
}

void Ui::layerMenu(Canvas& canvas) {
    const bool open = m_layerMenu && m_panel == Panel::Layers && !m_blendPage;
    const ImGuiID menuId = ImHashStr("##layer-menu");
    const float p = ui::anim::followFrom(menuId, 0.0f, open ? 1.0f : 0.0f, open ? 20.0f : 26.0f);
    if (!open && p <= 0.002f) {
        return;
    }
    const Layout& L = m_layout;
    LayerStack& layers = canvas.layers();
    const int index = layers.activeIndex();
    const Layer& layer = layers.at(index);
    constexpr int kRows = 6;
    const float width = pt(296.0f);
    const float pad = pt(8.0f);
    const float gap = pt(6.0f);
    const float divider = pt(6.0f + 1.0f + 6.0f);
    // Fichas y filas algo más bajas si no cabe (un teléfono en horizontal).
    float tile = pt(60.0f);
    float toggleTile = pt(52.0f);
    float item = pt(44.0f);
    const float fixed = pad * 2.0f + gap + divider;
    const float needed = tile + toggleTile + item * static_cast<float>(kRows);
    const float available = L.bottom - L.top - fixed;
    if (needed > available) {
        const float k = std::max(available / needed, 0.7f);
        tile *= k;
        toggleTile *= k;
        item *= k;
    }
    const float height = fixed + tile + toggleTile + item * static_cast<float>(kRows);
    float x = 0.0f;
    float y = 0.0f;
    ImVec2 anchor;
    if (L.narrow) {
        x = L.right - width;
        y = std::max(L.leftBar.Max.y + pt(8.0f), m_layersRect.Min.y - height - pt(8.0f));
        y = std::max(L.top, std::min(y, L.bottom - height));
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

    enum Action { Rename, Duplicate, Up, Down, AlphaLock, Clip, Reference, Select, Fill, Invert, Merge, Clear, Delete };
    struct Item {
        Action action;
        const char* label;
        const char* glyph;
        bool enabled;
        bool on;   // interruptores
    };
    const Item tiles[4] = {
        {Rename, "Renombrar", icon::kPencil, true, false},
        {Duplicate, "Duplicar", icon::kCopy, layers.count() < canvas.maxLayers(), false},
        {Up, "Subir", icon::kArrowUp, index < layers.count() - 1, false},
        {Down, "Bajar", icon::kArrowDown, index > 0, false},
    };
    const Item toggles[3] = {
        {AlphaLock, "Bloqueo alfa", icon::kLock, true, layer.alphaLock},
        {Clip, "Recorte", icon::kClip, canvas.canClip(index) || layer.clipping, layer.clipping},
        {Reference, "Referencia", icon::kBookmark, true, layer.reference},
    };
    // Con selección, rellenar, invertir y limpiar cambian solo lo seleccionado.
    const bool selected = canvas.hasSelection();
    const Item rows[kRows] = {
        {Select, "Seleccionar el contenido", icon::kSelection, true, false},
        {Fill, selected ? "Rellenar lo seleccionado" : "Rellenar con el color", icon::kPaintBucket, true, false},
        {Invert, selected ? "Invertir lo seleccionado" : "Invertir colores", icon::kContrast, true, false},
        {Merge, "Combinar con la de abajo", icon::kMerge, canvas.canMergeDown(index), false},
        {Clear, selected ? "Borrar lo seleccionado" : "Limpiar capa", icon::kBrushCleaning, true, false},
        {Delete, "Eliminar capa", icon::kTrash, layers.count() > 1, false},
    };
    int chosen = -1;

    // Arriba, las cuatro acciones más usadas como fichas con icono.
    float top = rect.Min.y + pad;
    const float tileWidth = (width - pad * 2.0f - gap * 3.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const float x0 = rect.Min.x + pad + (tileWidth + gap) * static_cast<float>(i);
        const ImRect r(ImVec2(x0, top), ImVec2(x0 + tileWidth, top + tile));
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
    top += tile + gap;

    // Debajo, los interruptores de la capa: se encienden con el color de acento.
    const float toggleWidth = (width - pad * 2.0f - gap * 2.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float x0 = rect.Min.x + pad + (toggleWidth + gap) * static_cast<float>(i);
        const ImRect r(ImVec2(x0, top), ImVec2(x0 + toggleWidth, top + toggleTile));
        ImGui::PushID(20 + i);
        const ui::Choice c = ui::choice("##toggle", r, toggles[i].on, toggles[i].enabled);
        ImGui::PopID();
        const ImU32 color = toggles[i].enabled ? ui::mix(th::kLabel, th::kAccentText, c.on)
                                               : ui::withAlpha(th::kLabel, 0.3f);
        const float middle = r.GetCenter().y;
        ui::icon(dl, toggles[i].glyph, ImVec2(r.GetCenter().x, middle - toggleTile * 0.15f), 17.0f, color);
        ui::label(dl, Weight::Regular, th::kMicro, ImVec2(r.GetCenter().x, middle + toggleTile * 0.24f), Align::Center,
                  color, toggles[i].label, toggleWidth - pt(6.0f));
        if (c.press.clicked) {
            chosen = toggles[i].action;
        }
    }
    top += toggleTile + pt(6.0f);
    ui::separator(dl, rect.Min.x + pad + pt(4.0f), rect.Max.x - pad - pt(4.0f), top, th::kRule);
    top += pt(1.0f + 6.0f);

    // El resto en filas; eliminar, en rojo. Rellenar muestra el color que se usará.
    for (int i = 0; i < kRows; ++i) {
        const ImRect r(ImVec2(rect.Min.x + pad, top), ImVec2(rect.Max.x - pad, top + item));
        ImGui::PushID(10 + i);
        if (menuRow(dl, "##row", r, rows[i].glyph, rows[i].label, nullptr, false, rows[i].enabled,
                    rows[i].action == Delete ? th::kRed : th::kLabel)) {
            chosen = rows[i].action;
        }
        ImGui::PopID();
        if (rows[i].action == Fill) {
            const ImVec2 center(r.Max.x - pt(10.0f + 9.0f), r.GetCenter().y);
            dl->AddCircleFilled(center, pt(8.0f), ui::fromFloat(canvas.brushSettings().color), 0);
            dl->AddCircle(center, pt(8.0f), IM_COL32(255, 255, 255, 64), 0, ui::hairline());
        }
        top += item;
    }
    ui::transform(mark, anchor, 0.92f + 0.08f * p, ImVec2(0.0f, 0.0f), std::min(1.0f, p * 1.3f));
    ui::endSurface();

    switch (chosen) {
    case Rename:
        openDialog(Dialog::RenameLayer, &canvas);
        break;
    case Duplicate:
        if (!canvas.duplicateLayer(index)) {
            notify("No hay memoria para otra capa", Notice::Error);
        } else {
            m_scrollToLayer = static_cast<int>(layers.active().id);
        }
        break;
    case Up:
        canvas.moveLayer(index, index + 1);
        break;
    case Down:
        canvas.moveLayer(index, index - 1);
        break;
    case AlphaLock:
        canvas.setLayerAlphaLock(index, !layer.alphaLock);
        break;
    case Clip:
        canvas.setLayerClipping(index, !layer.clipping);
        break;
    case Reference:
        canvas.setReferenceLayer(layer.reference ? -1 : index);
        break;
    case Select:
        switch (canvas.selectLayerContent(index)) {
        case Canvas::Edit::Done:
            // Como en Procreate: se pasa a la herramienta Selección para verla y usarla.
            closePanels();
            setCanvasTool(canvas, CanvasTool::Select);
            break;
        case Canvas::Edit::NoMemory:
            notify("No hay memoria suficiente", Notice::Error);
            break;
        default:
            notify("La capa está vacía", Notice::Info);
            break;
        }
        break;
    case Fill:
        canvas.fillLayer(index, canvas.brushSettings().color);
        break;
    case Invert:
        canvas.invertLayer(index);
        break;
    case Merge:
        canvas.mergeDown(index);
        break;
    case Clear:
        canvas.clearLayer(index);
        break;
    case Delete:
        openDialog(Dialog::DeleteLayer, &canvas);
        break;
    default:
        break;
    }
    // Subir, bajar y los interruptores dejan el menú abierto (para seguir cambiando cosas).
    if (chosen >= 0 && chosen != Up && chosen != Down && chosen != AlphaLock && chosen != Clip &&
        chosen != Reference) {
        m_layerMenu = false;
    }
}

// -----------------------------------------------------------------------------
// Color
// -----------------------------------------------------------------------------

void Ui::targetColor(const Canvas& canvas, ColorTarget target, float rgb[3]) const {
    const float* color = target == ColorTarget::Background ? canvas.background().color : canvas.brushSettings().color;
    std::copy(color, color + 3, rgb);
}

void Ui::setTargetColor(Canvas& canvas, ColorTarget target, const float rgb[3]) {
    if (target == ColorTarget::Brush) {
        setColor(canvas, rgb);
        return;
    }
    // Elegir un color de fondo lo muestra. Todo lo que se pruebe en la página es un solo
    // paso de deshacer: se guarda al salir de ella.
    CanvasBackground background = canvas.background();
    std::copy(rgb, rgb + 3, background.color);
    background.visible = true;
    canvas.setBackground(background, false);
    m_backgroundEditing = true;
}

void Ui::syncHsv(const float rgb[3]) {
    if (ui::sameColor(rgb, m_hsvSource)) {
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

void Ui::applyHsv(Canvas& canvas, ColorTarget target) {
    float rgb[3];
    ImGui::ColorConvertHSVtoRGB(m_hsv[0], m_hsv[1], m_hsv[2], rgb[0], rgb[1], rgb[2]);
    setTargetColor(canvas, target, rgb);
    std::copy(rgb, rgb + 3, m_hsvSource);
}


void Ui::colorPicker(Canvas& canvas, ImDrawList* dl, ColorTarget target, float left, float right, float y, float wheel,
                     bool wide) {
    const ImGuiIO& io = ImGui::GetIO();
    float color[3];
    targetColor(canvas, target, color);
    syncHsv(color);
    auto choose = [&](const float rgb[3]) {
        setTargetColor(canvas, target, rgb);
        targetColor(canvas, target, color);
        syncHsv(color);
    };
    y += pt(kPickerPadTop);

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
        ui::claimDrag();   // si el panel se desplaza, arrastrar aquí cambia el color
        m_hsv[1] = std::clamp((io.MousePos.x - square.Min.x) / square.GetWidth(), 0.0f, 1.0f);
        m_hsv[2] = 1.0f - std::clamp((io.MousePos.y - square.Min.y) / square.GetHeight(), 0.0f, 1.0f);
        applyHsv(canvas, target);
        targetColor(canvas, target, color);
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
        ui::claimDrag();
        float hue = std::atan2(dx, -dy) / (2.0f * IM_PI);
        if (hue < 0.0f) {
            hue += 1.0f;
        }
        m_hsv[0] = hue;
        applyHsv(canvas, target);
        targetColor(canvas, target, color);
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
        left += pt(wheel + kPickerColumnGap);
    } else {
        y += pt(wheel + 14.0f);
    }

    // Hexadecimal (se puede escribir).
    {
        const ImRect row(ImVec2(left, y), ImVec2(right, y + pt(kPickerHex)));
        char hex[16];
        ui::formatHex(color, hex, sizeof(hex));
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
                if (ui::parseHex(m_hexBuffer, rgb)) {
                    choose(rgb);
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
    y += pt(kPickerHex + 14.0f);

    // Recientes y paleta: muestras cuadradas en seis columnas.
    const float gap = pt(kPickerSwatchGap);
    const float cell = (right - left - gap * 5.0f) / 6.0f;
    auto cellRect = [&](int column, float top) {
        const float x0 = left + (cell + gap) * static_cast<float>(column);
        return ImRect(ImVec2(x0, top), ImVec2(x0 + cell, top + pt(kPickerSwatch)));
    };
    ui::sectionLabel(dl, left, right, y + pt(7.0f), "RECIENTES");
    y += pt(kPickerSection);
    if (m_recentCount == 0) {
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left, y + pt(kPickerSwatch * 0.5f)), Align::Left,
                  th::kTertiaryLabel, "Aparecerán los colores que uses", right - left);
    }
    for (int i = 0; i < m_recentCount; ++i) {
        ImGui::PushID(i);
        if (ui::swatch("##recent", cellRect(i, y), ui::fromFloat(m_recent[i]), ui::sameColor(m_recent[i], color))) {
            choose(m_recent[i]);
        }
        ImGui::PopID();
    }
    y += pt(kPickerSwatch + 14.0f);
    ui::sectionLabel(dl, left, right, y + pt(7.0f), "PALETA");
    y += pt(kPickerSection);
    for (int i = 0; i < 12; ++i) {
        float rgb[3];
        ui::toFloat(kPalette[i], rgb);
        ImGui::PushID(100 + i);
        if (ui::swatch("##palette", cellRect(i % 6, y + (pt(kPickerSwatch) + gap) * static_cast<float>(i / 6)),
                       kPalette[i], ui::sameColor(rgb, color))) {
            choose(rgb);
        }
        ImGui::PopID();
    }
}

void Ui::colorPanel(Canvas& canvas) {
    const Layout& L = m_layout;
    // Si no cabe de alto (un teléfono en horizontal), la rueda va a la izquierda y el
    // resto a su derecha, y la rueda encoge un poco si hace falta.
    const float available = (L.bottom - L.popoverTop) / ui::scale();
    const float tall = th::kHeaderHeight + kPickerTall;
    const bool wide = !L.narrow && tall > available &&
                      L.right - L.left >= pt(kPickerPadX * 2.0f + kPickerWheel + kPickerColumnGap + kPickerSwatchColumn);
    const float wheel = wide ? std::clamp(available - th::kHeaderHeight - kPickerPadTop - kPickerPadBottom, 180.0f,
                                          kPickerWheel)
                             : kPickerWheel;
    const float content =
        wide ? th::kHeaderHeight + kPickerPadTop + std::max(wheel, kPickerSwatches) + kPickerPadBottom : tall;
    const float width = pt(wide ? kPickerPadX * 2.0f + wheel + kPickerColumnGap + kPickerSwatchColumn : 300.0f);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding) + L.barButton * 3.5f;
    PanelFrame f;
    if (!beginPanel(f, Panel::Color, "##panel-color", L.rightBar.Max.x - width, width, pt(content), anchorX, true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    float y = f.content.Min.y - f.scroll;

    // Cabecera con el color actual | el anterior (tocar el anterior lo recupera).
    const float cy = panelHeader(dl, f.content, y, "Color");
    if (colorPill(dl, f.content.Max.x - pt(12.0f), cy, canvas.brushSettings().color, m_previousColor)) {
        setColor(canvas, m_previousColor);
    }
    y += pt(th::kHeaderHeight);
    colorPicker(canvas, dl, ColorTarget::Brush, f.content.Min.x + pt(kPickerPadX), f.content.Max.x - pt(kPickerPadX),
                y, wheel, wide);
    endPanel(f);
}
