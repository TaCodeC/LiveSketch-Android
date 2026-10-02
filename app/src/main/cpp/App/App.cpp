#include "App/App.h"

#include "Gfx/GL.h"
#include "Gfx/GLObjects.h"
#include "UI/Anim.h"
#include "UI/Kit.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char* kGlslVersion = "#version 300 es";

// RAM que puede ocupar la copia de las capas que se hace al pasar a segundo plano, por
// si el driver pierde el contexto GL mientras tanto.
constexpr size_t kSnapshotBudget = size_t{256} * 1024 * 1024;

// Frames que se dibujan después de cada entrada (ver App::event).
constexpr int kFramesAfterInput = 4;

// "Centrar lienzo": duración de la animación.
constexpr uint64_t kFitDurationMs = 320;

// Borde de la selección: avanza un paso cada tanto (no hace falta dibujar a 60 fps), y
// con la herramienta Selección oscurece un poco lo que no está seleccionado.
constexpr uint64_t kAntsStepMs = 120;
constexpr float kSelectionVeil = 0.22f;

// Fondo desenfocado del cristal: radio del desenfoque (pt) y, mientras se dibuja, cada
// cuánto se rehace como mucho.
constexpr float kBackdropBlurPoints = 22.0f;
constexpr uint64_t kBackdropStrokeIntervalMs = 90;

ImTextureID textureId(GLuint texture) {
    return texture != 0 ? static_cast<ImTextureID>(texture) : ImTextureID_Invalid;
}

// La interfaz ya dibujó el cristal con la textura del fondo que había; si esa textura se
// volvió a crear (cambió el tamaño de la ventana), se cambia en los comandos.
void retargetTexture(ImDrawData* data, ImTextureID from, ImTextureID to) {
    if (!data || from == to || from == ImTextureID_Invalid) {
        return;
    }
    for (ImDrawList* list : data->CmdLists) {
        for (ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.TexRef._TexData == nullptr && cmd.TexRef._TexID == from) {
                cmd.TexRef._TexID = to;
            }
        }
    }
}

uint64_t mixHash(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64_t floatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Eventos propios. Despiertan el bucle cuando está dormido esperando eventos; los
// empujan un temporizador u otros hilos.
Uint32 g_wakeEventType = 0;         // temporizador de App::wakeAt
Uint32 g_exportEventType = 0;       // terminó un guardado de PNG
Uint32 g_permissionEventType = 0;   // respuesta al permiso de almacenamiento (code: 1 si se concedió)

void pushEvent(Uint32 type, Sint32 code = 0) {
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.user.code = code;
    SDL_PushEvent(&event);
}

Uint32 SDLCALL pushWakeEvent(void*, SDL_TimerID, Uint32) {
    pushEvent(g_wakeEventType);
    return 0;
}

#ifdef SDL_PLATFORM_ANDROID
void SDLCALL onStoragePermission(void*, const char*, bool granted) {
    pushEvent(g_permissionEventType, granted ? 1 : 0);
}
#endif

} // namespace

// -----------------------------------------------------------------------------
// Arranque y cierre
// -----------------------------------------------------------------------------

