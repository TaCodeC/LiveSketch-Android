#pragma once

#include <glm/vec2.hpp>

#include <span>
#include <vector>

// Forma rápida: reconoce en un trazo a mano la forma perfecta que se le parece (línea,
// arco, polilínea, círculo, elipse, rectángulo, triángulo o polígono) y la ajusta mientras
// se mantiene el trazo. Sin GL: todo en píxeles del lienzo.
namespace quickshape {

enum class Kind { None, Line, Arc, Polyline, Circle, Ellipse, Rectangle, Triangle, Polygon };

struct Shape {
    Kind kind = Kind::None;
    // Línea: sus extremos. Arco: el inicio, un punto del medio y el final. Polilínea,
    // rectángulo, triángulo y polígono: los vértices en orden (los cerrados sin repetir el
    // primero).
    std::vector<glm::vec2> points;
    // Formas cerradas: su centro. Círculo y elipse: además sus semiejes (el segundo, negativo
    // si el contorno va al revés, como el trazo), el giro del primero (radianes) y el ángulo
    // (en el marco de la elipse) por donde empieza el contorno, que es por donde empezó el
    // trazo.
    glm::vec2 center{0.0f};
    glm::vec2 radii{0.0f};
    float angle = 0.0f;
    float start = 0.0f;
    // Hecha perfecta (ver regular): al ajustarla sigue siéndolo.
    bool regular = false;

    bool closed() const;
};

struct Options {
    // La horizontal de la pantalla en el lienzo: las líneas, los rectángulos y las elipses
    // casi derechos se enderezan con ella o con los ejes del lienzo.
    glm::vec2 screenRight{1.0f, 0.0f};
    // Los trazos más cortos (en píxeles del lienzo) no son ninguna forma.
    float minLength = 24.0f;
};

Shape recognize(std::span<const glm::vec2> stroke, const Options& options = {});

// La forma al llevar el puntero de `from` (donde estaba al reconocerla) a `to` sin soltar:
// la línea y la polilínea llevan su último punto al puntero, el arco su final (con la
// misma curvatura) y las cerradas giran y crecen alrededor de su centro.
Shape adjust(const Shape& shape, glm::vec2 from, glm::vec2 to, const Options& options = {});

// La forma perfecta: la elipse pasa a círculo, el rectángulo a cuadrado, el triángulo y el
// polígono a regulares (con el mismo centro y sentido, y el primer vértice hacia el mismo
// lado) y la línea gira de 15 en 15°. El arco y la polilínea no cambian.
Shape regular(const Shape& shape);

// Contorno para trazarla: las curvas, con puntos que se separan de la curva como mucho
// `tolerance` píxeles. Las cerradas terminan en su primer punto.
std::vector<glm::vec2> outline(const Shape& shape, float tolerance = 0.15f);

// Nombre de la forma para mostrarlo ("Elipse", "Cuadrado"...).
const char* name(const Shape& shape);

} // namespace quickshape
