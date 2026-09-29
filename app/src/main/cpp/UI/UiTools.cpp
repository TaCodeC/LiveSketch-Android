// Herramientas Selección y Transformar: su entrada sobre el lienzo, deshacer, los atajos
// de teclado, la barra de opciones de abajo, lo que dibujan encima del lienzo y sus
// paneles (difuminar y, en un teléfono, Modificar, que las abre).
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"
#include "UI/PanelParts.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;
using ui::parts::pressFeedback;

namespace {

// --- Barra de opciones (pt) ---
constexpr float kDockPad = 6.0f;
constexpr float kCellHeight = 50.0f;
constexpr float kButtonHeight = 38.0f;
constexpr float kItemGap = 4.0f;
constexpr float kGroupGap = 13.0f;   // entre grupos, con una línea en medio
constexpr float kRowRule = 7.0f;     // entre filas, con una línea en medio
constexpr float kCellWidth = 64.0f;  // ancho cómodo de una celda
constexpr float kMinSingleRow = 60.0f;   // en una fila solo si las celdas miden al menos esto
constexpr float kButtonWeight = 1.55f;   // un botón de texto mide esto en celdas

// Elemento de la barra: una celda (icono con su nombre debajo) que se elige o que hace
// algo, o un botón con texto.
struct DockItem {
    int action = 0;
    const char* glyph = nullptr;
    const char* label = nullptr;
    bool choice = false;     // se elige: fondo de acento si `selected`
    bool selected = false;
    bool enabled = true;
    bool button = false;     // botón de texto
    bool primary = false;
    int group = 0;           // entre grupos distintos hay una línea
};

struct DockRow {
    std::vector<DockItem> items;
    bool stretch = false;    // ocupa todo el ancho; si no, celdas del ancho común, centradas
};

float weight(const DockItem& item) { return item.button ? kButtonWeight : 1.0f; }

float weights(const DockRow& row) {
    float sum = 0.0f;
    for (const DockItem& item : row.items) {
        sum += weight(item);
    }
    return sum;
}

float gaps(const DockRow& row) {
    float sum = 0.0f;
    for (size_t i = 1; i < row.items.size(); ++i) {
        sum += pt(row.items[i].group != row.items[i - 1].group ? kGroupGap : kItemGap);
    }
    return sum;
}

float rowHeight(const DockRow& row) {
    float height = 0.0f;
    for (const DockItem& item : row.items) {
        height = std::max(height, pt(item.button ? kButtonHeight : kCellHeight));
    }
    return height;
}

// Ancho de celda con el que cabe una fila en `available`.
float unitFor(const DockRow& row, float available) {
    return std::min(pt(kCellWidth), (available - gaps(row)) / std::max(weights(row), 1.0f));
}

bool drawItem(ImDrawList* dl, const ImRect& r, const DockItem& item) {
    if (item.button) {
        return ui::button("##item", r, item.label, item.primary ? ui::ButtonStyle::Primary : ui::ButtonStyle::Secondary,
                          item.enabled);
    }
    bool clicked = false;
    ImU32 color = IM_COL32(235, 235, 245, 222);
    if (item.choice) {
        const ui::Choice c = ui::choice("##item", r, item.selected, item.enabled, false);
        color = ui::mix(color, th::kAccentText, c.on);
        clicked = c.press.clicked;
    } else {
        const ImGuiID id = ImGui::GetID("##item");
        const Press press = ui::pressable(id, r, item.enabled);
        pressFeedback(dl, id, r, press, pt(th::kControlRadius));
        clicked = press.clicked;
    }
    if (!item.enabled) {
        color = ui::withAlpha(color, 0.32f);
    }
    const float middle = r.GetCenter().y;
    const float height = r.GetHeight();
    ui::icon(dl, item.glyph, ImVec2(r.GetCenter().x, middle - height * 0.15f), 19.0f, color);
    ui::label(dl, Weight::Regular, th::kMicro, ImVec2(r.GetCenter().x, middle + height * 0.26f), Align::Center, color,
              item.label, r.GetWidth() - pt(2.0f));
    return clicked;
}

// Dibuja una fila. `unit`: ancho de una celda (las filas que se estiran calculan el suyo).
// Devuelve la acción tocada o -1; `watchRect` sale con el sitio de la acción `watch`.
int drawRow(ImDrawList* dl, const ImRect& rect, const DockRow& row, float unit, int watch, ImRect* watchRect) {
    if (row.stretch) {
        unit = (rect.GetWidth() - gaps(row)) / std::max(weights(row), 1.0f);
    }
    const float width = unit * weights(row) + gaps(row);
    float x = std::round(rect.GetCenter().x - width * 0.5f);
    int clicked = -1;
    for (size_t i = 0; i < row.items.size(); ++i) {
        const DockItem& item = row.items[i];
        if (i > 0) {
            if (item.group != row.items[i - 1].group) {
                const float mid = std::round(x + pt(kGroupGap) * 0.5f);
                dl->AddLine(ImVec2(mid, rect.Min.y + pt(9.0f)), ImVec2(mid, rect.Max.y - pt(9.0f)), th::kRule,
                            ui::hairline());
                x += pt(kGroupGap);
            } else {
                x += pt(kItemGap);
            }
        }
        const float w = unit * weight(item);
        const float h = pt(item.button ? kButtonHeight : kCellHeight);
        const ImRect r(ImVec2(x, rect.GetCenter().y - h * 0.5f), ImVec2(x + w, rect.GetCenter().y + h * 0.5f));
        x += w;
        ImGui::PushID(item.action);
        if (drawItem(dl, r, item)) {
            clicked = item.action;
        }
        ImGui::PopID();
        if (item.action == watch && watchRect) {
            *watchRect = r;
        }
    }
    return clicked;
}

// Sitio de la barra: abajo y centrada; en un teléfono, de lado a lado.
ImRect dockRect(const std::vector<DockRow>& rows, float left, float right, float bottom, bool fullWidth, float* unit) {
    const float available = right - left - pt(kDockPad) * 2.0f;
    float u = pt(kCellWidth);
    for (const DockRow& row : rows) {
        if (!row.stretch) {
            u = std::min(u, unitFor(row, available));
        }
    }
    float width = 0.0f;
    float height = pt(kDockPad) * 2.0f;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].stretch) {
            width = std::max(width, u * weights(rows[i]) + gaps(rows[i]));
        }
        height += rowHeight(rows[i]) + (i > 0 ? pt(kRowRule) : 0.0f);
    }
    width = fullWidth ? available : std::min(width, available);
    width += pt(kDockPad) * 2.0f;
    const float x = std::round((left + right - width) * 0.5f);
    *unit = u;
    return ImRect(x, bottom - height, x + width, bottom);
}

