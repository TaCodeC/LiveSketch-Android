#pragma once

#include "Canvas/LayerStack.h"
#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

// Grano del papel de un trazo: multiplica su alfa, fijo al lienzo (se ve igual en todos
// los trazos que pasan por el mismo sitio).
struct StrokeGrain {
    GLuint texture = 0;    // blanca con el relieve en el alfa, que se repite; 0: sin grano
    float scale = 1.0f;    // uv del grano por píxel del lienzo
    float depth = 0.0f;    // 0: no se nota; 1: donde el relieve es 0 no llega pintura
    bool active() const { return texture != 0 && depth > 0.0f; }
};

// Trazo en curso que se muestra sobre la capa activa antes de fundirse con ella.
struct StrokePreview {
    enum class Mode {
        Paint,       // encima de la capa
        Erase,       // borra la capa según su alfa
        PaintAtop,   // encima, solo donde la capa tiene pintura (bloqueo alfa)
        Replace,     // la textura sustituye a la capa (copia de trabajo)
    };
    GLuint texture = 0;     // buffer de trazo (RGBA premultiplicado, tamaño del lienzo)
    float opacity = 1.0f;
    Mode mode = Mode::Paint;
    StrokeGrain grain;
    GLuint selection = 0;   // máscara de la selección (R8): el trazo solo llega a lo seleccionado
};

// Composición de capas en el espacio del lienzo (sin zoom ni pan). El resultado es lo
// que se ve en pantalla, lo que sale por NDI y lo que se exporta a PNG.
//
// Las capas en modo Normal se mezclan con la GPU sobre el compuesto. Las de otros modos
// necesitan leer lo que tienen debajo: se dibujan en un segundo destino leyendo el
// primero como fondo, y los dos se turnan (ping-pong).
class Compositor {
public:
    bool init(int width, int height);
    void destroy();

    // Recompone `rect` del compuesto con las capas visibles, de abajo arriba, cada una con
    // su opacidad, su modo de fusión y su máscara de recorte. `preview`: trazo en curso de
    // la capa activa.
    void compose(const LayerStack& layers, const IRect& rect, const StrokePreview* preview);

    enum class Blend {
        Over,    // fuente encima con `opacity`
        Erase,   // borra el destino según el alfa de la fuente por `opacity`
        Atop,    // encima, solo donde el destino tiene alfa (que no cambia)
        Mask,    // multiplica el destino por el alfa de la fuente por `opacity`
    };
    // Dibuja la textura `source` (del tamaño del lienzo) sobre el FBO `target` dentro de
    // `rect`, con el grano `grain` si lo hay (un trazo que se funde con su capa). Con
    // `mask` (R8, del tamaño del lienzo), la fuente se multiplica por ella.
    void draw(GLuint target, GLuint source, float opacity, Blend blend, const IRect& rect,
              const StrokeGrain* grain = nullptr, GLuint mask = 0);
    // Multiplica el contenido del FBO `target` por `factor` dentro de `rect` (hornea la
    // opacidad de una capa en sus píxeles).
    void scale(GLuint target, float factor, const IRect& rect);
    // Pinta `upper` sobre `lower` como se verían en el compuesto: con su opacidad, su modo
    // de fusión y, si `clip` no es 0, recortada por el alfa de esa textura (del tamaño del
    // lienzo). `scratch` (también del tamaño del lienzo) sirve de intermedio y queda
    // transparente en `rect`.
    void merge(const gfx::RenderTarget& lower, const Layer& upper, GLuint clip, const gfx::RenderTarget& scratch,
               const IRect& rect);

    enum class Fill {
        Replace,   // el color sustituye lo que hubiera
        Atop,      // solo donde ya hay pintura, conservando su alfa
    };
    // Rellena `rect` de `target` con un color RGB opaco. Con `mask` (R8, del tamaño del
    // lienzo), solo en lo seleccionado y con sus bordes suaves.
    void fill(GLuint target, const float rgb[3], Fill mode, const IRect& rect, GLuint mask = 0);
    // Deja transparente `rect` de `target`; con `mask`, solo lo seleccionado.
    void clear(GLuint target, const IRect& rect, GLuint mask = 0);
    // Invierte el color de `target` en `rect` sin tocar su alfa.
    void invert(GLuint target, const IRect& rect);

    enum class Filter {
        Invert,   // colores invertidos (el alfa no cambia)
    };
    // Aplica un filtro a `target` en `rect`, solo donde `mask` (R8; 0: en todo) y mezclado
    // según ella. `scratch` (del tamaño del lienzo) sirve de intermedio y queda
    // transparente en `rect`.
    void filter(const gfx::RenderTarget& target, Filter filter, GLuint mask, const gfx::RenderTarget& scratch,
                const IRect& rect);

    const gfx::RenderTarget& composite() const { return m_composite; }

private:
    // Una capa (o una textura) que se dibuja con el programa del compositor.
    struct Pass {
        GLuint layer = 0;
        float opacity = 1.0f;
        BlendMode blend = BlendMode::Normal;
        const StrokePreview* stroke = nullptr;       // trazo sobre esta capa
        GLuint clip = 0;                              // base del recorte (0: sin recorte)
        const StrokePreview* clipStroke = nullptr;   // trazo sobre la base
        GLuint backdrop = 0;                          // lo de debajo; 0: mezcla la GPU
        const StrokeGrain* layerGrain = nullptr;      // grano sobre la propia capa (un trazo)
        GLuint selection = 0;                         // máscara de la selección
        int selectionOn = 0;                          // 1: sobre el trazo; 2: sobre la capa
    };

    void bindCanvasPass(GLuint target, const IRect& rect);
    void drawPass(const Pass& pass);
    // Color liso; con `mask`, multiplicado por ella.
    void drawColor(const float rgba[4], GLuint mask = 0);
    void finishPass();
    bool ensureScratch();

    gfx::Program m_program;
    GLint m_uLayer = -1;
    GLint m_uStroke = -1;
    GLint m_uBackdrop = -1;
    GLint m_uClip = -1;
    GLint m_uOpacity = -1;
    GLint m_uStrokeOpacity = -1;
    GLint m_uStrokeMode = -1;
    GLint m_uClipMode = -1;
    GLint m_uClipStrokeMode = -1;
    GLint m_uBlend = -1;
    GLint m_uUseBackdrop = -1;
    GLint m_uGrain = -1;
    GLint m_uGrainScale = -1;
    GLint m_uGrainDepth = -1;
    GLint m_uGrainOn = -1;
    GLint m_uSelectionOn = -1;
    gfx::Program m_colorProgram;
    GLint m_uColor = -1;
    gfx::Program m_maskedColorProgram;
    GLint m_uMaskedColor = -1;
    gfx::Program m_filterProgram;
    GLint m_uFilterSource = -1;
    GLint m_uFilterMask = -1;
    GLint m_uFilterMaskOn = -1;
    GLint m_uFilterKind = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
    gfx::RenderTarget m_composite;
    gfx::RenderTarget m_scratch;   // segundo destino del compuesto; se crea al primer modo que no es Normal
    bool m_scratchFailed = false;  // no hubo memoria para crearlo: esas capas se ven en modo Normal
};
