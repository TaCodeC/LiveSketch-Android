#pragma once

#include "Canvas/BrushLibrary.h"

#include <cstdint>
#include <vector>

// Imágenes de las puntas y los granos, generadas por código (sin archivos): siempre las
// mismas, porque todo el azar sale de semillas fijas. Un byte por píxel, fila 0 arriba.
namespace brushtips {

inline constexpr int kTipSize = 256;
inline constexpr int kGrainSize = 256;

// Cobertura de la punta (0..255) en `size`×`size`. Llega a 0 antes de los bordes, así que
// se puede estirar sobre el cuadrado del sello sin cortes. Round es un disco duro (solo se
// usa para mostrarla: al pintar, la redonda se calcula en el shader). Las clásicas no se
// generan: salen de brush0.png ... brush3.png.
std::vector<uint8_t> makeTip(BrushTip tip, int size = kTipSize);

// Relieve del papel (0..255; 255: la pintura agarra del todo) en `size`×`size`. Se repite
// sin costuras en los dos ejes.
std::vector<uint8_t> makeGrain(BrushGrain grain, int size = kGrainSize);

} // namespace brushtips