// La barra entera, con su cristal; aparece subiendo desde abajo.
int drawDockFrame(const char* name, bool open, const ImRect& rect, const std::vector<DockRow>& rows, float unit,
                  int watch, ImRect* watchRect) {
    const float presence = ui::anim::followFrom(ImHashStr(name), 0.0f, open ? 1.0f : 0.0f, open ? 18.0f : 24.0f);
    if (!open && presence <= 0.002f) {
        return -1;
    }
    ui::beginSurface(name, rect, open, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    const float radius = pt(th::kPanelRadius);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(30.0f), pt(8.0f), 0.26f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kPanelTint);
    float y = rect.Min.y + pt(kDockPad);
    int clicked = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            ui::separator(dl, rect.Min.x + pt(12.0f), rect.Max.x - pt(12.0f), std::round(y + pt(kRowRule) * 0.5f),
                          th::kRule);
            y += pt(kRowRule);
        }
        const float h = rowHeight(rows[i]);
        const ImRect row(ImVec2(rect.Min.x + pt(kDockPad), y), ImVec2(rect.Max.x - pt(kDockPad), y + h));
        const int action = drawRow(dl, row, rows[i], unit, watch, watchRect);
        if (action >= 0) {
            clicked = action;
        }
        y += h;
    }
    ui::transform(mark, ImVec2(rect.GetCenter().x, rect.Max.y), 1.0f, ImVec2(0.0f, pt(18.0f) * (1.0f - presence)),
                  std::min(1.0f, presence * 1.4f));
    ui::endSurface();
    return open ? clicked : -1;
}

DockItem cell(int action, const char* glyph, const char* label, int group, bool enabled = true) {
    DockItem item;
    item.action = action;
    item.glyph = glyph;
    item.label = label;
    item.enabled = enabled;
    item.group = group;
    return item;
}

DockItem choiceCell(int action, const char* glyph, const char* label, int group, bool selected) {
    DockItem item = cell(action, glyph, label, group);
    item.choice = true;
    item.selected = selected;
    return item;
}

DockItem textButton(int action, const char* label, int group, bool primary) {
    DockItem item;
    item.action = action;
    item.label = label;
    item.button = true;
    item.primary = primary;
    item.group = group;
    return item;
}

void append(std::vector<DockItem>& to, std::initializer_list<DockItem> items) { to.insert(to.end(), items); }

// Acciones de las barras.
enum SelectAction {
    kLasso = 1, kRectangle, kEllipse, kAuto,
    kAdd, kSubtract,
    kInvert, kFeather, kDeselect,
    kCopy, kCut, kPaste, kDuplicate,
    kFill, kClear,
};
enum TransformAction {
    kFree = 101, kUniform, kDistort,
    kSnap, kNearest,
    kFlipH, kFlipV, kRotate, kFit, kReset,
    kCancel, kApply,
};

// Avisa de lo que pasó con una operación de la selección o del portapapeles.
void report(Ui& ui, Canvas::Edit edit, const char* done, const char* nothing) {
    switch (edit) {
    case Canvas::Edit::Done:
        if (done) {
            ui.notify(done, Notice::Success, 1800);
        }
        break;
    case Canvas::Edit::Nothing:
        if (nothing) {
            ui.notify(nothing, Notice::Info);
        }
        break;
    case Canvas::Edit::Hidden:
        ui.notify("La capa activa está oculta", Notice::Warning);
        break;
    case Canvas::Edit::Full:
        ui.notify("No caben más capas", Notice::Warning);
        break;
    case Canvas::Edit::NoMemory:
        ui.notify("No hay memoria suficiente", Notice::Error);
        break;
    }
}

