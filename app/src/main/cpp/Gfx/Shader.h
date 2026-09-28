#pragma once

#include "Gfx/GLObjects.h"

namespace gfx {

// Compila y enlaza un programa GLSL ES 3.00. Las fuentes van sin la línea #version:
// se añade aquí junto con la precisión por defecto. Si falla, deja el error en el log
// y devuelve un Program vacío.
Program makeProgram(const char* name, const char* vertexSource, const char* fragmentSource);

} // namespace gfx
