#include "Canvas/PressureCurve.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

// Al comprobar la separación de los puntos se perdonan los redondeos del float: un punto
// arrastrado contra su vecino queda a kMinGap, pero restando puede salir un poco menos.
constexpr float kGapSlack = 1e-6f;

float sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }

// El número más corto que, al leerlo, vuelve a dar el mismo float; con punto, igual en
// cualquier idioma del sistema.
std::string exactNumber(float value) {
    char buffer[32];
    for (int digits = 6; digits <= 9; ++digits) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", digits, static_cast<double>(value));
        if (std::strtof(buffer, nullptr) == value) {
            break;
        }
    }
    std::string text(buffer);
    std::replace(text.begin(), text.end(), ',', '.');
    return text;
}

// Pendiente en un extremo con la parábola de sus tres puntos, sin que la curva se pase
// (como el pchip de SciPy). `h0` y `d0` son el tramo del extremo; `h1` y `d1`, el siguiente.
float endTangent(float h0, float h1, float d0, float d1) {
    const float m = ((2.0f * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if (sign(m) != sign(d0)) {
        return 0.0f;
    }
    if (sign(d0) != sign(d1) && std::fabs(m) > 3.0f * std::fabs(d0)) {
        return 3.0f * d0;
    }
    return m;
}

} // namespace

PressureCurve::PressureCurve() : PressureCurve(std::vector<glm::vec2>{}) {}

PressureCurve::PressureCurve(std::vector<glm::vec2> points) : m_points(std::move(points)) { normalize(); }

bool PressureCurve::isDefault() const {
    return m_points.size() == 2 && m_points[0] == glm::vec2(0.0f, 0.0f) && m_points[1] == glm::vec2(1.0f, 1.0f);
}

float PressureCurve::operator()(float pressure) const {
    const float x = std::clamp(pressure, 0.0f, 1.0f);
    size_t k = 0;
    while (k + 2 < m_points.size() && x > m_points[k + 1].x) {
        ++k;
    }
    const glm::vec2 a = m_points[k];
    const glm::vec2 b = m_points[k + 1];
    const float h = b.x - a.x;
    const float t = (x - a.x) / h;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float y = (2.0f * t3 - 3.0f * t2 + 1.0f) * a.y + (t3 - 2.0f * t2 + t) * h * m_tangents[k] +
                    (3.0f * t2 - 2.0f * t3) * b.y + (t3 - t2) * h * m_tangents[k + 1];
    return std::clamp(y, 0.0f, 1.0f);
}

void PressureCurve::move(size_t index, glm::vec2 to) {
    if (index >= m_points.size()) {
        return;
    }
    const size_t last = m_points.size() - 1;
    to.y = std::clamp(to.y, 0.0f, 1.0f);
    if (index == 0) {
        to.x = 0.0f;
    } else if (index == last) {
        to.x = 1.0f;
    } else {
        const float lo = m_points[index - 1].x + kMinGap;
        const float hi = m_points[index + 1].x - kMinGap;
        to.x = lo <= hi ? std::clamp(to.x, lo, hi) : m_points[index].x;
    }
    m_points[index] = to;
    computeTangents();
}

int PressureCurve::add(glm::vec2 at) {
    if (m_points.size() >= kMaxPoints) {
        return -1;
    }
    at = glm::clamp(at, glm::vec2(0.0f), glm::vec2(1.0f));
    size_t i = 1;
    while (i + 1 < m_points.size() && m_points[i].x < at.x) {
        ++i;
    }
    if (at.x - m_points[i - 1].x < kMinGap || m_points[i].x - at.x < kMinGap) {
        return -1;
    }
    m_points.insert(m_points.begin() + static_cast<std::ptrdiff_t>(i), at);
    computeTangents();
    return static_cast<int>(i);
}

void PressureCurve::remove(size_t index) {
    if (index == 0 || index + 1 >= m_points.size()) {
        return;
    }
    m_points.erase(m_points.begin() + static_cast<std::ptrdiff_t>(index));
    computeTangents();
}

std::string PressureCurve::toText() const {
    std::string text;
    for (glm::vec2 p : m_points) {
        if (!text.empty()) {
            text += ' ';
        }
        text += exactNumber(p.x) + ' ' + exactNumber(p.y);
    }
    return text;
}

PressureCurve PressureCurve::fromText(const std::string& text) {
    std::istringstream in(text);
    std::vector<float> numbers;
    std::string word;
    while (in >> word) {
        char* end = nullptr;
        const float value = std::strtof(word.c_str(), &end);
        if (end != word.c_str() + word.size()) {
            break;
        }
        numbers.push_back(value);
    }
    std::vector<glm::vec2> points;
    for (size_t i = 0; i + 1 < numbers.size(); i += 2) {
        points.emplace_back(numbers[i], numbers[i + 1]);
    }
    return PressureCurve(std::move(points));
}

void PressureCurve::normalize() {
    std::vector<glm::vec2> points;
    for (glm::vec2 p : m_points) {
        if (std::isfinite(p.x) && std::isfinite(p.y)) {
            points.push_back(glm::clamp(p, glm::vec2(0.0f), glm::vec2(1.0f)));
        }
    }
    if (points.size() < 2) {
        points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    }
    std::stable_sort(points.begin(), points.end(), [](glm::vec2 a, glm::vec2 b) { return a.x < b.x; });
    points.front().x = 0.0f;
    points.back().x = 1.0f;
    m_points.assign(1, points.front());
    for (size_t i = 1; i + 1 < points.size() && m_points.size() + 1 < kMaxPoints; ++i) {
        if (points[i].x - m_points.back().x >= kMinGap - kGapSlack && 1.0f - points[i].x >= kMinGap - kGapSlack) {
            m_points.push_back(points[i]);
        }
    }
    m_points.push_back(points.back());
    computeTangents();
}

void PressureCurve::computeTangents() {
    // pchip: en cada punto de en medio, la media armónica (con pesos) de las pendientes de
    // los dos tramos si van en el mismo sentido, y plana si no (un máximo o un mínimo).
    const size_t n = m_points.size();
    std::vector<float> h(n - 1);
    std::vector<float> d(n - 1);
    for (size_t k = 0; k + 1 < n; ++k) {
        h[k] = m_points[k + 1].x - m_points[k].x;
        d[k] = (m_points[k + 1].y - m_points[k].y) / h[k];
    }
    m_tangents.assign(n, 0.0f);
    if (n == 2) {
        m_tangents[0] = d[0];
        m_tangents[1] = d[0];
        return;
    }
    for (size_t k = 1; k + 1 < n; ++k) {
        if (d[k - 1] * d[k] <= 0.0f) {
            continue;
        }
        const float w1 = 2.0f * h[k] + h[k - 1];
        const float w2 = h[k] + 2.0f * h[k - 1];
        m_tangents[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
    }
    m_tangents[0] = endTangent(h[0], h[1], d[0], d[1]);
    m_tangents[n - 1] = endTangent(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
}
