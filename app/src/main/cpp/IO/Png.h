#pragma once

#include "Canvas/CanvasSpec.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
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

// Como filterRows, pero en el mismo buffer: `image` (RGBA sin premultiplicar, filas seguidas
// de arriba abajo) pasa a ser las filas filtradas, que ocupan un byte más por fila (si se
// reservó antes, no se copia nada). False si no hay memoria (la imagen queda como estaba).
bool filterRowsInPlace(std::vector<uint8_t>& image, int width, int height);

// Comprime las filas filtradas y entrega el PNG. Si falla, deja el motivo en SDL_GetError().
bool write(const std::vector<uint8_t>& filtered, int width, int height, const Info& info, const Sink& sink);

// PNG de un solo color opaco (el fondo de un proyecto, para otros programas): con paleta de un
// bit por píxel, ocupa casi nada a cualquier tamaño.
bool writeSolid(int width, int height, const uint8_t rgb[3], const Info& info, const Sink& sink);

// filterRows y write.
bool encode(const uint8_t* rgba, int width, int height, size_t stride, const Info& info, const Sink& sink);

// Píxeles por metro del chunk pHYs para una resolución en ppp.
uint32_t pixelsPerMeter(float ppi);

// Imagen leída de un PNG: RGBA8 sin premultiplicar, con la fila 0 arriba.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// Lee un PNG comprobando el CRC de cada chunk. Lo que escribe LiveSketch (RGBA de 8 bits sin
// entrelazar) se lee con libdeflate, varias veces más rápido; el resto de tipos, con
// stb_image. Falla si está dañado o tiene más de `maxPixels` píxeles (motivo en `error`).
bool decode(std::span<const uint8_t> data, Image& out, size_t maxPixels, std::string* error = nullptr);

} // namespace png