SDL_AppResult App::init(int, char**) {
    SDL_SetAppMetadata("LiveSketch", "0.3.0-alpha", "com.tacodec.livesketch");

    // El lápiz llega como eventos de lápiz (con presión) y, para ImGui, también como
    // ratón. Los dedos manejan ImGui a través del ratón emulado y el lienzo con sus
    // propios eventos. Un ratón de verdad no se hace pasar por un dedo.
    SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    // El botón atrás de Android llega como tecla: cierra lo último que se abrió y, con
    // todo cerrado, pide confirmación antes de salir.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "0");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    const Uint32 firstEvent = SDL_RegisterEvents(3);
    if (firstEvent == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_RegisterEvents: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    g_wakeEventType = firstEvent;
    g_exportEventType = firstEvent + 1;
    g_permissionEventType = firstEvent + 2;

    if (!createWindow() || !initImGui()) {
        return SDL_APP_FAILURE;
    }
#ifdef LIVESKETCH_WITH_NDI
    m_ndiAvailable = true;
#endif
    if (!m_view.init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudieron compilar los shaders de la vista");
        return SDL_APP_FAILURE;
    }
    // Sin miniaturas o sin desenfoque la interfaz funciona igual (con cristal opaco).
    if (!m_ui.init()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No se pudieron crear las miniaturas de la interfaz");
    }
    if (!m_backdrop.init()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No se pudo crear el fondo desenfocado de la interfaz");
    }

    GLint maxTexture = 0;
    GLint maxViewport[2] = {0, 0};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewport);
    m_maxCanvasSize = std::min({maxTexture, maxViewport[0], maxViewport[1]});

    updateWindowSize();
    SDL_Log("GL %s · %s · lienzo máximo %d px", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
            reinterpret_cast<const char*>(glGetString(GL_RENDERER)), m_maxCanvasSize);
    return SDL_APP_CONTINUE;
}

bool App::createWindow() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Redimensionable: en Android permite todas las orientaciones, como antes.
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(SDL_PLATFORM_ANDROID)
    flags |= SDL_WINDOW_FULLSCREEN;
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    // En el navegador ocupa toda la página y sigue su tamaño.
    flags |= SDL_WINDOW_FILL_DOCUMENT;
#endif
    m_window = SDL_CreateWindow("LiveSketch", 1280, 800, flags);
    if (!m_window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }

    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo crear el contexto OpenGL ES 3.0: %s", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(m_window, m_glContext);
    SDL_GL_SetSwapInterval(1);
    return true;
}

bool App::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // en Android no hay dónde escribir imgui.ini
    ImGui::StyleColorsDark();
    // Las ventanas de ImGui son solo zonas de la pantalla (barras, paneles): el aspecto lo
    // dibuja la interfaz.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.WindowBorderSize = 0.0f;
    style.WindowRounding = 0.0f;
    style.WindowMinSize = ImVec2(1.0f, 1.0f);
    style.ItemSpacing = ImVec2(0.0f, 0.0f);
    if (!ui::loadFonts()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Faltan fuentes o iconos de la interfaz: se usa la fuente de ImGui");
    }

    if (!ImGui_ImplSDL3_InitForOpenGL(m_window, m_glContext) || !ImGui_ImplOpenGL3_Init(kGlslVersion)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo iniciar ImGui");
        return false;
    }
    m_imguiReady = true;
    return true;
}

void App::shutdownImGui() {
    if (!m_imguiReady) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_imguiReady = false;
}

