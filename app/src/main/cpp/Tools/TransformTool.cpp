#include "Tools/TransformTool.h"

#include "Canvas/Canvas.h"
#include "Canvas/Warp.h"
#include "UI/Kit.h"
#include "UI/Theme.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace {

// Medidas en pt (en la pantalla).
constexpr float kHitRadius = 22.0f;        // alcance de los tiradores
constexpr float kDragSlop = 3.0f;          // pulsar y mover menos que esto no cambia nada
constexpr float kRotationOffset = 30.0f;   // del lado al tirador de girar
constexpr float kSnapReach = 8.0f;         // el imán atrae desde esta distancia
constexpr float kMinEdgeHandle = 40.0f;    // un lado más corto no tiene tirador en medio
constexpr float kSmallBox = 66.0f;         // en una caja más estrecha, dentro siempre mueve
constexpr float kCornerRadius = 7.0f;
constexpr float kEdgeRadius = 4.5f;

constexpr float kSnapAngle = 15.0f * 3.14159265f / 180.0f;
constexpr float kMinEdge = 1.0f;           // píxeles del lienzo
constexpr float kMaxCoordinate = 1.0e6f;

constexpr ImU32 kHalo = IM_COL32(0, 0, 0, 140);
constexpr ImU32 kLine = IM_COL32(255, 255, 255, 235);

float cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

glm::vec2 toVec(ImVec2 p) { return glm::vec2(p.x, p.y); }

float distance2(ImVec2 a, ImVec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// El punto está dentro del cuadrilátero convexo, gire hacia donde gire.
bool insideQuad(const std::array<ImVec2, 4>& quad, ImVec2 p) {
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 a = toVec(quad[i]);
        const glm::vec2 b = toVec(quad[(i + 1) % 4]);
        const float c = cross(b - a, toVec(p) - a);
        if (c == 0.0f) {
            continue;
        }
        if (sign == 0.0f) {
            sign = c;
        } else if ((c > 0.0f) != (sign > 0.0f)) {
            return false;
        }
    }
    return true;
}

// Con la y hacia abajo, un ángulo positivo gira en el sentido de las agujas del reloj.
glm::vec2 rotateAround(glm::vec2 p, glm::vec2 pivot, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const glm::vec2 d = p - pivot;
    return pivot + glm::vec2(d.x * c - d.y * s, d.x * s + d.y * c);
}

void knob(ImDrawList* dl, ImVec2 p, float radius, ImU32 fill) {
    dl->AddCircleFilled(p, radius + ui::pt(1.5f), kHalo, 0);
    dl->AddCircleFilled(p, radius, fill, 0);
}

} // namespace

void TransformTool::start(Canvas& canvas) {
    m_active = canvas.transforming();
    m_source = canvas.transformSource();
    m_corners = sourceQuad();
    m_homography = glm::mat3(1.0f);
    m_history.clear();
    m_handle = Handle::None;
    m_moved = false;
    m_guideX.clear();
    m_guideY.clear();
    m_canvasSize = glm::vec2(static_cast<float>(canvas.width()), static_cast<float>(canvas.height()));
    if (m_active && m_nearest) {
        canvas.setTransform(m_corners.data(), true);
    }
}

void TransformTool::stop() {
    m_active = false;
    m_history.clear();
    m_handle = Handle::None;
    m_moved = false;
    m_guideX.clear();
    m_guideY.clear();
}

void TransformTool::setNearest(Canvas& canvas, bool nearest) {
    m_nearest = nearest;
    if (m_active) {
        canvas.setTransform(m_corners.data(), m_nearest);
    }
}

// -----------------------------------------------------------------------------
// Gestos
// -----------------------------------------------------------------------------

void TransformTool::press(Canvas& canvas, const ToolView& view, ImVec2 position) {
    cancel(canvas);
    if (!m_active) {
        return;
    }
    m_knobInside = rotationKnob(view).inside;
    m_handle = hit(view, position, m_index);
    m_startCorners = m_corners;
    m_startHomography = m_homography;
    m_startPointer = view.toCanvas(position);
    m_pivot = center();
    m_moved = false;
}

