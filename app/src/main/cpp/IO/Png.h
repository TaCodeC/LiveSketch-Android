#pragma once

#include "Canvas/CanvasSpec.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// PNG RGBA de 8 bits comprimido con libdeflate. Cada fila va con el filtro Up (la
// diferencia con la de arriba): en pintura comprime casi como elegir el mejor filtro fila a
// fila y cuesta mucho menos. Lleva la resolución (pHYs: se imprime al tamaño del lienzo), el
// perfil de color (sRGB, o Display P3 con su perfil ICC) y, si lo tiene, el nombre del lienzo.
namespace png {

struct Info {
    float ppi = 0.0f;                            // píxeles por pulgada; 0: sin resolución
    ColorProfile profile = ColorProfile::Srgb;
    std::string title;                           // nombre del lienzo (UTF-8); vacío: no va
};

// Recibe el PNG por partes, en orden. Devuelve false si no pudo (y se para).
using Sink = std::function<bool(const uint8_t* data, size_t size)>;

// Las filas filtradas, listas para comprimir: cada una con el tipo de filtro delante.
// `rgba` sin premultiplicar, con la fila 0 arriba y `stride` bytes de una fila a la
// siguiente. Vacío si no hay memoria.
std::vector<uint8_t> filterRows(const uint8_t* rgba, int width, int height, size_t stride);

// Comprime las filas filtradas y entrega el PNG. Si falla, deja el motivo en SDL_GetError().
bool write(const std::vector<uint8_t>& filtered, int width, int height, const Info& info, const Sink& sink);

// filterRows y write.
bool encode(const uint8_t* rgba, int width, int height, size_t stride, const Info& info, const Sink& sink);

// Píxeles por metro del chunk pHYs para una resolución en ppp.
uint32_t pixelsPerMeter(float ppi);

} // namespace png
