// Paneles que se abren desde las barras: acciones, NDI, pinceles, capas (con el menú de
// la capa activa) y color. En una tableta son popovers bajo su botón; en un teléfono en
// vertical, hojas que suben desde abajo.
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

// Resalte de una fila al pulsarla, con las esquinas de su grupo.
void rowHighlight(ImDrawList* dl, ImGuiID id, const ImRect& row, const Press& press, ImDrawFlags corners) {
    const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
    if (t > 0.002f) {
        const float radius = corners == ImDrawFlags_RoundCornersNone ? 0.0f : pt(th::kGroupRadius);
        dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(th::kPressed, t), radius, corners);
    }
}

void sectionTitle(ImDrawList* dl, float x, float y, const char* text) {
    ui::label(dl, Weight::SemiBold, th::kFootnote, ImVec2(x, y + pt(8.0f)), Align::Left, th::kSecondaryLabel, text);
}

// Icono de color y título de una fila de Ajustes. `reserve`: ancho que ocupa lo de la
// derecha (el título se corta antes).
void rowLead(ImDrawList* dl, const ImRect& row, ImU32 tile, const char* glyph, const char* title, bool enabled,
             float reserve) {
    const float cy = row.GetCenter().y;
    const ImRect square(ImVec2(row.Min.x + pt(12.0f), cy - pt(14.0f)), ImVec2(row.Min.x + pt(40.0f), cy + pt(14.0f)));
    ui::iconTile(dl, square, enabled ? tile : ui::withAlpha(tile, 0.45f), glyph, 17.0f);
    const float x = row.Min.x + pt(52.0f);
    ui::label(dl, Weight::Regular, th::kBody, ImVec2(x, cy), Align::Left, enabled ? th::kLabel : th::kDisabledLabel,
              title, std::max(pt(40.0f), row.Max.x - reserve - x));
}

// Fila pulsable de Ajustes con un valor o una flecha a la derecha.
bool settingsRow(ImDrawList* dl, const char* id, const ImRect& row, ImU32 tile, const char* glyph, const char* title,
                 const char* value, bool chevron, bool enabled, ImDrawFlags corners) {
    const ImGuiID gid = ImGui::GetID(id);
    const Press press = ui::pressable(gid, row, enabled);
    rowHighlight(dl, gid, row, press, corners);
    const float cy = row.GetCenter().y;
    float right = row.Max.x - pt(12.0f);
    if (chevron) {
        ui::icon(dl, icon::kChevronRight, ImVec2(right - pt(9.0f), cy), 18.0f, th::kTertiaryLabel);
        right -= pt(24.0f);
    }
    float reserve = row.Max.x - right + pt(8.0f);
    if (value) {
        ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(right, cy), Align::Right, IM_COL32(235, 235, 245, 140),
                  value);
        reserve += ui::measure(Weight::Regular, th::kSubhead, value).x + pt(8.0f);
    }
    rowLead(dl, row, tile, glyph, title, enabled, reserve);
    return press.clicked;
}

