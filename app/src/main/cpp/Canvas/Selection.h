#pragma once

#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

#include <cstdint>
#include <vector>

// Cómo entra una forma nueva en la selección que hay.
enum class SelectOp {
    Replace,    // la sustituye
    Add,        // se suma
    Subtract,   // se resta
};

// Lo que dice si hay selección y dónde. Va en el historial con los píxeles de la máscara.
// La máscara no se borra al deseleccionar (así deshacer solo cambia `active`): lo que
// quede en ella no cuenta mientras no haya selección.
struct SelectionState {
    bool active = false;
    IRect content;   // caja de lo que no es 0 en la máscara; puede sobrar, nunca faltar

    bool operator==(const SelectionState&) const = default;
};

// Máscara de la selección en la GPU: un canal (R8) del tamaño del lienzo que vale 1 en lo
// seleccionado y 0 fuera, con valores intermedios en los bordes suavizados o difuminados.
// Sin selección activa no se usa. Aquí solo se dibuja en la máscara; el historial y lo
// que depende de ella (pintar, transformar) lo lleva Canvas.
class Selection {
public:
    bool init(int width, int height);   // programas; las texturas se crean al usarlas
    void destroy();

    int width() const { return m_width; }
    int height() const { return m_height; }

    const SelectionState& state() const { return m_state; }
    void setState(const SelectionState& state) { m_state = state; }
    bool active() const { return m_state.active; }
    // Caja de lo seleccionado (vacía sin selección).
    IRect bounds() const { return m_state.active ? m_state.content : IRect{}; }
    // Textura de la máscara si hay selección activa (0 si no).
    GLuint activeMask() const { return m_state.active && m_mask ? m_mask.texture.id() : 0; }
    const gfx::RenderTarget& mask() const { return m_mask; }
    gfx::RenderTarget& mask() { return m_mask; }
    bool ensureMask();

    // Copia de trabajo del tamaño del lienzo: la máscara de antes de un cambio en vivo
    // (umbral, difuminado) e intermedio para deshacer.
    bool ensureScratch();
    gfx::RenderTarget& scratch() { return m_scratch; }

    // Cómo se dibuja una fuente en la máscara.
    enum class Combine {
        Union,      // máximo de las dos
        Subtract,   // mínimo con la fuente invertida
    };
    // Una cobertura de rect.width() × rect.height() bytes (fila 0 arriba) en `rect`.
    bool drawCoverage(const std::vector<uint8_t>& coverage, const IRect& rect, Combine how);
    // Niveles de la selección automática (tamaño del lienzo). drawLevels dibuja en `rect`
    // lo que tiene nivel <= `cutoff`, con un borde suave de `softness` niveles.
    bool uploadLevels(const std::vector<uint8_t>& levels);
    void dropLevels();
    void drawLevels(const IRect& rect, int cutoff, float softness, Combine how);
    // El alfa de una textura del tamaño del lienzo (el contenido de una capa).
    void drawAlpha(GLuint texture, const IRect& rect, Combine how);

    void clear(const IRect& rect);
    void fill(const IRect& rect);     // a 1
    void invert(const IRect& rect);
    void copyToScratch(const IRect& rect);
    void copyFromScratch(const IRect& rect);
    // Máscara = copia de trabajo desenfocada (gaussiana de `sigma` píxeles) dentro de `rect`.
    void blurFromScratch(const IRect& rect, float sigma);

    // Lee la máscara en `rect` como RGBA (el valor en el canal rojo).
    bool read(const IRect& rect, std::vector<uint8_t>& rgba) const;

private:
    enum class Source { Red, Alpha, Levels, One };
    void draw(GLuint texture, int textureWidth, int textureHeight, int originX, int originY, Source source,
              const IRect& rect, bool invertSource, float cutoff = 0.0f, float softness = 1.0f);
    void bindMaskPass(GLuint fbo, int width, int height, const IRect& scissor);
    void blurPass(const gfx::RenderTarget& target, GLuint source, const float map[4], float stepX, float stepY,
                  float sigma);
    void finishPass();

    int m_width = 0;
    int m_height = 0;
    SelectionState m_state;
    gfx::RenderTarget m_mask;
    gfx::RenderTarget m_scratch;
    gfx::Texture m_upload;           // cobertura de una forma
    gfx::Texture m_levels;           // niveles de la selección automática
    gfx::Texture m_dummy;            // 1 × 1: la fuente de lo que no lee ninguna
    std::vector<gfx::RenderTarget> m_blurLevels;
    gfx::RenderTarget m_blurTemp[2];

    gfx::Program m_program;
    GLint m_uSource = -1;
    GLint m_uSourceMap = -1;
    GLint m_uKind = -1;
    GLint m_uCutoff = -1;
    GLint m_uSoftness = -1;
    GLint m_uInvert = -1;
    gfx::Program m_blurProgram;
    GLint m_uBlurSource = -1;
    GLint m_uBlurMap = -1;
    GLint m_uBlurStep = -1;
    GLint m_uBlurSigma = -1;
    GLint m_uBlurRadius = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
};