void TransformTool::drag(Canvas& canvas, const ToolView& view, ImVec2 position) {
    if (m_handle == Handle::None) {
        return;
    }
    const glm::vec2 pointer = view.toCanvas(position);
    if (!m_moved) {
        const glm::vec2 d = (pointer - m_startPointer) * view.zoom;
        const float slop = ui::pt(kDragSlop);
        if (glm::dot(d, d) < slop * slop) {
            return;
        }
        m_moved = true;
    }

    Quad next = m_startCorners;
    switch (m_handle) {
    case Handle::Move: {
        glm::vec2 delta = pointer - m_startPointer;
        if (m_snap) {
            delta = snapMove(canvas, view, delta);
        }
        for (glm::vec2& corner : next) {
            corner += delta;
        }
        break;
    }
    case Handle::Rotate: {
        const glm::vec2 from = m_startPointer - m_pivot;
        const glm::vec2 to = pointer - m_pivot;
        if (glm::dot(from, from) < 1e-6f || glm::dot(to, to) < 1e-6f) {
            return;
        }
        float angle = std::atan2(cross(from, to), glm::dot(from, to));
        if (m_snap) {
            // El lado de arriba, de 15 en 15 grados.
            const glm::vec2 top = m_startCorners[1] - m_startCorners[0];
            const float start = std::atan2(top.y, top.x);
            angle = std::round((start + angle) / kSnapAngle) * kSnapAngle - start;
        }
        for (glm::vec2& corner : next) {
            corner = rotateAround(corner, m_pivot, angle);
        }
        break;
    }
    case Handle::Corner:
        if (m_mode == Mode::Distort) {
            next[m_index] += pointer - m_startPointer;
        } else if (!scaleCorner(m_index, pointer, next)) {
            return;
        }
        break;
    case Handle::Edge:
        if (m_mode == Mode::Distort) {
            const glm::vec2 delta = pointer - m_startPointer;
            next[m_index] += delta;
            next[(m_index + 1) % 4] += delta;
        } else if (!scaleEdge(m_index, pointer, next)) {
            return;
        }
        break;
    case Handle::None:
        return;
    }
    // Si no vale (se cruza, o es demasiado pequeña), se queda como en el último paso bueno.
    apply(canvas, next);
}

void TransformTool::release(Canvas& canvas) {
    (void)canvas;
    if (m_handle != Handle::None && m_moved && m_corners != m_startCorners) {
        m_history.push_back(m_startCorners);
    }
    m_handle = Handle::None;
    m_moved = false;
    m_guideX.clear();
    m_guideY.clear();
}

void TransformTool::cancel(Canvas& canvas) {
    if (m_handle != Handle::None && m_moved && m_active) {
        apply(canvas, m_startCorners);
    }
    m_handle = Handle::None;
    m_moved = false;
    m_guideX.clear();
    m_guideY.clear();
}

// -----------------------------------------------------------------------------
// Botones
// -----------------------------------------------------------------------------

void TransformTool::flip(Canvas& canvas, bool horizontal) {
    if (!m_active) {
        return;
    }
    cancel(canvas);
    // Como se ve en la pantalla: en espejo respecto al centro.
    const glm::vec2 pivot = center();
    Quad next = m_corners;
    for (glm::vec2& corner : next) {
        if (horizontal) {
            corner.x = 2.0f * pivot.x - corner.x;
        } else {
            corner.y = 2.0f * pivot.y - corner.y;
        }
    }
    change(canvas, next);
}

void TransformTool::rotate90(Canvas& canvas) {
    if (!m_active) {
        return;
    }
    cancel(canvas);
    const glm::vec2 pivot = center();
    Quad next;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 d = m_corners[i] - pivot;
        next[i] = pivot + glm::vec2(-d.y, d.x);
    }
    // Una caja derecha con las esquinas en píxeles enteros sigue así (con lados de
    // paridad distinta caerían en medio píxel y se suavizaría).
    const glm::vec2 shift = glm::vec2(std::round(next[0].x), std::round(next[0].y)) - next[0];
    bool whole = true;
    for (const glm::vec2& corner : next) {
        const glm::vec2 p = corner + shift;
        if (std::fabs(p.x - std::round(p.x)) > 1e-3f || std::fabs(p.y - std::round(p.y)) > 1e-3f) {
            whole = false;
        }
    }
    if (whole) {
        for (glm::vec2& corner : next) {
            corner = glm::vec2(std::round(corner.x + shift.x), std::round(corner.y + shift.y));
        }
    }
    change(canvas, next);
}