// Fila de información: título a la izquierda y valor a la derecha.
void infoRow(ImDrawList* dl, const ImRect& row, const char* title, const char* value) {
    const float cy = row.GetCenter().y;
    ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(row.Min.x + pt(14.0f), cy), Align::Left, th::kLabel, title);
    ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(row.Max.x - pt(14.0f), cy), Align::Right,
              th::kSecondaryLabel, value);
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
        const float grabber = pt(20.0f);
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
    const float radius = pt(f.sheet ? 30.0f : th::kPopoverRadius);
    const ImDrawFlags corners = f.sheet ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
    ui::pushUnclipped(f.dl);
    if (f.sheet) {
        ui::shadow(f.dl, f.rect, radius, pt(36.0f), -pt(8.0f), 0.3f, corners);
    } else {
        ui::shadow(f.dl, f.rect, radius, pt(40.0f), pt(14.0f), 0.26f, corners);
    }
    ui::popUnclipped(f.dl);
    ui::glass(f.dl, f.rect, radius, th::kPopoverTint, corners);
    if (f.sheet) {
        const ImVec2 center(f.rect.GetCenter().x, f.rect.Min.y + pt(10.5f));
        f.dl->AddRectFilled(ImVec2(center.x - pt(18.0f), center.y - pt(2.5f)),
                            ImVec2(center.x + pt(18.0f), center.y + pt(2.5f)), IM_COL32(235, 235, 245, 77),
                            pt(2.5f));
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
        ui::transform(f.mark, f.anchor, 0.9f + 0.1f * e, ImVec2(0.0f, 0.0f), std::min(1.0f, e * 1.3f));
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
    constexpr float kPadTop = 16.0f;
    constexpr float kTitle = 22.0f;
    constexpr float kTitleGap = 12.0f;
    constexpr float kSection = 22.0f;   // rótulo y hueco
    constexpr float kRow = 48.0f;
    constexpr float kGroupGap = 14.0f;
    constexpr float kSizeRow = 92.0f;
    constexpr float kFooterGap = 10.0f;
    constexpr float kFooter = 16.0f;
    constexpr float kPadBottom = 14.0f;
    const float content = kPadTop + kTitle + kTitleGap + kSection + kRow * 2.0f + kGroupGap + kSection + kRow +
                          kGroupGap + kSection + kRow * 2.0f + kSizeRow + kFooterGap + kFooter + kPadBottom;
    const float anchorX = L.leftBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * 0.5f);
    PanelFrame f;
    if (!beginPanel(f, Panel::Actions, "##panel-actions", L.leftBar.Min.x, pt(336.0f), pt(content), anchorX, true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    const float left = f.content.Min.x + pt(14.0f);
    const float right = f.content.Max.x - pt(14.0f);
    float y = f.content.Min.y - f.scroll + pt(kPadTop);
    ui::label(dl, Weight::SemiBold, th::kHeadline, ImVec2(left + pt(4.0f), y + pt(kTitle * 0.5f)), Align::Left,
              th::kLabel, "Acciones");
    y += pt(kTitle + kTitleGap);

    // Lienzo.
    sectionTitle(dl, left + pt(4.0f), y, "Lienzo");
    y += pt(kSection);
    ui::group(dl, ImRect(left, y, right, y + pt(kRow * 2.0f)), pt(th::kGroupRadius));
    if (settingsRow(dl, "##fit", ImRect(left, y, right, y + pt(kRow)), th::kGray2, icon::kScan, "Centrar lienzo",
                    nullptr, false, true, ImDrawFlags_RoundCornersTop)) {
        requests.fitView = true;
        closePanels();
    }
    ui::separator(dl, left + pt(52.0f), right, y + pt(kRow));
    if (settingsRow(dl, "##new", ImRect(left, y + pt(kRow), right, y + pt(kRow * 2.0f)), th::kAccent,
                    icon::kFilePlus, "Nuevo lienzo…", nullptr, true, true, ImDrawFlags_RoundCornersBottom)) {
        openDialog(Dialog::NewCanvas, &canvas);
    }
    y += pt(kRow * 2.0f + kGroupGap);

    // Compartir.
    sectionTitle(dl, left + pt(4.0f), y, "Compartir");
    y += pt(kSection);
    ui::group(dl, ImRect(left, y, right, y + pt(kRow)), pt(th::kGroupRadius));
    char size[32];
    std::snprintf(size, sizeof(size), "%d × %d", canvas.width(), canvas.height());
    if (settingsRow(dl, "##save", ImRect(left, y, right, y + pt(kRow)), th::kGreen, icon::kImageDown,
                    m_status.exporting ? "Guardando PNG…" : "Guardar PNG", size, false, !m_status.exporting,
                    ImDrawFlags_RoundCornersAll)) {
        requests.savePng = true;
        closePanels();
    }
    y += pt(kRow + kGroupGap);

    // Preferencias.
    sectionTitle(dl, left + pt(4.0f), y, "Preferencias");
    y += pt(kSection);
    ui::group(dl, ImRect(left, y, right, y + pt(kRow * 2.0f + kSizeRow)), pt(th::kGroupRadius));
    const ImVec2 toggleSize = ui::toggleSize();
    auto toggleRow = [&](const char* id, float top, ImU32 tile, const char* glyph, const char* title, bool* value) {
        const ImRect row(left, top, right, top + pt(kRow));
        rowLead(dl, row, tile, glyph, title, true, toggleSize.x + pt(20.0f));
        const ImVec2 pos(row.Max.x - pt(12.0f) - toggleSize.x, row.GetCenter().y - toggleSize.y * 0.5f);
        return ui::toggle(id, pos, value);
    };
    bool finger = m_prefs.drawWithFinger;
    if (toggleRow("##finger", y, th::kOrange, icon::kHand, "Dibujar con el dedo", &finger)) {
        m_prefs.drawWithFinger = finger;
        savePrefs();
    }
    ui::separator(dl, left + pt(52.0f), right, y + pt(kRow));
    bool sideRight = m_prefs.sidebarRight;
    if (toggleRow("##side", y + pt(kRow), th::kIndigo, icon::kPanelRight, "Barra lateral derecha", &sideRight)) {
        m_prefs.sidebarRight = sideRight;
        savePrefs();
    }
    ui::separator(dl, left + pt(52.0f), right, y + pt(kRow * 2.0f));
    {
        const float top = y + pt(kRow * 2.0f);
        const ImRect head(left, top + pt(10.0f), right, top + pt(38.0f));
        rowLead(dl, head, th::kGray, icon::kTextSize, "Tamaño de la interfaz", true, pt(12.0f));
        const ImRect control(left + pt(12.0f), top + pt(48.0f), right - pt(12.0f), top + pt(80.0f));
        const char* labels[3] = {"Pequeña", "Normal", "Grande"};
        int choice = std::clamp(m_prefs.size, 0, 2);
        if (ui::segmented("##uisize", control, labels, 3, &choice)) {
            m_prefs.size = choice;
            savePrefs();
        }
    }
    y += pt(kRow * 2.0f + kSizeRow + kFooterGap);
    ui::label(dl, Weight::Regular, th::kCaption, ImVec2((left + right) * 0.5f, y + pt(kFooter * 0.5f)), Align::Center,
              th::kTertiaryLabel, "LiveSketch 0.3 · Android y web");
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
    const float width = pt(324.0f);
    const float contentWidth = (L.narrow ? L.display.x - L.safe[1] - L.safe[3] : width) - pt(14.0f + 4.0f) * 2.0f;

#ifdef SDL_PLATFORM_EMSCRIPTEN
    const char* note = available ? "Los receptores ven el lienzo completo, sin el zoom ni la interfaz."
                                 : "Un navegador no puede emitir NDI. Para emitir, usa la app de Android.";
#else
    const char* note = available ? "Los receptores ven el lienzo completo, sin el zoom ni la interfaz."
                                 : "Esta compilación no incluye el SDK de NDI.";
#endif
    const float noteHeight = ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f), contentWidth,
                                           Align::Left, 0, note);
    const float errorHeight = failed ? ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f),
                                                     contentWidth, Align::Left, 0, m_status.ndiError.c_str()) +
                                           pt(8.0f)
                                     : 0.0f;
    const float content = pt(16.0f + 24.0f + 12.0f + 52.0f + 12.0f + 132.0f + 12.0f + 14.0f) + noteHeight + errorHeight;
    const float anchorX = L.narrow ? L.leftBar.Max.x : L.ndiBar.GetCenter().x;
    PanelFrame f;
    if (!beginPanel(f, Panel::Ndi, "##panel-ndi", L.narrow ? L.left : L.ndiBar.Min.x, width, content, anchorX,
                    true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    const float left = f.content.Min.x + pt(14.0f);
    const float right = f.content.Max.x - pt(14.0f);
    float y = f.content.Min.y - f.scroll + pt(16.0f);

    // Título y estado.
    const float cy = y + pt(12.0f);
    ui::label(dl, Weight::SemiBold, th::kHeadline, ImVec2(left + pt(4.0f), cy), Align::Left, th::kLabel, "NDI");
    const char* state = !available ? "NO DISPONIBLE" : failed ? "ERROR" : running ? "EN VIVO" : "APAGADO";
    const ImU32 pillColor = failed ? th::kOrange : (running ? th::kRed : IM_COL32(118, 118, 128, 77));
    const ImU32 pillText = running ? th::kLabel : IM_COL32(235, 235, 245, 180);
    const float pillWidth = ui::trackedWidth(Weight::Bold, th::kCaption, state, 0.05f) + pt(20.0f);
    const ImRect pill(ImVec2(right - pt(4.0f) - pillWidth, cy - pt(12.0f)), ImVec2(right - pt(4.0f), cy + pt(12.0f)));
    dl->AddRectFilled(pill.Min, pill.Max, pillColor, pt(12.0f));
    ui::tracked(dl, Weight::Bold, th::kCaption, ImVec2(pill.Min.x + pt(10.0f), cy), pillText, state, 0.05f);
    y += pt(24.0f + 12.0f);

    // Interruptor.
    const ImRect toggleRow(left, y, right, y + pt(52.0f));
    ui::group(dl, toggleRow, pt(th::kGroupRadius));
    const ImVec2 toggleSize = ui::toggleSize();
    rowLead(dl, toggleRow, th::kRed, icon::kRadio, "Emitir por NDI", available, toggleSize.x + pt(20.0f));
    bool on = running;
    if (ui::toggle("##ndi-toggle",
                   ImVec2(right - pt(12.0f) - toggleSize.x, toggleRow.GetCenter().y - toggleSize.y * 0.5f), &on,
                   available)) {
        requests.ndi = on ? 1 : 0;
    }
    y += pt(52.0f + 12.0f);

    // Datos de la emisión.
    ui::group(dl, ImRect(left, y, right, y + pt(132.0f)), pt(th::kGroupRadius));
    char receivers[16];
    std::snprintf(receivers, sizeof(receivers), "%d", m_status.ndiConnections);
    char resolution[32];
    std::snprintf(resolution, sizeof(resolution), "%d × %d", canvas.width(), canvas.height());
    infoRow(dl, ImRect(left, y, right, y + pt(44.0f)), "Fuente", "LiveSketch");
    ui::separator(dl, left + pt(14.0f), right, y + pt(44.0f));
    infoRow(dl, ImRect(left, y + pt(44.0f), right, y + pt(88.0f)), "Receptores", running ? receivers : "—");
    ui::separator(dl, left + pt(14.0f), right, y + pt(88.0f));
    infoRow(dl, ImRect(left, y + pt(88.0f), right, y + pt(132.0f)), "Resolución", resolution);
    y += pt(132.0f + 12.0f);

    if (failed) {
        y += ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(4.0f), y), contentWidth, Align::Left,
                           th::kRed, m_status.ndiError.c_str()) +
             pt(8.0f);
    }
    ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(4.0f), y), contentWidth, Align::Left,
                  th::kSecondaryLabel, note);
    endPanel(f);
}

