#include "Canvas/QuickShape.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace quickshape {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegree = kPi / 180.0f;

// --- Umbrales: casi todos relativos al tamaño del trazo ---
// Línea: el trazo mide como mucho esto más que la recta de punta a punta y se separa de
// ella como mucho esto de su largo.
constexpr float kLineLength = 1.18f;
constexpr float kLineDeviation = 0.06f;
// Cerrado: el final vuelve a menos de esto (de la diagonal de su caja) del principio, y el
// trazo mide al menos esto de veces la diagonal (si no, es una línea de ida y vuelta).
constexpr float kCloseGap = 0.22f;
constexpr float kClosedLength = 1.8f;
// Esquinas: el trazo gira al menos esto en cada vértice.
constexpr float kMinCorner = 25.0f * kDegree;
// Las esquinas se buscan con esta tolerancia (del largo del trazo) y los lados más cortos
// que esto (del perímetro) son una esquina redondeada, no un lado.
constexpr float kCornerTolerance = 0.03f;
constexpr float kMinSide = 0.06f;
// Polilínea: tramos en los extremos más cortos que esto son un gancho del trazo.
constexpr float kHook = 0.08f;
// Cada lado se separa de su recta como mucho esto de su largo (más un píxel).
constexpr float kSideDeviation = 0.08f;
// Arco: los puntos se separan del círculo como mucho esto de su radio, el arco abarca
// entre estos ángulos y retrocede como mucho esto.
constexpr float kArcDeviation = 0.045f;
constexpr float kMinArc = 25.0f * kDegree;
constexpr float kMaxArc = 320.0f * kDegree;
constexpr float kArcBacktrack = 10.0f * kDegree;
// Formas cerradas: error medio admitido (del radio medio) y cuánto mejor tiene que ser el
// polígono que la elipse para quedarse con él.
constexpr float kPolygonError = 0.06f;
constexpr float kEllipseError = 0.09f;
constexpr float kPolygonAdvantage = 0.6f;
constexpr float kMinAspect = 0.1f;
// Rectángulo: sus ángulos se apartan de 90° como mucho esto. Cuadrado: sus lados se
// diferencian como mucho esto.
constexpr float kRightAngle = 16.0f * kDegree;
constexpr float kSquare = 0.05f;
// Círculo: el semieje menor es al menos esto del mayor.
constexpr float kRound = 0.88f;
// Enderezar: a esta distancia de los ejes del lienzo o de la pantalla.
constexpr float kStraighten = 6.0f * kDegree;
constexpr float kStraightenLine = 3.0f * kDegree;
// Al ajustar sin soltar: un giro menor que esto no gira (y la forma no se tuerce sin
// querer al cambiar su tamaño).
constexpr float kAdjustTurn = 5.0f * kDegree;
// La línea perfecta gira de tanto en tanto.
constexpr float kRegularStep = 15.0f * kDegree;

float cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

