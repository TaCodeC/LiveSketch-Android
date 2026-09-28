#pragma once

#include "UI/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <string>

// Kit de la interfaz con aspecto de iOS. ImGui pone la entrada, el texto y los IDs; el
// aspecto se dibuja aquí con ImDrawList: cristal que desenfoca el lienzo, sombras suaves,
// iconos de Lucide y controles como los de UIKit.
//
// Todas las medidas de diseño van en puntos (pt): pt() las pasa a unidades de ImGui
// (las coordenadas de la ventana de SDL), según la densidad de la pantalla y el tamaño
// de interfaz que elija el usuario.
namespace ui {

// --- Escala -------------------------------------------------------------------------

// Unidades de ImGui por punto y píxeles físicos por unidad de ImGui.
void setScale(float unitsPerPoint, float pixelsPerUnit);
float scale();
inline float pt(float points) { return points * scale(); }
inline ImVec2 pt(float x, float y) { return ImVec2(pt(x), pt(y)); }
// Grosor de una línea de un píxel físico.
float hairline();
// Redondea a píxeles físicos (bordes nítidos).
float snap(float units);

// --- Texto e iconos -----------------------------------------------------------------

enum class Weight { Regular, SemiBold, Bold };

// Carga Inter y los iconos. Si faltan los archivos se usa la fuente de ImGui.
bool loadFonts();
ImFont* font(Weight weight);
// Tamaño de ImGui (altura de línea) de un texto de `points` pt de cuerpo, como un
// font-size de CSS o de UIKit (Inter mide 1,21 em de alto).
float fontSize(float points);

ImVec2 measure(Weight weight, float points, const char* text, const char* end = nullptr, float wrapWidth = 0.0f);
// `pos` es la esquina superior izquierda de la caja de la línea: centrar esa caja
// centra las mayúsculas.
void text(ImDrawList* dl, Weight weight, float points, ImVec2 pos, ImU32 color, const char* text,
          const char* end = nullptr, float wrapWidth = 0.0f);
// Texto en una línea: si no cabe en `maxWidth` se corta con "…".
void textFit(ImDrawList* dl, Weight weight, float points, ImVec2 pos, float maxWidth, ImU32 color, const char* text);
// Texto centrado en `center` (una línea).
void textCentered(ImDrawList* dl, Weight weight, float points, ImVec2 center, ImU32 color, const char* text);
// Icono (constante de UI/Icons.h) de `points` pt centrado en `center`.
void icon(ImDrawList* dl, const char* glyph, ImVec2 center, float points, ImU32 color);

enum class Align { Left, Center, Right };
// Una línea con su centro vertical en `anchor.y`. `anchor.x` es el borde izquierdo, el
// centro o el borde derecho según `align`. Con `maxWidth` > 0 se corta con "…".
void label(ImDrawList* dl, Weight weight, float points, ImVec2 anchor, Align align, ImU32 color, const char* text,
           float maxWidth = 0.0f);
// Rótulo con espacio extra entre letras (`tracking` en em), como "EN VIVO". `anchor`:
// borde izquierdo y centro vertical.
float trackedWidth(Weight weight, float points, const char* text, float tracking);
void tracked(ImDrawList* dl, Weight weight, float points, ImVec2 anchor, ImU32 color, const char* text,
             float tracking);
// Texto que cabe en `maxWidth` (cortado con "…" si hace falta).
std::string fitText(Weight weight, float points, const char* text, float maxWidth);
// Párrafo repartido en líneas de `width` como mucho, alineadas según `align` dentro de
// [pos.x, pos.x + width]. Devuelve su altura. Con `dl` null solo mide.
float paragraph(ImDrawList* dl, Weight weight, float points, ImVec2 pos, float width, Align align, ImU32 color,
                const char* text, float lineHeight = 1.3f);

// --- Color --------------------------------------------------------------------------

ImU32 withAlpha(ImU32 color, float factor);
ImU32 mix(ImU32 a, ImU32 b, float t);
ImU32 fromFloat(const float rgb[3], float alpha = 1.0f);
// Luminancia aproximada (0..1) para decidir si encima va texto claro u oscuro.
float luminance(const float rgb[3]);

// --- Materiales ---------------------------------------------------------------------

// Textura con el lienzo desenfocado (0 si no hay) y tamaño de la pantalla en unidades.
void setBackdrop(ImTextureID texture, ImVec2 displaySize);
// Sombra difusa por fuera de un rectángulo redondeado: no oscurece lo que tiene encima,
// que suele ser translúcido. `offsetY` la desplaza hacia abajo; todo en unidades.
void shadow(ImDrawList* dl, const ImRect& rect, float radius, float blur, float offsetY, float alpha,
            ImDrawFlags corners = 0, ImU32 color = theme::kShadow);
// Cristal esmerilado: el lienzo desenfocado, un tinte, un borde fino y un brillo arriba.
// `dim`: oscurecido que hay entre el lienzo y el cristal (el de detrás de una alerta).
void glass(ImDrawList* dl, const ImRect& rect, float radius, ImU32 tint, ImDrawFlags corners = 0, float dim = 0.0f);
// Dibuja sin el recorte de la ventana (sombras que se salen de ella).
void pushUnclipped(ImDrawList* dl);
void popUnclipped(ImDrawList* dl);

// Fondo de un grupo de filas (como las listas agrupadas de Ajustes).
void group(ImDrawList* dl, const ImRect& rect, float radius);
// Línea horizontal de un píxel.
void separator(ImDrawList* dl, float x0, float x1, float y, ImU32 color = theme::kSeparator);
// Icono blanco sobre un cuadrado redondeado de color (filas de Ajustes).
void iconTile(ImDrawList* dl, const ImRect& rect, ImU32 color, const char* glyph, float iconPoints);
// Indicador de actividad de iOS (ocho radios que giran). Mantiene el bucle despierto.
void spinner(ImDrawList* dl, ImVec2 center, float radius, ImU32 color);
// Rectángulo redondeado con un degradado lineal de `from` a `to` (bordes suavizados).
void linearGradient(ImDrawList* dl, const ImRect& rect, float radius, ImVec2 from, ImVec2 to, ImU32 colorFrom,
                    ImU32 colorTo);
// Anillo con todos los tonos: el rojo arriba y en el sentido de las agujas del reloj.
void hueRing(ImDrawList* dl, ImVec2 center, float innerRadius, float outerRadius);

// --- Superficies --------------------------------------------------------------------

// Ventana de ImGui sin decoración ni fondo que ocupa `rect`. Cada barra, panel o diálogo
// es una: la entrada que cae dentro es de la interfaz. `front` la pone encima del resto
// (paneles y diálogos). Siempre hay que llamar a endSurface().
void beginSurface(const char* name, const ImRect& rect, bool interactive = true, bool front = false);
void endSurface();

// Marca de lo dibujado hasta ahora en una lista, para animarlo después con transform().
struct DrawMark {
    ImDrawList* dl = nullptr;
    int vertex = 0;
    int command = 0;
};
DrawMark mark(ImDrawList* dl);
// Escala (alrededor de `anchor`), desplaza y aplica opacidad a lo dibujado desde `mark`.
// El cristal se recalcula para seguir mostrando lo que queda detrás.
void transform(const DrawMark& mark, ImVec2 anchor, float scale, ImVec2 offset, float alpha);

// --- Controles ----------------------------------------------------------------------

struct Press {
    bool clicked = false;
    bool hovered = false;   // solo con ratón o lápiz (en pantallas táctiles no hay "encima")
    bool held = false;
};
// Zona pulsable. No cuenta como clic si el dedo se arrastró (p. ej. al desplazar una lista).
Press pressable(ImGuiID id, const ImRect& bb, bool enabled = true);
Press pressable(const char* id, const ImRect& bb, bool enabled = true);

// Fondo animado de un botón: `on` (abierto o seleccionado), pulsado o con el ratón encima.
void highlight(ImDrawList* dl, ImGuiID id, const ImRect& rect, float radius, bool on, const Press& press,
               ImU32 onColor = theme::kPressed);

// Interruptor de 51×31 pt con la esquina superior izquierda en `pos`.
bool toggle(const char* id, ImVec2 pos, bool* value, bool enabled = true);
inline ImVec2 toggleSize() { return pt(51.0f, 31.0f); }

// Control segmentado.
bool segmented(const char* id, const ImRect& rect, const char* const* labels, int count, int* selected,
               bool enabled = true);

// Deslizador horizontal (pista de 4 pt y bolita de 28 pt). `active`: se está arrastrando.
bool slider(const char* id, const ImRect& rect, float* value, float min, float max, bool* active = nullptr);

// Deslizador vertical de relleno de la barra lateral: `t` va de 0 (abajo) a 1 (arriba) y
// se mueve con el arrastre, sin saltar al punto que se toca.
bool fillSlider(const char* id, const ImRect& rect, float* t, bool* active = nullptr);

// Campo de texto. `focus`: poner el teclado en él en este frame. Devuelve true con Intro.
bool textField(const char* id, const ImRect& rect, char* buffer, size_t size, bool focus, const char* placeholder);

// Lista con desplazamiento por arrastre (con inercia) y rueda. Devuelve el desplazamiento
// que hay que restar a la y del contenido. Entre las dos llamadas se recorta a `view`.
float beginScroll(const char* id, const ImRect& view, float contentHeight);
void endScroll();
// Mueve la lista para que se vea el tramo [top, bottom] del contenido.
void scrollIntoView(const char* id, float top, float bottom, float viewHeight);

// Círculo de color con anillo de selección.
bool swatch(const char* id, ImVec2 center, float radius, ImU32 color, bool selected);

} // namespace ui
