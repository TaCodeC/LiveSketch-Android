#pragma once

#include <imgui.h>

// Animaciones cortas de la interfaz. Cada valor animado se identifica con un ImGuiID y lo
// avanza, una vez por frame, el código que lo dibuja: si un frame no se pide, se queda
// quieto. Mientras alguno no llegue a su destino, active() pide seguir dibujando.
namespace ui::anim {

// Al empezar cada frame de la interfaz. `dt` en segundos (se limita para que una pausa
// larga, con el bucle dormido, no haga saltar las animaciones).
void newFrame(float dt);
// Algún valor se movió en el último frame: hay que dibujar el siguiente.
bool active();
float dt();
// Segundos desde el arranque (para efectos periódicos).
double time();

// Se acerca a `target` de forma exponencial; `speed` es la inversa del tiempo
// característico (con 18 recorre el 90 % en 0,13 s). La primera vez que se pide un id
// vale `target` sin animar.
float follow(ImGuiID id, float target, float speed = 18.0f, float epsilon = 0.002f);
// Como follow(), pero la primera vez parte de `initial` (p. ej. un panel que aparece).
float followFrom(ImGuiID id, float initial, float target, float speed = 18.0f, float epsilon = 0.002f);
// Muelle amortiguado: `frequency` en Hz y `damping` < 1 da un pequeño rebote.
float spring(ImGuiID id, float target, float frequency = 3.0f, float damping = 0.75f, float epsilon = 0.002f);
// Fija el valor sin animar.
void set(ImGuiID id, float value);
// Pide dibujar el frame siguiente aunque nada se mueva (p. ej. un indicador de actividad).
void keepAlive();
// Valor actual o `fallback` si nunca se pidió.
float value(ImGuiID id, float fallback);

inline float easeOutCubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

inline float easeInOutCubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * 0.5f;
}

} // namespace ui::anim
