#pragma once

#include <glm/vec2.hpp>

// Posición, escala, giro y volteo del lienzo en la ventana:
//   pantalla = offset + zoom × R(ángulo) × V × lienzo
// donde V invierte la x si la vista está volteada y R gira en el sentido de las agujas
// del reloj (la y va hacia abajo). `offset` es dónde cae el origen del lienzo (su esquina
// de arriba a la izquierda). Todo en píxeles físicos, con el origen arriba a la izquierda.
// La cámara solo afecta a la vista: el compuesto, NDI y el PNG son siempre el lienzo
// completo a su tamaño real, sin girar ni voltear.
class Camera {
public:
    // Tamaño de la ventana. Si el usuario no ha movido la vista, se vuelve a ajustar el
    // lienzo (p. ej. al girar el dispositivo); si la movió, se conserva el punto del
    // lienzo que estaba en el centro.
    void setViewport(glm::vec2 size);
    // Tamaño del lienzo. Deja la vista ajustada (y sin voltear).
    void setCanvasSize(glm::vec2 size);
    // Bordes de la ventana que tapa la interfaz (barras): el ajuste centra el lienzo en
    // lo que queda. Si el usuario no ha movido la vista, se vuelve a ajustar.
    void setInsets(float top, float right, float bottom, float left);
    // Lienzo completo, centrado, derecho y con su proporción. El volteo se queda.
    void fit();
    // Zoom y posición del centro del lienzo que dejaría fit(), sin aplicarlos (para
    // animar el ajuste).
    void fitView(float& zoom, glm::vec2& center) const;
    // Pone una vista intermedia de la animación del ajuste: el centro del lienzo en
    // `center` (pantalla) con ese zoom y ese giro.
    void place(float zoom, float angle, glm::vec2 center);
    bool userMoved() const { return m_userMoved; }

    // Vista completa, para deshacer el pequeño movimiento de un toque con dos dedos.
    struct View {
        float zoom = 1.0f;
        glm::vec2 offset{0.0f, 0.0f};
        float angle = 0.0f;
        bool flipped = false;
        bool userMoved = false;
    };
    View view() const { return {m_zoom, m_offset, m_angle, m_flipped, m_userMoved}; }
    void restoreView(const View& view);

    // La vista sin depender de la ventana (se guarda con el proyecto): el zoom dividido por
    // el del ajuste y el punto del lienzo que cae en el centro de la zona libre.
    struct Placement {
        float zoom = 1.0f;
        glm::vec2 center{0.0f};
        float angle = 0.0f;
    };
    Placement placement() const;
    // Vuelve a ella en esta ventana (con el volteo que tenga ya la vista). Cuenta como
    // movida por el usuario.
    void setPlacement(const Placement& placement);

    glm::vec2 canvasToScreen(glm::vec2 p) const { return m_offset + orient(p) * m_zoom; }
    glm::vec2 screenToCanvas(glm::vec2 p) const { return unorient((p - m_offset) / m_zoom); }
    // Una dirección del lienzo en la pantalla (girada y volteada, sin el zoom), y al revés.
    glm::vec2 orient(glm::vec2 v) const;
    glm::vec2 unorient(glm::vec2 v) const;

    void pan(glm::vec2 delta);
    // Multiplica el zoom por `factor` dejando fijo el punto de pantalla `anchor`.
    void zoomAt(glm::vec2 anchor, float factor);
    // Gira la vista `radians` (en el sentido de las agujas del reloj) alrededor del punto
    // de pantalla `anchor`.
    void rotateAt(glm::vec2 anchor, float radians);
    // Voltea la vista en horizontal (en la vertical del centro de la zona libre): lo que
    // estaba a la izquierda pasa a la derecha y el giro cambia de sentido.
    void setFlipped(bool flipped);

    float zoom() const { return m_zoom; }
    glm::vec2 offset() const { return m_offset; }
    // Giro de la vista en radianes, de -π a π.
    float angle() const { return m_angle; }
    bool flipped() const { return m_flipped; }
    glm::vec2 viewport() const { return m_viewport; }
    glm::vec2 canvasSize() const { return m_canvas; }

    float minZoom() const;
    float maxZoom() const;

private:
    float fitZoom() const;
    void setAngle(float angle);
    void clampView();

    // Zona libre para el ajuste: la ventana sin los bordes que tapa la interfaz.
    void fitArea(glm::vec2& origin, glm::vec2& size) const;

    glm::vec2 m_viewport{1.0f, 1.0f};
    glm::vec2 m_canvas{1.0f, 1.0f};
    float m_insets[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // arriba, derecha, abajo, izquierda
    glm::vec2 m_offset{0.0f, 0.0f};
    float m_zoom = 1.0f;
    float m_angle = 0.0f;
    float m_cos = 1.0f;
    float m_sin = 0.0f;
    bool m_flipped = false;
    bool m_userMoved = false;
};