void App::quit() {
    // Un PNG a medio guardar se termina antes de salir.
    m_exporter.wait();
    if (m_wakeTimer) {
        SDL_RemoveTimer(m_wakeTimer);
        m_wakeTimer = 0;
    }
    // En Android se puede llegar aquí sin contexto actual (la app ya estaba en segundo
    // plano). Destruir el contexto libera igualmente todos sus objetos, así que los
    // objetos RAII solo se olvidan.
    if (!m_foreground || !m_glContext) {
        gfx::onContextRecreated();
    }
    m_ndi.stop();
    m_canvas.destroy();
    m_ui.destroy();
    m_backdrop.destroy();
    m_view.destroy();
    shutdownImGui();
    if (m_glContext) {
        SDL_GL_DestroyContext(m_glContext);
        m_glContext = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

// -----------------------------------------------------------------------------
// Eventos y ciclo de vida
// -----------------------------------------------------------------------------

SDL_AppResult App::event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
        return SDL_APP_SUCCESS;

    // Estos cuatro llegan en el acto, desde el hilo de SDL, con el contexto todavía
    // actual (SDL lo suelta justo después de WILL_ENTER_BACKGROUND).
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        onWillEnterBackground();
        return SDL_APP_CONTINUE;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        onDidEnterForeground();
        return SDL_APP_CONTINUE;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        return SDL_APP_CONTINUE;
    case SDL_EVENT_LOW_MEMORY:
        if (!m_foreground) {
            m_canvas.dropSnapshot();
        }
        return SDL_APP_CONTINUE;

    // SDL no pudo recuperar el contexto al volver y creó uno nuevo (llega antes que
    // DID_ENTER_FOREGROUND).
    case SDL_EVENT_RENDER_DEVICE_RESET:
        onRenderDeviceReset();
        return SDL_APP_CONTINUE;

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        updateWindowSize();
        break;
    default:
        break;
    }

    if (event.type == g_wakeEventType) {
        m_wakeTimer = 0;
        m_redrawFrames = std::max(m_redrawFrames, 1);
        return SDL_APP_CONTINUE;
    }
    if (event.type == g_exportEventType) {
        m_redrawFrames = std::max(m_redrawFrames, 1);   // iterate() recoge el resultado
        return SDL_APP_CONTINUE;
    }
    if (event.type == g_permissionEventType) {
        m_awaitingPermission = false;
        if (event.user.code != 0) {
            m_exportPending = true;   // se lee el lienzo en iterate(), con el contexto actual
        } else {
            m_ui.notify("Sin el permiso de almacenamiento no se puede guardar en Descargas", Notice::Error, 5000);
        }
        m_redrawFrames = std::max(m_redrawFrames, 1);
        return SDL_APP_CONTINUE;
    }

    if (m_imguiReady) {
        routeEvent(event);
    }
    // ImGui reparte las entradas rápidas en varios frames y una ventana o un popup
    // nuevos tardan dos frames en mostrarse con su tamaño: tras cualquier entrada se
    // dibujan unos cuantos frames antes de volver a dormir.
    m_redrawFrames = std::max(m_redrawFrames, kFramesAfterInput);
    return SDL_APP_CONTINUE;
}

void App::onWillEnterBackground() {
    m_foreground = false;
    endGestures();
    m_ui.saveNow();
    // NDI sigue reenviando el último frame desde su hilo, que no usa GL.
    m_ndi.dropInFlight();
    if (m_canvas.ready()) {
        m_canvas.update();
        m_canvas.takeSnapshot(kSnapshotBudget);
    }
    glFinish();
}

void App::onDidEnterForeground() {
    m_foreground = true;
    m_canvas.dropSnapshot();
    updateWindowSize();
    m_redrawFrames = std::max(m_redrawFrames, 2);
}

void App::onRenderDeviceReset() {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Se perdió el contexto GL: recreando los recursos");
    gfx::onContextRecreated();

    // Los objetos de ImGui eran del contexto perdido. Destruirlos aquí no toca nada del
    // contexto nuevo porque todavía no se ha creado ningún objeto en él.
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplOpenGL3_Init(kGlslVersion);
    m_view.init();
    m_ui.init();
    m_backdrop.init();
    m_backdropTexture = 0;
    m_ndi.recreateGpu();

    if (m_canvas.layers().count() > 0) {
        bool restored = false;
        if (!m_canvas.recreateGpu(&restored)) {
            m_ui.notify("No se pudo recuperar el lienzo tras perder el contexto gráfico", Notice::Error, 6000);
        } else if (restored) {
            m_ui.notify("Se recuperó el lienzo tras perder el contexto gráfico", Notice::Success, 4000);
        } else {
            m_ui.notify("Se perdió el contexto gráfico y el dibujo no se pudo recuperar", Notice::Error, 6000);
        }
    }
    gfx::clearErrors();
}

