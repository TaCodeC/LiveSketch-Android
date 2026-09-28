#pragma once

#include "Canvas/Camera.h"
#include "Gfx/GLObjects.h"

// Dibuja en la ventana el fondo con cuadrícula y, encima, el lienzo compuesto con la
// posición y el zoom de la cámara.
class CanvasView {
public:
    bool init();
    void destroy();

    // `windowWidth`/`windowHeight`: tamaño del framebuffer de la ventana en píxeles.
    void draw(const Camera& camera, GLuint compositeTexture, int windowWidth, int windowHeight);

private:
    gfx::Program m_gridProgram;
    gfx::Program m_canvasProgram;
    GLint m_uRect = -1;
    GLint m_uViewport = -1;
    GLint m_uCanvas = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
};