const char* emptyText(const Canvas& canvas) {
    return canvas.hasSelection() ? "No hay nada seleccionado en esta capa" : "La capa está vacía";
}

} // namespace

// -----------------------------------------------------------------------------
// Cambiar de herramienta
// -----------------------------------------------------------------------------

void Ui::setCanvasTool(Canvas& canvas, CanvasTool tool) {
    if (tool == m_canvasTool) {
        return;
    }
    const CanvasTool previous = m_canvasTool;
    // Se sale de la de ahora: lo que tuviera a medias se cierra.
    switch (previous) {
    case CanvasTool::Select:
        m_select.cancel(canvas);
        m_select.dropPolygon();
        if (canvas.feathering()) {
            canvas.endFeather(true);
        }
        if (m_panel == Panel::Feather) {
            closePanels();
        }
        break;
    case CanvasTool::Transform:
        m_transform.cancel(canvas);
        if (canvas.transforming()) {
            canvas.applyTransform();
        }
        m_transform.stop();
        break;
    case CanvasTool::Paint:
        break;
    }
    m_canvasTool = tool == CanvasTool::Transform ? previous : tool;
    m_eyedropperArmed = false;
    if (tool == CanvasTool::Transform) {
        enterTransform(canvas, false);
    }
}

bool Ui::enterTransform(Canvas& canvas, bool quiet) {
    if (m_canvasTool == CanvasTool::Transform) {
        return true;
    }
    const Canvas::Edit edit = canvas.beginTransform();
    if (edit != Canvas::Edit::Done) {
        if (!quiet || edit != Canvas::Edit::Nothing) {
            report(*this, edit, nullptr, canvas.hasSelection() ? "No hay nada seleccionado en esta capa"
                                                               : "No hay nada que transformar en esta capa");
        }
        return false;
    }
    if (m_canvasTool == CanvasTool::Select) {
        m_select.cancel(canvas);
        m_select.dropPolygon();
    }
    m_toolBeforeTransform = m_canvasTool;
    m_canvasTool = CanvasTool::Transform;
    m_transform.start(canvas);
    return true;
}

void Ui::paintWith(Canvas& canvas, Tool tool) {
    setCanvasTool(canvas, CanvasTool::Paint);
    selectTool(tool);
}

void Ui::canvasCreated() {
    m_select.dropPolygon();
    m_transform.stop();
    m_canvasTool = CanvasTool::Paint;
    if (m_panel == Panel::Feather || m_panel == Panel::Modify) {
        closePanels();
    }
}

void Ui::toolFrame(Canvas& canvas) {
    // La transformación terminó por otro lado: deshacer la canceló u otra operación la
    // aplicó (cambiar de capa, rellenar...).
    if (m_canvasTool == CanvasTool::Transform && !canvas.transforming()) {
        m_transform.stop();
        m_canvasTool = m_toolBeforeTransform;
    }
    // Difuminar: si deshacer lo quitó, se cierra su panel; si el panel se cerró (tocando
    // fuera), se guarda.
    if (m_panel == Panel::Feather && (!canvas.feathering() || m_canvasTool != CanvasTool::Select)) {
        closePanels();
    }
    if (m_panel != Panel::Feather && canvas.feathering()) {
        canvas.endFeather(true);
    }
    if (m_panel == Panel::Modify && !m_layout.narrow) {
        closePanels();
    }
}

bool Ui::selectionAnimating(const Canvas& canvas) const { return canvas.hasSelection() && !canvas.transforming(); }

// -----------------------------------------------------------------------------
// Entrada sobre el lienzo
// -----------------------------------------------------------------------------

void Ui::toolPress(Canvas& canvas, const ToolView& view, ImVec2 position, SelectOp modifier) {
    switch (m_canvasTool) {
    case CanvasTool::Select:
        m_select.press(canvas, view, position, modifier);
        break;
    case CanvasTool::Transform:
        m_transform.press(canvas, view, position);
        break;
    case CanvasTool::Paint:
        break;
    }
}

void Ui::toolDrag(Canvas& canvas, const ToolView& view, ImVec2 position, bool constrain) {
    switch (m_canvasTool) {
    case CanvasTool::Select:
        m_select.drag(canvas, view, position, constrain);
        if (m_select.adjustingThreshold() && !m_toast.text.empty()) {
            // El umbral se muestra donde salen los avisos.
            m_toast.until = std::min(m_toast.until, SDL_GetTicks());
        }
        break;
    case CanvasTool::Transform:
        m_transform.drag(canvas, view, position);
        break;
    case CanvasTool::Paint:
        break;
    }
}

void Ui::toolRelease(Canvas& canvas, const ToolView& view, ImVec2 position) {
    switch (m_canvasTool) {
    case CanvasTool::Select:
        selectionNotice(m_select.release(canvas, view, position));
        break;
    case CanvasTool::Transform:
        m_transform.release(canvas);
        break;
    case CanvasTool::Paint:
        break;
    }
}

