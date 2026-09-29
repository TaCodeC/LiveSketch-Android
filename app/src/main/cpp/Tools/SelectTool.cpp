#include "Tools/SelectTool.h"

#include "Canvas/Canvas.h"
#include "Canvas/SelectionShapes.h"
#include "UI/Kit.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace {

// Medidas en pt (en la pantalla).
constexpr float kTapSlop = 6.0f;          // un toque se puede mover esto sin ser un arrastre
constexpr float kCloseRadius = 18.0f;     // tocar a esta distancia del primer punto cierra
constexpr float kFreehandStep = 1.5f;     // distancia entre los puntos del lazo a mano alzada
constexpr float kThresholdSpan = 300.0f;  // arrastrar esto cambia el umbral del 0 al 100 %

constexpr ImU32 kHalo = IM_COL32(0, 0, 0, 140);
constexpr ImU32 kLine = IM_COL32(255, 255, 255, 235);

std::vector<ImVec2> toScreen(const ToolView& view, const std::vector<glm::vec2>& points) {
    std::vector<ImVec2> out;
    out.reserve(points.size());
    for (const glm::vec2& p : points) {
        out.push_back(view.toScreen(p));
    }
    return out;
}

// Línea blanca con un borde oscuro: se ve sobre cualquier dibujo.
void outlinedPath(ImDrawList* dl, const std::vector<ImVec2>& points, bool closed) {
    if (points.size() < 2) {
        return;
    }
    const ImDrawFlags flags = closed ? ImDrawFlags_Closed : ImDrawFlags_None;
    dl->AddPolyline(points.data(), static_cast<int>(points.size()), kHalo, flags, ui::pt(3.5f));
    dl->AddPolyline(points.data(), static_cast<int>(points.size()), kLine, flags, ui::pt(1.5f));
}

} // namespace

void SelectTool::setShape(Shape shape) {
    if (shape != m_shape) {
        dropPolygon();
    }
    m_shape = shape;
}

void SelectTool::press(Canvas& canvas, const ToolView& view, ImVec2 position, SelectOp modifier) {
    cancel(canvas);
    m_pressScreen = position;
    m_moved = false;
    m_shiftAtPress = modifier == SelectOp::Add;
    m_gestureOp = modifier != SelectOp::Replace ? modifier : m_op;
    const glm::vec2 p = view.toCanvas(position);
    switch (m_shape) {
    case Shape::Lasso:
        m_gesture = Gesture::Freehand;
        m_stroke.assign(1, p);
        break;
    case Shape::Rectangle:
    case Shape::Ellipse:
        m_gesture = Gesture::Box;
        // El rectángulo va de píxel entero a píxel entero (ver boxCorner): bordes nítidos
        // también en el lado donde empieza.
        m_anchor = m_shape == Shape::Rectangle ? glm::vec2(std::round(p.x), std::round(p.y)) : p;
        m_current = p;
        m_constrain = false;
        break;
    case Shape::Auto:
        m_autoStart = m_threshold;
        m_autoMoved = false;
        if (m_gestureOp == SelectOp::Subtract && !canvas.hasSelection()) {
            m_deferred = Result::NoSelection;
        } else if (canvas.beginAutoSelect(p.x, p.y, m_gestureOp, m_threshold)) {
            m_gesture = Gesture::Auto;
        }
        break;
    }
}

void SelectTool::drag(Canvas& canvas, const ToolView& view, ImVec2 position, bool constrain) {
    if (m_gesture == Gesture::None) {
        return;
    }
    const float dx = position.x - m_pressScreen.x;
    const float dy = position.y - m_pressScreen.y;
    const float slop = ui::pt(kTapSlop);
    if (!m_moved && dx * dx + dy * dy > slop * slop) {
        m_moved = true;
    }
    const glm::vec2 p = view.toCanvas(position);
    switch (m_gesture) {
    case Gesture::Freehand:
        if (glm::distance(p, m_stroke.back()) >= view.toCanvasLength(ui::pt(kFreehandStep))) {
            m_stroke.push_back(p);
        }
        break;
    case Gesture::Box:
        m_current = p;
        m_constrain = constrain && !m_shiftAtPress;
        break;
    case Gesture::Auto:
        if (m_moved) {
            m_autoMoved = true;
            m_threshold = std::clamp(m_autoStart + dx / ui::pt(kThresholdSpan), 0.0f, 1.0f);
            canvas.setAutoThreshold(m_threshold);
        }
        break;
    case Gesture::None:
        break;
    }
}

SelectTool::Result SelectTool::release(Canvas& canvas, const ToolView& view, ImVec2 position) {
    const Gesture gesture = m_gesture;
    m_gesture = Gesture::None;
    switch (gesture) {
    case Gesture::None: {
        const Result deferred = m_deferred;
        m_deferred = Result::None;
        return deferred;
    }

    case Gesture::Freehand: {
        if (!m_moved) {
            // Un toque: cierra el polígono en su primer punto o le añade un vértice.
            if (m_polygon.size() >= 3 && nearFirstPoint(view, m_pressScreen)) {
                m_stroke.clear();
                return closePolygon(canvas);
            }
            if (m_polygon.empty()) {
                m_polygonOp = m_gestureOp;
            }
            m_segments.push_back(m_polygon.size());
            m_polygon.push_back(m_stroke.front());
            m_stroke.clear();
            return Result::None;
        }
        const glm::vec2 p = view.toCanvas(position);
        if (m_stroke.back() != p) {
            m_stroke.push_back(p);
        }
        if (!m_polygon.empty()) {
            // Un tramo a mano alzada del polígono.
            m_segments.push_back(m_polygon.size());
            m_polygon.insert(m_polygon.end(), m_stroke.begin(), m_stroke.end());
            m_stroke.clear();
            if (nearFirstPoint(view, position)) {
                return closePolygon(canvas);
            }
            return Result::None;
        }
        const std::vector<glm::vec2> polygon = std::move(m_stroke);
        m_stroke.clear();
        return select(canvas, polygon, m_gestureOp);
    }

    case Gesture::Box: {
        if (!m_moved) {
            if (m_gestureOp == SelectOp::Replace && canvas.hasSelection()) {
                canvas.deselect();
                return Result::Deselected;
            }
            return Result::None;
        }
        const glm::vec2 corner = boxCorner();
        const std::vector<glm::vec2> polygon = m_shape == Shape::Rectangle ? selection::rectangle(m_anchor, corner)
                                                                          : selection::ellipse(m_anchor, corner);
        return select(canvas, polygon, m_gestureOp);
    }

    case Gesture::Auto:
        canvas.endAutoSelect(true);
        return Result::Selected;
    }
    return Result::None;
}

