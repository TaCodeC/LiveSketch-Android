// Interfaz: marco del frame, disposición, barras superiores, barra lateral, avisos,
// HUD de tamaño y opacidad, lupa del cuentagotas y atajos de teclado.
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;

namespace {

// Tamaño de la interfaz: pequeña, normal y grande.
constexpr float kSizeFactors[3] = {0.85f, 1.0f, 1.15f};
// Por debajo de este ancho (en puntos) los paneles salen como hojas desde abajo; si el
// lado corto de la pantalla es menor, las medidas son las compactas.
constexpr float kNarrowWidth = 600.0f;

// Deshacer mantenido pulsado: empieza a repetirse tras un momento.
constexpr double kRepeatDelay = 0.45;
constexpr double kRepeatInterval = 0.09;

std::string prefsPath() {
    char* folder = SDL_GetPrefPath("TaCodec", "LiveSketch");
    if (!folder) {
        return {};
    }
    std::string path = std::string(folder) + "interfaz.txt";
    SDL_free(folder);
    return path;
}

bool sameColor(const float a[3], const float b[3]) {
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(a[i] - b[i]) > 1.0f / 512.0f) {
            return false;
        }
    }
    return true;
}

} // namespace

// -----------------------------------------------------------------------------
// Ciclo de vida
// -----------------------------------------------------------------------------

bool Ui::init() {
    if (!m_prefsLoaded) {
        m_prefsLoaded = true;
        loadPrefs();
        initBrushes();
    }
    return m_previews.init();
}

void Ui::destroy() {
    saveBrushesIfDue(true);
    m_previews.destroy();
}

void Ui::loadPrefs() {
    const std::string path = prefsPath();
    if (path.empty()) {
        return;
    }
    std::ifstream in(path);
    std::string key;
    int value = 0;
    while (in >> key >> value) {
        if (key == "dedo") {
            m_prefs.drawWithFinger = value != 0;
        } else if (key == "barra-derecha") {
            m_prefs.sidebarRight = value != 0;
        } else if (key == "tamano") {
            m_prefs.size = std::clamp(value, 0, 2);
        }
    }
}

void Ui::savePrefs() const {
    const std::string path = prefsPath();
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::trunc);
    out << "dedo " << (m_prefs.drawWithFinger ? 1 : 0) << '\n'
        << "barra-derecha " << (m_prefs.sidebarRight ? 1 : 0) << '\n'
        << "tamano " << m_prefs.size << '\n';
}

void Ui::beginFrame(const UiStatus& status) {
    m_status = status;
    ui::setScale(status.pointScale * kSizeFactors[std::clamp(m_prefs.size, 0, 2)], status.pixelsPerUnit);
    ui::anim::newFrame(ImGui::GetIO().DeltaTime);
}

void Ui::build(Canvas* canvas, UiRequests& requests) {
    computeLayout();
    saveBrushesIfDue(false);
    if (!canvas) {
        m_panel = Panel::None;
        m_layerMenu = false;
        m_eyedropperArmed = false;
        m_picker.active = false;
        m_dialog = Dialog::None;
        m_dialogShown = Dialog::None;
        startScreen(requests);
        drawToast();
        return;
    }
    syncBrush(*canvas);
    handleKeys(*canvas);
    toolFrame(*canvas);
    if (m_panel != Panel::None) {
        drawScrim();
    }
    drawToolOverlay();
    drawTopBars(*canvas, requests);
    drawSidebar(*canvas);
    drawDock(*canvas);
    drawPanels(*canvas, requests);
    drawHud(*canvas);
    drawThreshold(*canvas);
    drawPicker(*canvas);
    drawDrop(*canvas);
    drawDialogs(canvas, requests);
    drawToast();
    m_previews.pruneThumbnails(canvas->layers());
}

// -----------------------------------------------------------------------------
// Disposición
// -----------------------------------------------------------------------------