void App::updateWindowSize() {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(m_window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    m_pixelWidth = width;
    m_pixelHeight = height;
    m_camera.setViewport({static_cast<float>(width), static_cast<float>(height)});
}

void App::createCanvas(const CanvasSpec& spec) {
    // NDI emite al tamaño del lienzo: con uno nuevo se vuelve a empezar.
    const bool ndi = m_ndi.running();
    m_ndi.stop();
    endGestures();
    m_fitAnimation.active = false;
    if (!m_canvas.init(spec)) {
        m_ui.notify("No se pudo crear un lienzo de " + std::to_string(spec.width) + " × " + std::to_string(spec.height),
                    Notice::Error, 5000);
        return;
    }
    m_ui.canvasCreated();
    m_camera.setCanvasSize({static_cast<float>(spec.width), static_cast<float>(spec.height)});
    SDL_Log("Lienzo de %dx%d a %.0f ppp (hasta %d capas)", spec.width, spec.height, static_cast<double>(spec.ppi),
            m_canvas.maxLayers());
    if (ndi) {
        setNdiEnabled(true);
    }
}

void App::setNdiEnabled(bool enabled) {
    if (!enabled) {
        m_ndi.stop();
        return;
    }
    if (m_ndi.running() || !m_canvas.ready()) {
        return;
    }
    std::unique_ptr<FrameSink> sink = makeNdiSink("LiveSketch");
    if (!sink) {
        m_ui.notify("Esta compilación no incluye NDI", Notice::Warning);
        return;
    }
    if (!m_ndi.start(std::move(sink), m_canvas.width(), m_canvas.height())) {
        m_ui.notify("No se pudo iniciar NDI", Notice::Error);
    }
}

void App::requestPngExport() {
    if (!m_canvas.ready() || m_exporter.busy() || m_awaitingPermission || m_exportPending) {
        return;
    }
#ifdef SDL_PLATFORM_ANDROID
    // Hasta Android 10 escribir en Descargas pide el permiso de almacenamiento. Si ya está
    // concedido, SDL responde enseguida y se guarda en el frame siguiente.
    if (SDL_GetAndroidSDKVersion() <= 29) {
        m_awaitingPermission =
            SDL_RequestAndroidPermission("android.permission.WRITE_EXTERNAL_STORAGE", onStoragePermission, nullptr);
        if (!m_awaitingPermission) {
            m_ui.notify("No se pudo pedir el permiso de almacenamiento", Notice::Error);
        }
        return;
    }
#endif
    exportPng();
}

void App::exportPng() {
    if (!m_canvas.ready()) {
        return;
    }
    // Se guarda el lienzo completo (sin el zoom de la pantalla), con transparencia si el
    // fondo está oculto o borrado.
    std::vector<uint8_t> pixels;
    if (!m_canvas.readComposite(pixels)) {
        m_ui.notify("No se pudo leer el lienzo para guardar el PNG", Notice::Error, 5000);
        return;
    }
    const std::string path = io::timestampedPath(io::downloadsFolder(), "LiveSketch", ".png");
    if (!m_exporter.start(std::move(pixels), m_canvas.width(), m_canvas.height(), path,
                          [] { pushEvent(g_exportEventType); })) {
        m_ui.notify("No se pudo empezar a guardar el PNG", Notice::Error, 5000);
        return;
    }
    m_ui.notify("Guardando PNG…", Notice::Progress, 60000);
}

void App::onPngSaved(const io::PngExporter::Result& result) {
    if (result.ok) {
        io::announceFile(result.path, "image/png");
        SDL_Log("PNG guardado en %s", result.path.c_str());
#ifdef SDL_PLATFORM_EMSCRIPTEN
        // El archivo estaba en la memoria de la página; lo guarda el navegador.
        m_ui.notify("PNG descargado: " + result.path.substr(result.path.find_last_of('/') + 1), Notice::Success,
                    5000);
#elif defined(SDL_PLATFORM_ANDROID)
        m_ui.notify("PNG guardado en Descargas", Notice::Success, 4000);
#else
        m_ui.notify("PNG guardado en " + result.path, Notice::Success, 5000);
#endif
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo guardar %s: %s", result.path.c_str(),
                     result.error.c_str());
        m_ui.notify("No se pudo guardar el PNG: " + result.error, Notice::Error, 8000);
    }
}

