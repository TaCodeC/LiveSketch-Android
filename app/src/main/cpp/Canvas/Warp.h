#pragma once

#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>

// Transformaciones proyectivas (homografías) en píxeles del lienzo, con la y hacia abajo.
// Las esquinas van siempre en el orden arriba izquierda, arriba derecha, abajo derecha y
// abajo izquierda.
namespace warp {

// Homografía que lleva las esquinas del rectángulo `from` a `to`. Devuelve false si `to`
// está degenerado (tres esquinas en línea o el cuadrilátero se cruza).
bool homography(const IRect& from, const glm::vec2 to[4], glm::mat3& out);
// Aplica `h` a un punto. `valid` sale false si el punto cae detrás del horizonte.
glm::vec2 apply(const glm::mat3& h, glm::vec2 p, bool* valid = nullptr);
// Caja, en píxeles y con un píxel de margen, de la imagen de `rect` por `h`, recortada a
// `clip`. Si la imagen no tiene límite (cruza el horizonte), es `clip` entero.
IRect imageBounds(const glm::mat3& h, const IRect& rect, const IRect& clip);

} // namespace warp

// Dibuja una textura transformada por una homografía (la vista previa de Transformar y
// la máscara de la selección al aplicarla).
class Warp {
public:
    bool init();
    void destroy();

    enum class Mode {
        Over,      // RGBA premultiplicado encima de lo que haya
        Replace,   // sustituye lo que haya (una máscara R8: el rojo)
    };
    // Dibuja `source` (textura de `sourceWidth` × `sourceHeight`) en el FBO `target`, de
    // `targetWidth` × `targetHeight`, dentro de `rect`. `toSource` lleva un píxel del
    // destino a píxeles de la fuente; fuera de ella queda transparente. `nearest`: sin
    // suavizar (píxel a píxel).
    void draw(GLuint target, int targetWidth, int targetHeight, GLuint source, int sourceWidth, int sourceHeight,
              const glm::mat3& toSource, const IRect& rect, Mode mode, bool nearest);

private:
    gfx::Program m_program;
    GLint m_uSource = -1;
    GLint m_uToSource = -1;
    GLint m_uSourceSize = -1;
    GLint m_uRed = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
};
