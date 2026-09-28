#pragma once

#include "Canvas/LayerStack.h"
#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

// Trazo en curso que se muestra encima de la capa activa antes de fundirse con ella.
struct StrokePreview {
    GLuint texture = 0;     // buffer de trazo (RGBA premultiplicado, tamaño del lienzo)
    float opacity = 1.0f;
    bool erase = false;
};

// Composición de capas en el espacio del lienzo (sin zoom ni pan). El resultado es lo
// que se ve en pantalla, lo que sale por NDI y lo que se exporta a PNG.
class Compositor {
public:
    bool init(int width, int height);
    void destroy();

    // Recompone `rect` del compuesto con las capas visibles, de abajo arriba.
    void compose(const LayerStack& layers, const IRect& rect, const StrokePreview* preview);

    enum class Blend { Over, Erase };
    // Dibuja la textura `source` (del tamaño del lienzo) sobre el FBO `target` dentro de
    // `rect`. Over: fuente encima con `opacity`. Erase: borra el destino según el alfa
    // de la fuente por `opacity`.
    void draw(GLuint target, GLuint source, float opacity, Blend blend, const IRect& rect);

    const gfx::RenderTarget& composite() const { return m_composite; }

private:
    void bindCanvasPass(GLuint target, const IRect& rect);
    void drawQuad(GLuint layerTexture, float opacity, const StrokePreview* preview);

    gfx::Program m_program;
    GLint m_uLayer = -1;
    GLint m_uStroke = -1;
    GLint m_uOpacity = -1;
    GLint m_uStrokeOpacity = -1;
    GLint m_uStrokeMode = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
    gfx::RenderTarget m_composite;
};