glm::vec2 rotate(glm::vec2 v, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

glm::vec2 direction(float angle) { return {std::cos(angle), std::sin(angle)}; }

float segmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float length2 = glm::dot(ab, ab);
    const float t = length2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / length2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

// Distancia de `p` a la polilínea (cerrada si `closed`).
float pathDistance(glm::vec2 p, const std::vector<glm::vec2>& path, bool closed) {
    float best = std::numeric_limits<float>::infinity();
    const size_t n = path.size();
    const size_t edges = closed ? n : n - 1;
    for (size_t i = 0; i < edges; ++i) {
        best = std::min(best, segmentDistance(p, path[i], path[(i + 1) % n]));
    }
    return best;
}

float meanDistance(const std::vector<glm::vec2>& points, const std::vector<glm::vec2>& path, bool closed) {
    float sum = 0.0f;
    for (const glm::vec2& p : points) {
        sum += pathDistance(p, path, closed);
    }
    return sum / static_cast<float>(std::max<size_t>(points.size(), 1));
}

// Lleva `angle` al múltiplo de `period` más cercano, contado desde los ejes del lienzo o
// desde los de la pantalla, si está a menos de `reach`.
float straighten(float angle, float period, float reach, const Options& options) {
    const float bases[2] = {0.0f, std::atan2(options.screenRight.y, options.screenRight.x)};
    float best = angle;
    float bestDifference = reach;
    for (const float base : bases) {
        const float target = base + std::round((angle - base) / period) * period;
        const float difference = std::fabs(angle - target);
        if (difference <= bestDifference) {
            bestDifference = difference;
            best = target;
        }
    }
    return best;
}

// `count` puntos (al menos 2) repartidos por igual a lo largo del trazo.
std::vector<glm::vec2> resample(const std::vector<glm::vec2>& points, float length, int count) {
    std::vector<glm::vec2> out;
    out.reserve(static_cast<size_t>(count));
    out.push_back(points.front());
    const float step = length / static_cast<float>(count - 1);
    float next = step;
    float walked = 0.0f;
    for (size_t i = 1; i < points.size() && static_cast<int>(out.size()) < count - 1; ++i) {
        const glm::vec2 a = points[i - 1];
        const glm::vec2 b = points[i];
        const float segment = glm::length(b - a);
        while (segment > 0.0f && walked + segment >= next && static_cast<int>(out.size()) < count - 1) {
            out.push_back(a + (b - a) * ((next - walked) / segment));
            next += step;
        }
        walked += segment;
    }
    while (static_cast<int>(out.size()) < count) {
        out.push_back(points.back());
    }
    out.back() = points.back();
    return out;
}

// Ramer-Douglas-Peucker: índices de los puntos que hay que dejar para que la polilínea no
// se separe del trazo más de `epsilon`.
void simplify(const std::vector<glm::vec2>& p, size_t first, size_t last, float epsilon, std::vector<size_t>& keep) {
    if (last <= first + 1) {
        return;
    }
    float worst = -1.0f;
    size_t index = first;
    for (size_t i = first + 1; i < last; ++i) {
        const float d = segmentDistance(p[i], p[first], p[last]);
        if (d > worst) {
            worst = d;
            index = i;
        }
    }
    if (worst > epsilon) {
        simplify(p, first, index, epsilon, keep);
        keep.push_back(index);
        simplify(p, index, last, epsilon, keep);
    }
}

std::vector<size_t> corners(const std::vector<glm::vec2>& p, float epsilon) {
    std::vector<size_t> keep{0};
    simplify(p, 0, p.size() - 1, epsilon, keep);
    keep.push_back(p.size() - 1);
    return keep;
}

// Cuánto gira el camino en el vértice `k` de `idx` (cíclico si `closed`).
float turnAt(const std::vector<glm::vec2>& p, const std::vector<size_t>& idx, size_t k) {
    const size_t n = idx.size();
    const glm::vec2 a = p[idx[k]] - p[idx[(k + n - 1) % n]];
    const glm::vec2 b = p[idx[(k + 1) % n]] - p[idx[k]];
    if (glm::dot(a, a) < 1e-8f || glm::dot(b, b) < 1e-8f) {
        return 0.0f;
    }
    return std::fabs(std::atan2(cross(a, b), glm::dot(a, b)));
}

// Quita los vértices donde el camino apenas gira (los extremos de uno abierto se quedan).
void dropShallow(const std::vector<glm::vec2>& p, std::vector<size_t>& idx, bool closed) {
    const size_t minimum = closed ? 3 : 2;
    while (idx.size() > minimum) {
        const size_t n = idx.size();
        float smallest = kMinCorner;
        size_t at = n;
        for (size_t k = closed ? 0 : 1; k < (closed ? n : n - 1); ++k) {
            const float turn = turnAt(p, idx, k);
            if (turn < smallest) {
                smallest = turn;
                at = k;
            }
        }
        if (at == n) {
            break;
        }
        idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(at));
    }
}

// Los puntos del lado que va del vértice `k` al siguiente, sin los de cerca de las
// esquinas (que suelen estar redondeadas).
std::vector<glm::vec2> sidePoints(const std::vector<glm::vec2>& p, const std::vector<size_t>& idx, size_t k,
                                  bool closed) {
    const size_t n = idx.size();
    const size_t from = idx[k];
    size_t to = idx[(k + 1) % n];
    const size_t count = p.size() - (closed ? 1 : 0);   // los cerrados repiten el primero al final
    size_t span = to >= from ? to - from : to + count - from;
    std::vector<glm::vec2> out;
    const size_t trim = span / 6;
    for (size_t i = trim; i + trim <= span; ++i) {
        out.push_back(p[(from + i) % count]);
    }
    return out;
}

struct Line {
    glm::vec2 point;
    glm::vec2 direction;
};

// Recta que mejor pasa por los puntos (la dirección principal).
bool fitLine(const std::vector<glm::vec2>& points, Line& out) {
    if (points.size() < 2) {
        return false;
    }
    glm::vec2 mean(0.0f);
    for (const glm::vec2& p : points) {
        mean += p;
    }
    mean /= static_cast<float>(points.size());
    float xx = 0.0f;
    float yy = 0.0f;
    float xy = 0.0f;
    for (const glm::vec2& p : points) {
        const glm::vec2 d = p - mean;
        xx += d.x * d.x;
        yy += d.y * d.y;
        xy += d.x * d.y;
    }
    if (xx + yy < 1e-6f) {
        return false;
    }
    out.point = mean;
    out.direction = direction(0.5f * std::atan2(2.0f * xy, xx - yy));
    return true;
}

bool intersect(const Line& a, const Line& b, glm::vec2& out) {
    const float denominator = cross(a.direction, b.direction);
    // Casi paralelas: el corte se iría lejos.
    if (std::fabs(denominator) < std::sin(12.0f * kDegree)) {
        return false;
    }
    out = a.point + a.direction * (cross(b.point - a.point, b.direction) / denominator);
    return true;
}

