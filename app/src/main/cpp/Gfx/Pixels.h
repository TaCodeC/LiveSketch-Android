#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

// Pasa un buffer RGBA8 de alfa premultiplicado (como el compuesto del lienzo) a alfa
// normal, que es lo que esperan NDI y PNG. Los píxeles transparentes quedan en negro.
void unpremultiply(uint8_t* rgba, size_t pixelCount);

// Al revés: de alfa normal a premultiplicado (c · a / 255, redondeado). Con los redondeos de
// las dos, premultiplicar lo que salió de unpremultiply devuelve los mismos píxeles.
void premultiply(uint8_t* rgba, size_t pixelCount);

} // namespace gfx
