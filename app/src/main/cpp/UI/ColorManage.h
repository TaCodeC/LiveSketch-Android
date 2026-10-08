#pragma once

#include "Gfx/ColorSpace.h"

#include <imgui.h>

// Gestión del color de la interfaz. Lo que dibuja la interfaz está en sRGB, salvo los
// colores del dibujo (el selector, las muestras, la paleta, el fondo...), que son números
// del perfil del lienzo: esos se dibujan dentro de un gamut::Scope. Al final del frame, los
// colores de los vértices pasan al perfil de la pantalla. Las imágenes (miniaturas, cristal)
// ya llegan en el perfil de la pantalla y se dibujan con vértices blancos o grises, que
// ningún perfil cambia.
namespace ui::gamut {

// Al empezar el frame: el perfil de la pantalla y el del lienzo (sRGB si no hay lienzo).
void beginFrame(ColorProfile display, ColorProfile canvas);
ColorProfile display();
ColorProfile canvas();

// Los colores de lo que se dibuja en `dl` mientras vive son de `profile` (por defecto, el
// del lienzo). No se anidan.
class Scope {
public:
    explicit Scope(ImDrawList* dl);
    Scope(ImDrawList* dl, ColorProfile profile);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    ImDrawList* m_dl;
    int m_begin;
    ColorProfile m_profile;
};

// Pasa los colores de los vértices de `data` al perfil de la pantalla. Una vez por frame,
// después de ImGui::Render().
void convert(ImDrawData* data);

// Pasa los colores de los vértices [begin, end) de `dl` de `from` a `to` (el alfa no cambia).
void convertVertices(ImDrawList* dl, int begin, int end, ColorProfile from, ColorProfile to);

} // namespace ui::gamut
