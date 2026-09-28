#pragma once

#include "Canvas/Camera.h"
#include "Gfx/GLObjects.h"

// Dibuja la escena: el fondo liso, la sombra del lienzo y, encima, el lienzo compuesto
// (con un damero donde es transparente) en la posición y con el zoom de la cámara.
class CanvasView {
public:
    bool init();
    void destroy();

    // Medidas de la sombra según la densidad de la interfaz (píxeles por punto).
    void setPixelsPerPoint(float pixelsPerPoint) { m_pixelsPerPoint = pixelsPerPoint; }

    // Dibuja en `fbo` (0: la ventana), de `width`×`height` píxeles. `scale` son píxeles
    // de destino por píxel de la ventana: 1 en la ventana, menos para el fondo desenfocado
    // de la interfaz. La cámara siempre trabaja en píxeles de la ventana.
    void draw(const Camera& camera, GLuint compositeTexture, GLuint fbo, int width, int height, float scale = 1.0f);

private:
    gfx::Program m_shadowProgram;
    GLint m_uShadowRect = -1;
    GLint m_uShadowViewport = -1;
    GLint m_uShadowParams = -1;   // sigma grande, desplazamiento grande, sigma pequeña, desplazamiento pequeño
    GLint m_uShadowAlpha = -1;
    GLint m_uShadowExtent = -1;

    gfx::Program m_canvasProgram;
    GLint m_uRect = -1;
    GLint m_uViewport = -1;
    GLint m_uCanvas = -1;
    GLint m_uCell = -1;

    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
    float m_pixelsPerPoint = 1.0f;
};