void TransformTool::fitCanvas(Canvas& canvas) {
    if (!m_active) {
        return;
    }
    cancel(canvas);
    // Conserva el volteo y el giro redondeado a cuartos de vuelta.
    Quad current = m_corners;
    const bool mirrored = cross(current[1] - current[0], current[3] - current[0]) < 0.0f;
    if (mirrored) {
        current = Quad{current[1], current[0], current[3], current[2]};
    }
    const glm::vec2 top = current[1] - current[0];
    const float quarter = 3.14159265f * 0.5f;
    const int turns = ((static_cast<int>(std::lround(std::atan2(top.y, top.x) / quarter)) % 4) + 4) % 4;

    const float w = static_cast<float>(m_source.width());
    const float h = static_cast<float>(m_source.height());
    const float boxW = (turns % 2 == 0) ? w : h;
    const float boxH = (turns % 2 == 0) ? h : w;
    const float scale = std::min(m_canvasSize.x / boxW, m_canvasSize.y / boxH);
    const float fitW = boxW * scale;
    const float fitH = boxH * scale;
    const float x0 = (m_canvasSize.x - fitW) * 0.5f;
    const float y0 = (m_canvasSize.y - fitH) * 0.5f;
    const Quad box = {glm::vec2(x0, y0), glm::vec2(x0 + fitW, y0), glm::vec2(x0 + fitW, y0 + fitH),
                      glm::vec2(x0, y0 + fitH)};
    Quad next;
    for (int i = 0; i < 4; ++i) {
        next[i] = box[(i + turns) % 4];
    }
    if (mirrored) {
        next = Quad{next[1], next[0], next[3], next[2]};
    }
    change(canvas, next);
}

void TransformTool::reset(Canvas& canvas) {
    if (!m_active) {
        return;
    }
    cancel(canvas);
    change(canvas, sourceQuad());
}

void TransformTool::nudge(Canvas& canvas, glm::vec2 delta) {
    if (!m_active) {
        return;
    }
    cancel(canvas);
    Quad next = m_corners;
    for (glm::vec2& corner : next) {
        corner += delta;
    }
    change(canvas, next);
}

bool TransformTool::undo(Canvas& canvas) {
    cancel(canvas);
    if (m_history.empty()) {
        return false;
    }
    const Quad corners = m_history.back();
    m_history.pop_back();
    apply(canvas, corners);
    return true;
}

// -----------------------------------------------------------------------------
// Geometría
// -----------------------------------------------------------------------------

bool TransformTool::apply(Canvas& canvas, const Quad& corners) {
    for (int i = 0; i < 4; ++i) {
        const glm::vec2& c = corners[i];
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || std::fabs(c.x) > kMaxCoordinate ||
            std::fabs(c.y) > kMaxCoordinate) {
            return false;
        }
        const glm::vec2 edge = corners[(i + 1) % 4] - c;
        if (glm::dot(edge, edge) < kMinEdge * kMinEdge) {
            return false;
        }
    }
    glm::mat3 h(1.0f);
    if (!warp::homography(m_source, corners.data(), h) || !canvas.setTransform(corners.data(), m_nearest)) {
        return false;
    }
    m_corners = corners;
    m_homography = h;
    return true;
}

void TransformTool::change(Canvas& canvas, const Quad& corners) {
    const Quad before = m_corners;
    if (corners != before && apply(canvas, corners)) {
        m_history.push_back(before);
    }
}

TransformTool::Quad TransformTool::sourceQuad() const {
    const float x0 = static_cast<float>(m_source.x0);
    const float y0 = static_cast<float>(m_source.y0);
    const float x1 = static_cast<float>(m_source.x1);
    const float y1 = static_cast<float>(m_source.y1);
    return Quad{glm::vec2(x0, y0), glm::vec2(x1, y0), glm::vec2(x1, y1), glm::vec2(x0, y1)};
}

glm::vec2 TransformTool::image(const glm::mat3& homography, glm::vec2 p) const {
    return warp::apply(homography, p);
}

glm::vec2 TransformTool::center() const {
    const glm::vec2 middle((m_source.x0 + m_source.x1) * 0.5f, (m_source.y0 + m_source.y1) * 0.5f);
    return image(m_homography, middle);
}

