#pragma once

#include "Gfx/ColorSpace.h"
#include "Gfx/GLObjects.h"

namespace gfx {

// Compila y enlaza un programa GLSL ES 3.00. Las fuentes van sin la línea #version:
// se añade aquí junto con la precisión por defecto. `fragmentPrelude` (opcional) va delante
// del fragment shader: código común, como colorspace::kGlsl. Si falla, deja el error en el
// log y devuelve un Program vacío.
Program makeProgram(const char* name, const char* vertexSource, const char* fragmentSource,
                    const char* fragmentPrelude = nullptr);

// Los uniforms de colorspace::kGlsl en un programa.
struct GamutUniforms {
    GLint matrix = -1;
    GLint on = -1;

    void locate(const Program& program);
    // Con el programa en uso.
    void set(const colorspace::Transform& transform) const;
};

} // namespace gfx