// Los vértices finales: donde se cortan las rectas de los lados (más limpio que el punto
// del trazo, que en las esquinas suele estar redondeado). Los extremos de un camino
// abierto se quedan donde estaban.
std::vector<glm::vec2> refine(const std::vector<glm::vec2>& p, const std::vector<size_t>& idx, bool closed) {
    const size_t n = idx.size();
    const size_t sides = closed ? n : n - 1;
    std::vector<Line> lines(sides);
    std::vector<bool> fitted(sides, false);
    for (size_t k = 0; k < sides; ++k) {
        fitted[k] = fitLine(sidePoints(p, idx, k, closed), lines[k]);
    }
    std::vector<glm::vec2> out;
    for (size_t k = 0; k < n; ++k) {
        glm::vec2 vertex = p[idx[k]];
        const bool end = !closed && (k == 0 || k == n - 1);
        const size_t before = (k + sides - 1) % sides;
        const size_t after = k % sides;
        glm::vec2 corner;
        if (!end && fitted[before] && fitted[after] && intersect(lines[before], lines[after], corner)) {
            // Solo si cae cerca: con lados muy cortos la recta no es fiable.
            const float shortest = std::min(glm::distance(p[idx[k]], p[idx[(k + n - 1) % n]]),
                                            glm::distance(p[idx[k]], p[idx[(k + 1) % n]]));
            if (glm::distance(corner, vertex) <= 0.3f * shortest) {
                vertex = corner;
            }
        }
        out.push_back(vertex);
    }
    return out;
}

float signedArea(const std::vector<glm::vec2>& polygon) {
    float area = 0.0f;
    for (size_t i = 0; i < polygon.size(); ++i) {
        area += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    }
    return area * 0.5f;
}

// Resuelve el sistema de `n` ecuaciones (eliminación con pivote). False si es singular.
template <int N>
bool solve(float (&m)[N][N + 1], float (&x)[N]) {
    for (int col = 0; col < N; ++col) {
        int pivot = col;
        for (int row = col + 1; row < N; ++row) {
            if (std::fabs(m[row][col]) > std::fabs(m[pivot][col])) {
                pivot = row;
            }
        }
        if (std::fabs(m[pivot][col]) < 1e-9f) {
            return false;
        }
        for (int k = 0; k <= N; ++k) {
            std::swap(m[col][k], m[pivot][k]);
        }
        for (int row = 0; row < N; ++row) {
            if (row == col) {
                continue;
            }
            const float f = m[row][col] / m[col][col];
            for (int k = col; k <= N; ++k) {
                m[row][k] -= f * m[col][k];
            }
        }
    }
    for (int i = 0; i < N; ++i) {
        x[i] = m[i][N] / m[i][i];
    }
    return true;
}

glm::vec2 centroid(const std::vector<glm::vec2>& points) {
    glm::vec2 sum(0.0f);
    for (const glm::vec2& p : points) {
        sum += p;
    }
    return sum / static_cast<float>(std::max<size_t>(points.size(), 1));
}

struct Ellipse {
    glm::vec2 center{0.0f};
    glm::vec2 radii{0.0f};
    float angle = 0.0f;
    float error = std::numeric_limits<float>::infinity();   // medio, del radio medio
    bool ok = false;
};

// Distancia (aproximada, en la dirección del centro) de `p` a la elipse.
float ellipseDistance(glm::vec2 p, glm::vec2 center, glm::vec2 radii, float angle) {
    const glm::vec2 local = rotate(p - center, -angle);
    const float r = glm::length(local);
    if (r < 1e-6f) {
        return std::min(radii.x, radii.y);
    }
    const float c = local.x / r;
    const float s = local.y / r;
    const float edge = 1.0f / std::sqrt(c * c / (radii.x * radii.x) + s * s / (radii.y * radii.y));
    return std::fabs(r - edge);
}

// Elipse que mejor se ajusta a un contorno cerrado: los ejes salen de la dirección
// principal de los puntos y el centro y los semiejes, de mínimos cuadrados en ese marco
// (A·u² + B·v² + C·u + D·v = 1).
Ellipse fitEllipse(const std::vector<glm::vec2>& loop, float meanRadius) {
    Ellipse e;
    const glm::vec2 mean = centroid(loop);
    float xx = 0.0f;
    float yy = 0.0f;
    float xy = 0.0f;
    for (const glm::vec2& p : loop) {
        const glm::vec2 d = p - mean;
        xx += d.x * d.x;
        yy += d.y * d.y;
        xy += d.x * d.y;
    }
    const float angle = 0.5f * std::atan2(2.0f * xy, xx - yy);
    const float scale = std::max(meanRadius, 1e-3f);
    float m[4][5] = {};
    for (const glm::vec2& p : loop) {
        const glm::vec2 q = rotate(p - mean, -angle) / scale;
        const float f[4] = {q.x * q.x, q.y * q.y, q.x, q.y};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                m[i][j] += f[i] * f[j];
            }
            m[i][4] += f[i];
        }
    }
    float x[4];
    if (!solve<4>(m, x) || !(x[0] > 0.0f) || !(x[1] > 0.0f)) {
        return e;
    }
    const float u0 = -x[2] / (2.0f * x[0]);
    const float v0 = -x[3] / (2.0f * x[1]);
    const float k = 1.0f + x[0] * u0 * u0 + x[1] * v0 * v0;
    if (!(k > 0.0f)) {
        return e;
    }
    e.center = mean + rotate(glm::vec2(u0, v0) * scale, angle);
    e.radii = glm::vec2(std::sqrt(k / x[0]), std::sqrt(k / x[1])) * scale;
    e.angle = angle;
    float sum = 0.0f;
    for (const glm::vec2& p : loop) {
        sum += ellipseDistance(p, e.center, e.radii, e.angle);
    }
    e.error = sum / static_cast<float>(loop.size()) / scale;
    e.ok = std::isfinite(e.error) && std::isfinite(e.radii.x) && std::isfinite(e.radii.y);
    return e;
}