bool TransformTool::scaleCorner(int corner, glm::vec2 pointer, Quad& out) const {
    const glm::mat3 toSource = glm::inverse(m_startHomography);
    bool grabValid = false;
    bool nowValid = false;
    const glm::vec2 grab = warp::apply(toSource, m_startPointer, &grabValid);
    const glm::vec2 now = warp::apply(toSource, pointer, &nowValid);
    if (!grabValid || !nowValid) {
        return false;
    }
    const Quad s = sourceQuad();
    const glm::vec2 fixed = s[(corner + 2) % 4];
    const glm::vec2 w0 = s[corner] - fixed;   // los dos ejes miden al menos 1 px
    glm::vec2 w = s[corner] + (now - grab) - fixed;
    if (m_mode == Mode::Uniform) {
        const float t = glm::dot(w, w0) / glm::dot(w0, w0);
        const float tMin = std::max(1.0f / std::fabs(w0.x), 1.0f / std::fabs(w0.y));
        w = w0 * std::max(t, tMin);
    } else {
        // Cada eje por su lado, sin pasar al otro lado de la esquina fija.
        w.x = std::copysign(std::max(w.x * std::copysign(1.0f, w0.x), 1.0f), w0.x);
        w.y = std::copysign(std::max(w.y * std::copysign(1.0f, w0.y), 1.0f), w0.y);
    }
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 p(s[i].x == fixed.x ? fixed.x : fixed.x + w.x, s[i].y == fixed.y ? fixed.y : fixed.y + w.y);
        bool valid = false;
        out[i] = warp::apply(m_startHomography, p, &valid);
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool TransformTool::scaleEdge(int edge, glm::vec2 pointer, Quad& out) const {
    const glm::mat3 toSource = glm::inverse(m_startHomography);
    bool grabValid = false;
    bool nowValid = false;
    const glm::vec2 grab = warp::apply(toSource, m_startPointer, &grabValid);
    const glm::vec2 now = warp::apply(toSource, pointer, &nowValid);
    if (!grabValid || !nowValid) {
        return false;
    }
    const glm::vec2 d = now - grab;
    float x0 = static_cast<float>(m_source.x0);
    float y0 = static_cast<float>(m_source.y0);
    float x1 = static_cast<float>(m_source.x1);
    float y1 = static_cast<float>(m_source.y1);
    const float width = x1 - x0;
    const float height = y1 - y0;
    switch (edge) {
    case 0: y0 = std::min(y0 + d.y, y1 - 1.0f); break;
    case 1: x1 = std::max(x1 + d.x, x0 + 1.0f); break;
    case 2: y1 = std::max(y1 + d.y, y0 + 1.0f); break;
    default: x0 = std::min(x0 + d.x, x1 - 1.0f); break;
    }
    if (m_mode == Mode::Uniform) {
        // El otro eje crece igual, desde su centro.
        if (edge == 0 || edge == 2) {
            const float half = width * (y1 - y0) / height * 0.5f;
            const float middle = (x0 + x1) * 0.5f;
            x0 = middle - half;
            x1 = middle + half;
        } else {
            const float half = height * (x1 - x0) / width * 0.5f;
            const float middle = (y0 + y1) * 0.5f;
            y0 = middle - half;
            y1 = middle + half;
        }
    }
    const glm::vec2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i) {
        bool valid = false;
        out[i] = warp::apply(m_startHomography, corners[i], &valid);
        if (!valid) {
            return false;
        }
    }
    return true;
}

glm::vec2 TransformTool::snapMove(const Canvas& canvas, const ToolView& view, glm::vec2 delta) {
    m_guideX.clear();
    m_guideY.clear();
    glm::vec2 lo(std::numeric_limits<float>::max());
    glm::vec2 hi(-std::numeric_limits<float>::max());
    for (const glm::vec2& corner : m_startCorners) {
        lo = glm::min(lo, corner + delta);
        hi = glm::max(hi, corner + delta);
    }
    const float reach = view.toCanvasLength(ui::pt(kSnapReach));
    // Los bordes y el centro de la caja se pegan a los bordes y al centro del lienzo.
    const auto snapAxis = [reach](float low, float high, float size, std::vector<float>& guides) {
        const float features[3] = {low, (low + high) * 0.5f, high};
        const float targets[3] = {0.0f, size * 0.5f, size};
        float shift = 0.0f;
        float best = reach;
        bool found = false;
        for (float feature : features) {
            for (float target : targets) {
                const float distance = std::fabs(target - feature);
                if (distance <= best) {
                    best = distance;
                    shift = target - feature;
                    found = true;
                }
            }
        }
        if (!found) {
            return 0.0f;
        }
        for (float target : targets) {
            for (float feature : features) {
                if (std::fabs(feature + shift - target) < 0.5f) {
                    guides.push_back(target);
                    break;
                }
            }
        }
        return shift;
    };
    delta.x += snapAxis(lo.x, hi.x, static_cast<float>(canvas.width()), m_guideX);
    delta.y += snapAxis(lo.y, hi.y, static_cast<float>(canvas.height()), m_guideY);
    return delta;
}