void Ui::toolCancel(Canvas& canvas) {
    m_select.cancel(canvas);
    m_transform.cancel(canvas);
}

void Ui::selectionNotice(SelectTool::Result result) {
    switch (result) {
    case SelectTool::Result::Empty:
        notify("No se seleccionó nada", Notice::Info, 1800);
        break;
    case SelectTool::Result::NoSelection:
        notify("No hay selección de la que restar", Notice::Info);
        break;
    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// Deshacer
// -----------------------------------------------------------------------------

bool Ui::undoStep(Canvas& canvas, bool redo) {
    // Lo que la herramienta tenga a medias va primero: los puntos del lazo y los pasos de
    // la transformación (sin ellos, deshacer la cancela).
    if (!redo && m_canvasTool == CanvasTool::Select) {
        m_select.cancel(canvas);
        if (m_select.undoPoint()) {
            return true;
        }
    }
    if (!redo && m_canvasTool == CanvasTool::Transform && m_transform.undo(canvas)) {
        return true;
    }
    return redo ? canvas.redo() : canvas.undo();
}

bool Ui::canUndo(const Canvas& canvas) const {
    return canvas.canUndo() || (m_canvasTool == CanvasTool::Select && m_select.pendingPolygon());
}

void Ui::undoGesture(Canvas& canvas, bool redo) { undo(canvas, redo); }

// -----------------------------------------------------------------------------
// Operaciones
// -----------------------------------------------------------------------------

void Ui::copySelection(Canvas& canvas, bool cut) {
    const char* nothing = emptyText(canvas);
    report(*this, cut ? canvas.cutSelection() : canvas.copySelection(), cut ? "Cortado" : "Copiado", nothing);
}

void Ui::pasteClipboard(Canvas& canvas) {
    const Canvas::Edit edit = canvas.paste();
    report(*this, edit, "Pegado en una capa nueva", "No hay nada copiado");
    if (edit == Canvas::Edit::Done) {
        m_scrollToLayer = static_cast<int>(canvas.layers().active().id);
        // Como al pegar en Procreate: se puede mover enseguida.
        enterTransform(canvas, true);
    }
}

void Ui::duplicateSelection(Canvas& canvas) {
    const char* nothing = emptyText(canvas);
    const Canvas::Edit edit = canvas.duplicateSelection();
    report(*this, edit, "Duplicado en una capa nueva", nothing);
    if (edit == Canvas::Edit::Done) {
        m_scrollToLayer = static_cast<int>(canvas.layers().active().id);
        enterTransform(canvas, true);
    }
}

void Ui::clearSelected(Canvas& canvas) {
    if (canvas.hasSelection()) {
        canvas.clearLayer(canvas.layers().activeIndex());
    }
}

void Ui::fillSelected(Canvas& canvas) {
    if (canvas.hasSelection()) {
        canvas.fillLayer(canvas.layers().activeIndex(), canvas.brushSettings().color);
    }
}

// -----------------------------------------------------------------------------
// Teclado
// -----------------------------------------------------------------------------

bool Ui::toolKeys(Canvas& canvas) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeySuper) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            copySelection(canvas, false);
        } else if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            copySelection(canvas, true);
        } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            pasteClipboard(canvas);
        } else if (ImGui::IsKeyPressed(ImGuiKey_J, false)) {
            duplicateSelection(canvas);
        } else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            canvas.deselect();
        } else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            canvas.selectAll();
        } else if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
            canvas.invertSelection();
        } else if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
            setCanvasTool(canvas, CanvasTool::Transform);
        } else {
            return false;
        }
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        setCanvasTool(canvas, CanvasTool::Select);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        setCanvasTool(canvas, CanvasTool::Transform);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        if (m_canvasTool == CanvasTool::Select && m_select.pendingPolygon()) {
            selectionNotice(m_select.closePolygon(canvas));
            return true;
        }
        if (m_canvasTool == CanvasTool::Transform) {
            setCanvasTool(canvas, m_toolBeforeTransform);
            return true;
        }
        return false;
    }
    if ((ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) &&
        canvas.hasSelection() && m_canvasTool != CanvasTool::Transform) {
        clearSelected(canvas);
        return true;
    }
    if (m_canvasTool == CanvasTool::Transform) {
        const float step = io.KeyShift ? 10.0f : 1.0f;
        glm::vec2 delta(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
            delta.x -= step;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
            delta.x += step;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            delta.y -= step;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            delta.y += step;
        }
        if (delta.x != 0.0f || delta.y != 0.0f) {
            m_transform.nudge(canvas, delta);
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Dibujo
// -----------------------------------------------------------------------------

void Ui::drawToolOverlay() {
    // Encima del lienzo y debajo de todas las ventanas de la interfaz.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ToolView& view = m_status.canvasView;
    if (m_canvasTool == CanvasTool::Select) {
        m_select.drawOverlay(dl, view);
    } else if (m_canvasTool == CanvasTool::Transform) {
        // La barra de opciones es la del frame anterior (se dibuja después).
        const Layout& L = m_layout;
        m_transform.setCovered({L.leftBar, L.ndiBar, L.rightBar, L.sidebar, m_dock}, L.display);
        m_transform.drawOverlay(dl, view);
    }
}

void Ui::drawDock(Canvas& canvas) {
    m_dock = ImRect();
    selectDock(canvas);
    transformDock(canvas);
    drawPolygonBar(canvas);
}

void Ui::selectDock(Canvas& canvas) {
    const Layout& L = m_layout;
    const bool open = m_canvasTool == CanvasTool::Select;
    const bool has = canvas.hasSelection();
    const SelectTool::Shape shape = m_select.shape();
    const SelectOp op = m_select.op();
    using Shape = SelectTool::Shape;

    const std::initializer_list<DockItem> shapes = {
        choiceCell(kLasso, icon::kLasso, "Lazo", 0, shape == Shape::Lasso),
        choiceCell(kRectangle, icon::kSquareDashed, "Rectángulo", 0, shape == Shape::Rectangle),
        choiceCell(kEllipse, icon::kCircleDashed, "Elipse", 0, shape == Shape::Ellipse),
        choiceCell(kAuto, icon::kWand, "Automática", 0, shape == Shape::Auto),
    };
    const std::initializer_list<DockItem> ops = {
        choiceCell(kAdd, icon::kUnite, "Sumar", 1, op == SelectOp::Add),
        choiceCell(kSubtract, icon::kSubtract, "Restar", 1, op == SelectOp::Subtract),
    };
    const std::initializer_list<DockItem> selection = {
        cell(kInvert, icon::kContrast, "Invertir", 2),
        cell(kFeather, icon::kFeather, "Difuminar", 2, has),
        cell(kDeselect, icon::kSquareX, "Quitar", 2, has),
    };
    const std::initializer_list<DockItem> clipboard = {
        cell(kCopy, icon::kCopy, "Copiar", 3),
        cell(kCut, icon::kScissors, "Cortar", 3),
        cell(kPaste, icon::kClipboardPaste, "Pegar", 3, canvas.canPaste()),
        cell(kDuplicate, icon::kCopyPlus, "Duplicar", 3),
    };
    const std::initializer_list<DockItem> content = {
        cell(kFill, icon::kPaintBucket, "Rellenar", 4, has),
        cell(kClear, icon::kBrushCleaning, "Vaciar", 4, has),
    };

    // Teléfono: las formas arriba, de lado a lado, y las acciones en dos filas. Tableta:
    // todo en una fila si cabe; si no, cómo seleccionar arriba y qué hacer con ello abajo.
    std::vector<DockRow> rows;
    const float available = L.right - L.left - pt(kDockPad) * 2.0f;
    if (L.narrow) {
        rows.resize(3);
        append(rows[0].items, shapes);
        rows[0].stretch = true;
        append(rows[1].items, ops);
        append(rows[1].items, selection);
        append(rows[2].items, clipboard);
        append(rows[2].items, content);
    } else {
        DockRow single;
        append(single.items, shapes);
        append(single.items, ops);
        append(single.items, selection);
        append(single.items, clipboard);
        append(single.items, content);
        if (unitFor(single, available) >= pt(kMinSingleRow)) {
            rows.push_back(std::move(single));
        } else {
            rows.resize(2);
            append(rows[0].items, shapes);
            append(rows[0].items, ops);
            append(rows[0].items, selection);
            append(rows[1].items, clipboard);
            append(rows[1].items, content);
        }
    }
    float unit = 0.0f;
    const ImRect rect = dockRect(rows, L.left, L.right, L.bottom, L.narrow, &unit);
    if (open) {
        m_dock = rect;
    }
    ImRect feather;
    const int action = drawDockFrame("##dock-select", open, rect, rows, unit, kFeather, &feather);
    if (feather.GetWidth() > 0.0f) {
        m_featherButton = feather;
    }

    switch (action) {
    case kLasso:
        m_select.setShape(Shape::Lasso);
        break;
    case kRectangle:
        m_select.setShape(Shape::Rectangle);
        break;
    case kEllipse:
        m_select.setShape(Shape::Ellipse);
        break;
    case kAuto:
        m_select.setShape(Shape::Auto);
        if (!m_autoHint) {
            m_autoHint = true;
            notify("Toca un color; arrastra a los lados para ajustar el umbral", Notice::Info, 4500);
        }
        break;
    case kAdd:
        m_select.setOp(op == SelectOp::Add ? SelectOp::Replace : SelectOp::Add);
        break;
    case kSubtract:
        m_select.setOp(op == SelectOp::Subtract ? SelectOp::Replace : SelectOp::Subtract);
        break;
    case kInvert:
        canvas.invertSelection();
        break;
    case kFeather:
        if (canvas.beginFeather()) {
            m_featherValue = 0.0f;
            m_panel = Panel::Feather;
            m_layerMenu = false;
        }
        break;
    case kDeselect:
        canvas.deselect();
        break;
    case kCopy:
        copySelection(canvas, false);
        break;
    case kCut:
        copySelection(canvas, true);
        break;
    case kPaste:
        pasteClipboard(canvas);
        break;
    case kDuplicate:
        duplicateSelection(canvas);
        break;
    case kFill:
        fillSelected(canvas);
        break;
    case kClear:
        clearSelected(canvas);
        break;
    default:
        break;
    }
}

void Ui::transformDock(Canvas& canvas) {
    const Layout& L = m_layout;
    const bool open = m_canvasTool == CanvasTool::Transform && m_transform.active();
    using Mode = TransformTool::Mode;
    const Mode mode = m_transform.mode();

    const std::initializer_list<DockItem> modes = {
        choiceCell(kFree, icon::kScaling, "Libre", 0, mode == Mode::Free),
        choiceCell(kUniform, icon::kProportions, "Uniforme", 0, mode == Mode::Uniform),
        choiceCell(kDistort, icon::kVectorSquare, "Distorsión", 0, mode == Mode::Distort),
    };
    const std::initializer_list<DockItem> toggles = {
        choiceCell(kSnap, icon::kMagnet, "Imán", 1, m_transform.snap()),
        choiceCell(kNearest, icon::kGrid, "Nítido", 1, m_transform.nearest()),
    };
    const std::initializer_list<DockItem> actions = {
        cell(kFlipH, icon::kFlipHorizontal, "Horizontal", 2),
        cell(kFlipV, icon::kFlipVertical, "Vertical", 2),
        cell(kRotate, icon::kRotateCw, "Girar 90°", 2),
        cell(kFit, icon::kMaximize, "Ajustar", 2),
        cell(kReset, icon::kRotateCcw, "Restaurar", 2),
    };
    const std::initializer_list<DockItem> finish = {
        textButton(kCancel, "Cancelar", 3, false),
        textButton(kApply, "Aplicar", 3, true),
    };

    std::vector<DockRow> rows;
    const float available = L.right - L.left - pt(kDockPad) * 2.0f;
    if (L.narrow) {
        rows.resize(3);
        append(rows[0].items, modes);
        append(rows[0].items, toggles);
        rows[0].stretch = true;
        append(rows[1].items, actions);
        rows[1].stretch = true;
        append(rows[2].items, finish);
        rows[2].stretch = true;
    } else {
        DockRow single;
        append(single.items, modes);
        append(single.items, toggles);
        append(single.items, actions);
        append(single.items, finish);
        if (unitFor(single, available) >= pt(kMinSingleRow)) {
            rows.push_back(std::move(single));
        } else {
            rows.resize(2);
            append(rows[0].items, modes);
            append(rows[0].items, toggles);
            append(rows[1].items, actions);
            append(rows[1].items, finish);
        }
    }
    float unit = 0.0f;
    const ImRect rect = dockRect(rows, L.left, L.right, L.bottom, L.narrow, &unit);
    if (open) {
        m_dock = rect;
    }
    const int action = drawDockFrame("##dock-transform", open, rect, rows, unit, -1, nullptr);

    switch (action) {
    case kFree:
        m_transform.setMode(Mode::Free);
        break;
    case kUniform:
        m_transform.setMode(Mode::Uniform);
        break;
    case kDistort:
        m_transform.setMode(Mode::Distort);
        break;
    case kSnap:
        m_transform.setSnap(!m_transform.snap());
        break;
    case kNearest:
        m_transform.setNearest(canvas, !m_transform.nearest());
        break;
    case kFlipH:
        m_transform.flip(canvas, true);
        break;
    case kFlipV:
        m_transform.flip(canvas, false);
        break;
    case kRotate:
        m_transform.rotate90(canvas);
        break;
    case kFit:
        m_transform.fitCanvas(canvas);
        break;
    case kReset:
        m_transform.reset(canvas);
        break;
    case kCancel:
        m_transform.cancel(canvas);
        canvas.cancelTransform();
        m_transform.stop();
        m_canvasTool = m_toolBeforeTransform;
        break;
    case kApply:
        setCanvasTool(canvas, m_toolBeforeTransform);
        break;
    default:
        break;
    }
}

void Ui::drawPolygonBar(Canvas& canvas) {
    // Lazo por puntos a medias: cerrarlo o descartarlo (también se cierra tocando el
    // primer punto, y deshacer quita el último).
    const bool shown = m_canvasTool == CanvasTool::Select && m_select.pendingPolygon();
    const ImGuiID id = ImHashStr("##polygon-bar");
    const float p = ui::anim::followFrom(id, 0.0f, shown ? 1.0f : 0.0f, shown ? 20.0f : 26.0f);
    if (p <= 0.002f) {
        return;
    }
    const Layout& L = m_layout;
    const float pad = pt(6.0f);
    const float closeWidth = pt(128.0f);
    const float dropWidth = pt(112.0f);
    const float width = pad * 3.0f + closeWidth + dropWidth;
    const float height = pad * 2.0f + pt(kButtonHeight);
    const float bottom = (m_dock.GetWidth() > 0.0f ? m_dock.Min.y : L.bottom) - pt(10.0f);
    const float x = std::round((m_dock.GetWidth() > 0.0f ? m_dock.GetCenter().x : L.display.x * 0.5f) - width * 0.5f);
    const ImRect rect(x, bottom - height, x + width, bottom);

    ui::beginSurface("##polygon-bar", rect, shown, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    const float radius = pt(14.0f);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(24.0f), pt(6.0f), 0.24f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kPanelTint);
    const ImRect close(ImVec2(rect.Min.x + pad, rect.Min.y + pad),
                       ImVec2(rect.Min.x + pad + closeWidth, rect.Max.y - pad));
    const ImRect drop(ImVec2(close.Max.x + pad, rect.Min.y + pad), ImVec2(rect.Max.x - pad, rect.Max.y - pad));
    const bool closeIt = ui::button("##close-polygon", close, "Cerrar lazo", ui::ButtonStyle::Primary, shown);
    const bool dropIt = ui::button("##drop-polygon", drop, "Descartar", ui::ButtonStyle::Secondary, shown);
    ui::transform(mark, ImVec2(rect.GetCenter().x, rect.Max.y), 0.96f + 0.04f * p, ImVec2(0.0f, pt(8.0f) * (1.0f - p)),
                  p);
    ui::endSurface();
    if (shown && closeIt) {
        selectionNotice(m_select.closePolygon(canvas));
    } else if (shown && dropIt) {
        m_select.dropPolygon();
    }
}

void Ui::drawThreshold() {
    // Umbral de la selección automática mientras se arrastra, donde salen los avisos.
    const bool shown = m_canvasTool == CanvasTool::Select && m_select.adjustingThreshold();
    const ImGuiID id = ImHashStr("##threshold");
    const float p = ui::anim::followFrom(id, 0.0f, shown ? 1.0f : 0.0f, shown ? 22.0f : 9.0f);
    if (p <= 0.002f) {
        return;
    }
    const Layout& L = m_layout;
    const float width = pt(236.0f);
    const float height = pt(52.0f);
    const float x = std::round((L.display.x - width) * 0.5f);
    const ImRect rect(x, L.toastTop, x + width, L.toastTop + height);

    ui::beginSurface("##threshold", rect, false, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    const float radius = pt(14.0f);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(26.0f), pt(8.0f), 0.24f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, IM_COL32(40, 40, 44, 184));
    const float t = m_select.threshold();
    char value[16];
    std::snprintf(value, sizeof(value), "%d %%", static_cast<int>(std::lround(t * 100.0f)));
    const float left = rect.Min.x + pt(16.0f);
    const float right = rect.Max.x - pt(16.0f);
    const float cy = rect.Min.y + pt(20.0f);
    ui::icon(dl, icon::kWand, ImVec2(left + pt(8.0f), cy), 16.0f, th::kAccentText);
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(left + pt(24.0f), cy), Align::Left, th::kLabel, "Umbral");
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(right, cy), Align::Right, th::kLabel, value);
    const float barY = rect.Max.y - pt(13.0f);
    dl->AddRectFilled(ImVec2(left, barY - pt(1.5f)), ImVec2(right, barY + pt(1.5f)), IM_COL32(255, 255, 255, 46),
                      pt(1.5f));
    dl->AddRectFilled(ImVec2(left, barY - pt(1.5f)), ImVec2(left + (right - left) * t, barY + pt(1.5f)), th::kAccent,
                      pt(1.5f));
    ui::transform(mark, rect.GetCenter(), 1.0f, ImVec2(0.0f, -pt(10.0f) * (1.0f - p)), p);
    ui::endSurface();
}

