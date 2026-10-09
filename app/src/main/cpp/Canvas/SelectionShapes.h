#pragma once

#include "Canvas/Rect.h"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Lo que la selección calcula en la CPU: rellenar un polígono (lazo, rectángulo o elipse)
// con los bordes suavizados y la selección automática por color. Todo en píxeles del
// lienzo, con la fila 0 arriba.
namespace selection {

// Cobertura (0..255) de un polígono cerrado, con los bordes suavizados y la regla de
// "distinto de cero": un lazo que se cruza consigo mismo queda relleno entero. Solo se
// calcula dentro de `clip`. `bounds` sale con la zona calculada y `coverage` con
// bounds.width() × bounds.height() bytes. Devuelve false si no cubre ningún píxel.
bool rasterize(std::span<const glm::vec2> polygon, const IRect& clip, std::vector<uint8_t>& coverage,
               IRect& bounds);

// Polígonos de las formas. El rectángulo y la elipse van inscritos en la caja de las
// esquinas `a` y `b` (en cualquier orden).
std::vector<glm::vec2> rectangle(glm::vec2 a, glm::vec2 b);
std::vector<glm::vec2> ellipse(glm::vec2 a, glm::vec2 b);

// Selección automática. Para cada píxel, el umbral a partir del cual entra en la zona
// del punto tocado: la mayor diferencia de color con ese punto que hay que cruzar en el
// mejor camino hasta él (0..254; la diferencia es la mayor de los cuatro canales).
struct AutoLevels {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> levels;
    std::array<IRect, 256> bounds;   // caja de los píxeles con nivel <= n
};
// `rgba`: imagen premultiplicada de width × height. Devuelve false si (x, y) cae fuera.
bool autoLevels(const uint8_t* rgba, int width, int height, int x, int y, AutoLevels& out);
// Nivel de corte para un umbral de 0 a 1.
int autoCutoff(float threshold);

// Caja de los píxeles de una imagen RGBA de width × height cuyo canal `channel` no es 0
// (y, si se da `mask`, otra imagen RGBA del mismo tamaño, cuyo canal `maskChannel`
// tampoco lo es), en coordenadas de la imagen. Vacía si no hay ninguno.
IRect tightBounds(const uint8_t* rgba, int width, int height, int channel, const uint8_t* mask = nullptr,
                  int maskChannel = 0);

} // namespace selection
