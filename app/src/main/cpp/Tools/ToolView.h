#pragma once

#include <glm/vec2.hpp>
#include <imgui.h>

// Dónde se ve el lienzo en la ventana, para las herramientas que se manejan sobre él
// (Selección y Transformar). La entrada llega y lo que dibujan encima va en unidades de
// ImGui (coordenadas de la ventana); ellas trabajan en píxeles del lienzo.
struct ToolView {
    glm::vec2 offset{0.0f};   // esquina superior izquierda del lienzo, en unidades
    float zoom = 1.0f;        // unidades por píxel del lienzo

    glm::vec2 toCanvas(ImVec2 p) const { return (glm::vec2(p.x, p.y) - offset) / zoom; }
    ImVec2 toScreen(glm::vec2 p) const {
        const glm::vec2 s = offset + p * zoom;
        return ImVec2(s.x, s.y);
    }
    // Una distancia en unidades, en píxeles del lienzo.
    float toCanvasLength(float units) const { return units / zoom; }
};
