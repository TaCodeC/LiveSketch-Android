#pragma once

#include <cstdint>
#include <string>

class Canvas;

// Estado de la interfaz que no pertenece al lienzo.
struct UiState {
    bool drawWithFinger = false;   // si no, los dedos solo mueven y hacen zoom
};

// Lo que el menú pide a la app en este frame.
struct UiRequests {
    int canvasWidth = 0;    // > 0: crear el lienzo con este tamaño (pantalla de inicio)
    int canvasHeight = 0;
    bool fitView = false;
    bool quit = false;
};

// Menú de ImGui. Opera directamente sobre el lienzo; lo que no es del lienzo lo pide a
// la app a través de UiRequests.
class Menu {
public:
    // Pantalla de inicio para elegir el tamaño del lienzo. `maxSize` es el lado máximo de
    // textura que admite la GPU.
    void startup(int screenWidth, int screenHeight, int maxSize, UiRequests& requests);

    // Panel de control.
    void controls(Canvas& canvas, UiState& state, UiRequests& requests);

    // Diálogos modales y avisos. Se llama en todos los frames, después del resto.
    void overlays(Canvas* canvas, UiRequests& requests);

    // Pide confirmación para salir (botón atrás de Android).
    void askExit() { m_exitRequested = true; }

    // Muestra un aviso breve en la parte de abajo de la pantalla.
    void notify(std::string text, uint64_t durationMs = 3000);
    // Instante (SDL_GetTicks) en que se oculta el aviso visible, o 0 si no hay ninguno.
    uint64_t noticeDeadline() const { return m_notice.empty() ? 0 : m_noticeUntil; }

private:
    void layerPanel(Canvas& canvas);
    void brushPanel(Canvas& canvas, UiState& state);

    int m_preset = 5;   // "Full", como antes

    // Diálogos pendientes de abrir (OpenPopup se llama desde el nivel de overlays()).
    bool m_exitRequested = false;
    bool m_renameRequested = false;
    bool m_deleteRequested = false;
    bool m_clearRequested = false;
    uint32_t m_dialogLayerId = 0;   // capa a la que se refiere el diálogo abierto
    char m_renameBuffer[64] = {};

    std::string m_notice;
    uint64_t m_noticeUntil = 0;
};
