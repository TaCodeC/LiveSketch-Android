#pragma once

#include "App/CanvasProject.h"
#include "App/DisplayGamut.h"
#include "Canvas/Camera.h"
#include "Canvas/Canvas.h"
#include "Canvas/CanvasView.h"
#include "IO/ImageExport.h"
#include "IO/Library.h"
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
    // En la web, la página se oculta o se cierra: se guarda ya todo, como al pasar a segundo
    // plano en Android. True si queda algo que se perdería al cerrarla.
    bool suspendPage();

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
    enum class StorageTask { None, Png, Export };
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
    // Cada lienzo es un proyecto de la biblioteca (una carpeta de la app, con un archivo .lvskt
    // por proyecto) que se guarda solo: un momento después de cada cambio, al pasar a segundo
    // plano y antes de dejarlo (otro lienzo, otro proyecto, la pantalla de Proyectos o salir).
    // Guardar otra vez solo comprime lo que cambió desde la última vez (ver canvasproject).
    // Al arrancar: la carpeta de proyectos y el último que estaba abierto.
    void openLibrary();
    // El último proyecto que estaba abierto (vacío: ninguno), para volver a él al arrancar.
    void setLastProject(const std::string& path);
    // Lo que se guarda del lienzo, la vista y el pincel (sin las cajas de las capas).
    project::Document projectDocument() const;
    // La vista, la capa activa y el color del pincel: no cambian el dibujo, pero se guardan
    // con él al dejarlo o al pasar a segundo plano.
    uint64_t projectMeta() const;
    // El lienzo tiene algo que no está en su archivo. `meta`: también lo de projectMeta().
    bool projectUnsaved(bool meta) const;
    // Por qué se guarda: solo (un momento después de cambiar), porque lo pidió el usuario,
    // para dejar el lienzo, al pasar a segundo plano o para exportarlo.
    enum class SaveReason { Auto, User, Leave, Background, Export };
    // Empieza a guardar el lienzo en su archivo de la biblioteca (`full`: sin copiar nada del
    // anterior). El resultado llega a onProjectSaved().
    void startSave(SaveReason reason, bool full = false);
    void onProjectSaved(const project::Saver::Result& result);
    // Cada frame: la biblioteca, el guardado en marcha, guardar cuando toca y dejar el lienzo.
    void updateProjects();
    // Cuándo toca el guardado automático (SDL_GetTicks; 0 si no hay nada que guardar), para
    // despertar el bucle a esa hora.
    uint64_t autosaveDeadline() const;
    // Guardar (el botón o Ctrl+S): se avisa y, un momento después (para que el aviso ya se vea
    // mientras se leen las capas), se guarda.
    void requestProjectSave();
    // Pasa a segundo plano (o se oculta la página): se guarda lo que haya y se espera a que
    // esté escrito (el sistema puede cerrar la app sin avisar).
    void saveBeforeSuspend();
    // En la web, si el navegador no guarda los datos de la app, se dice (una vez).
    void checkStorage();

    // Dejar el lienzo (ir a Proyectos, crear otro, abrir otro proyecto o salir): primero se
    // guarda, mientras la interfaz lo dice y no deja dibujar; si no se puede, se pregunta si
    // seguir sin guardar.
    enum class Leave { None, Projects, NewCanvas, Open, Quit };
    void requestLeave(Leave action, const CanvasSpec* spec = nullptr);
    void stepLeave();
    void performLeave();
    // Cierra el lienzo (se ve la pantalla de Proyectos).
    void closeCanvas();
    // El lienzo de ahora pasa a ser otro (uno nuevo o el de un proyecto que se abre): su
    // proyecto empieza de cero.
    void resetProject();
    // Lo que pide la pantalla de Proyectos y lo que hizo la biblioteca.
    void applyProjectAction(const UiRequests::ProjectAction& action);
    void onLibraryDone(const library::Library::Done& done);
    // Exportar: una copia del archivo de un proyecto de la biblioteca en Descargas (en la web,
    // una descarga) o donde se elija (Android y escritorio). `name`: el del lienzo, para el
    // nombre de la copia. Con el lienzo, se guarda antes.
    enum class ExportTo { None, Downloads, Choose };
    void exportProject(const std::string& path, const std::string& name, ExportTo to);
    void exportCanvasProject(ExportTo to);
    // El sitio elegido para la copia (una ruta o, en Android, la URI de un documento).
    void exportChosen(const std::string& source, const std::string& target);

    // Abrir proyecto: el selector de archivos del sistema (en la web, el del navegador). El
    // archivo elegido (o soltado en la ventana) llega como evento y se abre en iterate().
    void chooseProject();
    void fileChosen(std::string path);
    // Lee el índice y la descripción del proyecto (de la biblioteca o de fuera); si hay un
    // lienzo, se deja antes. `temporary`: una copia que se borra al terminar (en la web).
    void openProjectFile(const std::string& path, bool temporary);
    // Cambia el lienzo por el del proyecto (aún sin píxeles) y empieza a leer las capas. Uno de
    // fuera se guarda en la biblioteca al terminar de abrirlo.
    void startProjectOpen();
    // Cada frame mientras se abre: sube a la GPU las capas que ya están leídas.
    void stepProjectOpen();
    void finishProjectOpen();
    // Se descarta lo que se iba a abrir.
    void closeProjectFile();
    // Se perdió el contexto gráfico y el dibujo con él: se vuelve a abrir el archivo del
    // proyecto (o lo que se estaba abriendo). False si no tiene archivo o no se pudo abrir.
    bool reloadProject();

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
    static constexpr uint64_t kNoVersion = ~uint64_t{0};
    library::Library m_library;
    project::Saver m_projectSaver;
    // El proyecto del lienzo.
    struct ProjectState {
        std::string path;                // su archivo en la biblioteca (vacío: aún no tiene)
        bool stored = false;             // ese archivo existe y es este lienzo
        canvasproject::Record record;    // lo que tiene el archivo, para copiar lo que no cambió
        uint64_t savedVersion = 0;       // documentVersion() de lo que tiene
        uint64_t savedMeta = 0;          // projectMeta() de lo que tiene
        // Se abrió de fuera de la biblioteca: hay que guardarlo en ella (se copia de ahí).
        bool import = false;
        std::string source;              // el archivo de fuera
        bool temporary = false;          // en la web, una copia que se borra después
        uint64_t failedVersion = kNoVersion;   // no se pudo guardar esta versión: no se repite sola
        std::string error;               // por qué (vacío: el último guardado salió bien)
        uint64_t seenVersion = 0;        // documentVersion() del frame anterior
        uint64_t changedMs = 0;          // cuándo cambió por última vez (SDL_GetTicks)
    } m_project;
    // Sube con cada lienzo: un guardado que termina después de cambiar de lienzo no es de este.
    uint64_t m_canvasGeneration = 0;
    // El guardado en marcha.
    struct SaveState {
        bool active = false;
        SaveReason reason = SaveReason::Auto;
        bool full = false;               // ya sin copiar nada del archivo anterior
        uint64_t version = 0;            // documentVersion() y projectMeta() de lo que se guarda
        uint64_t meta = 0;
        uint64_t generation = 0;
        canvasproject::Pending pending;
    } m_saving;
    uint64_t m_userSaveAtMs = 0;         // Guardar: cuándo empezar (SDL_GetTicks; 0: nada pendiente)
    ExportTo m_exportCanvas = ExportTo::None;      // exportar el lienzo al terminar el guardado en marcha
    ExportTo m_exportAfterSave = ExportTo::None;   // el guardado de SaveReason::Export: adónde después
    struct LeaveState {
        Leave action = Leave::None;
        CanvasSpec spec;                 // NewCanvas: el lienzo que se crea
        bool saved = false;              // ya se guardó (o se intentó)
        bool asking = false;             // no se pudo guardar: se pregunta si seguir
        uint64_t sinceMs = 0;            // desde cuándo se deja
    } m_leave;
    bool m_quit = false;                 // se sale en este frame
    bool m_storageWarned = false;        // ya se dijo que el navegador no guarda los datos
    bool m_ndiResume = false;            // NDI emitía al cerrar el lienzo: vuelve con el siguiente
    // Exportar: el proyecto de la biblioteca que espera al permiso de almacenamiento.
    struct PendingExport {
        std::string path;
        std::string name;
    } m_pendingExport;
    // Exportar donde se elija: el proyecto que se exporta mientras se elige dónde.
    std::optional<std::string> m_exportChoosing;
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
        bool library = false;            // es un proyecto de la biblioteca (si no, se importa)
        bool loading = false;            // ya es el lienzo: se suben sus capas
        uint64_t version = 0;            // documentVersion() al empezar a subirlas
        int failedUploads = 0;           // capas que no se pudieron subir a la GPU
        std::vector<canvasproject::SavedLayer> layers;   // las que quedaron como en el archivo
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