void SelectTool::cancel(Canvas& canvas) {
    if (m_gesture == Gesture::Auto) {
        canvas.endAutoSelect(false);
        m_threshold = m_autoStart;
    }
    m_gesture = Gesture::None;
    m_stroke.clear();
    m_deferred = Result::None;
}

SelectTool::Result SelectTool::closePolygon(Canvas& canvas) {
    const std::vector<glm::vec2> polygon = std::move(m_polygon);
    dropPolygon();
    if (polygon.size() < 3) {
        return Result::None;
    }
    return select(canvas, polygon, m_polygonOp);
}

void SelectTool::dropPolygon() {
    m_polygon.clear();
    m_segments.clear();
}

bool SelectTool::undoPoint() {
    if (m_polygon.empty()) {
        return false;
    }
    const size_t start = m_segments.empty() ? 0 : m_segments.back();
    m_polygon.resize(std::min(start, m_polygon.size()));
    if (!m_segments.empty()) {
        m_segments.pop_back();
    }
    if (m_polygon.empty()) {
        dropPolygon();
    }
    return true;
}

SelectTool::Result SelectTool::select(Canvas& canvas, const std::vector<glm::vec2>& polygon, SelectOp op) {
    if (op == SelectOp::Subtract && !canvas.hasSelection()) {
        return Result::NoSelection;
    }
    if (polygon.size() < 3) {
        return Result::Empty;
    }
    return canvas.selectPolygon(polygon, op) ? Result::Selected : Result::Empty;
}

glm::vec2 SelectTool::boxCorner() const {
    glm::vec2 corner = m_current;
    if (m_constrain) {
        const glm::vec2 d = m_current - m_anchor;
        const float side = std::max(std::fabs(d.x), std::fabs(d.y));
        corner = m_anchor + glm::vec2(d.x < 0.0f ? -side : side, d.y < 0.0f ? -side : side);
    }
    if (m_shape == Shape::Rectangle) {
        // Esquinas en píxeles enteros: bordes nítidos.
        return glm::vec2(std::round(corner.x), std::round(corner.y));
    }
    return corner;
}

bool SelectTool::nearFirstPoint(const ToolView& view, ImVec2 position) const {
    if (m_polygon.empty()) {
        return false;
    }
    const ImVec2 first = view.toScreen(m_polygon.front());
    const float dx = position.x - first.x;
    const float dy = position.y - first.y;
    const float r = ui::pt(kCloseRadius);
    return dx * dx + dy * dy <= r * r;
}

void SelectTool::drawOverlay(ImDrawList* dl, const ToolView& view) const {
    // Lazo: el polígono a medias y el tramo que se está dibujando, con una línea tenue
    // que vuelve al principio (así se cerrará).
    std::vector<ImVec2> path = toScreen(view, m_polygon);
    if (m_gesture == Gesture::Freehand && (m_moved || m_polygon.empty())) {
        for (const glm::vec2& p : m_stroke) {
            path.push_back(view.toScreen(p));
        }
    }
    if (path.size() >= 2) {
        outlinedPath(dl, path, false);
        if (path.size() >= 3) {
            dl->AddLine(path.back(), path.front(), IM_COL32(255, 255, 255, 110), ui::pt(1.0f));
        }
    }
    if (!m_polygon.empty()) {
        // Los vértices puestos con toques y el primero, más grande: tocarlo cierra.
        for (size_t i = 0; i < m_segments.size(); ++i) {
            const size_t start = m_segments[i];
            const size_t end = i + 1 < m_segments.size() ? m_segments[i + 1] : m_polygon.size();
            if (end - start == 1 && start > 0) {
                const ImVec2 p = view.toScreen(m_polygon[start]);
                dl->AddCircleFilled(p, ui::pt(4.0f), kHalo, 0);
                dl->AddCircleFilled(p, ui::pt(2.5f), kLine, 0);
            }
        }
        const ImVec2 first = view.toScreen(m_polygon.front());
        dl->AddCircleFilled(first, ui::pt(9.0f), kHalo, 0);
        dl->AddCircleFilled(first, ui::pt(7.5f), ui::theme::kAccent, 0);
        dl->AddCircle(first, ui::pt(7.5f), kLine, 0, ui::pt(1.5f));
    }

    // Rectángulo o elipse mientras se arrastra.
    if (m_gesture == Gesture::Box && m_moved) {
        const glm::vec2 corner = boxCorner();
        const std::vector<glm::vec2> shape = m_shape == Shape::Rectangle ? selection::rectangle(m_anchor, corner)
                                                                        : selection::ellipse(m_anchor, corner);
        outlinedPath(dl, toScreen(view, shape), true);
    }
}
