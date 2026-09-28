#pragma once

#include "Canvas/Layer.h"
#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

// Propiedades de una capa que se pueden deshacer.
struct LayerProperties {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
};

// Un paso de deshacer. Cada paso es simétrico: deshacerlo lo deja listo para rehacerlo y
// al revés (los píxeles guardados se intercambian con los de la capa; una capa quitada
// de la pila se guarda aquí mientras no está).
struct HistoryStep {
    enum class Kind {
        Pixels,       // trazo o limpiar: una zona de la capa
        AddLayer,     // añadir o duplicar
        RemoveLayer,
        MoveLayer,
        MergeDown,
        Properties,   // nombre, visibilidad u opacidad
    };

    Kind kind = Kind::Pixels;
    uint32_t layerId = 0;          // capa afectada (MergeDown: la de abajo)
    uint32_t otherId = 0;          // MergeDown: la de arriba
    IRect rect;                    // Pixels y MergeDown: zona guardada, en píxeles del lienzo
    gfx::RenderTarget pixels;      // esos píxeles
    std::unique_ptr<Layer> layer;  // capa fuera de la pila
    int index = 0;                 // posición de la capa (Move: origen)
    int target = 0;                // Move: destino
    float opacity = 1.0f;          // MergeDown: opacidad de la capa de abajo
    LayerProperties before;
    LayerProperties after;
    size_t bytes = 0;              // memoria de GPU que puede ocupar el paso
};

// Pila de deshacer y rehacer con límite de pasos y de memoria de GPU: al pasarse se
// olvidan los pasos más antiguos.
class History {
public:
    void setLimits(size_t maxBytes, int maxSteps);
    void clear();

    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < m_steps.size(); }
    int undoCount() const { return static_cast<int>(m_cursor); }
    size_t bytes() const { return m_bytes; }

    // Añade un paso ya hecho. Lo que se podía rehacer se descarta.
    void push(HistoryStep step);
    // Paso que hay que deshacer (o rehacer) ahora; mueve el cursor. nullptr si no hay.
    HistoryStep* stepToUndo();
    HistoryStep* stepToRedo();

private:
    void enforceLimits();

    std::deque<HistoryStep> m_steps;   // [0, m_cursor) hechos; [m_cursor, fin) deshechos
    size_t m_cursor = 0;
    size_t m_bytes = 0;
    size_t m_maxBytes = size_t{128} * 1024 * 1024;
    int m_maxSteps = 100;
};