void Ui::computeLayout() {
    Layout& L = m_layout;
    L.display = ImGui::GetIO().DisplaySize;
    L.safe[0] = m_status.safeTop;
    L.safe[1] = m_status.safeRight;
    L.safe[2] = m_status.safeBottom;
    L.safe[3] = m_status.safeLeft;
    // La decisión de teléfono o tableta no depende del tamaño de interfaz elegido.
    const float base = std::max(m_status.pointScale, 0.01f);
    L.narrow = L.display.x / base < kNarrowWidth;
    L.compact = std::min(L.display.x, L.display.y) / base < kNarrowWidth;
    L.margin = pt(L.compact ? th::kCompactMargin : th::kMargin);
    L.left = L.safe[3] + L.margin;
    L.top = L.safe[0] + L.margin;
    L.right = L.display.x - L.safe[1] - L.margin;
    L.bottom = L.display.y - L.safe[2] - L.margin;

    // Barras de arriba. A la izquierda, en una tableta: acciones, guardar y centrar, y
    // Ajustes, Selección y Transformar; en un teléfono: acciones, NDI y Modificar. Si no
    // caben con las de la derecha (un teléfono estrecho), los botones se estrechan un poco.
    const float barHeight = pt(th::kBarHeight);
    const float pad = pt(th::kBarPadding);
    const float leftButtons = L.narrow ? 3.0f : 6.0f;
    const float rightButtons = 5.0f;
    const float room = (L.right - L.left) - pt(8.0f) - pad * 4.0f;
    const float button = std::max(pt(36.0f), std::min(pt(th::kBarButtonWidth), room / (leftButtons + rightButtons)));
    L.barButton = button;
    const float leftWidth = pad * 2.0f + button * leftButtons;
    L.leftBar = ImRect(L.left, L.top, L.left + leftWidth, L.top + barHeight);
    L.rightBar = ImRect(L.right - (pad * 2.0f + button * rightButtons), L.top, L.right, L.top + barHeight);
    if (L.narrow) {
        L.ndiBar = ImRect();
    } else {
        const float x = L.leftBar.Max.x + pt(10.0f);
        L.ndiBar = ImRect(x, L.top, x + ndiCapsuleWidth(), L.top + barHeight);
    }

    // Barra lateral: tamaño, cuentagotas, opacidad, separador, deshacer y rehacer.
    const bool compact = L.compact;
    const float sideWidth = pt(compact ? 48.0f : th::kSidebarWidth);
    const float control = pt(compact ? 32.0f : 36.0f);
    const float step = pt(compact ? 32.0f : 34.0f);
    const float gap = pt(compact ? 10.0f : 12.0f);
    const float padY = pt(8.0f);
    float slider = pt(compact ? 80.0f : 120.0f);
    auto total = [&](float sliderHeight) {
        return padY + sliderHeight + gap + control + gap + sliderHeight + gap + ui::hairline() + pt(8.0f) + step +
               pt(4.0f) + step + padY;
    };
    const float areaTop = L.leftBar.Max.y + pt(12.0f);
    const float available = L.bottom - areaTop;
    if (total(slider) > available) {
        slider = std::max(pt(40.0f), slider - (total(slider) - available) * 0.5f);
    }
    const float height = total(slider);
    const float y0 = L.narrow ? areaTop + pt(28.0f) : std::max(areaTop, (L.display.y - height) * 0.5f);
    const float x0 = m_prefs.sidebarRight ? L.right - sideWidth : L.left;
    L.sidebar = ImRect(x0, y0, x0 + sideWidth, y0 + height);
    const float cx = L.sidebar.GetCenter().x;
    const float half = control * 0.5f;
    float y = y0 + padY;
    L.sizeSlider = ImRect(cx - half, y, cx + half, y + slider);
    y += slider + gap;
    L.eyedropper = ImRect(cx - half, y, cx + half, y + control);
    y += control + gap;
    L.opacitySlider = ImRect(cx - half, y, cx + half, y + slider);
    y += slider + gap;
    L.separatorY = y;
    y += ui::hairline() + pt(8.0f);
    L.undo = ImRect(cx - half, y, cx + half, y + step);
    y += step + pt(4.0f);
    L.redo = ImRect(cx - half, y, cx + half, y + step);

    L.popoverTop = L.leftBar.Max.y + pt(th::kPopoverGap);
    L.toastTop = L.leftBar.Max.y + pt(14.0f);

    // Lo que tapa la interfaz, para que el lienzo ajustado quede libre. En una tableta el
    // lienzo queda centrado entre la barra lateral y el otro lado (mismo margen a los
    // dos); en un teléfono no hay sitio que perder y la barra lateral flota sobre el
    // borde del lienzo, como en Procreate Pocket.
    const float clearance = pt(8.0f);
    m_insets[0] = L.leftBar.Max.y + clearance;
    m_insets[2] = L.display.y - L.bottom;
    if (L.compact) {
        m_insets[1] = L.display.x - L.right;
        m_insets[3] = L.left;
    } else {
        const float sideInset = m_prefs.sidebarRight ? L.display.x - L.sidebar.Min.x + clearance
                                                     : L.sidebar.Max.x + clearance;
        m_insets[1] = m_insets[3] = sideInset;
    }
}

float Ui::ndiCapsuleWidth() const {
    if (m_status.ndiRunning && m_status.ndiError.empty()) {
        char count[16];
        std::snprintf(count, sizeof(count), "%d", m_status.ndiConnections);
        const float badge = std::max(pt(22.0f), ui::measure(Weight::Bold, th::kCaption, count).x + pt(14.0f));
        return pt(14.0f) + pt(8.0f) + pt(8.0f) + ui::trackedWidth(Weight::Bold, th::kFootnote, "EN VIVO", 0.06f) +
               pt(8.0f) + badge + pt(16.0f);
    }
    return pt(14.0f) + pt(20.0f) + pt(8.0f) + ui::measure(Weight::SemiBold, th::kSubhead, "NDI").x + pt(16.0f);
}

// -----------------------------------------------------------------------------
// Herramientas y color
// -----------------------------------------------------------------------------

void Ui::applyPreset(Canvas& canvas, Tool tool) {
    BrushSettings& settings = canvas.brushSettings();
    const ToolPreset& preset = m_presets[static_cast<int>(tool)];
    settings.brush = toolBrush(tool);
    settings.radius = toolRadius(tool);
    settings.opacity = std::clamp(preset.opacity, 0.01f, 1.0f);
    settings.eraser = tool == Tool::Eraser;
    settings.smudge = tool == Tool::Smudge;
}

void Ui::syncBrush(Canvas& canvas) {
    // Durante un trazo no se toca nada: cambiar de pincel lo terminaría.
    if (!canvas.stroking()) {
        applyPreset(canvas, m_tool);
    }
}