// -----------------------------------------------------------------------------
// Frame
// -----------------------------------------------------------------------------

SDL_AppResult App::iterate() {
    // Después de WILL_ENTER_BACKGROUND SDL todavía llama una vez más, ya sin contexto.
    if (!m_foreground || !m_imguiReady) {
        return SDL_APP_CONTINUE;
    }
    flushPenSample();
    checkLongPress();
    checkHold();
    if (m_exportPending) {
        m_exportPending = false;
        exportPng();
    }
    if (std::optional<io::PngExporter::Result> saved = m_exporter.takeResult()) {
        onPngSaved(*saved);
    }
    samplePick();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    const UiStatus status = uiStatus();
    m_view.setPixelsPerPoint(status.pointScale * status.pixelsPerUnit);
    // La estabilización del trazo se mide en la pantalla: píxeles del lienzo por punto.
    m_canvas.setViewScale(status.pointScale * status.pixelsPerUnit / std::max(m_camera.zoom(), 1e-4f));
    m_ui.beginFrame(status);
    ImGui::NewFrame();
    m_backdropTexture = m_backdrop.texture();
    ui::setBackdrop(textureId(m_backdropTexture), ImGui::GetIO().DisplaySize);

    UiRequests requests;
    m_ui.build(m_canvas.ready() ? &m_canvas : nullptr, requests);
    ImGui::Render();

    if (requests.quit) {
        return SDL_APP_SUCCESS;
    }
    applyRequests(requests);

    // El ajuste del lienzo deja libre lo que tapa la interfaz.
    const float density = SDL_GetWindowPixelDensity(m_window);
    m_camera.setInsets(m_ui.insetTop() * density, m_ui.insetRight() * density, m_ui.insetBottom() * density,
                       m_ui.insetLeft() * density);
    stepFitAnimation();

    m_canvas.update();
    if (m_ndi.running()) {
        m_ndi.capture(m_canvas.composite().fbo.id(), m_canvas.version());
    }
    updateBackdrop();
    renderFrame();
    schedulePacing();
    return SDL_APP_CONTINUE;
}

UiStatus App::uiStatus() const {
    UiStatus status;
    const float density = std::max(SDL_GetWindowPixelDensity(m_window), 0.01f);
    const float displayScale = SDL_GetWindowDisplayScale(m_window);
    // Unidades de ImGui (coordenadas de la ventana) por punto: en Android las coordenadas
    // son píxeles y la escala es la densidad de la pantalla; en el escritorio y la web,
    // la ventana ya va en puntos.
    status.pointScale = displayScale > 0.0f ? displayScale / density : 1.0f;
    status.pixelsPerUnit = density;
    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(m_window, &windowWidth, &windowHeight);
    SDL_Rect safe;
    if (SDL_GetWindowSafeArea(m_window, &safe) && windowWidth > 0 && windowHeight > 0) {
        status.safeTop = static_cast<float>(std::max(safe.y, 0));
        status.safeLeft = static_cast<float>(std::max(safe.x, 0));
        status.safeRight = static_cast<float>(std::max(windowWidth - (safe.x + safe.w), 0));
        status.safeBottom = static_cast<float>(std::max(windowHeight - (safe.y + safe.h), 0));
    }
    status.screenWidth = m_pixelWidth;
    status.screenHeight = m_pixelHeight;
    status.maxCanvasSize = m_maxCanvasSize;
    status.canvasZoom = m_camera.zoom();
    status.canvasView = toolView();
    status.viewAngle = m_camera.angle() * (180.0f / 3.14159265358979f);
    status.viewFlipped = m_camera.flipped();
    status.viewTurning = m_twisting || SDL_GetTicks() < m_angleShownUntil;
    if (m_canvas.ready() && m_canvas.stroking()) {
        if (m_pen.drawing) {
            status.strokePointer = StrokePointer::Pen;
        } else if (m_fingerDrawing) {
            status.strokePointer = StrokePointer::Finger;
        } else if (m_mouseDrawing) {
            status.strokePointer = StrokePointer::Mouse;
        }
    }
    status.penPressure = m_pen.down ? m_pen.pressure : -1.0f;
    status.touchInput = m_uiTouch;
    status.ndiAvailable = m_ndiAvailable;
    status.ndiRunning = m_ndi.running();
    status.ndiConnections = m_ndi.connections();
    status.ndiError = m_ndi.error();
    status.exporting = m_awaitingPermission || m_exportPending || m_exporter.busy();
    return status;
}