// -----------------------------------------------------------------------------
// Tiradores
// -----------------------------------------------------------------------------

ImVec2 TransformTool::edgeMid(const ToolView& view, int edge) const {
    const Quad s = sourceQuad();
    const glm::vec2 middle = (s[edge] + s[(edge + 1) % 4]) * 0.5f;
    return view.toScreen(image(m_homography, middle));
}

TransformTool::Knob TransformTool::rotationKnob(const ToolView& view) const {
    if (m_handle == Handle::Rotate) {
        return {m_index, m_knobInside};
    }
    // Los lados de más arriba a más abajo en la pantalla.
    const glm::vec2 middle = toVec(view.toScreen(center()));
    std::array<std::pair<float, int>, 4> edges;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 d = toVec(edgeMid(view, i)) - middle;
        const float length = glm::length(d);
        edges[i] = {length < 1e-3f ? 0.0f : d.y / length, i};
    }
    std::stable_sort(edges.begin(), edges.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first - 1e-3f; });
    // El de arriba; si la interfaz tapa su tirador, el de abajo, y si no, uno de los otros.
    for (int k : {0, 3, 1, 2}) {
        const Knob knob{edges[k].second, false};
        if (!covered(rotationHandle(view, knob))) {
            return knob;
        }
    }
    return {edges[0].second, true};
}

void TransformTool::setCovered(std::vector<ImRect> covered, ImVec2 display) {
    m_covered = std::move(covered);
    m_display = display;
}

bool TransformTool::covered(ImVec2 p) const {
    const float r = ui::pt(kCornerRadius + 2.0f);
    if (m_display.x > 0.0f && (p.x < r || p.y < r || p.x > m_display.x - r || p.y > m_display.y - r)) {
        return true;
    }
    for (const ImRect& rect : m_covered) {
        if (p.x > rect.Min.x - r && p.x < rect.Max.x + r && p.y > rect.Min.y - r && p.y < rect.Max.y + r) {
            return true;
        }
    }
    return false;
}

ImVec2 TransformTool::rotationHandle(const ToolView& view, Knob knob) const {
    const glm::vec2 mid = toVec(edgeMid(view, knob.edge));
    const glm::vec2 middle = toVec(view.toScreen(center()));
    const glm::vec2 a = toVec(view.toScreen(m_corners[knob.edge]));
    const glm::vec2 b = toVec(view.toScreen(m_corners[(knob.edge + 1) % 4]));
    glm::vec2 normal(b.y - a.y, a.x - b.x);
    const float length = glm::length(normal);
    normal = length > 1e-3f ? normal / length : glm::vec2(0.0f, -1.0f);
    if ((glm::dot(normal, mid - middle) < 0.0f) != knob.inside) {
        normal = -normal;
    }
    const glm::vec2 p = mid + normal * ui::pt(kRotationOffset);
    return ImVec2(p.x, p.y);
}

