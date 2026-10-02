#pragma once

#include "Canvas/CanvasSpec.h"
#include "Canvas/Layer.h"
#include "Canvas/Rect.h"
#include "Canvas/Selection.h"
#include "Gfx/GLObjects.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// Propiedades de una capa que se pueden deshacer (la referencia va aparte: es de la pila).
struct LayerProperties {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool alphaLock = false;
    bool clipping = false;

    bool operator==(const LayerProperties&) const = default;
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
        Properties,   // nombre, visibilidad, opacidad, fusión, bloqueo alfa o recorte
        Reference,    // cambia la capa de referencia
        Background,   // el color de fondo o si se ve
        Selection,    // la selección: una zona de la máscara (puede no haber) y su estado
        Group,        // varios pasos que se deshacen juntos (p. ej. transformar)
    };

    Kind kind = Kind::Pixels;
    uint32_t layerId = 0;          // capa afectada (MergeDown: la de abajo; Reference: la nueva o 0)
    uint32_t otherId = 0;          // MergeDown: la de arriba; Reference: la anterior o 0
    IRect rect;                    // Pixels, MergeDown y Selection: zona guardada, en píxeles del lienzo
    gfx::RenderTarget pixels;      // esos píxeles (Selection: de la máscara, R8)
    std::unique_ptr<Layer> layer;  // capa fuera de la pila
    int index = 0;                 // posición de la capa (Move: origen)
    int target = 0;                // Move: destino
    LayerProperties before;        // Properties y MergeDown (la capa de abajo)
    LayerProperties after;
    SelectionState selectionBefore;   // Selection
    SelectionState selectionAfter;
    CanvasBackground backgroundBefore;   // Background
    CanvasBackground backgroundAfter;
    std::vector<HistoryStep> children;   // Group, en el orden en que se hicieron
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
