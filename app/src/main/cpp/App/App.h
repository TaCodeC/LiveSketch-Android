#pragma once

#include "App/DisplayGamut.h"
#include "Canvas/Camera.h"
#include "Canvas/Canvas.h"
#include "Canvas/CanvasView.h"
#include "IO/ImageExport.h"
#include "IO/ProjectFile.h"
#include "NDI/NdiOutput.h"
#include "UI/Backdrop.h"
#include "UI/Ui.h"

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    void createCanvas(const CanvasSpec& spec);
    void setNdiEnabled(bool enabled);
    // Guardar en Descargas. En Android 10 o anterior hace falta el permiso de almacenamiento:
    // si aún no se sabe si está, se pide y devuelve false (lo de `task` se hace al llegar la
    // respuesta).
    enum class StorageTask { None, Png, Project };
    bool storageReady(StorageTask task);
    void requestPngExport();
    void exportPng();
    void onPngSaved(const io::PngExporter::Result& result);
    UiStatus uiStatus() const;
    void applyRequests(const UiRequests& requests);
    void startFitAnimation();
    void stepFitAnimation();
    ColorProfile canvasProfile() const {
        return m_canvas.ready() ? m_canvas.info().profile : ColorProfile::Srgb;
    }
    void updateBackdrop();
    void renderFrame();
    void schedulePacing();
    void wakeAt(uint64_t ticksMs);

    // --- Proyectos (AppProjects.cpp) ---
    // El lienzo tiene cambios que no están en ningún proyecto guardado (o abierto).
    bool canvasDirty() const { return m_canvas.ready() && m_canvas.documentVersion() != m_savedVersion; }
    // Lo que se guarda del lienzo, la vista y el pincel (sin las cajas de las capas).
    project::Document projectDocument() const;
    // Guardar proyecto: se avisa y, un momento después (para que el aviso ya se vea mientras
    // se leen las capas), se guarda.
    void requestProjectSave();
    void scheduleProjectSave();
    void saveProject();
    void onProjectSaved(const project::Saver::Result& result);
    // Abrir proyecto: el selector de archivos del sistema (en la web, el del navegador). El
    // archivo elegido (o soltado en la ventana) llega como evento y se abre en iterate().
    void chooseProject();
    void fileChosen(std::string path);
    // Lee el índice y la descripción del proyecto; si el lienzo tiene cambios, lo pregunta
    // antes de empezar. `temporary`: una copia que se borra al terminar (en la web).
    void openProjectFile(const std::string& path, bool temporary);
    // Cambia el lienzo por el del proyecto (aún sin píxeles) y empieza a leer las capas.
    void startProjectOpen();
    // Cada frame mientras se abre: sube a la GPU las capas que ya están leídas.
    void stepProjectOpen();
    void finishProjectOpen();
    // Se descarta lo que se iba a abrir.
    void closeProjectFile();

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
    // Empieza el giro con dos dedos desde el giro que tiene la vista.
    void startTwist();
    // Gira la vista: con el teclado, de 15 en 15 grados (`steps`) alrededor del centro; con
    // `steps` = 0, la deja derecha.
    void rotateView(int steps);
    void notifyStrokeBlocked(bool eraserTip);
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

    // Herramientas Selección y Transformar: las maneja un puntero a la vez (el lápiz, un
    // dedo o el ratón). Posiciones en coordenadas de ventana.
    enum class ToolPointer { None, Pen, Finger, Mouse };
    bool toolActive() const { return m_ui.canvasTool() != CanvasTool::Paint; }
    ToolView toolView() const;
    void beginToolGesture(ToolPointer pointer, SDL_FingerID finger, float x, float y);
    void moveToolGesture(float x, float y);
    // `cancel`: otro dedo o el sistema lo interrumpen y no se hace nada.
    void endToolGesture(bool cancel);

    // Forma rápida: el puntero que dibuja se queda quieto un momento al final del trazo
    // (`pixels`: su posición en la ventana).
    void startHold(ToolPointer pointer, glm::vec2 pixels);
    void trackHold(glm::vec2 pixels);
    // Cada frame: si lleva quieto lo bastante, el trazo pasa a ser la forma que se le
    // parece; con Mayús (o tocando con un dedo, ver onFingerEvent), perfecta.
    void checkHold();

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
    // Perfil de color de la pantalla en este frame (ver DisplayGamut).
    DisplayGamut m_display;
    ColorProfile m_displayProfile = ColorProfile::Srgb;
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
        float fromAngle = 0.0f;
        glm::vec2 fromCenter{0.0f};   // centro del lienzo en la pantalla
    } m_fitAnimation;

    // Guardar PNG: la lectura del lienzo es en el hilo de GL; la compresión, en otro.
    io::PngExporter m_exporter;
    StorageTask m_awaitingPermission = StorageTask::None;   // lo que espera al permiso de almacenamiento
    bool m_exportPending = false;        // permiso concedido: guardar en el próximo frame

    // Proyectos. Como el PNG, las capas se leen de la GPU en este hilo y se comprimen en otros.
    project::Saver m_projectSaver;
    uint64_t m_savedVersion = 0;         // documentVersion() de lo último guardado, creado o abierto
    uint64_t m_savingVersion = 0;        // la que se está guardando
    uint64_t m_saveAtMs = 0;             // cuándo empezar a guardar (SDL_GetTicks; 0: nada pendiente)
    bool m_choosingFile = false;         // el selector de archivos del sistema está abierto
    struct ChosenFile {
        std::string path;
        bool temporary = false;
    };
    std::optional<ChosenFile> m_chosenFile;   // elegido o soltado: se abre en el próximo frame
    struct OpenState {
        std::unique_ptr<project::Loader> loader;
        std::string path;
        std::string title;               // el nombre del lienzo o, si no tiene, el del archivo
        bool temporary = false;
        bool confirming = false;         // se pregunta si perder los cambios del lienzo
        bool loading = false;            // ya es el lienzo: se suben sus capas
        bool ndi = false;                // NDI emitía: vuelve al terminar
        int failedUploads = 0;           // capas que no se pudieron subir a la GPU
    } m_open;

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
    // Referencia del gesto de uno o dos dedos (centro y distancia en píxeles, y dirección
    // de un dedo al otro).
    glm::vec2 m_gestureCenter{0.0f};
    float m_gestureDistance = 0.0f;
    float m_gestureDirection = 0.0f;
    // Giro con dos dedos: lo que han girado desde que se puso el segundo, sobre el giro que
    // tenía la vista (sin el imán: ver magnetAngle).
    float m_twistStart = 0.0f;
    float m_twist = 0.0f;
    bool m_twisting = false;          // este gesto ya giró la vista
    uint64_t m_angleShownUntil = 0;   // el giro se muestra arriba hasta entonces (teclado)

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

    struct ToolGesture {
        ToolPointer pointer = ToolPointer::None;
        SDL_FingerID finger = 0;
        uint64_t startMs = 0;
        float x = 0.0f;              // última posición (coordenadas de ventana)
        float y = 0.0f;
    } m_toolGesture;

    // Forma rápida del trazo en curso.
    struct Hold {
        ToolPointer pointer = ToolPointer::None;   // con qué se dibuja
        glm::vec2 anchor{0.0f};      // dónde se paró (píxeles)
        uint64_t sinceMs = 0;        // desde cuándo
        int stillFrames = 0;         // frames seguidos sin moverse de ahí
        bool tried = false;          // ya se probó ahí (no se parecía a ninguna forma)
        bool snapped = false;        // el trazo ya es una forma
        std::vector<SDL_FingerID> fingers;   // dedos que la hicieron perfecta (no son gestos)
    } m_hold;

    // Ratón de verdad (no el emulado desde el lápiz o los dedos).
    bool m_mouseDrawing = false;
    bool m_mousePanning = false;
    glm::vec2 m_mouseLast{0.0f};

    // Botones del ratón (reales o emulados) cuya pulsación recibió ImGui. Su soltar
    // también tiene que llegarle; el resto de pulsaciones son del lienzo.
    uint32_t m_uiMouseButtons = 0;
    // La última pulsación que recibió ImGui fue de un dedo o del lápiz: no suele haber
    // teclado a mano (la tarjeta de lienzo nuevo saca el suyo para escribir números).
    bool m_uiTouch = false;
};
