#include "Canvas/DrawingGuide.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace guide {
namespace {

constexpr float kPi = 3.14159265358979f;

// Seno y coseno sin el error de redondeo de los múltiplos de 90° (así las copias de un
// trazo vertical caen en los mismos píxeles que el original).
glm::vec2 unit(float angle) {
    glm::vec2 v(std::cos(angle), std::sin(angle));
    for (int i = 0; i < 2; ++i) {
        if (std::fabs(v[i]) < 1e-6f) {
            v[i] = 0.0f;
        } else if (std::fabs(std::fabs(v[i]) - 1.0f) < 1e-6f) {
            v[i] = std::copysign(1.0f, v[i]);
        }
    }
    return v;
}

// Giro de `angle` (en el sentido de las agujas del reloj, con la y hacia abajo).
Copy rotation(float angle) {
    const glm::vec2 cs = unit(angle);
    Copy copy;
    copy.matrix = glm::mat2(cs.x, cs.y, -cs.y, cs.x);
    copy.angle = angle;
    return copy;
}

// Reflejo en la recta que pasa por el centro con dirección `angle`.
Copy reflection(float angle) {
    const glm::vec2 cs = unit(2.0f * angle);
    Copy copy;
    copy.matrix = glm::mat2(cs.x, cs.y, cs.y, -cs.x);
    copy.mirrored = true;
    copy.angle = 2.0f * angle;
    return copy;
}

// La recta q + t·d dentro del lienzo [0, size]. False si no lo cruza.
bool clipLine(glm::vec2 q, glm::vec2 d, glm::vec2 size, Segment& out) {
    float t0 = -std::numeric_limits<float>::infinity();
    float t1 = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 2; ++axis) {
        if (std::fabs(d[axis]) < 1e-9f) {
            if (q[axis] < 0.0f || q[axis] > size[axis]) {
                return false;
            }
            continue;
        }
        float a = -q[axis] / d[axis];
        float b = (size[axis] - q[axis]) / d[axis];
        if (a > b) {
            std::swap(a, b);
        }
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (!(t0 < t1)) {
        return false;
    }
    out = {q + d * t0, q + d * t1};
    return true;
}

void addLine(std::vector<Segment>& out, glm::vec2 q, glm::vec2 d, glm::vec2 size) {
    Segment segment;
    if (clipLine(q, d, size, segment)) {
        out.push_back(segment);
    }
}

} // namespace

std::vector<Copy> copies(const DrawingGuide& guide) {
    std::vector<Copy> out{Copy{}};
    if (!guide.mirrors()) {
        return out;
    }
    const float a = guide.angle;
    switch (guide.symmetry) {
    case SymmetryKind::Vertical:
        out.push_back(guide.rotational ? rotation(kPi) : reflection(a + kPi * 0.5f));
        break;
    case SymmetryKind::Horizontal:
        out.push_back(guide.rotational ? rotation(kPi) : reflection(a));
        break;
    case SymmetryKind::Quadrant:
        if (guide.rotational) {
            for (int k = 1; k < 4; ++k) {
                out.push_back(rotation(kPi * 0.5f * static_cast<float>(k)));
            }
        } else {
            out.push_back(reflection(a + kPi * 0.5f));
            out.push_back(reflection(a));
            out.push_back(rotation(kPi));
        }
        break;
    case SymmetryKind::Radial:
        if (guide.rotational) {
            for (int k = 1; k < 8; ++k) {
                out.push_back(rotation(kPi * 0.25f * static_cast<float>(k)));
            }
        } else {
            for (int k = 1; k < 4; ++k) {
                out.push_back(rotation(kPi * 0.5f * static_cast<float>(k)));
            }
            for (int k = 0; k < 4; ++k) {
                out.push_back(reflection(a + kPi * 0.25f * static_cast<float>(k)));
            }
        }
        break;
    }
    return out;
}

glm::vec2 transform(glm::vec2 point, const Copy& copy, glm::vec2 center) {
    return center + copy.matrix * (point - center);
}

Dab transform(const Dab& dab, const Copy& copy, glm::vec2 center) {
    Dab out = dab;
    const glm::vec2 p = transform(glm::vec2(dab.x, dab.y), copy, center);
    out.x = p.x;
    out.y = p.y;
    out.angle = copy.mirrored ? copy.angle - dab.angle : dab.angle + copy.angle;
    return out;
}

std::vector<Segment> lines(const DrawingGuide& guide, glm::vec2 size, float minSpacing) {
    std::vector<Segment> out;
    const glm::vec2 c = guide.center;
    const glm::vec2 u = unit(guide.angle);
    const glm::vec2 v(-u.y, u.x);
    if (guide.kind == GuideKind::Symmetry) {
        switch (guide.symmetry) {
        case SymmetryKind::Vertical:
            addLine(out, c, v, size);
            break;
        case SymmetryKind::Horizontal:
            addLine(out, c, u, size);
            break;
        case SymmetryKind::Quadrant:
            addLine(out, c, u, size);
            addLine(out, c, v, size);
            break;
        case SymmetryKind::Radial:
            for (int k = 0; k < 4; ++k) {
                addLine(out, c, unit(guide.angle + kPi * 0.25f * static_cast<float>(k)), size);
            }
            break;
        }
        return out;
    }

    // Cuadrícula: si las líneas quedarían más juntas que `minSpacing`, se dibuja una de
    // cada dos (o de cada cuatro...).
    float step = std::clamp(guide.gridSize, kMinGridSize, kMaxGridSize);
    while (step < minSpacing) {
        step *= 2.0f;
    }
    const glm::vec2 corners[4] = {{0.0f, 0.0f}, {size.x, 0.0f}, {0.0f, size.y}, size};
    for (const glm::vec2 normal : {v, u}) {
        const glm::vec2 direction = normal == v ? u : v;
        float low = std::numeric_limits<float>::infinity();
        float high = -low;
        for (const glm::vec2 corner : corners) {
            const float t = glm::dot(corner - c, normal);
            low = std::min(low, t);
            high = std::max(high, t);
        }
        const int first = static_cast<int>(std::ceil(low / step));
        const int last = static_cast<int>(std::floor(high / step));
        for (int k = first; k <= last && out.size() < 4096; ++k) {
            addLine(out, c + normal * (step * static_cast<float>(k)), direction, size);
        }
    }
    return out;
}

DrawingGuide defaults(glm::vec2 size) {
    DrawingGuide guide;
    guide.center = size * 0.5f;
    // Una cuadrícula de unos 12 cuadros a lo ancho, redondeada a 10 px.
    guide.gridSize = std::clamp(std::round(std::max(size.x, size.y) / 12.0f / 10.0f) * 10.0f, 20.0f, 400.0f);
    return guide;
}

} // namespace guide
