#include "Canvas/Camera.h"

#include <glm/common.hpp>

#include <algorithm>

namespace {

// Margen de la ventana, en píxeles, que el ajuste deja libre alrededor del lienzo.
constexpr float kFitMargin = 16.0f;

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
    m_offset = m_viewport * 0.5f - center * m_zoom;
    clampView();
}

void Camera::setCanvasSize(glm::vec2 size) {
    m_canvas = glm::max(size, glm::vec2(1.0f));
    fit();
}

float Camera::fitZoom() const {
    const glm::vec2 room = glm::max(m_viewport - 2.0f * kFitMargin, glm::vec2(1.0f));
    return std::min(room.x / m_canvas.x, room.y / m_canvas.y);
}

float Camera::minZoom() const { return fitZoom() * 0.1f; }

// Al menos 8 píxeles de pantalla por píxel del lienzo, para poder ver de cerca un lienzo
// grande en una pantalla pequeña.
float Camera::maxZoom() const { return std::max(fitZoom() * 10.0f, 8.0f); }

void Camera::fit() {
    m_zoom = fitZoom();
    m_offset = (m_viewport - m_canvas * m_zoom) * 0.5f;
    m_userMoved = false;
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

void Camera::clampView() {
    const float zoom = std::clamp(m_zoom, minZoom(), maxZoom());
    if (zoom != m_zoom) {
        const glm::vec2 anchor = m_viewport * 0.5f;
        m_offset = anchor - (anchor - m_offset) * (zoom / m_zoom);
        m_zoom = zoom;
    }
    // Siempre queda a la vista un trozo del lienzo: nunca se pierde fuera de la pantalla.
    const glm::vec2 onScreen = m_canvas * m_zoom;
    const float margin = 0.2f * std::min(m_viewport.x, m_viewport.y);
    const glm::vec2 keep = glm::min(glm::vec2(margin), onScreen);
    m_offset = glm::clamp(m_offset, keep - onScreen, m_viewport - keep);
}
