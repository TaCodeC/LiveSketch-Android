#pragma once

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <imgui.h>

// Dónde se ve el lienzo en la ventana, para las herramientas que se manejan sobre él
// (Selección, Transformar, la guía de dibujo...). La entrada llega y lo que dibujan encima
// va en unidades de ImGui (coordenadas de la ventana); ellas trabajan en píxeles del lienzo.
// La vista puede estar girada y volteada (ver Camera).
struct ToolView {
    glm::vec2 offset{0.0f};   // dónde cae el origen del lienzo, en unidades
    float zoom = 1.0f;        // unidades por píxel del lienzo
    // Hacia dónde van en la pantalla los ejes x e y del lienzo (vectores unitarios).
    glm::vec2 axisX{1.0f, 0.0f};
    glm::vec2 axisY{0.0f, 1.0f};

    glm::vec2 toCanvas(ImVec2 p) const {
        const glm::vec2 d = (glm::vec2(p.x, p.y) - offset) / zoom;
        return {glm::dot(d, axisX), glm::dot(d, axisY)};
    }
    ImVec2 toScreen(glm::vec2 p) const {
        const glm::vec2 s = offset + (axisX * p.x + axisY * p.y) * zoom;
        return ImVec2(s.x, s.y);
    }
    // Una distancia en unidades, en píxeles del lienzo.
    float toCanvasLength(float units) const { return units / zoom; }
    // Los ejes del lienzo van con los de la pantalla (sin giro o girada un múltiplo de 90°).
    bool upright() const { return axisX.x == 0.0f || axisX.y == 0.0f; }
    // Hacia dónde va en el lienzo la horizontal de la pantalla (vector unitario).
    glm::vec2 screenRight() const { return {axisX.x, axisY.x}; }
};