void Ui::prepareStroke(Canvas& canvas, bool eraserTip) { applyPreset(canvas, eraserTip ? Tool::Eraser : m_tool); }

void Ui::strokeStarted(const Canvas& canvas, bool erasing) {
    // Difuminar no pone color: no cuenta como usado.
    if (!erasing && !canvas.brushSettings().smudge) {
        pushRecent(canvas.brushSettings().color);
    }
}

void Ui::selectTool(Tool tool) { m_tool = tool; }

int Ui::toolSlot(Tool tool) {
    switch (tool) {
    case Tool::Brush:
        return 0;
    case Tool::Smudge:
        return 1;
    case Tool::Eraser:
        return 2;
    }
    return 0;
}

void Ui::setColor(Canvas& canvas, const float rgb[3]) {
    float* color = canvas.brushSettings().color;
    for (int i = 0; i < 3; ++i) {
        color[i] = std::clamp(rgb[i], 0.0f, 1.0f);
    }
}

void Ui::pushRecent(const float rgb[3]) {
    int found = -1;
    for (int i = 0; i < m_recentCount; ++i) {
        if (sameColor(m_recent[i], rgb)) {
            found = i;
            break;
        }
    }
    if (found == 0) {
        return;
    }
    const int last = found >= 0 ? found : std::min(m_recentCount, 5);
    for (int i = last; i > 0; --i) {
        std::copy(m_recent[i - 1], m_recent[i - 1] + 3, m_recent[i]);
    }
    std::copy(rgb, rgb + 3, m_recent[0]);
    if (found < 0) {
        m_recentCount = std::min(m_recentCount + 1, 6);
    }
}

void Ui::showPicker(ImVec2 position, const float* rgb, bool touch) {
    m_picker.active = true;
    m_picker.touch = touch;
    m_picker.position = position;
    m_picker.valid = rgb != nullptr;
    if (rgb) {
        std::copy(rgb, rgb + 3, m_picker.rgb);
    }
}

void Ui::pickColor(Canvas& canvas, const float rgb[3]) { setColor(canvas, rgb); }

// -----------------------------------------------------------------------------
// Paneles, avisos y teclado
// -----------------------------------------------------------------------------

void Ui::togglePanel(Panel panel) {
    m_panel = m_panel == panel ? Panel::None : panel;
    m_layerMenu = false;
    m_hexEditing = false;
}

void Ui::closePanels() {
    m_panel = Panel::None;
    m_layerMenu = false;
    m_hexEditing = false;
}

bool Ui::closeTopmost(Canvas* canvas) {
    if (m_dialog != Dialog::None) {
        m_dialog = Dialog::None;
        return true;
    }
    if (m_layerMenu) {
        m_layerMenu = false;
        return true;
    }
    if (m_blendPage && m_panel == Panel::Layers) {
        m_blendPage = false;   // vuelve a la lista de capas
        return true;
    }
    if (m_brushPage && m_panel == Panel::Brushes) {
        m_brushPage = false;   // vuelve a la lista de pinceles
        return true;
    }
    if (m_panel != Panel::None) {
        closePanels();
        return true;
    }
    if (m_eyedropperArmed) {
        m_eyedropperArmed = false;
        return true;
    }
    // Después, la herramienta: el lazo a medias se descarta, la transformación y el ajuste
    // de imagen se cancelan y la selección vuelve a pintar (la selección se queda).
    if (canvas && m_canvasTool == CanvasTool::Select) {
        if (m_select.pendingPolygon()) {
            m_select.dropPolygon();
        } else {
            setCanvasTool(*canvas, CanvasTool::Paint);
        }
        return true;
    }
    if (canvas && m_canvasTool == CanvasTool::Transform) {
        m_transform.cancel(*canvas);
        canvas->cancelTransform();
        m_transform.stop();
        m_canvasTool = m_toolBeforeTransform;
        return true;
    }
    if (canvas && m_canvasTool == CanvasTool::Adjust) {
        finishAdjust(*canvas, false);
        return true;
    }
    return false;
}

void Ui::notify(std::string text, Notice kind, uint32_t durationMs) {
    m_toast.text = std::move(text);
    m_toast.kind = kind;
    m_toast.until = SDL_GetTicks() + durationMs;
}

void Ui::showUndo(bool redo, bool done) {
    if (done) {
        notify(redo ? "Rehacer" : "Deshacer", redo ? Notice::Redo : Notice::Undo, 1100);
    } else {
        notify(redo ? "Nada que rehacer" : "Nada que deshacer", Notice::Info, 1400);
    }
}

uint64_t Ui::wakeDeadline() const {
    const uint64_t toast = m_toast.text.empty() ? 0 : m_toast.until;
    if (m_brushSaveAt == 0 || (toast != 0 && toast < m_brushSaveAt)) {
        return toast;
    }
    return m_brushSaveAt;
}

void Ui::undo(Canvas& canvas, bool redo) { showUndo(redo, undoStep(canvas, redo)); }