// Círculo que pasa por tres puntos. False si están casi en línea.
bool circleThrough(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2& center, float& radius) {
    const glm::vec2 ab = b - a;
    const glm::vec2 ac = c - a;
    const float d = 2.0f * cross(ab, ac);
    const float size = std::max(glm::dot(ab, ab), glm::dot(ac, ac));
    if (std::fabs(d) < 1e-4f * size) {
        return false;
    }
    const float ab2 = glm::dot(ab, ab);
    const float ac2 = glm::dot(ac, ac);
    const glm::vec2 offset((ac.y * ab2 - ab.y * ac2) / d, (ab.x * ac2 - ac.x * ab2) / d);
    center = a + offset;
    radius = glm::length(offset);
    return std::isfinite(radius);
}

Shape makeLine(glm::vec2 a, glm::vec2 b, const Options& options) {
    // Casi horizontal, vertical o a 45°: del todo, girando alrededor del inicio.
    const glm::vec2 d = b - a;
    const float angle = straighten(std::atan2(d.y, d.x), kPi * 0.25f, kStraightenLine, options);
    Shape shape;
    shape.kind = Kind::Line;
    shape.points = {a, a + direction(angle) * glm::length(d)};
    return shape;
}

// Polilínea: tramos rectos con esquinas marcadas. `error`: separación media del trazo,
// del largo del trazo.
Shape makePolyline(const std::vector<glm::vec2>& p, float length, float* error) {
    std::vector<size_t> idx = corners(p, kCornerTolerance * length);
    dropShallow(p, idx, false);
    while (idx.size() > 2 && glm::distance(p[idx[1]], p[idx[0]]) < kHook * length) {
        idx.erase(idx.begin() + 1);
    }
    while (idx.size() > 2 && glm::distance(p[idx[idx.size() - 1]], p[idx[idx.size() - 2]]) < kHook * length) {
        idx.erase(idx.end() - 2);
    }
    if (idx.size() < 3 || idx.size() > 7) {
        return {};
    }
    for (size_t k = 0; k + 1 < idx.size(); ++k) {
        const glm::vec2 a = p[idx[k]];
        const glm::vec2 b = p[idx[k + 1]];
        const float limit = kSideDeviation * glm::distance(a, b) + 1.0f;
        for (size_t i = idx[k]; i <= idx[k + 1]; ++i) {
            if (segmentDistance(p[i], a, b) > limit) {
                return {};
            }
        }
    }
    Shape shape;
    shape.kind = Kind::Polyline;
    shape.points = refine(p, idx, false);
    *error = meanDistance(p, shape.points, false) / length;
    return shape;
}

// Arco de circunferencia (ajustada por mínimos cuadrados).
Shape makeArc(const std::vector<glm::vec2>& p, float length, float* error) {
    const glm::vec2 mean = centroid(p);
    const float scale = std::max(length, 1e-3f);
    float m[3][4] = {};
    for (const glm::vec2& point : p) {
        const glm::vec2 q = (point - mean) / scale;
        const float f[3] = {q.x, q.y, 1.0f};
        const float target = -(q.x * q.x + q.y * q.y);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                m[i][j] += f[i] * f[j];
            }
            m[i][3] += f[i] * target;
        }
    }
    float x[3];
    if (!solve<3>(m, x)) {
        return {};
    }
    const glm::vec2 c(-x[0] * 0.5f, -x[1] * 0.5f);
    const float r2 = glm::dot(c, c) - x[2];
    if (!(r2 > 0.0f)) {
        return {};
    }
    const glm::vec2 center = mean + c * scale;
    const float radius = std::sqrt(r2) * scale;
    float deviation = 0.0f;
    for (const glm::vec2& point : p) {
        deviation += std::fabs(glm::distance(point, center) - radius);
    }
    deviation /= static_cast<float>(p.size());
    if (deviation > kArcDeviation * radius) {
        return {};
    }
    // Recorre el círculo en un solo sentido y abarca un trozo razonable.
    float previous = std::atan2(p[0].y - center.y, p[0].x - center.x);
    const float first = previous;
    float unwrapped = previous;
    float forward = 0.0f;
    float backward = 0.0f;
    for (size_t i = 1; i < p.size(); ++i) {
        const float a = std::atan2(p[i].y - center.y, p[i].x - center.x);
        const float d = std::remainder(a - previous, 2.0f * kPi);
        unwrapped += d;
        (d >= 0.0f ? forward : backward) += std::fabs(d);
        previous = a;
    }
    const float span = unwrapped - first;
    const float back = span >= 0.0f ? backward : forward;
    if (std::fabs(span) < kMinArc || std::fabs(span) > kMaxArc || back > kArcBacktrack) {
        return {};
    }
    Shape shape;
    shape.kind = Kind::Arc;
    shape.points = {p.front(), center + direction(first + span * 0.5f) * radius, p.back()};
    *error = deviation / length;
    return shape;
}

