#pragma once

#include "Canvas/Selection.h"
#include "Tools/ToolView.h"

#include <glm/vec2.hpp>
#include <imgui.h>

#include <cstddef>
#include <vector>

class Canvas;

// Herramienta Selección: recoge la forma sobre el lienzo y se la pasa a Canvas.
//
// - Lazo: arrastrar dibuja a mano alzada y al soltar se cierra. Tocar pone los vértices
//   de un polígono (entre toque y toque también se puede arrastrar); se cierra tocando
//   el primer punto, con Intro o con el botón de cerrar.
// - Rectángulo y elipse: de esquina a esquina (con Mayús, cuadrado o círculo). Un toque
//   sin arrastrar quita la selección.
// - Automática: tocar selecciona lo parecido al color de ese punto y unido a él; arrastrar
//   en horizontal cambia el umbral mientras se ve el resultado.
//
// Cómo entra la forma nueva (sustituye, suma o resta) lo fija la barra de la herramienta;
// con el ratón, Mayús suma y Alt resta.
class SelectTool {
public:
    enum class Shape { Lasso, Rectangle, Ellipse, Auto };

    Shape shape() const { return m_shape; }
    void setShape(Shape shape);
    SelectOp op() const { return m_op; }
    void setOp(SelectOp op) { m_op = op; }
    // Umbral de la automática (0..1); se recuerda para la siguiente vez.
    float threshold() const { return m_threshold; }

    // Lo que pasó al soltar o al cerrar, para avisar.
    enum class Result {
        None,
        Selected,
        Deselected,
        Empty,         // la forma no cubre ningún píxel del lienzo
        NoSelection,   // restar sin selección
    };

    // Entrada, en unidades de ImGui. `modifier`: Add o Subtract si los pide el teclado;
    // Replace para usar el de la barra. `constrain`: Mayús mientras se arrastra.
    void press(Canvas& canvas, const ToolView& view, ImVec2 position, SelectOp modifier);
    void drag(Canvas& canvas, const ToolView& view, ImVec2 position, bool constrain);
    Result release(Canvas& canvas, const ToolView& view, ImVec2 position);
    // Otro dedo o el sistema cancelan el gesto: no se hace nada.
    void cancel(Canvas& canvas);
    bool gestureActive() const { return m_gesture != Gesture::None; }

    // Polígono del lazo a medias.
    bool pendingPolygon() const { return !m_polygon.empty(); }
    Result closePolygon(Canvas& canvas);
    void dropPolygon();
    // Quita el último punto del polígono (deshacer). false si no hay polígono.
    bool undoPoint();

    // Se está arrastrando el umbral de la automática (para mostrarlo).
    bool adjustingThreshold() const { return m_gesture == Gesture::Auto && m_autoMoved; }

    // Lo que se está dibujando (el lazo, la caja), encima del lienzo.
    void drawOverlay(ImDrawList* dl, const ToolView& view) const;

private:
    enum class Gesture { None, Freehand, Box, Auto };

    Result select(Canvas& canvas, const std::vector<glm::vec2>& polygon, SelectOp op);
    // La caja del rectángulo o la elipse, con la proporción fijada si hace falta.
    glm::vec2 boxCorner() const;
    bool nearFirstPoint(const ToolView& view, ImVec2 position) const;

    Shape m_shape = Shape::Lasso;
    SelectOp m_op = SelectOp::Replace;
    float m_threshold = 0.25f;

    Gesture m_gesture = Gesture::None;
    SelectOp m_gestureOp = SelectOp::Replace;
    ImVec2 m_pressScreen;
    bool m_moved = false;              // pasó de un toque a un arrastre
    bool m_shiftAtPress = false;       // Mayús ya pedía sumar: no fija la proporción
    std::vector<glm::vec2> m_stroke;   // tramo a mano alzada del gesto, en el lienzo
    glm::vec2 m_anchor{0.0f};          // caja: la esquina donde empezó
    glm::vec2 m_current{0.0f};
    bool m_constrain = false;
    float m_autoStart = 0.0f;          // umbral al empezar a arrastrar
    bool m_autoMoved = false;

    Result m_deferred = Result::None;  // lo que dirá soltar un gesto que no empezó

    std::vector<glm::vec2> m_polygon;  // polígono del lazo a medias
    std::vector<size_t> m_segments;    // dónde empieza cada tramo (un toque o un arrastre)
    SelectOp m_polygonOp = SelectOp::Replace;
};