void Ui::handleKeys(Canvas& canvas) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool back = ImGui::IsKeyPressed(ImGuiKey_AppBack, false);
    if (back || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // Arrastrando el color, lo cancela.
        if (m_drop.dragging && !m_drop.finished) {
            cancelDrop(canvas);
            return;
        }
        // El botón atrás cierra lo último que se abrió; con todo cerrado, pregunta si salir.
        if (!closeTopmost(&canvas) && back) {
            askExit();
        }
        return;
    }
    if (io.WantTextInput || m_dialog != Dialog::None) {
        return;
    }
    const bool command = io.KeyCtrl || io.KeySuper;
    if (command) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, true)) {
            undo(canvas, io.KeyShift);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Y, true)) {
            undo(canvas, true);
        } else {
            toolKeys(canvas);
        }
        return;
    }
    if (io.KeyAlt) {
        return;
    }
    if (toolKeys(canvas)) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        paintWith(canvas, Tool::Brush);
    } else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
        paintWith(canvas, Tool::Smudge);
    } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        paintWith(canvas, Tool::Eraser);
    } else if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true) || ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) {
        ToolPreset& preset = m_presets[static_cast<int>(m_tool)];
        const float step = ImGui::IsKeyDown(ImGuiKey_RightBracket) ? 0.02f : -0.02f;
        preset.size = std::clamp(preset.size + step, 0.0f, 1.0f);
        scheduleBrushSave();
    }
}

// -----------------------------------------------------------------------------
// Barras
// -----------------------------------------------------------------------------

void Ui::drawScrim() {
    // Detrás de las barras: un toque fuera del panel lo cierra y no pinta.
    const ImRect screen(ImVec2(0.0f, 0.0f), m_layout.display);
    ui::beginSurface("##scrim", screen, true, false);
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    const ImGuiID id = ImGui::GetID("##outside");
    ImGui::ItemAdd(screen, id);
    bool hovered = false;
    bool held = false;
    if (ImGui::ButtonBehavior(screen, id, &hovered, &held,
                              ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus)) {
        closePanels();
    }
    ui::endSurface();
}

void Ui::beginBar(const char* name, const ImRect& rect, float radius) {
    ui::beginSurface(name, rect, true, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(18.0f), pt(4.0f), 0.16f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kBarTint);
}