Shape openShape(const std::vector<glm::vec2>& p, float length, const Options& options) {
    const glm::vec2 a = p.front();
    const glm::vec2 b = p.back();
    const float chord = glm::distance(a, b);
    if (length <= kLineLength * chord) {
        float worst = 0.0f;
        for (const glm::vec2& q : p) {
            worst = std::max(worst, segmentDistance(q, a, b));
        }
        if (worst <= kLineDeviation * chord) {
            return makeLine(a, b, options);
        }
    }
    float polylineError = 0.0f;
    float arcError = 0.0f;
    Shape polyline = makePolyline(p, length, &polylineError);
    Shape arc = makeArc(p, length, &arcError);
    if (polyline.kind != Kind::None && (arc.kind == Kind::None || polylineError <= arcError)) {
        return polyline;
    }
    return arc;
}

// Rectángulo a partir de un cuadrilátero con los ángulos casi rectos.
Shape makeRectangle(const std::vector<glm::vec2>& quad, glm::vec2 start, const Options& options) {
    // Orientación media de los lados, cada uno según su largo (módulo 90°).
    glm::vec2 sum(0.0f);
    for (size_t i = 0; i < 4; ++i) {
        const glm::vec2 e = quad[(i + 1) % 4] - quad[i];
        const float a = std::atan2(e.y, e.x);
        sum += direction(4.0f * a) * glm::length(e);
    }
    const float angle = straighten(std::atan2(sum.y, sum.x) * 0.25f, kPi * 0.5f, kStraighten, options);
    const glm::vec2 center = centroid(quad);
    float us[4];
    float vs[4];
    for (size_t i = 0; i < 4; ++i) {
        const glm::vec2 local = rotate(quad[i] - center, -angle);
        us[i] = local.x;
        vs[i] = local.y;
    }
    std::sort(us, us + 4);
    std::sort(vs, vs + 4);
    float u0 = (us[0] + us[1]) * 0.5f;
    float u1 = (us[2] + us[3]) * 0.5f;
    float v0 = (vs[0] + vs[1]) * 0.5f;
    float v1 = (vs[2] + vs[3]) * 0.5f;
    const float w = u1 - u0;
    const float h = v1 - v0;
    if (std::fabs(w - h) <= kSquare * std::max(w, h)) {
        const float side = (w + h) * 0.5f;
        const float mu = (u0 + u1) * 0.5f;
        const float mv = (v0 + v1) * 0.5f;
        u0 = mu - side * 0.5f;
        u1 = mu + side * 0.5f;
        v0 = mv - side * 0.5f;
        v1 = mv + side * 0.5f;
    }
    std::vector<glm::vec2> corners = {
        center + rotate({u0, v0}, angle), center + rotate({u1, v0}, angle),
        center + rotate({u1, v1}, angle), center + rotate({u0, v1}, angle)};
    // En el sentido en que se dibujó y empezando por la esquina más cercana al inicio.
    if ((signedArea(corners) > 0.0f) != (signedArea(quad) > 0.0f)) {
        std::reverse(corners.begin(), corners.end());
    }
    size_t first = 0;
    for (size_t i = 1; i < 4; ++i) {
        if (glm::distance(corners[i], start) < glm::distance(corners[first], start)) {
            first = i;
        }
    }
    std::rotate(corners.begin(), corners.begin() + static_cast<std::ptrdiff_t>(first), corners.end());
    Shape shape;
    shape.kind = Kind::Rectangle;
    shape.points = std::move(corners);
    shape.center = centroid(shape.points);
    return shape;
}

