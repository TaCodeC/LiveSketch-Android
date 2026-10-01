#pragma once

#include "Canvas/BrushLibrary.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <vector>

// Un sello listo para la GPU: seis floats seguidos, que se suben tal cual.
struct Dab {
    float x = 0.0f;          // centro, en píxeles del lienzo
    float y = 0.0f;
    float radius = 0.0f;     // píxeles
    float alpha = 0.0f;      // 0..1
    float angle = 0.0f;      // radianes (con la y hacia abajo, positivo gira a la derecha)
    float distance = 0.0f;   // recorrido desde el principio del trazo (la carga de la mezcla húmeda)
};

// Convierte las muestras del lápiz en sellos: estabilización, espaciado, presión, afinado
// de los extremos y variación al azar. Sin GL.
//
// El afinado del final depende de dónde acabe el trazo, y eso no se sabe hasta levantar
// el lápiz: los sellos de los últimos píxeles son provisionales (se recalculan con cada
// muestra) y los de antes, definitivos. Sin afinado, todos son definitivos.
class StrokePath {
public:
    struct Settings {
        float radius = 10.0f;          // radio con presión 1, en píxeles del lienzo
        float flow = 1.0f;             // alfa de cada sello con presión 1
        float pixelsPerPoint = 1.0f;   // píxeles del lienzo por punto de pantalla
        uint32_t seed = 0;             // azar del trazo (variación y dispersión)
    };

    void begin(const BrushParams& params, const Settings& settings, float x, float y, float pressure);
    void moveTo(float x, float y, float pressure);
    // Forma rápida: el recorrido pasa a ser `points` (en píxeles del lienzo) con la presión
    // `pressure`, sin estabilizar, y los sellos vuelven a salir desde el principio (los de
    // antes los tiene que quitar quien los pintó). El trazo sigue sin terminar.
    void reshape(std::span<const glm::vec2> points, float pressure);
    // Levanta el lápiz: el trazo alcanza al puntero y todos los sellos pasan a definitivos.
    void finish();
    bool active() const { return m_active; }
    bool finished() const { return m_finished; }

    // Añade a `out` los sellos definitivos nuevos desde la última llamada.
    void takeFinal(std::vector<Dab>& out);
    bool hasFinal() const { return !m_finalDabs.empty(); }
    // Sellos provisionales del final del trazo.
    const std::vector<Dab>& provisional() const { return m_provisional; }
    // Sube cada vez que se recalculan los sellos (los provisionales pueden cambiar).
    uint32_t revision() const { return m_revision; }
    // El trazo puede tener sellos provisionales: afina algún extremo.
    bool tapered() const { return m_startTaper + m_endTaper > 0.0f; }

    // Longitud del recorrido (ya estabilizado) en píxeles del lienzo.
    float length() const { return m_path.empty() ? 0.0f : m_path.back().s; }
    // Distancia a la que el trazo sigue al puntero (estabilización), en píxeles del lienzo.
    float pull() const { return m_pull; }

private:
    struct Point {
        float x;
        float y;
        float pressure;
        float s;   // longitud del recorrido hasta aquí
    };
    // Dónde va el próximo sello y lo que hace falta para seguir desde ahí.
    struct Cursor {
        float s = 0.0f;          // longitud del recorrido del próximo sello
        float lastS = 0.0f;      // la del anterior (suavizado de la dirección)
        size_t segment = 0;      // tramo del recorrido donde está `s` (pista para buscar)
        uint32_t index = 0;      // sellos hechos: elige el azar de cada uno
        float dirX = 1.0f;       // dirección del trazo, suavizada
        float dirY = 0.0f;
        bool hasDir = false;
    };
    struct Sample {
        float x;
        float y;
        float pressure;
        float dirX;
        float dirY;
        bool hasDir;
    };
    struct Taper {
        float start;
        float end;
        float tip;
        float factor(float s, float length) const;
    };

    void appendPoint(float x, float y, float pressure);
    void emit();
    Sample sampleAt(Cursor& cursor) const;
    void stamp(Cursor& cursor, const Sample& sample, const Taper& taper, float length, std::vector<Dab>& out) const;

    BrushParams m_params;
    Settings m_settings;
    std::vector<Point> m_path;
    float m_rawX = 0.0f;
    float m_rawY = 0.0f;
    float m_rawPressure = 1.0f;
    float m_pull = 0.0f;
    float m_startTaper = 0.0f;   // longitudes del afinado con el trazo ya largo
    float m_endTaper = 0.0f;
    bool m_active = false;
    bool m_finished = false;

    Cursor m_final;                 // tras el último sello definitivo
    std::vector<Dab> m_finalDabs;   // definitivos que aún no se han llevado
    std::vector<Dab> m_provisional;
    uint32_t m_revision = 0;
};
