#pragma once

#include "Canvas/StrokePath.h"

#include <glm/mat2x2.hpp>
#include <glm/vec2.hpp>

#include <vector>

// Guía de dibujo del lienzo: una cuadrícula o los ejes de una simetría, que se ven encima
// del lienzo (solo en la pantalla: no salen por NDI ni en el PNG). Con la simetría, cada
// trazo se repite reflejado (o girado) alrededor de su centro. Es parte del documento:
// cada lienzo tiene la suya.
enum class GuideKind { Grid, Symmetry };
enum class SymmetryKind { Vertical, Horizontal, Quadrant, Radial };

struct DrawingGuide {
    bool enabled = false;
    GuideKind kind = GuideKind::Grid;
    // De dónde parte la cuadrícula o dónde se cruzan los ejes, en píxeles del lienzo, y su
    // giro (radianes, en el sentido de las agujas del reloj).
    glm::vec2 center{0.0f};
    float angle = 0.0f;
    float opacity = 0.6f;       // de las líneas (0..1)
    float gridSize = 100.0f;    // lado de cada cuadro, en píxeles del lienzo
    SymmetryKind symmetry = SymmetryKind::Vertical;
    // Las copias van giradas alrededor del centro en vez de reflejadas.
    bool rotational = false;

    // Los trazos se repiten.
    bool mirrors() const { return enabled && kind == GuideKind::Symmetry; }
};

namespace guide {

inline constexpr float kMinGridSize = 4.0f;
inline constexpr float kMaxGridSize = 1000.0f;

// Una copia del trazo: p' = centro + matrix × (p − centro). Si refleja, la punta del
// pincel se ve al revés y su giro pasa a ser `angle` − giro; si no, giro + `angle`.
struct Copy {
    glm::mat2 matrix{1.0f};
    bool mirrored = false;
    float angle = 0.0f;
};

// Las copias que pide la guía, con el trazo mismo primero (una sola si no hay simetría).
std::vector<Copy> copies(const DrawingGuide& guide);

Dab transform(const Dab& dab, const Copy& copy, glm::vec2 center);
glm::vec2 transform(glm::vec2 point, const Copy& copy, glm::vec2 center);

// Lo que se dibuja de la guía, en píxeles del lienzo: segmentos ya recortados al lienzo
// (`size`). La cuadrícula, con sus líneas cada `gridSize` (o cada dos, cuatro... cuadros
// si quedarían a menos de `minSpacing`); la simetría, sus ejes.
struct Segment {
    glm::vec2 a;
    glm::vec2 b;
};
std::vector<Segment> lines(const DrawingGuide& guide, glm::vec2 size, float minSpacing = 0.0f);

// La guía por defecto de un lienzo: sin activar y centrada.
DrawingGuide defaults(glm::vec2 size);

} // namespace guide