Shape closedShape(const std::vector<glm::vec2>& loop, const Options& options) {
    const glm::vec2 mean = centroid(loop);
    float meanRadius = 0.0f;
    for (const glm::vec2& p : loop) {
        meanRadius += glm::distance(p, mean);
    }
    meanRadius /= static_cast<float>(loop.size());
    if (meanRadius < 1.0f) {
        return {};
    }
    const Ellipse ellipse = fitEllipse(loop, meanRadius);

    // Polígono: las esquinas, empezando por el punto más alejado del centro (casi seguro
    // una esquina), con el contorno cerrado.
    size_t far = 0;
    for (size_t i = 1; i < loop.size(); ++i) {
        if (glm::distance(loop[i], mean) > glm::distance(loop[far], mean)) {
            far = i;
        }
    }
    std::vector<glm::vec2> q(loop.begin() + static_cast<std::ptrdiff_t>(far), loop.end());
    q.insert(q.end(), loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>(far));
    q.push_back(q.front());
    float perimeter = 0.0f;
    for (size_t i = 1; i < q.size(); ++i) {
        perimeter += glm::distance(q[i], q[i - 1]);
    }
    std::vector<size_t> idx = corners(q, kCornerTolerance * perimeter);
    idx.pop_back();   // el último es el primero
    for (bool changed = true; changed && idx.size() >= 3;) {
        changed = false;
        // Un lado muy corto es una esquina redondeada: se queda su vértice que más gira.
        for (size_t k = 0; k < idx.size() && idx.size() > 3; ++k) {
            const size_t next = (k + 1) % idx.size();
            if (glm::distance(q[idx[k]], q[idx[next]]) < kMinSide * perimeter) {
                idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(turnAt(q, idx, k) < turnAt(q, idx, next) ? k : next));
                changed = true;
                break;
            }
        }
        const size_t before = idx.size();
        dropShallow(q, idx, true);
        changed = changed || idx.size() != before;
    }
    Shape polygon;
    float polygonError = std::numeric_limits<float>::infinity();
    if (idx.size() >= 3 && idx.size() <= 8) {
        polygon.points = refine(q, idx, true);
        polygonError = meanDistance(loop, polygon.points, true) / meanRadius;
    }

    const float aspect = ellipse.ok ? std::min(ellipse.radii.x, ellipse.radii.y) /
                                          std::max(ellipse.radii.x, ellipse.radii.y)
                                    : 0.0f;
    const bool ellipseOk = ellipse.ok && ellipse.error <= kEllipseError && aspect >= kMinAspect;
    const bool polygonOk = polygonError <= kPolygonError;
    const glm::vec2 start = loop.front();

    if (polygonOk && (!ellipseOk || polygonError < kPolygonAdvantage * ellipse.error)) {
        std::vector<glm::vec2>& v = polygon.points;
        // Empieza en el vértice más cercano al inicio del trazo.
        size_t first = 0;
        for (size_t i = 1; i < v.size(); ++i) {
            if (glm::distance(v[i], start) < glm::distance(v[first], start)) {
                first = i;
            }
        }
        std::rotate(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(first), v.end());
        if (v.size() == 4) {
            bool right = true;
            for (size_t i = 0; i < 4; ++i) {
                const glm::vec2 a = v[(i + 1) % 4] - v[i];
                const glm::vec2 b = v[(i + 2) % 4] - v[(i + 1) % 4];
                const float turn = std::fabs(std::atan2(cross(a, b), glm::dot(a, b)));
                right = right && std::fabs(turn - kPi * 0.5f) <= kRightAngle;
            }
            if (right) {
                return makeRectangle(v, start, options);
            }
        }
        polygon.kind = v.size() == 3 ? Kind::Triangle : Kind::Polygon;
        polygon.center = centroid(v);
        return polygon;
    }
    if (!ellipseOk) {
        return {};
    }
    Shape shape;
    shape.center = ellipse.center;
    shape.radii = ellipse.radii;
    shape.angle = straighten(ellipse.angle, kPi * 0.5f, kStraighten, options);
    if (aspect >= kRound) {
        shape.kind = Kind::Circle;
        const float r = (ellipse.radii.x + ellipse.radii.y) * 0.5f;
        shape.radii = glm::vec2(r);
    } else {
        shape.kind = Kind::Ellipse;
    }
    // Empieza donde empezó el trazo y va en el mismo sentido.
    const glm::vec2 local = rotate(start - shape.center, -shape.angle);
    shape.start = std::atan2(local.y / shape.radii.y, local.x / shape.radii.x);
    if (signedArea(loop) < 0.0f) {
        shape.radii.y = -shape.radii.y;   // el contorno va al revés
    }
    return shape;
}

} // namespace

bool Shape::closed() const {
    switch (kind) {
    case Kind::Circle:
    case Kind::Ellipse:
    case Kind::Rectangle:
    case Kind::Triangle:
    case Kind::Polygon:
        return true;
    default:
        return false;
    }
}

