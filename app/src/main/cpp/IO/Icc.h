#pragma once

#include "Gfx/ColorSpace.h"

#include <cstdint>
#include <vector>

// Perfiles ICC para llevar el perfil de color del lienzo dentro de las imágenes.
namespace icc {

// Perfil ICC v4 de pantalla (matriz y curvas) de Display P3, como el de Apple y Android:
// primarios DCI-P3, blanco D65 y la curva de sRGB. Para sRGB no hace falta: los PNG lo
// indican con su chunk sRGB. Vacío con sRGB.
const std::vector<uint8_t>& profile(ColorProfile profile);

} // namespace icc
