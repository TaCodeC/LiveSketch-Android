#pragma once

#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

// Caja de lo que no es transparente de una imagen del tamaño del lienzo, calculada en la
// GPU: la imagen se reduce a bloques de 16 × 16 (1 si hay algo) y de los bloques de los
// bordes se leen solo esas tiras a tamaño real. Así no hace falta leer el lienzo entero.
class BoundsFinder {
public:
    bool init();
    void destroy();

    // Caja de los píxeles de `image` distintos de 0 (el alfa si es RGBA, el rojo si es R8)
    // dentro de `within`. Con `mask` (R8, del mismo tamaño), solo cuentan donde la máscara
    // tampoco es 0. Vacía si no hay ninguno.
    IRect find(const gfx::RenderTarget& image, const gfx::RenderTarget* mask, const IRect& within);

private:
    bool readStrip(const gfx::RenderTarget& image, const gfx::RenderTarget* mask, const IRect& strip,
                   IRect& found);

    gfx::Program m_program;
    GLint m_uImage = -1;
    GLint m_uMask = -1;
    GLint m_uChannel = -1;
    GLint m_uUseMask = -1;
    GLint m_uWithin = -1;
    gfx::RenderTarget m_blocks;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
};