// -----------------------------------------------------------------------------
// Paneles
// -----------------------------------------------------------------------------

void Ui::featherPanel(Canvas& canvas) {
    // Sobre la barra de opciones, junto a su botón. Tocar fuera lo aplica.
    const bool open = m_panel == Panel::Feather && canvas.feathering();
    const ImGuiID id = ImHashStr("##feather");
    const float p = ui::anim::followFrom(id, 0.0f, open ? 1.0f : 0.0f, open ? 20.0f : 26.0f);
    if (!open && p <= 0.002f) {
        return;
    }
    const Layout& L = m_layout;
    const float pad = pt(10.0f);
    const float width = L.narrow ? L.right - L.left : pt(340.0f);
    const float height = pad + pt(44.0f) + pt(10.0f) + pt(kButtonHeight) + pad;
    const bool anchored = m_featherButton.GetWidth() > 0.0f;
    const ImVec2 anchor(anchored ? m_featherButton.GetCenter().x : L.display.x * 0.5f,
                        anchored ? m_featherButton.Min.y : L.bottom);
    const float x = std::clamp(anchor.x - width * 0.5f, L.left, std::max(L.left, L.right - width));
    const float bottom = (m_dock.GetWidth() > 0.0f ? m_dock.Min.y : L.bottom) - pt(10.0f);
    const ImRect rect(x, bottom - height, x + width, bottom);

    ui::beginSurface("##feather", rect, open, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    const float radius = pt(th::kPanelRadius);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(40.0f), pt(14.0f), 0.3f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, IM_COL32(24, 24, 28, 224));

    // El radio crece despacio al principio: los bordes finos se ajustan con precisión.
    const float maxRadius = canvas.maxFeather();
    char value[24];
    std::snprintf(value, sizeof(value), "%d px", static_cast<int>(std::lround(canvas.featherRadius())));
    const ImRect slider(ImVec2(rect.Min.x + pad, rect.Min.y + pad), ImVec2(rect.Max.x - pad, rect.Min.y + pad + pt(44.0f)));
    if (ui::paramSlider("##feather-slider", slider, &m_featherValue, "Difuminar el borde", value, open) && open) {
        canvas.setFeather(m_featherValue * m_featherValue * maxRadius);
    }
    const float buttonTop = slider.Max.y + pt(10.0f);
    const float half = (rect.GetWidth() - pad * 3.0f) * 0.5f;
    const ImRect cancel(ImVec2(rect.Min.x + pad, buttonTop), ImVec2(rect.Min.x + pad + half, buttonTop + pt(kButtonHeight)));
    const ImRect apply(ImVec2(cancel.Max.x + pad, buttonTop), ImVec2(rect.Max.x - pad, buttonTop + pt(kButtonHeight)));
    const bool cancelIt = ui::button("##feather-cancel", cancel, "Cancelar", ui::ButtonStyle::Secondary, open);
    const bool applyIt = ui::button("##feather-apply", apply, "Aplicar", ui::ButtonStyle::Primary, open);
    ui::transform(mark, anchor, 0.92f + 0.08f * p, ImVec2(0.0f, 0.0f), std::min(1.0f, p * 1.3f));
    ui::endSurface();

    if (open && (cancelIt || applyIt)) {
        canvas.endFeather(applyIt);
        closePanels();
    }
}

