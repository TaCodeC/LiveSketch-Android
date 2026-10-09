#include "App/App.h"

#include "App/AppEvents.h"
#include "Gfx/GL.h"
#include "Gfx/GLObjects.h"
#include "IO/FileChooser.h"
#include "IO/Storage.h"
#include "UI/Anim.h"
#include "UI/ColorManage.h"
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

#ifdef SDL_PLATFORM_EMSCRIPTEN
#include <emscripten/em_macros.h>
#endif

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


Uint32 SDLCALL pushWakeEvent(void*, SDL_TimerID, Uint32) {
    appevents::push(appevents::wake);
    return 0;
}

#ifdef SDL_PLATFORM_ANDROID
void SDLCALL onStoragePermission(void*, const char*, bool granted) {
    appevents::push(appevents::permission, granted ? 1 : 0);
}
#endif

} // namespace

namespace appevents {

Uint32 wake = 0;
Uint32 pngSaved = 0;
Uint32 permission = 0;
Uint32 projectSaved = 0;
Uint32 fileChosen = 0;
Uint32 saveFileChosen = 0;

void push(Uint32 type, Sint32 code, char* data) {
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.user.code = code;
    event.user.data1 = data;
    if (!SDL_PushEvent(&event)) {
        SDL_free(data);
    }
}

} // namespace appevents

#ifdef SDL_PLATFORM_EMSCRIPTEN
namespace {
App* g_pageApp = nullptr;   // la que se guarda al ocultar o cerrar la página
}

// La página se oculta o se cierra (src/web/storage.js). Devuelve 1 si queda algo que se
// perdería al cerrarla.
extern "C" EMSCRIPTEN_KEEPALIVE int liveSketchSuspend() {
    return g_pageApp && g_pageApp->suspendPage() ? 1 : 0;
}
#endif

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
    const Uint32 firstEvent = SDL_RegisterEvents(6);
    if (firstEvent == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_RegisterEvents: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    appevents::wake = firstEvent;
    appevents::pngSaved = firstEvent + 1;
    appevents::permission = firstEvent + 2;
    appevents::projectSaved = firstEvent + 3;
    appevents::fileChosen = firstEvent + 4;
    appevents::saveFileChosen = firstEvent + 5;

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
    // Los proyectos y, si había uno abierto, se vuelve a él. (En Android, si se abrió la app
    // con un proyecto desde otra, ese; y si después llega otro, se avisa.)
    io::setLaunchFileListener([] { appevents::push(appevents::wake, 3); });
    openLibrary();
#ifdef SDL_PLATFORM_EMSCRIPTEN
    g_pageApp = this;
    io::setStorageListener([] { appevents::push(appevents::wake, 2); });
#endif
    checkStorage();
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
#ifdef SDL_PLATFORM_EMSCRIPTEN
    g_pageApp = nullptr;
    io::setStorageListener(nullptr);
#endif
    // Lo que no esté guardado se guarda (si aún hay contexto: en Android ya se guardó al pasar
    // a segundo plano). Un PNG, un proyecto o una operación de la biblioteca a medias se
    // terminan antes de salir; lo que se abría, no.
    if (m_foreground && m_glContext && m_imguiReady) {
        saveBeforeSuspend();
    }
    m_exporter.wait();
    m_projectSaver.wait();
    m_library.wait();
    closeProjectFile();
    resetProject();
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
#if !defined(SDL_PLATFORM_ANDROID) && !defined(SDL_PLATFORM_EMSCRIPTEN)
        // Cerrar la ventana: primero se guarda el lienzo (y si no se puede, se pregunta).
        if (m_canvas.ready() && m_foreground && m_imguiReady) {
            requestLeave(Leave::Quit);
            m_redrawFrames = std::max(m_redrawFrames, 1);
            return SDL_APP_CONTINUE;
        }
#endif
        return SDL_APP_SUCCESS;
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
    // La ventana pasó a otra pantalla, o la pantalla cambió: puede mostrar otra gama.
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_DISPLAY_ADDED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        m_display.displayChanged();
        break;
    default:
        break;
    }

    if (event.type == appevents::wake) {
        // 0: el temporizador de wakeAt(); 1: la biblioteca tiene algo nuevo; 2: el navegador
        // no pudo guardar los datos; 3: Android pide abrir otro proyecto desde otra app.
        if (event.user.code == 0) {
            m_wakeTimer = 0;
        } else if (event.user.code == 2) {
            checkStorage();
        } else if (event.user.code == 3) {
            const std::string file = io::takeLaunchFile();
            if (!file.empty()) {
                SDL_Log("Abierto con LiveSketch: %s", file.c_str());
                fileChosen(file);
            }
        }
        m_redrawFrames = std::max(m_redrawFrames, 1);
        return SDL_APP_CONTINUE;
    }
    if (event.type == appevents::pngSaved || event.type == appevents::projectSaved) {
        m_redrawFrames = std::max(m_redrawFrames, 1);   // iterate() recoge el resultado
        return SDL_APP_CONTINUE;
    }
    if (event.type == appevents::permission) {
        const StorageTask task = m_awaitingPermission;
        m_awaitingPermission = StorageTask::None;
        const PendingExport pending = std::move(m_pendingExport);
        m_pendingExport = {};
        if (event.user.code == 0) {
            m_ui.notify("Sin el permiso de almacenamiento no se puede guardar en Descargas", Notice::Error, 5000);
        } else if (task == StorageTask::Png) {
            m_exportPending = true;   // se lee el lienzo en iterate(), con el contexto actual
        } else if (task == StorageTask::Export) {
            exportProject(pending.path, pending.name, ExportTo::Downloads);
        }
        m_redrawFrames = std::max(m_redrawFrames, 1);
        return SDL_APP_CONTINUE;
    }
    if (event.type == appevents::fileChosen) {
        char* data = static_cast<char*>(event.user.data1);
        m_choosingFile = false;
        if (event.user.code == 0 && data) {
            fileChosen(data);
        } else if (event.user.code < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Selector de archivos: %s", data ? data : "");
            m_ui.notify("No se pudo mostrar el selector de archivos", Notice::Error, 5000);
        }
        SDL_free(data);
        m_redrawFrames = std::max(m_redrawFrames, 1);
        return SDL_APP_CONTINUE;
    }
    if (event.type == appevents::saveFileChosen) {
        char* data = static_cast<char*>(event.user.data1);
        const std::optional<std::string> source = std::move(m_exportChoosing);
        m_exportChoosing.reset();
        if (event.user.code == 0 && data && source) {
            exportChosen(*source, data);
        } else if (event.user.code < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Elegir dónde exportar: %s", data ? data : "");
            m_ui.notify("No se pudo elegir dónde exportar el proyecto", Notice::Error, 5000);
        }
        SDL_free(data);
        m_redrawFrames = std::max(m_redrawFrames, 1);
        return SDL_APP_CONTINUE;
    }
    // Un archivo soltado en la ventana: se abre como proyecto (el primero, si son varios).
    if (event.type == SDL_EVENT_DROP_FILE) {
        if (event.drop.data) {
            fileChosen(event.drop.data);
        }
        m_redrawFrames = std::max(m_redrawFrames, kFramesAfterInput);
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
        // El sistema puede cerrar la app sin avisar: el proyecto se guarda ya (se espera a que
        // esté escrito; este hilo no es el de la interfaz de Android).
        saveBeforeSuspend();
        m_canvas.takeSnapshot(kSnapshotBudget);
    }
    glFinish();
}

