#pragma once

#include "Canvas/Camera.h"
#include "Canvas/Canvas.h"
#include "Canvas/CanvasView.h"
#include "IO/ImageExport.h"
#include "NDI/NdiOutput.h"
#include "UI/Backdrop.h"
#include "UI/Ui.h"

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

// La aplicación. SDL llama a init/event/iterate/quit desde su hilo principal, que es
// el único que toca OpenGL, ImGui y el lienzo.
class App {
public:
    SDL_AppResult init(int argc, char** argv);
    SDL_AppResult event(const SDL_Event& event);
    SDL_AppResult iterate();
    void quit();

private:
    // --- Ciclo de vida y contexto (App.cpp) ---
    bool createWindow();
    bool initImGui();
    void shutdownImGui();
    void onWillEnterBackground();
    void onDidEnterForeground();
    void onRenderDeviceReset();
    void updateWindowSize();
    void createCanvas(int width, int height);
    void setNdiEnabled(bool enabled);
    void requestPngExport();
    void exportPng();
    void onPngSaved(const io::PngExporter::Result& result);
    UiStatus uiStatus() const;
    void applyRequests(const UiRequests& requests);
    void startFitAnimation();
    void stepFitAnimation();
    void updateBackdrop();
    void renderFrame();
    void schedulePacing();
    void wakeAt(uint64_t ticksMs);

    // --- Entrada (AppInput.cpp) ---
    bool routeEvent(const SDL_Event& event);   // true si la interfaz se queda con el evento
    bool uiWantsPoint(float x, float y) const; // coordenadas de ventana (las de ImGui)
    glm::vec2 windowToPixels(float x, float y) const;
    glm::vec2 fingerToPixels(float x, float y) const;
    glm::vec2 toCanvas(glm::vec2 pixels) const { return m_camera.screenToCanvas(pixels); }
    float pointsToPixels(float points) const;
    bool penBlocksFingers() const;
    bool canvasInteractionActive() const;
    void resetGestureReference();
    void notifyHiddenLayer();
    // Empieza un trazo con los ajustes de la herramienta (o del borrador, con la goma
    // del lápiz). `pixels`: posición en la ventana, en píxeles.
    bool beginCanvasStroke(glm::vec2 pixels, float pressure, bool eraserTip);

    // Cuentagotas: mientras se arrastra, la lupa muestra el color de debajo; al soltar,
    // ese color pasa al pincel. Posiciones en coordenadas de ventana.
    enum class PickSource { None, Mouse, Pen, Finger };
    void beginPick(PickSource source, SDL_FingerID finger, float x, float y);
    void movePick(float x, float y);
    void endPick(bool apply);
    void samplePick();
    // Dedo quieto un momento al empezar a dibujar: cuentagotas.
    void checkLongPress();
    // Terminó un gesto de dedos: si fue un toque con dos (deshacer) o tres (rehacer).
    void finishTapGesture(bool canceled);

    void onPenEvent(const SDL_Event& event);
    void flushPenSample();
    void endPenStroke();
    void onFingerEvent(const SDL_Event& event);
    void onMouseEvent(const SDL_Event& event);
    void endGestures();

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_glContext = nullptr;
    bool m_imguiReady = false;
    bool m_foreground = true;
    int m_pixelWidth = 0;    // framebuffer de la ventana, en píxeles
    int m_pixelHeight = 0;
    int m_maxCanvasSize = 0; // lado máximo de textura/viewport que admite la GPU

    Canvas m_canvas;
    CanvasView m_view;
    Camera m_camera;
    Ui m_ui;
    Backdrop m_backdrop;
    NdiOutput m_ndi;
    bool m_ndiAvailable = false;

    // Fondo desenfocado del cristal: se rehace cuando cambia la escena (mientras se
    // dibuja, como mucho unas cuantas veces por segundo).
    GLuint m_backdropTexture = 0;       // la que usó la interfaz en este frame
    uint64_t m_backdropVersion = 0;     // versión del lienzo que tiene
    uint64_t m_backdropUpdatedMs = 0;

    // "Centrar lienzo" animado.
    struct FitAnimation {
        bool active = false;
        uint64_t startMs = 0;
        float fromZoom = 1.0f;
        glm::vec2 fromOffset{0.0f};
    } m_fitAnimation;

    // Guardar PNG: la lectura del lienzo es en el hilo de GL; la compresión, en otro.
    io::PngExporter m_exporter;
    bool m_awaitingPermission = false;   // Android 10 o anterior: permiso de almacenamiento
    bool m_exportPending = false;        // permiso concedido: guardar en el próximo frame

    // Ritmo del bucle: a vsync mientras algo se mueve, dormido esperando eventos si no.
    int m_redrawFrames = 4;
    bool m_continuous = true;
    SDL_TimerID m_wakeTimer = 0;
    uint64_t m_wakeAt = 0;

    // --- Estado de la entrada ---
    // Lápiz. SDL manda primero la posición y después la presión de cada muestra (en la
    // web, también el toque llega antes que su presión), así que la muestra, y con la
    // primera el inicio del trazo, se aplica cuando llega la siguiente o al dibujar el frame.
    struct PenState {
        bool inProximity = false;
        bool down = false;
        bool drawing = false;       // el trazo es del lienzo (tocó fuera de la interfaz)
        bool strokePending = false; // el trazo empieza con la muestra pendiente
        bool eraser = false;        // toca con la goma
        float x = 0.0f;             // coordenadas de ventana
        float y = 0.0f;
        float pressure = 1.0f;
        bool pendingSample = false;
        uint64_t lastActiveMs = 0;  // última vez que estuvo cerca o tocando
    } m_pen;

    struct FingerPoint {
        SDL_FingerID id = 0;
        glm::vec2 position{0.0f};   // píxeles
    };
    std::vector<FingerPoint> m_fingers;   // dedos que tocaron el lienzo
    bool m_fingerDrawing = false;
    uint64_t m_fingerStrokeStartMs = 0;
    // Referencia del gesto de uno o dos dedos (centro y distancia en píxeles).
    glm::vec2 m_gestureCenter{0.0f};
    float m_gestureDistance = 0.0f;

    // Toque con varios dedos (deshacer con dos, rehacer con tres) y dedo quieto al
    // empezar (cuentagotas). Sigue a todos los dedos del gesto, también al tercero.
    struct TapGesture {
        struct Finger {
            SDL_FingerID id = 0;
            glm::vec2 start{0.0f};   // píxeles
        };
        std::vector<Finger> fingers;
        bool candidate = false;      // todavía puede ser un toque
        bool moved = false;          // algún dedo se movió más de un toque
        int maxFingers = 0;
        uint64_t startMs = 0;
        Camera::View view;           // vista al empezar (el toque no debe moverla)
    } m_tap;

    struct Pick {
        PickSource source = PickSource::None;
        SDL_FingerID finger = 0;
        float x = 0.0f;              // coordenadas de ventana
        float y = 0.0f;
        bool valid = false;          // hay color debajo
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        bool dirty = false;          // se movió: hay que volver a leer el color
    } m_pick;

    // Ratón de verdad (no el emulado desde el lápiz o los dedos).
    bool m_mouseDrawing = false;
    bool m_mousePanning = false;
    glm::vec2 m_mouseLast{0.0f};

    // Botones del ratón (reales o emulados) cuya pulsación recibió ImGui. Su soltar
    // también tiene que llegarle; el resto de pulsaciones son del lienzo.
    uint32_t m_uiMouseButtons = 0;
};