bool Ui::barButton(const char* id, const ImRect& rect, const char* glyph, bool open, bool selected, bool enabled) {
    const ImGuiID gid = ImGui::GetID(id);
    const Press press = ui::pressable(gid, rect, enabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::highlight(dl, gid, rect, rect.GetHeight() * 0.5f, open, press);
    const float accent = ui::anim::follow(gid + 3u, selected ? 1.0f : 0.0f, 20.0f);
    const ImU32 color = enabled ? ui::mix(th::kLabel, th::kAccent, accent) : th::kDisabledLabel;
    ui::icon(dl, glyph, rect.GetCenter(), th::kIconSize, color);
    return press.clicked;
}

void Ui::drawTopBars(Canvas& canvas, UiRequests& requests) {
    const Layout& L = m_layout;
    const float pad = pt(th::kBarPadding);
    const float width = L.barButton;
    const float height = pt(th::kBarButtonHeight);
    auto slot = [&](const ImRect& bar, int index) {
        const float x = bar.Min.x + pad + width * static_cast<float>(index);
        const float y = bar.GetCenter().y - height * 0.5f;
        return ImRect(x, y, x + width, y + height);
    };

    // Izquierda: acciones, guardar, centrar, ajustes, selección y transformar (en un
    // teléfono, acciones, NDI y Modificar, que abre las tres últimas).
    beginBar("##bar-left", L.leftBar, L.leftBar.GetHeight() * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (barButton("##actions", slot(L.leftBar, 0), icon::kWrench, m_panel == Panel::Actions, false)) {
        togglePanel(Panel::Actions);
    }
    if (L.narrow) {
        const ImRect rect = slot(L.leftBar, 1);
        const ImGuiID id = ImGui::GetID("##ndi");
        const Press press = ui::pressable(id, rect);
        ui::highlight(dl, id, rect, rect.GetHeight() * 0.5f, m_panel == Panel::Ndi, press);
        const bool live = m_status.ndiRunning;
        const float on = ui::anim::follow(id + 3u, live ? 1.0f : 0.0f, 14.0f);
        if (on > 0.002f) {
            dl->AddCircleFilled(rect.GetCenter(), pt(15.0f), ui::withAlpha(th::kLive, on), 0);
        }
        const ImU32 idle = m_status.ndiAvailable ? IM_COL32(235, 235, 245, 180) : th::kTertiaryLabel;
        ui::icon(dl, icon::kRadio, rect.GetCenter(), 20.0f, ui::mix(idle, th::kLabel, on));
        if (press.clicked) {
            togglePanel(Panel::Ndi);
        }
        m_modifyButton = slot(L.leftBar, 2);
        if (barButton("##modify", m_modifyButton, icon::kModify, m_panel == Panel::Modify,
                      m_canvasTool != CanvasTool::Paint)) {
            togglePanel(Panel::Modify);
        }
    } else {
        const ImRect save = slot(L.leftBar, 1);
        if (m_status.exporting) {
            ui::pressable("##save", save, false);
            ui::spinner(dl, save.GetCenter(), pt(9.0f), th::kSecondaryLabel);
        } else if (barButton("##save", save, icon::kImageDown, false, false)) {
            requests.savePng = true;
            closePanels();
        }
        if (barButton("##fit", slot(L.leftBar, 2), icon::kScan, false, false)) {
            requests.fitView = true;
            closePanels();
        }
        // Las herramientas que cambian lo que ya está dibujado, separadas por una línea.
        const float divider = std::round(slot(L.leftBar, 3).Min.x);
        dl->AddLine(ImVec2(divider, L.leftBar.Min.y + pt(14.0f)), ImVec2(divider, L.leftBar.Max.y - pt(14.0f)),
                    IM_COL32(255, 255, 255, 36), ui::hairline());
        const bool adjusting = m_canvasTool == CanvasTool::Adjust;
        const bool selecting = m_canvasTool == CanvasTool::Select;
        const bool transforming = m_canvasTool == CanvasTool::Transform;
        m_adjustButton = slot(L.leftBar, 3);
        if (barButton("##adjust", m_adjustButton, icon::kAdjust, m_panel == Panel::Adjust, adjusting)) {
            togglePanel(Panel::Adjust);
        }
        if (barButton("##select", slot(L.leftBar, 4), icon::kSelection, false, selecting)) {
            closePanels();
            setCanvasTool(canvas, selecting ? CanvasTool::Paint : CanvasTool::Select);
        }
        if (barButton("##transform", slot(L.leftBar, 5), icon::kTransform, false, transforming)) {
            closePanels();
            // Tocarla otra vez aplica la transformación y vuelve a lo de antes.
            setCanvasTool(canvas, transforming ? m_toolBeforeTransform : CanvasTool::Transform);
        }
    }
    ui::endSurface();

    if (!L.narrow) {
        drawNdiCapsule();
    }

    // Derecha: pincel, difuminar, borrador, capas y color.
    beginBar("##bar-right", L.rightBar, L.rightBar.GetHeight() * 0.5f);
    dl = ImGui::GetWindowDrawList();
    const Tool tools[3] = {Tool::Brush, Tool::Smudge, Tool::Eraser};
    const char* ids[3] = {"##brush", "##smudge", "##eraser"};
    const char* glyphs[3] = {icon::kBrush, icon::kSmudge, icon::kEraser};
    for (int i = 0; i < 3; ++i) {
        const bool current = m_canvasTool == CanvasTool::Paint && m_tool == tools[i];
        if (barButton(ids[i], slot(L.rightBar, toolSlot(tools[i])), glyphs[i], current && m_panel == Panel::Brushes,
                      current)) {
            // Tocar la herramienta que ya está elegida abre su biblioteca de pinceles.
            if (current) {
                togglePanel(Panel::Brushes);
            } else {
                paintWith(canvas, tools[i]);
                if (m_panel == Panel::Brushes) {
                    closePanels();
                }
            }
        }
    }
    if (barButton("##layers", slot(L.rightBar, 3), icon::kLayers, m_panel == Panel::Layers, false)) {
        togglePanel(Panel::Layers);
    }
    {
        const ImRect rect = slot(L.rightBar, 4);
        const ImGuiID id = ImGui::GetID("##color");
        const Press press = ui::pressable(id, rect);
        // Arrastrarlo lleva el color al lienzo para rellenar (un toque abre el panel).
        colorDrop(canvas, id);
        ui::highlight(dl, id, rect, rect.GetHeight() * 0.5f, m_panel == Panel::Color, press);
        const float squeeze = ui::anim::follow(id + 3u, press.held ? 1.0f : 0.0f, 24.0f);
        const float radius = pt(13.0f) * (1.0f - 0.08f * squeeze);
        const ImVec2 center = rect.GetCenter();
        dl->AddCircleFilled(center, radius + pt(3.0f), IM_COL32(0, 0, 0, 90), 0);
        dl->AddCircleFilled(center, radius + pt(2.0f), IM_COL32_WHITE, 0);
        dl->AddCircleFilled(center, radius, ui::fromFloat(canvas.brushSettings().color), 0);
        if (press.clicked) {
            if (m_panel != Panel::Color) {
                std::copy(canvas.brushSettings().color, canvas.brushSettings().color + 3, m_previousColor);
            }
            togglePanel(Panel::Color);
        }
    }
    ui::endSurface();
}

void Ui::drawNdiCapsule() {
    const Layout& L = m_layout;
    const bool live = m_status.ndiRunning && m_status.ndiError.empty();
    const bool failed = m_status.ndiRunning && !m_status.ndiError.empty();
    const ImGuiID id = ImHashStr("##ndi-capsule");
    const float width = ui::anim::follow(id, L.ndiBar.GetWidth(), 20.0f, 0.1f);
    const ImRect rect(L.ndiBar.Min, ImVec2(L.ndiBar.Min.x + width, L.ndiBar.Max.y));
    const float radius = rect.GetHeight() * 0.5f;

    ui::beginSurface("##bar-ndi", rect, true, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(18.0f), pt(4.0f), 0.16f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kBarTint);
    // En vivo, la cápsula se vuelve roja.
    const float on = ui::anim::follow(id + 1u, live ? 1.0f : 0.0f, 12.0f);
    if (on > 0.002f) {
        dl->AddRectFilled(rect.Min, rect.Max, ui::withAlpha(th::kLive, on), radius);
    }
    const Press press = ui::pressable("##ndi", rect);
    ui::highlight(dl, ImGui::GetID("##ndi"), rect, radius, m_panel == Panel::Ndi, press);

    const float cy = rect.GetCenter().y;
    float x = rect.Min.x + pt(14.0f);
    if (live) {
        const ImVec2 dot(x + pt(4.0f), cy);
        dl->AddCircleFilled(dot, pt(8.0f), IM_COL32(255, 255, 255, 70), 0);
        dl->AddCircleFilled(dot, pt(4.0f), IM_COL32_WHITE, 0);
        x += pt(16.0f);
        ui::tracked(dl, Weight::Bold, th::kFootnote, ImVec2(x, cy), th::kLabel, "EN VIVO", 0.06f);
        x += ui::trackedWidth(Weight::Bold, th::kFootnote, "EN VIVO", 0.06f) + pt(8.0f);
        char count[16];
        std::snprintf(count, sizeof(count), "%d", m_status.ndiConnections);
        const float badge = std::max(pt(22.0f), ui::measure(Weight::Bold, th::kCaption, count).x + pt(14.0f));
        const ImRect pill(ImVec2(x, cy - pt(11.0f)), ImVec2(x + badge, cy + pt(11.0f)));
        dl->AddRectFilled(pill.Min, pill.Max, IM_COL32(255, 255, 255, 61), pt(11.0f));
        ui::label(dl, Weight::Bold, th::kCaption, pill.GetCenter(), Align::Center, th::kLabel, count);
    } else {
        ImU32 color = m_status.ndiAvailable ? IM_COL32(235, 235, 245, 180) : th::kTertiaryLabel;
        if (failed) {
            color = th::kOrange;
        }
        ui::icon(dl, failed ? icon::kCircleAlert : icon::kRadio, ImVec2(x + pt(10.0f), cy), 20.0f, color);
        x += pt(28.0f);
        ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(x, cy), Align::Left,
                  m_status.ndiAvailable ? th::kLabel : th::kSecondaryLabel, "NDI");
    }
    if (press.clicked) {
        togglePanel(Panel::Ndi);
    }
    ui::endSurface();
}

// -----------------------------------------------------------------------------
// Barra lateral
// -----------------------------------------------------------------------------

void Ui::drawSidebar(Canvas& canvas) {
    const Layout& L = m_layout;
    beginBar("##sidebar", L.sidebar, L.sidebar.GetWidth() * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ToolPreset& preset = m_presets[static_cast<int>(m_tool)];
    // Lo que va dibujado al pie de un deslizador es oscuro donde lo cubre el relleno y
    // claro donde no.
    auto twoTone = [&](const char* id, const ImRect& rect, float value, auto&& draw) {
        const float shown = ui::anim::value(ImGui::GetID(id) + 1u, value);
        const float cut = rect.Max.y - rect.GetHeight() * shown;
        dl->PushClipRect(rect.Min, ImVec2(rect.Max.x, cut), true);
        draw(IM_COL32(255, 255, 255, 190));
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(rect.Min.x, cut), rect.Max, true);
        draw(IM_COL32(28, 28, 30, 255));
        dl->PopClipRect();
    };

    // Tamaño.
    bool sizeActive = false;
    if (ui::fillSlider("##size", L.sizeSlider, &preset.size, &sizeActive)) {
        scheduleBrushSave();
    }
    {
        const ImRect& r = L.sizeSlider;
        const float dot = pt(2.0f) + pt(3.5f) * preset.size;
        const ImVec2 center(r.GetCenter().x, r.Max.y - pt(15.0f));
        twoTone("##size", r, preset.size, [&](ImU32 color) { dl->AddCircleFilled(center, dot, color, 0); });
    }

    // Cuentagotas.
    {
        const ImRect& r = L.eyedropper;
        const ImGuiID id = ImGui::GetID("##eyedropper");
        const Press press = ui::pressable(id, r);
        const float on = ui::anim::follow(id, m_eyedropperArmed ? 1.0f : 0.0f, 20.0f);
        const float radius = r.GetWidth() / 3.0f;
        dl->AddRectFilled(r.Min, r.Max, ui::mix(th::kFill, th::kAccent, on), radius);
        const float touch = ui::anim::follow(id + 1u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
        if (touch > 0.002f) {
            dl->AddRectFilled(r.Min, r.Max, ui::withAlpha(th::kPressed, touch), radius);
        }
        ui::icon(dl, icon::kPipette, r.GetCenter(), 18.0f, th::kLabel);
        if (press.clicked) {
            m_eyedropperArmed = !m_eyedropperArmed;
            closePanels();
        }
    }

    // Opacidad.
    bool opacityActive = false;
    float opacity = preset.opacity;
    if (ui::fillSlider("##opacity", L.opacitySlider, &opacity, &opacityActive)) {
        preset.opacity = std::max(opacity, 0.01f);
        scheduleBrushSave();
    }
    {
        const ImRect& r = L.opacitySlider;
        const ImVec2 center(r.GetCenter().x, r.Max.y - pt(16.0f));
        twoTone("##opacity", r, preset.opacity, [&](ImU32 color) { ui::icon(dl, icon::kDroplet, center, 15.0f, color); });
    }

    ui::separator(dl, L.sidebar.GetCenter().x - pt(12.0f), L.sidebar.GetCenter().x + pt(12.0f), L.separatorY,
                  IM_COL32(255, 255, 255, 36));

    // Deshacer y rehacer. Mantenerlos pulsados repite.
    auto historyButton = [&](const char* id, const ImRect& r, const char* glyph, bool enabled, bool redo) {
        const ImGuiID gid = ImGui::GetID(id);
        const Press press = ui::pressable(gid, r, enabled);
        ui::highlight(dl, gid, r, r.GetHeight() / 2.8f, false, press);
        ui::icon(dl, glyph, r.GetCenter(), 20.0f, enabled ? th::kLabel : th::kDisabledLabel);
        if (press.clicked && !(m_repeatId == gid && m_repeated)) {
            undoStep(canvas, redo);
        }
        if (press.held) {
            const double now = ImGui::GetTime();
            if (m_repeatId != gid) {
                m_repeatId = gid;
                m_repeatStart = now;
                m_repeatLast = now;
                m_repeated = false;
            } else if (now - m_repeatStart > kRepeatDelay && now - m_repeatLast > kRepeatInterval) {
                undoStep(canvas, redo);
                m_repeatLast = now;
                m_repeated = true;
            }
            ui::anim::keepAlive();
        } else if (m_repeatId == gid) {
            m_repeatId = 0;
            m_repeated = false;
        }
    };
    historyButton("##undo", L.undo, icon::kUndo, canUndo(canvas), false);
    historyButton("##redo", L.redo, icon::kRedo, canvas.canRedo(), true);

    if (sizeActive || opacityActive) {
        m_hudKind = sizeActive ? 0 : 1;
    }
    m_hudActive = sizeActive || opacityActive;
    ui::endSurface();
}

void Ui::drawHud(Canvas& canvas) {
    const ImGuiID id = ImHashStr("##hud");
    const float p = ui::anim::followFrom(id, 0.0f, m_hudActive ? 1.0f : 0.0f, m_hudActive ? 22.0f : 7.0f);
    if (p <= 0.002f) {
        return;
    }
    const Layout& L = m_layout;
    const float side = pt(L.compact ? 100.0f : 116.0f);
    const ImRect& slider = m_hudKind == 0 ? L.sizeSlider : L.opacitySlider;
    const float x = m_prefs.sidebarRight ? L.sidebar.Min.x - pt(12.0f) - side : L.sidebar.Max.x + pt(12.0f);
    const float y = std::clamp(slider.GetCenter().y - side * 0.5f, L.top, std::max(L.top, L.bottom - side));
    const ImRect rect(x, y, x + side, y + side);
    const float radius = pt(26.0f);

    ui::beginSurface("##hud", rect, false, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(28.0f), pt(10.0f), 0.24f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, IM_COL32(40, 40, 44, 184));

    const ToolPreset& preset = m_presets[static_cast<int>(m_tool)];
    // El borrador y difuminar no pintan con el color: se ven en blanco.
    const bool eraser = m_tool != Tool::Brush;
    const ImU32 color = eraser ? IM_COL32_WHITE : ui::fromFloat(canvas.brushSettings().color);
    const ImVec2 center(rect.GetCenter().x, rect.Min.y + side * 0.4f);
    char text[32];
    if (m_hudKind == 0) {
        // El círculo tiene el tamaño que tendrá el trazo en la pantalla (con el zoom).
        const float brushRadius = toolRadius(m_tool);
        const float onScreen = brushRadius * m_status.canvasZoom / std::max(m_status.pixelsPerUnit, 0.01f);
        const float r = std::clamp(onScreen, pt(1.5f), side * 0.27f);
        if (eraser) {
            dl->AddCircle(center, r, IM_COL32_WHITE, 0, pt(2.0f));
        } else {
            dl->AddCircleFilled(center, r, color, 0);
            dl->AddCircle(center, r, IM_COL32(255, 255, 255, 64), 0, ui::hairline());
        }
        std::snprintf(text, sizeof(text), "%d px", static_cast<int>(std::lround(brushRadius * 2.0f)));
    } else {
        // Damero debajo para que se note la transparencia.
        const float r = side * 0.2f;
        dl->AddCircleFilled(center, r, IM_COL32(255, 255, 255, 255), 0);
        for (int quadrant = 0; quadrant < 4; quadrant += 2) {
            const float a0 = IM_PI * 0.5f * static_cast<float>(quadrant);
            dl->PathLineTo(center);
            dl->PathArcTo(center, r, a0, a0 + IM_PI * 0.5f, 12);
            dl->PathFillConvex(IM_COL32(204, 204, 204, 255));
        }
        dl->AddCircleFilled(center, r, ui::withAlpha(color, preset.opacity), 0);
        dl->AddCircle(center, r, IM_COL32(255, 255, 255, 64), 0, ui::hairline());
        // En difuminar, el deslizador de la opacidad es su fuerza.
        std::snprintf(text, sizeof(text), m_tool == Tool::Smudge ? "Fuerza %d %%" : "%d %%",
                      static_cast<int>(std::lround(preset.opacity * 100.0f)));
    }
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(rect.GetCenter().x, rect.Max.y - side * 0.17f),
              Align::Center, th::kLabel, text);
    ui::transform(mark, rect.GetCenter(), 0.9f + 0.1f * p, ImVec2(0.0f, 0.0f), p);
    ui::endSurface();
}

void Ui::drawPicker(Canvas& canvas) {
    const ImGuiID id = ImHashStr("##picker");
    const float p = ui::anim::followFrom(id, 0.0f, m_picker.active ? 1.0f : 0.0f, 24.0f);
    if (p <= 0.002f) {
        return;
    }
    const Layout& L = m_layout;
    const float outer = pt(46.0f);
    const float ring = pt(15.0f);
    ImVec2 center = m_picker.position;
    if (m_picker.touch) {
        center.y -= pt(92.0f);   // encima del dedo
    }
    center.x = std::clamp(center.x, outer + pt(4.0f), std::max(outer + pt(4.0f), L.display.x - outer - pt(4.0f)));
    center.y = std::clamp(center.y, outer + pt(4.0f), std::max(outer + pt(4.0f), L.display.y - outer - pt(4.0f)));
    const float margin = pt(24.0f);
    const ImRect rect(ImVec2(center.x - outer - margin, center.y - outer - margin),
                      ImVec2(center.x + outer + margin, center.y + outer + margin));

    ui::beginSurface("##picker", rect, false, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    const ImRect circle(ImVec2(center.x - outer, center.y - outer), ImVec2(center.x + outer, center.y + outer));
    ui::pushUnclipped(dl);
    ui::shadow(dl, circle, outer, pt(22.0f), pt(6.0f), 0.3f);
    ui::popUnclipped(dl);
    // Arriba el color que hay debajo; abajo, el actual.
    const ImU32 current = ui::fromFloat(canvas.brushSettings().color);
    const ImU32 picked = m_picker.valid ? ui::fromFloat(m_picker.rgb) : current;
    const float middle = outer - ring * 0.5f;
    dl->PathArcTo(center, middle, IM_PI, 2.0f * IM_PI, 40);
    dl->PathStroke(picked, 0, ring);
    dl->PathArcTo(center, middle, 0.0f, IM_PI, 40);
    dl->PathStroke(current, 0, ring);
    dl->AddCircle(center, outer, IM_COL32(255, 255, 255, 235), 0, pt(1.5f));
    dl->AddCircle(center, outer - ring, IM_COL32(255, 255, 255, 235), 0, pt(1.5f));
    if (!m_picker.touch) {
        dl->AddCircle(center, pt(3.0f), IM_COL32(255, 255, 255, 235), 0, pt(1.5f));
        dl->AddCircle(center, pt(4.5f), IM_COL32(0, 0, 0, 90), 0, ui::hairline());
    }
    ui::transform(mark, center, 0.8f + 0.2f * p, ImVec2(0.0f, 0.0f), p);
    ui::endSurface();
}

void Ui::drawToast() {
    const uint64_t now = SDL_GetTicks();
    const bool visible = !m_toast.text.empty() && now < m_toast.until;
    const ImGuiID id = ImHashStr("##toast");
    const float p = ui::anim::followFrom(id, 0.0f, visible ? 1.0f : 0.0f, visible ? 16.0f : 9.0f);
    if (p <= 0.002f) {
        if (!visible) {
            m_toast.text.clear();
        }
        return;
    }
    const Layout& L = m_layout;
    const float height = pt(44.0f);
    const float iconSide = pt(20.0f);
    const float gap = pt(10.0f);
    const float padLeft = pt(12.0f);
    const float padRight = pt(18.0f);
    const float maxText = std::max(pt(80.0f), (L.right - L.left) - padLeft - iconSide - gap - padRight);
    const std::string shown = ui::fitText(Weight::SemiBold, th::kSubhead, m_toast.text.c_str(), maxText);
    const float width = padLeft + iconSide + gap + ui::measure(Weight::SemiBold, th::kSubhead, shown.c_str()).x +
                        padRight;
    const float x = std::round((L.display.x - width) * 0.5f);
    const ImRect rect(x, L.toastTop, x + width, L.toastTop + height);

    ui::beginSurface("##toast", rect, false, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, height * 0.5f, pt(26.0f), pt(8.0f), 0.24f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, height * 0.5f, IM_COL32(40, 40, 44, 184));
    const ImVec2 iconCenter(rect.Min.x + padLeft + iconSide * 0.5f, rect.GetCenter().y);
    switch (m_toast.kind) {
    case Notice::Success:
        ui::icon(dl, icon::kCircleCheck, iconCenter, 20.0f, th::kGreen);
        break;
    case Notice::Warning:
        ui::icon(dl, icon::kCircleAlert, iconCenter, 20.0f, th::kOrange);
        break;
    case Notice::Error:
        ui::icon(dl, icon::kCircleAlert, iconCenter, 20.0f, th::kRed);
        break;
    case Notice::Progress:
        ui::spinner(dl, iconCenter, pt(9.0f), th::kLabel);
        break;
    case Notice::Undo:
        ui::icon(dl, icon::kUndo, iconCenter, 20.0f, th::kLabel);
        break;
    case Notice::Redo:
        ui::icon(dl, icon::kRedo, iconCenter, 20.0f, th::kLabel);
        break;
    case Notice::Info:
        ui::icon(dl, icon::kInfo, iconCenter, 20.0f, th::kAccent);
        break;
    }
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(iconCenter.x + iconSide * 0.5f + gap, rect.GetCenter().y),
              Align::Left, th::kLabel, shown.c_str());
    ui::transform(mark, rect.GetCenter(), 1.0f, ImVec2(0.0f, -pt(10.0f) * (1.0f - p)), p);
    ui::endSurface();
}
