#pragma once

#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

#include <cstdint>
#include <vector>

// Relleno por color (arrastrar el color al lienzo), en la GPU. La zona sale de los niveles
// de la selección automática (selection::autoLevels, uno por píxel del lienzo): se suben
// una vez y cambiar el umbral solo vuelve a dibujar.
class ColorFill {
public:
    bool init();
    void destroy();

    // Niveles del tamaño del lienzo (un byte por píxel, fila 0 arriba).
    bool upload(const std::vector<uint8_t>& levels, int width, int height);
    void drop();
    bool loaded() const { return static_cast<bool>(m_levels); }

    // Dibuja en `target` (FBO de `width` × `height`, el del lienzo) dentro de `rect` la capa
    // `layer` (textura del mismo tamaño, premultiplicada) rellena: lo que tiene nivel <=
    // `cutoff` pasa a ser `rgb`, y el píxel de alrededor recibe el color por detrás de lo
    // que ya tenía, para que no quede un halo claro entre el relleno y el borde suavizado
    // de las líneas. Con `alphaLock`, solo cambia el color de lo que ya está pintado. Con
    // `mask` (R8 del tamaño del lienzo), solo en lo seleccionado y según su valor.
    void draw(GLuint target, int width, int height, GLuint layer, const IRect& rect, int cutoff, const float rgb[3],
              bool alphaLock, GLuint mask);

private:
    gfx::Program m_program;
    GLint m_uLayer = -1;
    GLint m_uLevels = -1;
    GLint m_uMask = -1;
    GLint m_uMaskOn = -1;
    GLint m_uCutoff = -1;
    GLint m_uColor = -1;
    GLint m_uAlphaLock = -1;
    gfx::Texture m_levels;
    int m_width = 0;
    int m_height = 0;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
};