void App::applyRequests(const UiRequests& requests) {
    if (requests.createCanvas) {
        createCanvas(requests.canvas);
        m_redrawFrames = std::max(m_redrawFrames, kFramesAfterInput);
    }
    if (requests.fitView) {
        startFitAnimation();
    }
    if (requests.rotateView != 0 || requests.straightenView) {
        rotateView(requests.rotateView);
    }
    if (requests.flipView && m_canvas.ready()) {
        m_fitAnimation.active = false;
        m_camera.setFlipped(!m_camera.flipped());
    }
    if (requests.savePng) {
        requestPngExport();
    }
    if (requests.ndi >= 0) {
        setNdiEnabled(requests.ndi == 1);
    }
}

void App::startFitAnimation() {
    if (!m_canvas.ready()) {
        return;
    }
    m_fitAnimation.active = true;
    m_fitAnimation.startMs = SDL_GetTicks();
    m_fitAnimation.fromZoom = m_camera.zoom();
    m_fitAnimation.fromAngle = m_camera.angle();
    m_fitAnimation.fromCenter = m_camera.canvasToScreen(m_camera.canvasSize() * 0.5f);
}

void App::stepFitAnimation() {
    if (!m_fitAnimation.active) {
        return;
    }
    // Si el usuario toca el lienzo mientras tanto, manda él.
    if (canvasInteractionActive() || !m_canvas.ready()) {
        m_fitAnimation.active = false;
        return;
    }
    const float t = static_cast<float>(SDL_GetTicks() - m_fitAnimation.startMs) / static_cast<float>(kFitDurationMs);
    if (t >= 1.0f) {
        m_camera.fit();
        m_fitAnimation.active = false;
        return;
    }
    float zoom = 1.0f;
    glm::vec2 center{0.0f};
    m_camera.fitView(zoom, center);
    // El zoom avanza en escala logarítmica y el centro del lienzo en línea recta: así el
    // movimiento se ve uniforme aunque el zoom cambie mucho. El giro vuelve a 0 por el
    // camino corto (el ángulo va de -180° a 180°).
    const float e = ui::anim::easeInOutCubic(std::clamp(t, 0.0f, 1.0f));
    const float fromZoom = std::max(m_fitAnimation.fromZoom, 1e-6f);
    const float z = std::exp(std::log(fromZoom) + (std::log(std::max(zoom, 1e-6f)) - std::log(fromZoom)) * e);
    const glm::vec2 from = m_fitAnimation.fromCenter;
    m_camera.place(z, m_fitAnimation.fromAngle * (1.0f - e), from + (center - from) * e);
}

