#include "App/App.h"

#include "Gfx/GL.h"
#include "Gfx/GLObjects.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>

namespace {

constexpr const char* kGlslVersion = "#version 300 es";

// RAM que puede ocupar la copia de las capas que se hace al pasar a segundo plano, por
// si el driver pierde el contexto GL mientras tanto.
constexpr size_t kSnapshotBudget = size_t{256} * 1024 * 1024;

// Frames que se dibujan después de cada entrada (ver App::event).
constexpr int kFramesAfterInput = 4;

// Evento propio para despertar el bucle cuando está dormido esperando eventos.
Uint32 g_wakeEventType = 0;

Uint32 SDLCALL pushWakeEvent(void*, SDL_TimerID, Uint32) {
    SDL_Event event;
    SDL_zero(event);
    event.type = g_wakeEventType;
    SDL_PushEvent(&event);
    return 0;
}

} // namespace

// -----------------------------------------------------------------------------
// Arranque y cierre
// -----------------------------------------------------------------------------

SDL_AppResult App::init(int, char**) {
    SDL_SetAppMetadata("LiveSketch", "0.2.4-alpha", "com.tacodec.livesketch");

    // El lápiz llega como eventos de lápiz (con presión) y, para ImGui, también como
    // ratón. Los dedos manejan ImGui a través del ratón emulado y el lienzo con sus
    // propios eventos. Un ratón de verdad no se hace pasar por un dedo.
    SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    // El botón atrás de Android llega como tecla y pide confirmación antes de salir.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "0");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    g_wakeEventType = SDL_RegisterEvents(1);

    if (!createWindow() || !initImGui()) {
        return SDL_APP_FAILURE;
    }
#ifdef LIVESKETCH_WITH_NDI
    m_ui.ndiAvailable = true;
#endif
    if (!m_view.init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudieron compilar los shaders de la vista");
        return SDL_APP_FAILURE;
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
#ifdef SDL_PLATFORM_ANDROID
    flags |= SDL_WINDOW_FULLSCREEN;
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
    m_ndi.recreateGpu();

    if (m_canvas.layers().count() > 0) {
        bool restored = false;
        if (!m_canvas.recreateGpu(&restored)) {
            m_menu.notify("No se pudo recuperar el lienzo tras perder el contexto gráfico", 6000);
        } else if (restored) {
            m_menu.notify("Se recuperó el lienzo tras perder el contexto gráfico", 4000);
        } else {
            m_menu.notify("Se perdió el contexto gráfico y el dibujo no se pudo recuperar", 6000);
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

void App::createCanvas(int width, int height) {
    if (!m_canvas.init(width, height)) {
        m_menu.notify("No se pudo crear un lienzo de " + std::to_string(width) + "×" + std::to_string(height), 5000);
        return;
    }
    m_camera.setCanvasSize({static_cast<float>(width), static_cast<float>(height)});
    SDL_Log("Lienzo de %dx%d (hasta %d capas)", width, height, m_canvas.maxLayers());
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
        m_menu.notify("Esta compilación no incluye NDI");
        return;
    }
    if (!m_ndi.start(std::move(sink), m_canvas.width(), m_canvas.height())) {
        m_menu.notify("No se pudo iniciar NDI");
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

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    m_ui.ndiRunning = m_ndi.running();
    m_ui.ndiConnections = m_ndi.connections();
    m_ui.ndiError = m_ndi.error();

    UiRequests requests;
    if (m_canvas.ready()) {
        m_menu.controls(m_canvas, m_ui, requests);
    } else {
        m_menu.startup(m_pixelWidth, m_pixelHeight, m_maxCanvasSize, requests);
    }
    m_menu.overlays(m_canvas.ready() ? &m_canvas : nullptr, requests);
    ImGui::Render();

    if (requests.quit) {
        return SDL_APP_SUCCESS;
    }
    applyRequests(requests);

    m_canvas.update();
    if (m_ndi.running()) {
        m_ndi.capture(m_canvas.composite().fbo.id(), m_canvas.version());
    }
    renderFrame();
    schedulePacing();
    return SDL_APP_CONTINUE;
}

void App::applyRequests(const UiRequests& requests) {
    if (requests.canvasWidth > 0 && requests.canvasHeight > 0) {
        createCanvas(requests.canvasWidth, requests.canvasHeight);
        // El panel de control aparece en los frames siguientes.
        m_redrawFrames = std::max(m_redrawFrames, kFramesAfterInput);
    }
    if (requests.fitView) {
        m_camera.fit();
    }
    if (requests.ndi >= 0) {
        setNdiEnabled(requests.ndi == 1);
    }
}

void App::renderFrame() {
    const GLuint composite = m_canvas.ready() ? m_canvas.composite().texture.id() : 0;
    m_view.draw(m_camera, composite, m_pixelWidth, m_pixelHeight);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_window);
}

void App::schedulePacing() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    // Entradas que ImGui todavía no ha procesado o el oscurecido de un diálogo a medias.
    const bool imguiPending = g.InputEventsQueue.Size > 0 || (g.DimBgRatio > 0.0f && g.DimBgRatio < 1.0f);
    const bool busy = m_redrawFrames > 0 || imguiPending || canvasInteractionActive() || io.WantTextInput ||
                      ImGui::IsAnyItemActive() || m_ndi.busy();
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
        uint64_t deadline = m_menu.noticeDeadline();
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