Shape recognize(std::span<const glm::vec2> stroke, const Options& options) {
    std::vector<glm::vec2> raw;
    raw.reserve(stroke.size());
    for (const glm::vec2& p : stroke) {
        if (std::isfinite(p.x) && std::isfinite(p.y) && (raw.empty() || glm::distance(p, raw.back()) > 0.01f)) {
            raw.push_back(p);
        }
    }
    if (raw.size() < 2) {
        return {};
    }
    float length = 0.0f;
    glm::vec2 low = raw.front();
    glm::vec2 high = raw.front();
    for (size_t i = 1; i < raw.size(); ++i) {
        length += glm::distance(raw[i], raw[i - 1]);
        low = glm::min(low, raw[i]);
        high = glm::max(high, raw[i]);
    }
    const float diagonal = glm::distance(low, high);
    if (length < options.minLength || diagonal < options.minLength * 0.5f) {
        return {};
    }
    const int count = std::clamp(static_cast<int>(length / 1.5f), 48, 256);
    std::vector<glm::vec2> p = resample(raw, length, count);

    // Cerrado si en su segunda mitad vuelve cerca del principio; lo que se pase de largo
    // se quita.
    size_t closest = p.size() - 1;
    float gap = glm::distance(p.back(), p.front());
    for (size_t i = p.size() / 2; i < p.size(); ++i) {
        const float d = glm::distance(p[i], p.front());
        if (d < gap) {
            gap = d;
            closest = i;
        }
    }
    if (gap <= kCloseGap * diagonal && length >= kClosedLength * diagonal) {
        p.resize(closest + 1);
        if (p.size() >= 8) {
            return closedShape(p, options);
        }
        return {};
    }
    return openShape(p, length, options);
}

Shape adjust(const Shape& shape, glm::vec2 from, glm::vec2 to, const Options& options) {
    Shape out = shape;
    switch (shape.kind) {
    case Kind::None:
        break;
    case Kind::Line:
        if (glm::distance(shape.points[0], to) > 0.5f) {
            out = makeLine(shape.points[0], to, options);
            if (shape.regular) {
                out = regular(out);
            }
        }
        break;
    case Kind::Polyline:
        out.points.back() = to;
        break;
    case Kind::Arc: {
        // Con la misma curvatura: el punto del medio sigue a la cuerda.
        const glm::vec2 a = shape.points[0];
        const glm::vec2 chord = shape.points[2] - a;
        const float length = glm::length(chord);
        const glm::vec2 next = to - a;
        const float nextLength = glm::length(next);
        if (length < 1e-3f || nextLength < 1.0f) {
            break;
        }
        const glm::vec2 normal(-chord.y / length, chord.x / length);
        const glm::vec2 m = shape.points[1] - a;
        const float along = glm::dot(m, chord) / (length * length);
        const float bulge = glm::dot(m, normal) / length;
        const glm::vec2 nextNormal(-next.y / nextLength, next.x / nextLength);
        out.points[1] = a + next * along + nextNormal * (bulge * nextLength);
        out.points[2] = to;
        break;
    }
    case Kind::Circle:
    case Kind::Ellipse:
    case Kind::Rectangle:
    case Kind::Triangle:
    case Kind::Polygon: {
        const glm::vec2 f = from - shape.center;
        const glm::vec2 t = to - shape.center;
        if (glm::dot(f, f) < 4.0f || glm::dot(t, t) < 1.0f) {
            break;
        }
        const float scale = glm::length(t) / glm::length(f);
        float turn = std::atan2(cross(f, t), glm::dot(f, t));
        if (shape.kind == Kind::Circle || std::fabs(turn) < kAdjustTurn) {
            turn = 0.0f;
        }
        if (shape.kind == Kind::Circle || shape.kind == Kind::Ellipse) {
            out.radii = shape.radii * scale;
            out.angle = shape.angle + turn;
        } else {
            for (glm::vec2& p : out.points) {
                p = shape.center + rotate(p - shape.center, turn) * scale;
            }
        }
        break;
    }
    }
    return out;
}

Shape regular(const Shape& shape) {
    Shape out = shape;
    out.regular = true;
    switch (shape.kind) {
    case Kind::Line: {
        const glm::vec2 a = shape.points[0];
        const glm::vec2 d = shape.points[1] - a;
        const float angle = std::round(std::atan2(d.y, d.x) / kRegularStep) * kRegularStep;
        out.points[1] = a + direction(angle) * glm::length(d);
        break;
    }
    case Kind::Ellipse: {
        // Empieza hacia el mismo lado que la elipse.
        const float a = std::fabs(shape.radii.x);
        const float b = std::fabs(shape.radii.y);
        const float r = (a + b) * 0.5f;
        out.kind = Kind::Circle;
        out.radii = glm::vec2(r, std::copysign(r, shape.radii.y));
        out.start = std::atan2(b * std::sin(shape.start), a * std::cos(shape.start));
        break;
    }
    case Kind::Rectangle: {
        const glm::vec2 p0 = shape.points[0];
        const glm::vec2 p1 = shape.points[1];
        const glm::vec2 p2 = shape.points[2];
        const float w = glm::distance(p0, p1);
        const float h = glm::distance(p1, p2);
        if (w < 1e-3f || h < 1e-3f) {
            break;
        }
        const glm::vec2 u = (p1 - p0) / w;
        const glm::vec2 v = (p2 - p1) / h;
        const float side = (w + h) * 0.5f;
        const glm::vec2 q = shape.center - (u + v) * (side * 0.5f);
        out.points = {q, q + u * side, q + (u + v) * side, q + v * side};
        break;
    }
    case Kind::Triangle:
    case Kind::Polygon: {
        const size_t n = shape.points.size();
        if (n < 3) {
            break;
        }
        const glm::vec2 c = shape.center;
        float radius = 0.0f;
        for (const glm::vec2& p : shape.points) {
            radius += glm::distance(p, c);
        }
        radius /= static_cast<float>(n);
        const glm::vec2 first = shape.points[0] - c;
        const float a0 = std::atan2(first.y, first.x);
        const float turn = (signedArea(shape.points) >= 0.0f ? 2.0f : -2.0f) * kPi / static_cast<float>(n);
        for (size_t i = 0; i < n; ++i) {
            out.points[i] = c + direction(a0 + turn * static_cast<float>(i)) * radius;
        }
        break;
    }
    default:
        break;
    }
    return out;
}