TransformTool::Handle TransformTool::hit(const ToolView& view, ImVec2 position, int& index) const {
    std::array<ImVec2, 4> quad;
    float shortest = std::numeric_limits<float>::max();
    for (int i = 0; i < 4; ++i) {
        quad[i] = view.toScreen(m_corners[i]);
    }
    for (int i = 0; i < 4; ++i) {
        shortest = std::min(shortest, std::sqrt(distance2(quad[i], quad[(i + 1) % 4])));
    }

    // El tirador más cercano al dedo, si alguno está al alcance; si no, mover.
    const float radius = ui::pt(kHitRadius);
    float best = radius * radius;
    Handle result = Handle::Move;
    index = 0;
    const Knob knob = rotationKnob(view);
    const float rotation = distance2(position, rotationHandle(view, knob));
    if (rotation < best) {
        best = rotation;
        result = Handle::Rotate;
        index = knob.edge;
    }
    for (int i = 0; i < 4; ++i) {
        const float d = distance2(position, quad[i]);
        if (d < best) {
            best = d;
            result = Handle::Corner;
            index = i;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (std::sqrt(distance2(quad[i], quad[(i + 1) % 4])) < ui::pt(kMinEdgeHandle)) {
            continue;
        }
        const float d = distance2(position, edgeMid(view, i));
        if (d < best) {
            best = d;
            result = Handle::Edge;
            index = i;
        }
    }
    // En una caja pequeña los tiradores la taparían entera: dentro de ella, mueve.
    if (result != Handle::Rotate && shortest < ui::pt(kSmallBox) && insideQuad(quad, position)) {
        result = Handle::Move;
        index = 0;
    }
    return result;
}

// -----------------------------------------------------------------------------
// Dibujo
// -----------------------------------------------------------------------------

void TransformTool::drawOverlay(ImDrawList* dl, const ToolView& view) const {
    if (!m_active) {
        return;
    }
    const ImU32 accent = ui::theme::kAccent;

    // Guías del imán, de lado a lado del lienzo.
    for (float x : m_guideX) {
        dl->AddLine(view.toScreen(glm::vec2(x, 0.0f)), view.toScreen(glm::vec2(x, m_canvasSize.y)), accent,
                    ui::pt(1.0f));
    }
    for (float y : m_guideY) {
        dl->AddLine(view.toScreen(glm::vec2(0.0f, y)), view.toScreen(glm::vec2(m_canvasSize.x, y)), accent,
                    ui::pt(1.0f));
    }

    // La caja.
    std::array<ImVec2, 4> quad;
    for (int i = 0; i < 4; ++i) {
        quad[i] = view.toScreen(m_corners[i]);
    }
    dl->AddPolyline(quad.data(), 4, kHalo, ImDrawFlags_Closed, ui::pt(3.5f));
    dl->AddPolyline(quad.data(), 4, kLine, ImDrawFlags_Closed, ui::pt(1.5f));

    // Girar: un tirador unido a un lado (al de arriba, si no lo tapa la interfaz).
    const Knob rotationAt = rotationKnob(view);
    const ImVec2 mid = edgeMid(view, rotationAt.edge);
    const ImVec2 rotation = rotationHandle(view, rotationAt);
    dl->AddLine(mid, rotation, kHalo, ui::pt(3.0f));
    dl->AddLine(mid, rotation, kLine, ui::pt(1.25f));
    knob(dl, rotation, ui::pt(kCornerRadius), accent);
    dl->AddCircle(rotation, ui::pt(kCornerRadius), kLine, 0, ui::pt(1.5f));

    // Esquinas (de acento al distorsionar: van por libre) y lados.
    const ImU32 cornerFill = m_mode == Mode::Distort ? accent : kLine;
    for (int i = 0; i < 4; ++i) {
        const bool grabbed = m_handle == Handle::Corner && m_index == i;
        knob(dl, quad[i], ui::pt(grabbed ? kCornerRadius + 2.0f : kCornerRadius), cornerFill);
        if (m_mode == Mode::Distort) {
            dl->AddCircle(quad[i], ui::pt(kCornerRadius), kLine, 0, ui::pt(1.5f));
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (std::sqrt(distance2(quad[i], quad[(i + 1) % 4])) < ui::pt(kMinEdgeHandle)) {
            continue;
        }
        const bool grabbed = m_handle == Handle::Edge && m_index == i;
        knob(dl, edgeMid(view, i), ui::pt(grabbed ? kEdgeRadius + 2.0f : kEdgeRadius), kLine);
    }

    // Al girar, el ángulo del lado de arriba.
    if (m_handle == Handle::Rotate && m_moved) {
        const glm::vec2 top = m_corners[1] - m_corners[0];
        int degrees = static_cast<int>(std::lround(std::atan2(top.y, top.x) * 180.0f / 3.14159265f));
        if (degrees <= -180) {
            degrees += 360;
        }
        char text[16];
        std::snprintf(text, sizeof(text), "%d\xC2\xB0", degrees);
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImVec2 at(rotation.x + ui::pt(14.0f), rotation.y - size.y * 0.5f);
        const ImVec2 pad = ui::pt(6.0f, 3.0f);
        dl->AddRectFilled(ImVec2(at.x - pad.x, at.y - pad.y), ImVec2(at.x + size.x + pad.x, at.y + size.y + pad.y),
                          IM_COL32(0, 0, 0, 170), ui::pt(6.0f));
        dl->AddText(at, kLine, text);
    }
}
