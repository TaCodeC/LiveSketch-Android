#pragma once

#include "Gfx/GL.h"
#include "UI/Kit.h"

#include <cstdint>
#include <string>

// Piezas que comparten los paneles y las pantallas de la interfaz.
namespace ui::parts {

inline ImTextureID textureId(GLuint texture) { return static_cast<ImTextureID>(texture); }

// Resalte de una fila o un botón al pulsarlo (o con el ratón encima).
void pressFeedback(ImDrawList* dl, ImGuiID id, const ImRect& rect, const Press& press, float radius);

// Cabecera de un panel: el título a la izquierda y una línea debajo. Devuelve su centro
// vertical, para lo que va a la derecha.
float panelHeader(ImDrawList* dl, const ImRect& content, float top, const char* title);

// Cabecera de una página dentro de un panel: botón de volver, el título y una línea
// debajo. Devuelve el centro vertical; `back` dice si se tocó volver y `titleRight`
// (opcional) dónde acaba el título.
float backHeader(ImDrawList* dl, const ImRect& content, float top, const char* id, const char* title, bool* back,
                 float* titleRight = nullptr);

// Etiqueta en mayúsculas sobre un fondo suave, con un punto delante si `dot` no es 0.
// `right` es su borde derecho y `cy` su centro.
void tag(ImDrawList* dl, float right, float cy, const char* text, ImU32 fill, ImU32 color, ImU32 dot = 0);

// Fila con un interruptor a la derecha.
bool toggleRow(ImDrawList* dl, const char* id, const ImRect& row, const char* glyph, const char* title, bool* value);

// La marca de la app: un cuadrado rojo y «LIVESKETCH». `cy`: su centro vertical.
void brand(ImDrawList* dl, float left, float cy);

// Una fecha (SDL_Time) en la hora local, para mostrarla: «Hoy, 14:32», «3 oct, 9:05» o «3 oct
// 2025». «—» si no se sabe (0).
std::string dateText(int64_t when);

} // namespace ui::parts
