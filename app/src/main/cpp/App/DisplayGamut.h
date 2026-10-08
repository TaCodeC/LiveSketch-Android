#pragma once

#include "Gfx/ColorSpace.h"

#include <SDL3/SDL_video.h>

// Perfil de color en el que la ventana muestra lo que se dibuja. Por defecto es sRGB; con un
// lienzo en Display P3, la ventana pasa a P3 donde se puede (Android 9 o posterior con una
// pantalla de gama amplia, y Chrome o Safari con una pantalla P3). Donde no, la vista y la
// interfaz convierten los colores a sRGB.
class DisplayGamut {
public:
    // Pone la ventana en `wanted` si se puede (y si no, en sRGB). Devuelve en qué perfil
    // queda. Una vez por frame, antes de dibujar: en Android la superficie de la ventana
    // cambia al volver a primer plano y hay que ponerle el perfil otra vez.
    ColorProfile sync(SDL_Window* window, ColorProfile wanted);
    // La pantalla puede mostrar Display P3 (lo que se supo en el último sync).
    bool wideAvailable() const { return m_available; }
    // La pantalla pudo cambiar (otro monitor, otra configuración): se vuelve a comprobar.
    void displayChanged() { m_checked = false; }

private:
    ColorProfile m_profile = ColorProfile::Srgb;
    bool m_available = false;
    bool m_checked = false;
    [[maybe_unused]] int m_windowWide = -1;   // Android: lo que se pidió a la actividad (-1: nada)
};
