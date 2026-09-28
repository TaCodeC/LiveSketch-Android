#pragma once

#include <glm/vec2.hpp>

// Posición y escala del lienzo en la ventana: pantalla = offset + lienzo × zoom.
// Todo en píxeles físicos, con el origen arriba a la izquierda y la y hacia abajo.
// La cámara solo afecta a la vista: el compuesto, NDI y el PNG son siempre el lienzo
// completo a su tamaño real.
class Camera {
public:
    // Tamaño de la ventana. Si el usuario no ha movido la vista, se vuelve a ajustar el
    // lienzo (p. ej. al girar el dispositivo); si la movió, se conserva el punto del
    // lienzo que estaba en el centro.
    void setViewport(glm::vec2 size);
    // Tamaño del lienzo. Deja la vista ajustada.
    void setCanvasSize(glm::vec2 size);
    // Lienzo completo, centrado y con su proporción.
    void fit();

    glm::vec2 canvasToScreen(glm::vec2 p) const { return m_offset + p * m_zoom; }
    glm::vec2 screenToCanvas(glm::vec2 p) const { return (p - m_offset) / m_zoom; }

    void pan(glm::vec2 delta);
    // Multiplica el zoom por `factor` dejando fijo el punto de pantalla `anchor`.
    void zoomAt(glm::vec2 anchor, float factor);

    float zoom() const { return m_zoom; }
    glm::vec2 offset() const { return m_offset; }
    glm::vec2 viewport() const { return m_viewport; }
    glm::vec2 canvasSize() const { return m_canvas; }

    float minZoom() const;
    float maxZoom() const;

private:
    float fitZoom() const;
    void clampView();

    glm::vec2 m_viewport{1.0f, 1.0f};
    glm::vec2 m_canvas{1.0f, 1.0f};
    glm::vec2 m_offset{0.0f, 0.0f};
    float m_zoom = 1.0f;
    bool m_userMoved = false;
};