bool App::suspendPage() {
    if (!m_foreground || !m_imguiReady || !m_glContext) {
        return false;
    }
    endGestures();
    m_ui.saveNow();
    if (m_canvas.ready()) {
        m_canvas.update();
        saveBeforeSuspend();
    }
    m_redrawFrames = std::max(m_redrawFrames, 1);
    if (projectUnsaved(true)) {
        return true;
    }
    // Si el navegador no guarda nada, se pierde todo lo que haya.
    return !io::storageProblem().empty() && (m_canvas.ready() || !m_library.items().empty());
}

void App::onDidEnterForeground() {
    m_foreground = true;
    m_canvas.dropSnapshot();
    m_display.displayChanged();
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
        const bool recreated = m_canvas.recreateGpu(&restored);
        if (recreated && restored) {
            m_ui.notify("Se recuperó el lienzo tras perder el contexto gráfico", Notice::Success, 4000);
        } else if (reloadProject()) {
            // Del archivo del proyecto: lo último que se guardó.
            m_ui.notify("Se perdió el contexto gráfico: se vuelve a abrir el proyecto guardado", Notice::Warning,
                        6000);
        } else if (recreated) {
            // El lienzo quedó vacío: ya no es el del archivo (si lo tenía), que no se toca.
            resetProject();
            m_project.savedVersion = m_canvas.documentVersion();
            m_ui.notify("Se perdió el contexto gráfico y el dibujo no se pudo recuperar", Notice::Error, 6000);
        } else {
            resetProject();
            m_ui.notify("No se pudo recuperar el lienzo tras perder el contexto gráfico", Notice::Error, 6000);
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
    closeProjectFile();
    // NDI emite al tamaño del lienzo: con uno nuevo se vuelve a empezar.
    const bool ndi = m_ndiResume || m_ndi.running();
    m_ndiResume = false;
    m_ndi.stop();
    endGestures();
    m_fitAnimation.active = false;
    const bool created = m_canvas.init(spec);
    // Es otro lienzo (o ninguno, si no se pudo): su proyecto empieza de cero.
    resetProject();
    setLastProject({});
    m_library.refresh();
    if (!created) {
        m_ndiResume = ndi;
        m_ui.showAlert("No se pudo crear el lienzo",
                       "No hay memoria de gráficos para un lienzo de " + std::to_string(spec.width) + " × " +
                           std::to_string(spec.height) + " px.",
                       Notice::Error);
        return;
    }
    m_ui.canvasCreated();
    m_camera.setCanvasSize({static_cast<float>(spec.width), static_cast<float>(spec.height)});
    // Un lienzo recién creado no tiene nada que perder: se guarda al cambiar algo.
    m_project.savedVersion = m_canvas.documentVersion();
    m_project.seenVersion = m_project.savedVersion;
    m_project.changedMs = SDL_GetTicks();
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

bool App::storageReady([[maybe_unused]] StorageTask task) {
#ifdef SDL_PLATFORM_ANDROID
    // Hasta Android 10 escribir en Descargas pide el permiso de almacenamiento. Si ya está
    // concedido, SDL responde enseguida y se guarda en el frame siguiente.
    if (SDL_GetAndroidSDKVersion() <= 29) {
        if (m_awaitingPermission != StorageTask::None) {
            return false;   // ya se está pidiendo
        }
        m_awaitingPermission = task;
        if (!SDL_RequestAndroidPermission("android.permission.WRITE_EXTERNAL_STORAGE", onStoragePermission,
                                          nullptr)) {
            m_awaitingPermission = StorageTask::None;
            m_ui.notify("No se pudo pedir el permiso de almacenamiento", Notice::Error);
        }
        return false;
    }
#endif
    return true;
}

void App::requestPngExport() {
    if (!m_canvas.ready() || m_exporter.busy() || m_awaitingPermission == StorageTask::Png || m_exportPending) {
        return;
    }
    if (storageReady(StorageTask::Png)) {
        exportPng();
    }
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
    // Con el nombre del lienzo (o la fecha y hora), sus ppp (se imprime a su tamaño) y su
    // perfil de color.
    const CanvasInfo& info = m_canvas.info();
    png::Info png;
    png.ppi = info.ppi;
    png.profile = info.profile;
    png.title = info.name;
    if (!m_exporter.start(std::move(pixels), m_canvas.width(), m_canvas.height(), io::downloadsFolder(),
                          io::fileStem(info.name), std::move(png), [] { appevents::push(appevents::pngSaved); })) {
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
        m_ui.notify("Guardado en Descargas: " + result.path.substr(result.path.find_last_of('/') + 1),
                    Notice::Success, 4000);
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
    // Proyectos: la biblioteca, guardar cuando toca, dejar el lienzo, el archivo que se eligió
    // y las capas del que se abre.
    updateProjects();
    if (m_quit) {
        return SDL_APP_SUCCESS;
    }
    if (m_chosenFile) {
        const ChosenFile file = std::move(*m_chosenFile);
        m_chosenFile.reset();
        openProjectFile(file.path, file.temporary);
    }
    if (m_open.loading) {
        stepProjectOpen();
    }
    samplePick();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // La pantalla en el perfil del lienzo si puede (Display P3); si no, la vista y la
    // interfaz convierten sus colores.
    m_displayProfile = m_display.sync(m_window, canvasProfile());
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
    ui::gamut::convert(ImGui::GetDrawData());

    applyRequests(requests);
    if (m_quit) {
        return SDL_APP_SUCCESS;
    }

    // El ajuste del lienzo deja libre lo que tapa la interfaz.
    const float density = SDL_GetWindowPixelDensity(m_window);
    m_camera.setInsets(m_ui.insetTop() * density, m_ui.insetRight() * density, m_ui.insetBottom() * density,
                       m_ui.insetLeft() * density);
    stepFitAnimation();

    m_canvas.update();
    if (m_ndi.running()) {
        // Los receptores esperan sRGB.
        m_ndi.capture(m_canvas.composite(), m_canvas.version(),
                      colorspace::between(m_canvas.info().profile, ColorProfile::Srgb));
    }
    // El lienzo pudo cambiar de perfil con lo que pidió la interfaz; la pantalla lo sigue en
    // el frame siguiente.
    m_view.setGamut(colorspace::between(canvasProfile(), m_displayProfile));
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
    status.exporting = m_awaitingPermission == StorageTask::Png || m_exportPending || m_exporter.busy();
    if (m_canvas.ready() && !m_open.loading) {
        const bool asked = m_userSaveAtMs != 0 ||
                           (m_saving.active && m_saving.reason != SaveReason::Auto &&
                            m_saving.reason != SaveReason::Background);
        if (asked) {
            status.projectSave = ProjectSave::Saving;
        } else if (!m_project.error.empty()) {
            status.projectSave = ProjectSave::Failed;
            status.projectError = m_project.error;
        } else if (projectUnsaved(false)) {
            status.projectSave = ProjectSave::Unsaved;
        } else {
            status.projectSave = ProjectSave::Saved;
        }
        status.exportingProject = m_awaitingPermission == StorageTask::Export || m_exportCanvas != ExportTo::None ||
                                  m_exportChoosing ||
                                  (m_saving.active && m_saving.reason == SaveReason::Export) ||
                                  (!m_project.path.empty() && m_library.busy(m_project.path));
    }
    if (m_open.loading) {
        status.busy = Busy::Opening;
        status.busyProgress = m_open.loader->progress();
        status.busyTitle = m_open.title;
    } else if (m_leave.action != Leave::None && !m_leave.asking) {
        status.busy = Busy::Saving;
        status.busyTitle = m_canvas.ready() ? m_canvas.info().name : std::string();
        status.busySinceMs = m_leave.sinceMs;
    }
    status.library = m_library.ready() ? &m_library : nullptr;
    status.displayProfile = m_displayProfile;
    status.canvasProfile = canvasProfile();
    status.wideGamutScreen = m_display.wideAvailable();
    return status;
}

void App::applyRequests(const UiRequests& requests) {
    // Dejar el lienzo: si no se pudo guardar, la interfaz pregunta.
    if (m_leave.asking) {
        if (requests.leaveConfirmed) {
            m_leave.asking = false;
            performLeave();
        } else if (!m_ui.confirmingLeave()) {
            // Se quedó en el lienzo.
            if (m_leave.action == Leave::Open) {
                closeProjectFile();
            }
            m_leave = {};
        }
    }
    if (requests.createCanvas) {
        if (m_canvas.ready()) {
            requestLeave(Leave::NewCanvas, &requests.canvas);
        } else {
            createCanvas(requests.canvas);
        }
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
    if (requests.saveProject) {
        requestProjectSave();
    }
    if (requests.exportProject) {
        exportCanvasProject(ExportTo::Downloads);
    }
    if (requests.exportProjectTo) {
        exportCanvasProject(ExportTo::Choose);
    }
    if (requests.openProject) {
        chooseProject();
    }
    if (requests.showProjects && m_canvas.ready()) {
        requestLeave(Leave::Projects);
    }
    applyProjectAction(requests.project);
    if (requests.quit) {
        if (m_canvas.ready()) {
            requestLeave(Leave::Quit);
        } else {
            m_quit = true;
        }
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
    key = mixHash(key, static_cast<uint64_t>(m_view.gamut().key()));
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
    // Los proyectos: abrir, dejar el lienzo, guardar dentro de un momento y, en la web (sin
    // hilos), comprimir y mirar la carpeta. En el resto, los hilos avisan al terminar.
#ifdef SDL_PLATFORM_EMSCRIPTEN
    const bool projectWork = m_userSaveAtMs != 0 || m_open.loading || m_chosenFile || m_leave.action != Leave::None ||
                             m_projectSaver.busy() || m_library.working();
#else
    const bool projectWork = m_userSaveAtMs != 0 || m_open.loading || m_chosenFile ||
                             (m_leave.action != Leave::None && !m_leave.asking && !m_projectSaver.busy());
#endif
    const bool busy = m_redrawFrames > 0 || imguiPending || canvasInteractionActive() || io.WantTextInput ||
                      ImGui::IsAnyItemActive() || m_ndi.busy() || ui::anim::active() || m_fitAnimation.active ||
                      m_pick.source != PickSource::None || SDL_GetTicks() < m_angleShownUntil || projectWork;
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
        // El guardado automático.
        const uint64_t autosave = autosaveDeadline();
        if (autosave != 0) {
            deadline = deadline == 0 ? autosave : std::min(deadline, autosave);
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