std::vector<glm::vec2> outline(const Shape& shape, float tolerance) {
    tolerance = std::max(tolerance, 0.01f);
    // Paso de ángulo para que la cuerda se separe de un arco de radio `r` como mucho la
    // tolerancia.
    auto angleStep = [tolerance](float r) {
        return r > tolerance ? 2.0f * std::acos(1.0f - tolerance / r) : kPi * 0.25f;
    };
    std::vector<glm::vec2> out;
    switch (shape.kind) {
    case Kind::None:
        break;
    case Kind::Line:
    case Kind::Polyline:
        out = shape.points;
        break;
    case Kind::Rectangle:
    case Kind::Triangle:
    case Kind::Polygon:
        out = shape.points;
        if (!out.empty()) {
            out.push_back(out.front());
        }
        break;
    case Kind::Arc: {
        const glm::vec2 a = shape.points[0];
        const glm::vec2 m = shape.points[1];
        const glm::vec2 b = shape.points[2];
        glm::vec2 center;
        float radius = 0.0f;
        if (!circleThrough(a, m, b, center, radius) || radius > 1e6f) {
            out = {a, b};
            break;
        }
        const float ta = std::atan2(a.y - center.y, a.x - center.x);
        auto positive = [](float x) {
            x = std::fmod(x, 2.0f * kPi);
            return x < 0.0f ? x + 2.0f * kPi : x;
        };
        const float toMiddle = positive(std::atan2(m.y - center.y, m.x - center.x) - ta);
        const float toEnd = positive(std::atan2(b.y - center.y, b.x - center.x) - ta);
        const float sweep = toMiddle <= toEnd ? toEnd : toEnd - 2.0f * kPi;
        const int n = std::clamp(static_cast<int>(std::ceil(std::fabs(sweep) / angleStep(radius))), 2, 4096);
        for (int i = 0; i <= n; ++i) {
            out.push_back(center + direction(ta + sweep * static_cast<float>(i) / static_cast<float>(n)) * radius);
        }
        out.front() = a;
        out.back() = b;
        break;
    }
    case Kind::Circle:
    case Kind::Ellipse: {
        // radii.y negativo: el contorno va en el otro sentido.
        const float a = std::fabs(shape.radii.x);
        const float b = std::fabs(shape.radii.y);
        const float turn = shape.radii.y < 0.0f ? -1.0f : 1.0f;
        const int n = std::clamp(static_cast<int>(std::ceil(2.0f * kPi / angleStep(std::max(a, b)))), 12, 4096);
        for (int i = 0; i <= n; ++i) {
            const float t = shape.start + turn * 2.0f * kPi * static_cast<float>(i) / static_cast<float>(n);
            out.push_back(shape.center + rotate({a * std::cos(t), b * std::sin(t)}, shape.angle));
        }
        out.back() = out.front();
        break;
    }
    }
    return out;
}

const char* name(const Shape& shape) {
    switch (shape.kind) {
    case Kind::None:
        return "";
    case Kind::Line:
        return "Línea";
    case Kind::Arc:
        return "Arco";
    case Kind::Polyline:
        return "Polilínea";
    case Kind::Circle:
        return "Círculo";
    case Kind::Ellipse:
        return "Elipse";
    case Kind::Rectangle: {
        const float w = glm::distance(shape.points[0], shape.points[1]);
        const float h = glm::distance(shape.points[1], shape.points[2]);
        return std::fabs(w - h) <= 0.01f * std::max(w, h) ? "Cuadrado" : "Rectángulo";
    }
    case Kind::Triangle:
        return "Triángulo";
    case Kind::Polygon:
        switch (shape.points.size()) {
        case 4:
            return "Cuadrilátero";
        case 5:
            return "Pentágono";
        case 6:
            return "Hexágono";
        case 7:
            return "Heptágono";
        case 8:
            return "Octágono";
        default:
            return "Polígono";
        }
    }
    return "";
}

} // namespace quickshape