void Ui::modifyPanel(Canvas& canvas) {
    // Teléfono: Selección y Transformar van detrás de un botón.
    const Layout& L = m_layout;
    const float row = pt(64.0f);
    const float content = pt(th::kHeaderHeight + 8.0f) + row * 2.0f + pt(6.0f + 12.0f);
    const float anchorX = m_modifyButton.GetWidth() > 0.0f ? m_modifyButton.GetCenter().x : L.leftBar.GetCenter().x;
    PanelFrame f;
    if (!beginPanel(f, Panel::Modify, "##panel-modify", L.leftBar.Min.x, pt(320.0f), content, anchorX, false)) {
        return;
    }
    ImDrawList* dl = f.dl;
    float y = f.content.Min.y;
    ui::parts::panelHeader(dl, f.content, y, "Modificar");
    y += pt(th::kHeaderHeight + 8.0f);

    struct Entry {
        CanvasTool tool;
        const char* glyph;
        const char* title;
        const char* detail;
    };
    const Entry entries[2] = {
        {CanvasTool::Select, icon::kSelection, "Selección", "Lazo, rectángulo, elipse o automática"},
        {CanvasTool::Transform, icon::kTransform, "Transformar", "Mover, escalar, girar o distorsionar"},
    };
    CanvasTool chosen = m_canvasTool;
    bool picked = false;
    for (int i = 0; i < 2; ++i) {
        const Entry& e = entries[i];
        const ImRect r(ImVec2(f.content.Min.x + pt(8.0f), y), ImVec2(f.content.Max.x - pt(8.0f), y + row));
        const bool active = m_canvasTool == e.tool;
        ImGui::PushID(i);
        const ui::Choice c = ui::choice("##tool", r, active, f.open, false);
        ImGui::PopID();
        const float cy = r.GetCenter().y;
        const ImRect badge(ImVec2(r.Min.x + pt(10.0f), cy - pt(18.0f)), ImVec2(r.Min.x + pt(46.0f), cy + pt(18.0f)));
        dl->AddRectFilled(badge.Min, badge.Max, th::kControl, pt(9.0f));
        const ImU32 color = ui::mix(th::kLabel, th::kAccentText, c.on);
        ui::icon(dl, e.glyph, badge.GetCenter(), 20.0f, color);
        const float x = badge.Max.x + pt(12.0f);
        const float maxWidth = r.Max.x - pt(40.0f) - x;
        ui::label(dl, Weight::SemiBold, th::kBody, ImVec2(x, cy - pt(9.0f)), Align::Left, color, e.title, maxWidth);
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(x, cy + pt(11.0f)), Align::Left, th::kSecondaryLabel,
                  e.detail, maxWidth);
        if (active) {
            ui::icon(dl, icon::kCheck, ImVec2(r.Max.x - pt(22.0f), cy), 18.0f, th::kAccentText);
        }
        if (c.press.clicked) {
            // Tocar la que ya está elegida vuelve a pintar.
            chosen = active ? CanvasTool::Paint : e.tool;
            picked = true;
        }
        y += row + pt(6.0f);
    }
    endPanel(f);
    if (picked) {
        closePanels();
        setCanvasTool(canvas, chosen);
    }
}
