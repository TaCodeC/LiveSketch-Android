#pragma once

#include <glm/vec2.hpp>

#include <cstddef>
#include <string>
#include <vector>

// Curva de presión del lápiz: lleva la presión que mide el lápiz (x, de 0 a 1) a la que
// usa el pincel (y, de 0 a 1). Pasa por sus puntos, de izquierda a derecha: el primero
// está siempre en x = 0 y el último en x = 1, pero se pueden subir o bajar (la presión
// mínima y la máxima del trazo). Entre ellos va una curva suave que no se sale de la
// altura de los puntos que une (Hermite monótona, de Fritsch y Carlson), así que nunca
// baja de 0 ni pasa de 1 y, si los puntos suben, ella también.
class PressureCurve {
public:
    static constexpr size_t kMaxPoints = 6;     // con los dos extremos
    static constexpr float kMinGap = 0.05f;     // separación mínima en x entre dos puntos

    // La recta de (0, 0) a (1, 1): la presión tal cual.
    PressureCurve();
    // Con estos puntos, arreglados si hace falta (ver normalize()).
    explicit PressureCurve(std::vector<glm::vec2> points);

    float operator()(float pressure) const;

    const std::vector<glm::vec2>& points() const { return m_points; }
    bool isDefault() const;
    bool operator==(const PressureCurve& other) const { return m_points == other.m_points; }

    // Edición. Los extremos solo se mueven en vertical; los de en medio, sin pasar de sus
    // vecinos (a kMinGap de ellos).
    void move(size_t index, glm::vec2 to);
    // Añade un punto en `at` si caben más y no queda pegado a otro. Devuelve su índice o -1.
    int add(glm::vec2 at);
    // Quita un punto de en medio (los extremos no se pueden quitar).
    void remove(size_t index);

    // Para guardarla: "x y x y ..." con los números justos para leerla igual.
    std::string toText() const;
    // La curva guardada con toText() (arreglada si hace falta; la recta si no se entiende).
    static PressureCurve fromText(const std::string& text);

private:
    // Ordena, recorta a [0, 1], pone los extremos en x = 0 y x = 1, quita los puntos
    // pegados y los que sobran, y calcula las tangentes.
    void normalize();
    void computeTangents();

    std::vector<glm::vec2> m_points;
    std::vector<float> m_tangents;   // pendiente de la curva en cada punto
};