// -----------------------------------------------------------------------------
// Pinceles
// -----------------------------------------------------------------------------

void Ui::brushesPanel() {
    const Layout& L = m_layout;
    constexpr float kRow = 66.0f;
    constexpr float kRowGap = 4.0f;
    const float content = 14.0f + 28.0f + 6.0f + kRow * BrushSettings::kTypeCount +
                          kRowGap * (BrushSettings::kTypeCount - 1) + 10.0f;
    const float width = pt(340.0f);
    const int toolIndex = static_cast<int>(m_tool);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * (0.5f + toolIndex));
    PanelFrame f;
    if (!beginPanel(f, Panel::Brushes, "##panel-brushes", L.rightBar.Max.x - width, width, pt(content), anchorX,
                    true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    const float left = f.content.Min.x + pt(10.0f);
    const float right = f.content.Max.x - pt(10.0f);
    float y = f.content.Min.y - f.scroll + pt(14.0f);

    const float titleWidth = ui::measure(Weight::SemiBold, th::kHeadline, "Pinceles").x;
    ui::label(dl, Weight::SemiBold, th::kHeadline, ImVec2(left + pt(6.0f), y + pt(14.0f)), Align::Left, th::kLabel,
              "Pinceles");
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(6.0f) + titleWidth + pt(8.0f), y + pt(15.0f)),
              Align::Left, th::kSecondaryLabel, m_tool == Tool::Brush ? "para el pincel" : "para el borrador");
    y += pt(28.0f + 6.0f);

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
        rowHighlight(dl, id, row, press, ImDrawFlags_RoundCornersAll);

        const float cy = row.GetCenter().y;
        const float nameWidth = pt(118.0f);
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(row.Min.x + pt(12.0f), cy - pt(9.0f)), Align::Left,
                  th::kLabel, kBrushNames[i], nameWidth);
        ui::label(dl, Weight::Regular, th::kCaption, ImVec2(row.Min.x + pt(12.0f), cy + pt(10.0f)), Align::Left,
                  ui::mix(IM_COL32(235, 235, 245, 153), IM_COL32(255, 255, 255, 204), on), kBrushNotes[i],
                  nameWidth);

        // Trazo de muestra hecho con el propio pincel.
        const float previewWidth = std::min(pt(176.0f), row.GetWidth() - pt(12.0f + 118.0f + 8.0f + 12.0f));
        const float previewHeight = previewWidth * 44.0f / 176.0f;
        const ImRect preview(ImVec2(row.Max.x - pt(12.0f) - previewWidth, cy - previewHeight * 0.5f),
                             ImVec2(row.Max.x - pt(12.0f), cy + previewHeight * 0.5f));
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
    constexpr float kRowGap = 6.0f;
    constexpr float kPadTop = 14.0f;
    constexpr float kHeader = 32.0f;
    constexpr float kHeaderGap = 10.0f;
    constexpr float kFooter = 8.0f + 8.0f + 28.0f + 14.0f;   // separador, opacidad y margen
    const float listHeight = rowHeight * static_cast<float>(count) + kRowGap * static_cast<float>(count - 1);
    const float content = kPadTop + kHeader + kHeaderGap + listHeight + kFooter;
    const float width = pt(340.0f);
    const float anchorX = L.rightBar.Min.x + pt(th::kBarPadding + th::kBarButtonWidth * 2.5f);
    PanelFrame f;
    if (!beginPanel(f, Panel::Layers, "##panel-layers", L.rightBar.Max.x - width, width, pt(content), anchorX,
                    false)) {
        return;
    }
    m_layersRect = f.rect;
    ImDrawList* dl = f.dl;
    const float left = f.content.Min.x + pt(10.0f);
    const float right = f.content.Max.x - pt(10.0f);
    float y = f.content.Min.y + pt(kPadTop);

    // Título y añadir.
    ui::label(dl, Weight::SemiBold, th::kHeadline, ImVec2(left + pt(6.0f), y + pt(kHeader * 0.5f)), Align::Left,
              th::kLabel, "Capas");
    {
        const ImRect add(ImVec2(right - pt(6.0f + 32.0f), y), ImVec2(right - pt(6.0f), y + pt(32.0f)));
        const ImGuiID id = ImGui::GetID("##add");
        const bool full = count >= canvas.maxLayers();
        const Press press = ui::pressable(id, add);
        const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : 0.0f, 24.0f);
        dl->AddCircleFilled(add.GetCenter(), pt(16.0f), ui::withAlpha(th::kAccent, 0.22f + 0.14f * t), 0);
        ui::icon(dl, icon::kPlus, add.GetCenter(), 18.0f, full ? ui::withAlpha(th::kAccent, 0.5f) : th::kAccent);
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
    y += pt(kHeader + kHeaderGap);

    // Lista, de la capa de arriba a la de abajo.
    const float listTop = y;
    const float listBottom = f.content.Max.y - pt(kFooter);
    const ImRect view(ImVec2(f.content.Min.x, listTop), ImVec2(f.content.Max.x, listBottom));
    const int rows = layers.count();
    const float rowsHeight = pt(rowHeight * static_cast<float>(rows) + kRowGap * static_cast<float>(rows - 1));
    if (m_scrollToLayer >= 0) {
        const int index = layers.indexOf(static_cast<uint32_t>(m_scrollToLayer));
        if (index >= 0) {
            const float top = pt((rowHeight + kRowGap) * static_cast<float>(rows - 1 - index));
            ui::scrollIntoView("##list", top, top + pt(rowHeight), view.GetHeight());
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
        const float top = listTop - scroll + pt((rowHeight + kRowGap) * static_cast<float>(visual));
        const ImRect row(ImVec2(left, top), ImVec2(right, top + pt(rowHeight)));
        const bool active = index == layers.activeIndex();
        if (active) {
            m_activeRow = row;
        }
        if (row.Max.y < view.Min.y || row.Min.y > view.Max.y) {
            continue;
        }
        ImGui::PushID(static_cast<int>(layer.id));
        const float cy = row.GetCenter().y;
        // Primero los botones de dentro de la fila: se quedan el toque antes que la fila.
        const ImRect visibility(ImVec2(row.Max.x - pt(4.0f + 40.0f), cy - pt(20.0f)),
                                ImVec2(row.Max.x - pt(4.0f), cy + pt(20.0f)));
        const Press visibilityPress = ui::pressable("##visible", visibility);
        ImRect more;
        Press morePress;
        if (active) {
            more = ImRect(ImVec2(visibility.Min.x - pt(2.0f + 32.0f), cy - pt(16.0f)),
                          ImVec2(visibility.Min.x - pt(2.0f), cy + pt(16.0f)));
            morePress = ui::pressable("##more", more);
        }
        const ImGuiID rowId = ImGui::GetID("##row");
        const Press rowPress = ui::pressable(rowId, row);

        const float on = ui::anim::follow(rowId + 7u, active ? 1.0f : 0.0f, 20.0f);
        if (on > 0.002f) {
            dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(th::kAccent, on), pt(th::kRowRadius));
        }
        rowHighlight(dl, rowId, row, rowPress, ImDrawFlags_RoundCornersAll);

        const float dim = layer.visible ? 1.0f : 0.5f;
        // Miniatura real de la capa sobre un damero.
        const ImVec2 thumbMin(row.Min.x + pt(6.0f) + pt(64.0f - thumbWidth) * 0.5f, cy - pt(thumbHeight) * 0.5f);
        const ImVec2 thumbMax(thumbMin.x + pt(thumbWidth), thumbMin.y + pt(thumbHeight));
        const GLuint texture = m_previews.layerThumbnail(layer, thumbPixelsW, thumbPixelsH, checker);
        const float thumbRadius = pt(5.0f);
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
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(textX, cy - pt(8.0f)), Align::Left,
                  ui::withAlpha(th::kLabel, dim), layer.name.c_str(), textRight - textX);
        char detail[32];
        if (layer.visible) {
            std::snprintf(detail, sizeof(detail), "%d %%", static_cast<int>(std::lround(layer.opacity * 100.0f)));
        } else {
            std::snprintf(detail, sizeof(detail), "Oculta");
        }
        ui::label(dl, Weight::Regular, th::kCaption, ImVec2(textX, cy + pt(10.0f)), Align::Left,
                  ui::mix(IM_COL32(235, 235, 245, 153), IM_COL32(255, 255, 255, 204), on), detail);

        // Opciones de la capa activa.
        if (active) {
            const float t = ui::anim::follow(ImGui::GetID("##more") + 5u, morePress.held ? 1.0f : 0.0f, 24.0f);
            dl->AddCircleFilled(more.GetCenter(), pt(16.0f), IM_COL32(255, 255, 255, static_cast<int>(51 + 30 * t)),
                                0);
            ui::icon(dl, icon::kEllipsis, more.GetCenter(), 18.0f, th::kLabel);
        }

        // Casilla de visibilidad.
        const ImVec2 boxCenter = visibility.GetCenter();
        const ImRect box(ImVec2(boxCenter.x - pt(12.0f), boxCenter.y - pt(12.0f)),
                         ImVec2(boxCenter.x + pt(12.0f), boxCenter.y + pt(12.0f)));
        const float shown = ui::anim::follow(ImGui::GetID("##visible") + 5u, layer.visible ? 1.0f : 0.0f, 22.0f);
        const ImU32 fill = active ? IM_COL32_WHITE : th::kAccent;
        if (shown < 0.999f) {
            dl->AddRect(box.Min, box.Max,
                        ui::withAlpha(active ? IM_COL32(255, 255, 255, 204) : IM_COL32(255, 255, 255, 102),
                                      1.0f - shown),
                        pt(7.0f), 0, pt(1.5f));
        }
        if (shown > 0.001f) {
            dl->AddRectFilled(box.Min, box.Max, ui::withAlpha(fill, shown), pt(7.0f));
            ui::icon(dl, icon::kCheck, boxCenter, 15.0f,
                     ui::withAlpha(active ? th::kAccent : IM_COL32_WHITE, shown));
        }
        if (visibilityPress.held) {
            dl->AddRectFilled(box.Min, box.Max, IM_COL32(255, 255, 255, 40), pt(7.0f));
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

    // Opacidad de la capa activa.
    y = listBottom + pt(8.0f);
    ui::separator(dl, left + pt(6.0f), right - pt(6.0f), y);
    y += pt(8.0f);
    const int activeIndex = layers.activeIndex();
    const ImRect slider(ImVec2(left + pt(8.0f + 64.0f + 12.0f), y), ImVec2(right - pt(8.0f + 40.0f + 12.0f), y + pt(28.0f)));
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(8.0f), slider.GetCenter().y), Align::Left,
              th::kSecondaryLabel, "Opacidad");
    float opacity = layers.active().opacity;
    bool dragging = false;
    if (ui::slider("##opacity", slider, &opacity, 0.0f, 1.0f, &dragging)) {
        canvas.setLayerOpacity(activeIndex, opacity, false);
    }
    if (m_layerOpacityDragging && !dragging) {
        canvas.setLayerOpacity(activeIndex, layers.active().opacity, true);
    }
    m_layerOpacityDragging = dragging;
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d %%", static_cast<int>(std::lround(layers.active().opacity * 100.0f)));
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(right - pt(8.0f), slider.GetCenter().y), Align::Right,
              th::kLabel, percent);
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
    const float width = pt(250.0f);
    const float pad = pt(6.0f);
    const float gap = pt(9.0f);
    // Filas algo más bajas si no cabe bajo las barras (un teléfono en horizontal).
    const float item = std::clamp((L.bottom - L.popoverTop - pad * 2.0f - gap * 2.0f) / 7.0f, pt(34.0f), pt(44.0f));
    const float height = pad * 2.0f + item * 7.0f + gap * 2.0f;
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
    const float radius = pt(18.0f);

    ui::beginSurface("##layer-menu", rect, open, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(40.0f), pt(14.0f), 0.26f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, IM_COL32(30, 30, 34, 214));

    struct Item {
        const char* label;
        const char* glyph;
        bool enabled;
        bool destructive;
    };
    const Item items[7] = {
        {"Renombrar", icon::kPencil, true, false},
        {"Duplicar", icon::kCopy, layers.count() < canvas.maxLayers(), false},
        {"Combinar abajo", icon::kMerge, canvas.canMergeDown(index), false},
        {"Subir", icon::kChevronUp, index < layers.count() - 1, false},
        {"Bajar", icon::kChevronDown, index > 0, false},
        {"Limpiar", icon::kBrushCleaning, true, false},
        {"Eliminar", icon::kTrash, layers.count() > 1, true},
    };
    int chosen = -1;
    float top = rect.Min.y + pad;
    for (int i = 0; i < 7; ++i) {
        const ImRect row(ImVec2(rect.Min.x + pad, top), ImVec2(rect.Max.x - pad, top + item));
        ImGui::PushID(i);
        const ImGuiID id = ImGui::GetID("##item");
        const Press press = ui::pressable(id, row, items[i].enabled);
        ImGui::PopID();
        const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
        if (t > 0.002f) {
            dl->AddRectFilled(row.Min, row.Max, ui::withAlpha(th::kPressed, t), pt(12.0f));
        }
        ImU32 color = items[i].destructive ? th::kRed : th::kLabel;
        if (!items[i].enabled) {
            color = ui::withAlpha(color, 0.32f);
        }
        ui::label(dl, Weight::Regular, th::kBody, ImVec2(row.Min.x + pt(12.0f), row.GetCenter().y), Align::Left, color,
                  items[i].label);
        ui::icon(dl, items[i].glyph, ImVec2(row.Max.x - pt(12.0f + 9.0f), row.GetCenter().y), 18.0f, color);
        if (press.clicked) {
            chosen = i;
        }
        top += item;
        if (i == 2 || i == 4) {
            ui::separator(dl, rect.Min.x + pt(16.0f), rect.Max.x - pt(16.0f), top + gap * 0.5f);
            top += gap;
        }
    }
    ui::transform(mark, anchor, 0.85f + 0.15f * p, ImVec2(0.0f, 0.0f), std::min(1.0f, p * 1.3f));
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
    if (chosen >= 0) {
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
    constexpr float kHeader = 14.0f + 28.0f + 14.0f;                 // título y color actual | anterior
    constexpr float kWheel = 236.0f;
    constexpr float kSwatches = 30.0f + 14.0f + 56.0f + 14.0f + 96.0f;   // hex, recientes y paleta
    constexpr float kSwatchColumn = 244.0f;
    constexpr float kColumnGap = 24.0f;
    constexpr float kPadX = 16.0f;
    constexpr float kPadBottom = 16.0f;
    // Si no cabe de alto (un teléfono en horizontal), la rueda va a la izquierda y el
    // resto a su derecha, y la rueda encoge un poco si hace falta.
    const float available = (L.bottom - L.popoverTop) / ui::scale();
    const float tall = kHeader + kWheel + 14.0f + kSwatches + kPadBottom;
    const bool wide = !L.narrow && tall > available &&
                      L.right - L.left >= pt(kPadX * 2.0f + kWheel + kColumnGap + kSwatchColumn);
    const float wheel = wide ? std::clamp(available - kHeader - kPadBottom, 180.0f, kWheel) : kWheel;
    const float content = wide ? kHeader + std::max(wheel, kSwatches) + kPadBottom : tall;
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
    float left = f.content.Min.x + pt(kPadX);
    const float right = f.content.Max.x - pt(kPadX);
    float y = f.content.Min.y - f.scroll + pt(14.0f);

    // Título y color actual | anterior (tocar el anterior lo recupera).
    ui::label(dl, Weight::SemiBold, th::kHeadline, ImVec2(left, y + pt(14.0f)), Align::Left, th::kLabel, "Color");
    {
        const ImRect pill(ImVec2(right - pt(60.0f), y), ImVec2(right, y + pt(28.0f)));
        const float mid = pill.GetCenter().x;
        dl->AddRectFilled(pill.Min, ImVec2(mid, pill.Max.y), ui::fromFloat(color), pt(14.0f),
                          ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(mid, pill.Min.y), pill.Max, ui::fromFloat(m_previousColor), pt(14.0f),
                          ImDrawFlags_RoundCornersRight);
        dl->AddRect(pill.Min, pill.Max, IM_COL32(255, 255, 255, 46), pt(14.0f), 0, ui::hairline());
        const Press previous = ui::pressable("##previous", ImRect(ImVec2(mid, pill.Min.y), pill.Max));
        if (previous.clicked) {
            setColor(canvas, m_previousColor);
            syncHsv(color);
        }
    }
    y += pt(28.0f + 14.0f);

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
        const ImRect row(ImVec2(left, y), ImVec2(right, y + pt(30.0f)));
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left, row.GetCenter().y), Align::Left, th::kSecondaryLabel,
                  "Hex");
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(std::lround(color[0] * 255.0f)),
                      static_cast<int>(std::lround(color[1] * 255.0f)), static_cast<int>(std::lround(color[2] * 255.0f)));
        const float fieldWidth = pt(112.0f);
        const ImRect field(ImVec2(right - fieldWidth, row.Min.y), row.Max);
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
            const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : 0.0f, 24.0f);
            dl->AddRectFilled(field.Min, field.Max, ui::mix(IM_COL32(118, 118, 128, 61), IM_COL32(118, 118, 128, 100), t),
                              pt(9.0f));
            ui::label(dl, Weight::SemiBold, th::kSubhead, field.GetCenter(), Align::Center, th::kLabel, hex);
            if (press.clicked) {
                m_hexEditing = true;
                m_hexFocusFrames = 2;
                std::snprintf(m_hexBuffer, sizeof(m_hexBuffer), "%s", hex);
            }
        }
    }
    y += pt(30.0f + 14.0f);

    // Recientes y paleta, en las mismas columnas.
    const float swatch = pt(32.0f);
    const float columnGap = std::max(0.0f, (right - left - swatch * 6.0f) / 5.0f);
    auto column = [&](int i) { return left + swatch * 0.5f + (swatch + columnGap) * static_cast<float>(i); };
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left, y + pt(8.0f)), Align::Left, th::kSecondaryLabel,
              "Recientes");
    y += pt(16.0f + 8.0f);
    if (m_recentCount == 0) {
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left, y + swatch * 0.5f), Align::Left, th::kTertiaryLabel,
                  "Aparecerán los colores que uses");
    }
    for (int i = 0; i < m_recentCount; ++i) {
        ImGui::PushID(i);
        if (ui::swatch("##recent", ImVec2(column(i), y + swatch * 0.5f), swatch * 0.5f, ui::fromFloat(m_recent[i]),
                       sameColor(m_recent[i], color))) {
            setColor(canvas, m_recent[i]);
            syncHsv(color);
        }
        ImGui::PopID();
    }
    y += swatch + pt(14.0f);
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left, y + pt(8.0f)), Align::Left, th::kSecondaryLabel,
              "Paleta");
    y += pt(16.0f + 8.0f);
    for (int i = 0; i < 12; ++i) {
        float rgb[3];
        toFloat(kPalette[i], rgb);
        const ImVec2 at(column(i % 6), y + swatch * 0.5f + (swatch + pt(8.0f)) * static_cast<float>(i / 6));
        ImGui::PushID(100 + i);
        if (ui::swatch("##palette", at, swatch * 0.5f, kPalette[i], sameColor(rgb, color))) {
            setColor(canvas, rgb);
            syncHsv(color);
        }
        ImGui::PopID();
    }
    endPanel(f);
}