void App::updateBackdrop() {
    if (!m_canvas.ready()) {
        return;
    }
    // Mientras se dibuja no hace falta rehacerlo en cada frame: está desenfocado.
    const uint64_t now = SDL_GetTicks();
    if (m_canvas.version() != m_backdropVersion &&
        (!m_canvas.stroking() || now - m_backdropUpdatedMs >= kBackdropStrokeIntervalMs)) {
        m_backdropVersion = m_canvas.version();
        m_backdropUpdatedMs = now;
    }
    uint64_t key = mixHash(0, m_backdropVersion);
    key = mixHash(key, floatBits(m_camera.zoom()));
    key = mixHash(key, floatBits(m_camera.offset().x));
    key = mixHash(key, floatBits(m_camera.offset().y));
    key = mixHash(key, floatBits(m_camera.angle()));
    key = mixHash(key, m_camera.flipped() ? 1 : 0);
    key = mixHash(key, (static_cast<uint64_t>(m_pixelWidth) << 32) | static_cast<uint32_t>(m_pixelHeight));
    const float sigma = kBackdropBlurPoints * ui::scale() * SDL_GetWindowPixelDensity(m_window);
    m_backdrop.update(m_view, m_camera, m_canvas.composite().texture.id(), m_pixelWidth, m_pixelHeight, sigma, key);
    retargetTexture(ImGui::GetDrawData(), textureId(m_backdropTexture), textureId(m_backdrop.texture()));
}

void App::renderFrame() {
    const GLuint composite = m_canvas.ready() ? m_canvas.composite().texture.id() : 0;
    m_view.draw(m_camera, composite, 0, m_pixelWidth, m_pixelHeight);
    if (m_canvas.ready() && m_ui.selectionAnimating(m_canvas)) {
        const int phase = static_cast<int>(SDL_GetTicks() / kAntsStepMs);
        const float veil = m_ui.canvasTool() == CanvasTool::Select ? kSelectionVeil : 0.0f;
        m_view.drawSelection(m_camera, m_canvas.selectionMask(), phase, veil, 0, m_pixelWidth, m_pixelHeight);
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);
}

void App::schedulePacing() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    // Entradas que ImGui todavía no ha procesado o el oscurecido de un diálogo a medias.
    const bool imguiPending = g.InputEventsQueue.Size > 0 || (g.DimBgRatio > 0.0f && g.DimBgRatio < 1.0f);
    const bool busy = m_redrawFrames > 0 || imguiPending || canvasInteractionActive() || io.WantTextInput ||
                      ImGui::IsAnyItemActive() || m_ndi.busy() || ui::anim::active() || m_fitAnimation.active ||
                      m_pick.source != PickSource::None || SDL_GetTicks() < m_angleShownUntil;
    if (m_redrawFrames > 0) {
        --m_redrawFrames;
    }

    // Con algo en movimiento se dibuja a la frecuencia de la pantalla (vsync); si no,
    // el bucle duerme hasta el siguiente evento y la app no gasta batería.
    if (busy != m_continuous) {
        SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, busy ? "0" : "waitevent");
        m_continuous = busy;
    }
    if (!busy) {
        uint64_t deadline = m_ui.wakeDeadline();
        if (m_canvas.ready() && m_ui.selectionAnimating(m_canvas)) {
            // El siguiente paso del borde de la selección.
            const uint64_t step = (SDL_GetTicks() / kAntsStepMs + 1) * kAntsStepMs;
            deadline = deadline == 0 ? step : std::min(deadline, step);
        }
        if (m_ndi.running()) {
            // Refresca cada segundo el número de receptores que muestra el menú.
            const uint64_t refresh = SDL_GetTicks() + 1000;
            deadline = deadline == 0 ? refresh : std::min(deadline, refresh);
        }
        if (deadline != 0) {
            wakeAt(deadline);
        }
    }
}

void App::wakeAt(uint64_t ticksMs) {
    if (m_wakeTimer != 0 && m_wakeAt == ticksMs) {
        return;
    }
    if (m_wakeTimer != 0) {
        SDL_RemoveTimer(m_wakeTimer);
    }
    const uint64_t now = SDL_GetTicks();
    const Uint32 delay = ticksMs > now ? static_cast<Uint32>(ticksMs - now) : 1;
    m_wakeAt = ticksMs;
    m_wakeTimer = SDL_AddTimer(delay, pushWakeEvent, nullptr);
}
