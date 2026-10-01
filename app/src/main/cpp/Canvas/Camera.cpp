#include "Canvas/Camera.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

// Margen de la ventana, en píxeles, que el ajuste deja libre alrededor del lienzo.
constexpr float kFitMargin = 16.0f;

// De -π a π.
float wrapAngle(float angle) {
    angle = std::remainder(angle, 2.0f * kPi);
    return angle <= -kPi ? angle + 2.0f * kPi : angle;
}

} // namespace

void Camera::setViewport(glm::vec2 size) {
    size = glm::max(size, glm::vec2(1.0f));
    if (size == m_viewport) {
        return;
    }
    if (!m_userMoved) {
        m_viewport = size;
        fit();
        return;
    }
    const glm::vec2 center = screenToCanvas(m_viewport * 0.5f);
    m_viewport = size;
    m_offset = m_viewport * 0.5f - orient(center) * m_zoom;
    clampView();
}

void Camera::setCanvasSize(glm::vec2 size) {
    m_canvas = glm::max(size, glm::vec2(1.0f));
    m_flipped = false;
    fit();
}

void Camera::setInsets(float top, float right, float bottom, float left) {
    const float insets[4] = {top, right, bottom, left};
    if (std::equal(insets, insets + 4, m_insets)) {
        return;
    }
    std::copy(insets, insets + 4, m_insets);
    if (!m_userMoved) {
        fit();
    }
}

void Camera::fitArea(glm::vec2& origin, glm::vec2& size) const {
    origin = glm::vec2(m_insets[3], m_insets[0]);
    size = m_viewport - glm::vec2(m_insets[1] + m_insets[3], m_insets[0] + m_insets[2]);
    // En una ventana muy pequeña la interfaz taparía casi todo: se ignora.
    if (size.x < m_viewport.x * 0.4f || size.y < m_viewport.y * 0.4f) {
        origin = glm::vec2(0.0f);
        size = m_viewport;
    }
}

float Camera::fitZoom() const {
    glm::vec2 origin;
    glm::vec2 area;
    fitArea(origin, area);
    const glm::vec2 room = glm::max(area - 2.0f * kFitMargin, glm::vec2(1.0f));
    return std::min(room.x / m_canvas.x, room.y / m_canvas.y);
}

float Camera::minZoom() const { return fitZoom() * 0.1f; }

// Al menos 8 píxeles de pantalla por píxel del lienzo, para poder ver de cerca un lienzo
// grande en una pantalla pequeña.
float Camera::maxZoom() const { return std::max(fitZoom() * 10.0f, 8.0f); }

void Camera::fit() {
    float zoom = 1.0f;
    glm::vec2 center{0.0f};
    fitView(zoom, center);
    place(zoom, 0.0f, center);
    m_userMoved = false;
}

void Camera::fitView(float& zoom, glm::vec2& center) const {
    glm::vec2 origin;
    glm::vec2 area;
    fitArea(origin, area);
    zoom = fitZoom();
    center = origin + area * 0.5f;
}

void Camera::place(float zoom, float angle, glm::vec2 center) {
    if (!(zoom > 0.0f)) {
        return;
    }
    m_zoom = zoom;
    setAngle(angle);
    m_offset = center - orient(m_canvas * 0.5f) * m_zoom;
}

void Camera::restoreView(const View& view) {
    if (view.zoom > 0.0f) {
        m_zoom = view.zoom;
        m_offset = view.offset;
        m_flipped = view.flipped;
        setAngle(view.angle);
        m_userMoved = view.userMoved;
        clampView();
    }
}

glm::vec2 Camera::orient(glm::vec2 v) const {
    const float x = m_flipped ? -v.x : v.x;
    return {m_cos * x - m_sin * v.y, m_sin * x + m_cos * v.y};
}

glm::vec2 Camera::unorient(glm::vec2 v) const {
    const float x = m_cos * v.x + m_sin * v.y;
    const float y = -m_sin * v.x + m_cos * v.y;
    return {m_flipped ? -x : x, y};
}

void Camera::setAngle(float angle) {
    m_angle = wrapAngle(angle);
    // En los múltiplos de 90° (adonde lleva el imán) los ejes quedan exactos: los bordes
    // del lienzo caen en píxeles enteros.
    const float quarter = m_angle / (kPi * 0.5f);
    const float nearest = std::round(quarter);
    if (std::fabs(quarter - nearest) < 1e-5f) {
        const int k = (static_cast<int>(nearest) % 4 + 4) % 4;
        constexpr float kCos[4] = {1.0f, 0.0f, -1.0f, 0.0f};
        constexpr float kSin[4] = {0.0f, 1.0f, 0.0f, -1.0f};
        m_cos = kCos[k];
        m_sin = kSin[k];
        m_angle = wrapAngle(nearest * kPi * 0.5f);
    } else {
        m_cos = std::cos(m_angle);
        m_sin = std::sin(m_angle);
    }
}

void Camera::pan(glm::vec2 delta) {
    m_offset += delta;
    m_userMoved = true;
    clampView();
}

void Camera::zoomAt(glm::vec2 anchor, float factor) {
    if (!(factor > 0.0f)) {
        return;
    }
    const float zoom = std::clamp(m_zoom * factor, minZoom(), maxZoom());
    m_offset = anchor - (anchor - m_offset) * (zoom / m_zoom);
    m_zoom = zoom;
    m_userMoved = true;
    clampView();
}

void Camera::rotateAt(glm::vec2 anchor, float radians) {
    if (!std::isfinite(radians) || radians == 0.0f) {
        return;
    }
    // El punto del lienzo bajo `anchor` se queda donde está.
    const glm::vec2 fixed = screenToCanvas(anchor);
    setAngle(m_angle + radians);
    m_offset = anchor - orient(fixed) * m_zoom;
    m_userMoved = true;
    clampView();
}

void Camera::setFlipped(bool flipped) {
    if (flipped == m_flipped) {
        return;
    }
    // Espejo de la pantalla en la vertical que pasa por el centro de la zona libre: con el
    // lienzo ajustado no se mueve, y con zoom se sigue viendo la misma parte.
    glm::vec2 origin;
    glm::vec2 area;
    fitArea(origin, area);
    const float mirror = origin.x + area.x * 0.5f;
    m_flipped = flipped;
    setAngle(-m_angle);
    m_offset.x = 2.0f * mirror - m_offset.x;
    clampView();
}

void Camera::clampView() {
    const float zoom = std::clamp(m_zoom, minZoom(), maxZoom());
    if (zoom != m_zoom) {
        const glm::vec2 anchor = m_viewport * 0.5f;
        m_offset = anchor - (anchor - m_offset) * (zoom / m_zoom);
        m_zoom = zoom;
    }
    // Siempre queda a la vista un trozo del lienzo: nunca se pierde fuera de la pantalla.
    // Con la vista girada cuenta la caja del lienzo en la pantalla.
    glm::vec2 low{0.0f};
    glm::vec2 high{0.0f};
    const glm::vec2 corners[4] = {{0.0f, 0.0f}, {m_canvas.x, 0.0f}, {0.0f, m_canvas.y}, m_canvas};
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 p = canvasToScreen(corners[i]);
        low = i == 0 ? p : glm::min(low, p);
        high = i == 0 ? p : glm::max(high, p);
    }
    const glm::vec2 onScreen = high - low;
    const float margin = 0.2f * std::min(m_viewport.x, m_viewport.y);
    const glm::vec2 keep = glm::min(glm::vec2(margin), onScreen);
    const glm::vec2 clamped = glm::clamp(low, keep - onScreen, m_viewport - keep);
    m_offset += clamped - low;
}
