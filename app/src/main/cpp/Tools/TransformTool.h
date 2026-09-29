#pragma once

#include "Canvas/Rect.h"
#include "Tools/ToolView.h"

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <vector>

class Canvas;

// Herramienta Transformar: una caja con tiradores sobre lo que se transforma (lo
// seleccionado de la capa activa, o la capa entera).
//
// - Arrastrar dentro de la caja (o fuera, lejos de los tiradores) lo mueve.
// - Las esquinas y los lados escalan: en Libre cada eje por su lado, en Uniforme sin
//   cambiar la proporción. En Distorsionar cada esquina va adonde se lleve y un lado
//   mueve sus dos esquinas (perspectiva e inclinación).
// - El tirador de encima gira alrededor del centro.
// - Con el imán, al mover se pega al centro y a los bordes del lienzo, y al girar va de
//   15 en 15 grados.
//
// Las esquinas van en píxeles del lienzo (arriba izquierda, arriba derecha, abajo
// derecha, abajo izquierda) y cada cambio pasa a Canvas::setTransform. Mientras dura,
// guarda sus propios pasos para deshacer.
class TransformTool {
public:
    enum class Mode { Free, Uniform, Distort };

    // Canvas empezó a transformar: la caja parte de Canvas::transformSource().
    void start(Canvas& canvas);
    // La transformación terminó (se aplicó o se canceló).
    void stop();
    bool active() const { return m_active; }

    Mode mode() const { return m_mode; }
    void setMode(Mode mode) { m_mode = mode; }
    bool snap() const { return m_snap; }
    void setSnap(bool snap) { m_snap = snap; }
    bool nearest() const { return m_nearest; }
    void setNearest(Canvas& canvas, bool nearest);

    // Entrada, en unidades de ImGui.
    void press(Canvas& canvas, const ToolView& view, ImVec2 position);
    void drag(Canvas& canvas, const ToolView& view, ImVec2 position);
    void release(Canvas& canvas);
    // Otro dedo o el sistema cancelan el gesto: vuelve a como estaba al pulsar.
    void cancel(Canvas& canvas);
    bool gestureActive() const { return m_handle != Handle::None; }

    // Botones de la barra.
    void flip(Canvas& canvas, bool horizontal);
    void rotate90(Canvas& canvas);
    // Lo más grande que quepa en el lienzo, centrado y derecho.
    void fitCanvas(Canvas& canvas);
    // Como estaba al empezar.
    void reset(Canvas& canvas);
    void nudge(Canvas& canvas, glm::vec2 delta);

    bool canUndo() const { return !m_history.empty(); }
    bool undo(Canvas& canvas);

    void drawOverlay(ImDrawList* dl, const ToolView& view) const;

    // Lo que tapa la interfaz (barras, opciones) y el tamaño de la ventana, en unidades:
    // si el tirador de girar queda debajo o fuera, pasa al lado de abajo.
    void setCovered(std::vector<ImRect> covered, ImVec2 display);

private:
    using Quad = std::array<glm::vec2, 4>;
    enum class Handle { None, Move, Rotate, Corner, Edge };

    // Pasa las esquinas a Canvas. false (y todo sigue igual) si no valen.
    bool apply(Canvas& canvas, const Quad& corners);
    // Guarda las esquinas de ahora para deshacer y aplica `corners`.
    void change(Canvas& canvas, const Quad& corners);
    Quad sourceQuad() const;
    // Punto de la fuente (píxeles del lienzo antes de transformar) en el lienzo, con la
    // transformación de ahora o la del principio del gesto.
    glm::vec2 image(const glm::mat3& homography, glm::vec2 p) const;
    glm::vec2 center() const;
    Handle hit(const ToolView& view, ImVec2 position, int& index) const;
    // Punto medio de un lado (0 arriba, 1 derecha, 2 abajo, 3 izquierda), en la pantalla.
    ImVec2 edgeMid(const ToolView& view, int edge) const;
    // El tirador de girar va fuera del lado que queda más arriba en la pantalla, o de otro
    // si ese lo tapa la interfaz; si los tapa todos (la caja llena la pantalla), dentro
    // del de arriba. Durante el giro se queda donde estaba al pulsar.
    struct Knob {
        int edge = 0;
        bool inside = false;
    };
    Knob rotationKnob(const ToolView& view) const;
    ImVec2 rotationHandle(const ToolView& view, Knob knob) const;
    bool covered(ImVec2 p) const;
    // Escalar desde una esquina o un lado: se hace en el espacio de la fuente, así respeta
    // la perspectiva que ya tenga.
    bool scaleCorner(int corner, glm::vec2 pointer, Quad& out) const;
    bool scaleEdge(int edge, glm::vec2 pointer, Quad& out) const;
    glm::vec2 snapMove(const Canvas& canvas, const ToolView& view, glm::vec2 delta);

    bool m_active = false;
    IRect m_source;
    Quad m_corners{};
    glm::mat3 m_homography{1.0f};   // fuente → lienzo
    Mode m_mode = Mode::Free;
    bool m_snap = false;
    bool m_nearest = false;
    std::vector<Quad> m_history;

    // Gesto en curso.
    Handle m_handle = Handle::None;
    int m_index = 0;                  // la esquina o el lado
    bool m_knobInside = false;        // girando con el tirador de dentro de la caja
    Quad m_startCorners{};
    glm::mat3 m_startHomography{1.0f};
    glm::vec2 m_startPointer{0.0f};   // en el lienzo
    glm::vec2 m_pivot{0.0f};
    bool m_moved = false;
    // Guías del imán que se ven mientras se mueve (en el lienzo).
    std::vector<float> m_guideX;
    std::vector<float> m_guideY;
    glm::vec2 m_canvasSize{0.0f};
    std::vector<ImRect> m_covered;
    ImVec2 m_display{0.0f, 0.0f};
};
