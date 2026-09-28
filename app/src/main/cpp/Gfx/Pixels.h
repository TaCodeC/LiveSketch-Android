#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

// Pasa un buffer RGBA8 de alfa premultiplicado (como el compuesto del lienzo) a alfa
// normal, que es lo que esperan NDI y PNG. Los píxeles transparentes quedan en negro.
void unpremultiply(uint8_t* rgba, size_t pixelCount);

} // namespace gfx
