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
    fitView(m_zoom, m_offset);
    m_userMoved = false;
}

void Camera::fitView(float& zoom, glm::vec2& offset) const {
    glm::vec2 origin;
    glm::vec2 area;
    fitArea(origin, area);
    zoom = fitZoom();
    offset = origin + (area - m_canvas * zoom) * 0.5f;
}

void Camera::setView(float zoom, glm::vec2 offset) {
    if (zoom > 0.0f) {
        m_zoom = zoom;
        m_offset = offset;
    }
}

void Camera::restoreView(const View& view) {
    if (view.zoom > 0.0f) {
        m_zoom = view.zoom;
        m_offset = view.offset;
        m_userMoved = view.userMoved;
        clampView();
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
