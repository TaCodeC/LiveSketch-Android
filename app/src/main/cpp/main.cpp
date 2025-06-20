#include <android_native_app_glue.h>
#include <android/log.h>
#include "Renderer/Renderer.h"
#include <cmath>
#include <algorithm>
#include "DevUI/IMGUIMenu.h"
#include "imgui_internal.h"
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_android.h>
#include <thread>
#define LOG_TAG "LiveSketch"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#include <chrono> // para std::chrono
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "FileManager/FileManager.h"
long long int currentTimeMs();
void StylusEvent(android_app* app, AInputEvent* event, float sx, float sy);
void drawFrame(android_app* app);
void handleMenuAction(android_app* app);
void renderOnlIMGUI(android_app* app);
void SaveCompositeFBOAsPNG(GLuint fbo,int width, int height);
static void requestRuntimePermissions(ANativeActivity* activity);
long long lastPinchTime = 0;
const long long pinchTimeoutMs = 100; // 100 ms de "timeout"
static glm::vec2 screenToCanvas(float x, float y,const LayerManager& mgr);
static bool isInsideLayer(float x, float y,const LayerManager& mgr);
// Estado global para zoom
float lastDistance = 0.0f;
bool renderDirty = true;
bool isPinching = false; // nuevo
bool menuAction = false;
bool isPanning = false;
float  panLastX;
float  panLastY;
IMGUIMenu uiMenu;
MenuAction action;
//static FileManager* gFileManager = nullptr;
int renderWidth;
int renderHeight;

static bool showStartup = true;
static int  preset      = 5; // por defecto full
static int  customW     = 0, customH = 0;
ImVec2     presets[] = {
        {  640,  360},
        { 1280,  720},
        { 1920, 1080},
        { 2560, 1440},
        { 3840, 2160},
        {   0,     0}   // Full
};

/*
// Manejo de callbacks de ActivityResult (enlazado desde Java si es necesario)
extern "C" JNIEXPORT void JNICALL
Java_com_yourpackage_NativeActivity_onActivityResult(JNIEnv* env, jobject thiz,
                                                     jint requestCode, jint resultCode, jobject data) {
    if (gFileManager) {
        gFileManager->onActivityResult(requestCode, resultCode, data);
    }
}*/

struct Engine {
    ANativeWindow* window = nullptr;
    Renderer renderer;
    std::vector<uint8_t> ndiTemp; // buffer único
    bool running = true;
    bool menuInitialized = false;

};

static bool IsInsideUI(float x, float y) {
    // Busca la ventana de ImGui por su nombre
    ImGuiWindow* win = ImGui::FindWindowByName("LiveSketch Control");
    if (!win) return false;
    const ImVec2& p = win->Pos;
    const ImVec2& s = win->Size;
    return x >= p.x && x <= p.x + s.x
           && y >= p.y && y <= p.y + s.y;
}
int32_t handleInput(android_app* app, AInputEvent* event) {
    // Solo procesamos eventos de movimiento
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        isPinching = false;
        return 0;
    }

    // Datos comunes
    Engine* engine  = (Engine*)app->userData;
    int toolType    = AMotionEvent_getToolType(event, 0);
    int32_t action  = AMotionEvent_getAction(event);
    int32_t actionM = action & AMOTION_EVENT_ACTION_MASK;
    size_t pointers = AMotionEvent_getPointerCount(event);

    auto& mgr = engine->renderer.layerManager;

// Coordenadas originales de pantalla:
    float x = AMotionEvent_getX(event, 0);
    float y = AMotionEvent_getY(event, 0);


    if (!engine->menuInitialized) {
        ImGui_ImplAndroid_HandleInputEvent(event);
        renderDirty = true;
        return 0;
    }
    if (IsInsideUI(x, y)) {
        ImGui_ImplAndroid_HandleInputEvent(event);
        menuAction = true;
        renderDirty = true;
        renderOnlIMGUI(app);
        return 0;
    }
    // Manejo de stylus/eraser
    if (toolType == AMOTION_EVENT_TOOL_TYPE_ERASER ||
        toolType == AMOTION_EVENT_TOOL_TYPE_STYLUS) {
        StylusEvent(app, event, x, y);
        return 0;
    }



    // Pinch-zoom con dos dedos
    if (pointers >= 2) {
        isPinching = true;
        isPanning = false;
        lastPinchTime = currentTimeMs();

        // Lectura de posiciones
        float x0 = AMotionEvent_getX(event, 0);
        float y0 = AMotionEvent_getY(event, 0);
        float x1 = AMotionEvent_getX(event, 1);
        float y1 = AMotionEvent_getY(event, 1);

        float dx = x1 - x0;
        float dy = y1 - y0;
        float distance = std::sqrt(dx*dx + dy*dy);

        if (lastDistance > 0.0f) {
            float rawDelta    = distance / lastDistance;
            const float sens = 1.0f;
            float delta      = std::pow(rawDelta, sens);

            auto& mgr = engine->renderer.layerManager;
            mgr.currentScale *= delta;
            mgr.currentScale.x = std::clamp(mgr.currentScale.x, 0.1f, 10.0f);
            mgr.currentScale.y = std::clamp(mgr.currentScale.y, 0.1f, 10.0f);
            mgr.targetScale   = mgr.currentScale;

            mgr.scaled     = false;
            renderDirty   = true;
        }
        lastDistance = distance;
        return 1;
    }

    // Volver a modo de un dedo (pan)
    lastDistance = 0.0f;
    isPinching   = false;
    renderDirty  = true;

    ImGuiIO& io = ImGui::GetIO();

    switch (actionM) {
        case AMOTION_EVENT_ACTION_DOWN:
            if (!IsInsideUI(x, y)) {
                isPanning = true;
                panLastX  = x;
                panLastY  = y;
            }
            break;

        case AMOTION_EVENT_ACTION_MOVE: {
            // Ignorar primer move tras un pinch cercano
            long long now = currentTimeMs();
            if (now - lastPinchTime < pinchTimeoutMs) {
                panLastX = x;
                panLastY = y;
                break;
            }
            if (isPanning) {
                float dx = x - panLastX;
                float dy = y - panLastY;

                // Convertir píxeles a NDC
                float ndc_dx = dx / engine->renderer.width  * 2.0f;
                float ndc_dy = -dy / engine->renderer.height * 2.0f;

                auto& mgr = engine->renderer.layerManager;
                float newX = mgr.currentOffset.x + ndc_dx;
                float newY = mgr.currentOffset.y + ndc_dy;

                // Constrain en [-0.6, +0.6]
                mgr.currentOffset.x = std::clamp(newX, -2.6f, 2.6f);
                mgr.currentOffset.y = std::clamp(newY, -2.6f, 2.6f);

                mgr.targetOffset = mgr.currentOffset;
                renderDirty      = true;

                panLastX = x;
                panLastY = y;

                drawFrame(app);
            }
            break;
        }

        case AMOTION_EVENT_ACTION_UP:
            isPanning = false;
            break;
    }

    return 1;
}


long long int currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void handleCmd(android_app* app, int32_t cmd) {
    Engine* engine = (Engine*)app->userData;

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != nullptr) {
                requestRuntimePermissions(app->activity);
                engine->window = app->window;
                engine->renderer.init(engine->window);
                //gFileManager = new FileManager(app->activity);
                //\uiMenu.SetLayerManager(&engine->renderer.layerManager);
                // 🔧 ImGui Initialization
                IMGUI_CHECKVERSION();
                ImGui::CreateContext();
                // En APP_CMD_INIT_WINDOW, después de CreateContext():
                ImGui::StyleColorsDark();

                ImGuiIO& io = ImGui::GetIO(); (void)io;
                ImGui_ImplAndroid_Init(app->window);
                ImGui_ImplOpenGL3_Init("#version 300 es");

                break;

            }
            break;

            break;
        case APP_CMD_TERM_WINDOW:
            engine->renderer.terminate();
            break;
        case APP_CMD_DESTROY:
            engine->renderer.deletePBO();
           // gFileManager = nullptr;
            LOGI("NDI: broadcast detenido en APP_CMD_DESTROY.");
            }
           engine->running = false;
    }
void StylusEvent(android_app* app, AInputEvent* event, float sx, float sy) {
    Engine* engine = (Engine*)app->userData;
    int32_t action = AMotionEvent_getAction(event);
    int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;

    auto& mgr = engine->renderer.layerManager;
    float pressure = AMotionEvent_getPressure(event, 0);

    // ¿dentro de la capa? pasamos crudo:
    if (!isInsideLayer(sx, sy, mgr)) return;

    // mapeo único pantalla→lienzo→world:
    glm::vec2 c = screenToCanvas(sx, sy, mgr);

    int W = mgr.layerWidth;
    int H = mgr.layerHeight;
    GLuint f = mgr.layers[mgr.targetLayer]->fbo;
    switch (actionMasked) {
        case AMOTION_EVENT_ACTION_DOWN:
            if (IsInsideUI(sx, sy)) {
                ImGui_ImplAndroid_HandleInputEvent(event);
                menuAction = true;
                renderDirty = true;
                drawFrame(app);
                return;
            }
            engine->renderer.brush.hasLast = false;
            engine->renderer.brush.DrawInterpolated(c.x, c.y, pressure, W, H, f);
            drawFrame(app);
            renderDirty = true;
            break;

        case AMOTION_EVENT_ACTION_MOVE:
            if (IsInsideUI(sx, sy)) {
                ImGui_ImplAndroid_HandleInputEvent(event);
                menuAction = true;
                renderDirty = true;
                drawFrame(app);
                return;
            }
            engine->renderer.brush.DrawInterpolated(c.x, c.y, pressure, W, H, f);
            drawFrame(app);
            renderDirty = true;
            break;

        case AMOTION_EVENT_ACTION_UP:
            if (IsInsideUI(sx, sy)) {
                ImGui_ImplAndroid_HandleInputEvent(event);
                menuAction = true;
                renderDirty = true;
                drawFrame(app);
                drawFrame(app);
                return;
            }
            engine->renderer.brush.hasLast = false; // reseteamos la línea
            renderDirty = false;
            break;

        }

    }



void android_main(struct android_app* app) {
    Engine engine;
    app->userData = &engine;
    app->onAppCmd    = handleCmd;
    app->onInputEvent= handleInput;

    // Espera a APP_CMD_INIT_WINDOW…
    // luego handleCmd inicializa ImGui y setea showStartup = true

    long long lastFrame = currentTimeMs();
    const long long frameMs = 1000/60;

    while (!app->destroyRequested) {
        // 1) Poll events (sin bucle aparte de startup)
        int events;
        android_poll_source* source;
        int timeout = renderDirty ? 0
                                  : std::max<long long>(0, frameMs - (currentTimeMs() - lastFrame));
        while (ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            timeout = 0;
        }

        // 2) Dibujar si toca
        long long now = currentTimeMs();
        if (engine.window && (renderDirty || isPinching || isPanning)
            && now - lastFrame >= frameMs) {
            drawFrame(app);
            lastFrame = now;
        }
    }

    engine.renderer.terminate();
}

// convierte (x,y) en píxeles pantalla → píxeles lienzo descontando escala/offset
void drawFrame(android_app* app) {
    Engine* engine = (Engine*)app->userData;

    // 1) Prepara ImGui — aquí SÍ llamas a los backends ya inicializados


    // 2) Ventana de selección de resolución (startup)
    if (!engine->menuInitialized) {
        engine->renderer.renderBackground();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Selecciona resolución del lienzo",
                     nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Elige un preset o Full pantalla:");
        ImGui::Combo("Preset", &preset,
                     "640×360\0"
                     "1280×720\0"
                     "1920×1080\0"
                     "2560×1440\0"
                     "3840×2160\0"
                     "Full\0");

        if (ImGui::Button("OK")) {
            if (preset < 5) {
                customW = (int)presets[preset].x;
                customH = (int)presets[preset].y;
            } else {
                customW = 0;  customH = 0; // full screen
            }
            engine->renderer.initLayer(customW, customH);
            renderWidth  = engine->renderer.width;
            renderHeight = engine->renderer.height;
            uiMenu.SetLayerManager(&engine->renderer.layerManager);
            engine->renderer.brush.setBrush(app->activity->assetManager, 0);
            engine->menuInitialized = true;
        }

        ImGui::End();
        ImGui::Render();
        // 3) Blending + UI + swap
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        engine->renderer.swapBuffers();
        return;

    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
        // 3) Tu UI normal
        engine->renderer.renderFrame();
        action = uiMenu.Show();
        handleMenuAction(app);
    ImGui::Render();

    // 3) Blending + UI + swap
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    engine->renderer.swapBuffers();

    // Reset estado dirty; lo volveremos a poner true si aún pinch o pan.
    renderDirty = false;


    // Sólo quitamos el flag cuando YA has terminado zoom Y panning.
    if (engine->renderer.layerManager.scaled
        && !isPinching
        && !isPanning) {
        renderDirty = false;
    }
}

// convierte (x,y) en píxeles pantalla → píxeles lienzo descontando escala/offset
static glm::vec2 screenToCanvas(float x, float y, const LayerManager& mgr) {
    // 1) escala pantalla→lienzo
    float sx = x * (float(mgr.layerWidth)  / float(renderWidth));
    float sy = y * (float(mgr.layerHeight) / float(renderHeight));

    // 2) flip Y
    float fy = float(mgr.layerHeight) - sy;

    // 3) pixel→NDC en lienzo
    glm::vec2 ndc = {
            2.0f * sx / float(mgr.layerWidth)  - 1.0f,
            2.0f * fy / float(mgr.layerHeight) - 1.0f
    };

    // 4) inversa offset/scale world
    glm::vec2 worldNdc = (ndc - mgr.currentOffset) / mgr.currentScale;

    // 5) NDC→px lienzo + reflip Y
    return {
            (worldNdc.x * 0.5f + 0.5f) * mgr.layerWidth,
            mgr.layerHeight - ((worldNdc.y * 0.5f + 0.5f) * mgr.layerHeight)
    };
}


static bool isInsideLayer(float x, float y, const LayerManager& mgr){
    // mismo pre‑escalado
    float scaleX = float(mgr.layerWidth ) / renderWidth;
    float scaleY = float(mgr.layerHeight) / renderHeight;
    float sx = x * scaleX;
    float sy = y * scaleY;
    float fy = float(mgr.layerHeight) - sy;

    glm::vec2 ndc = {
            2.0f * sx / float(mgr.layerWidth) - 1.0f,
            2.0f * fy / float(mgr.layerHeight) - 1.0f
    };
    glm::vec2 world = (ndc - mgr.currentOffset) / mgr.currentScale;
    return world.x >= -1.0f && world.x <= 1.0f
           && world.y >= -1.0f && world.y <= 1.0f;
}


void handleMenuAction(android_app* app) {
    if (!menuAction) return;
    Engine* engine = (Engine*)app->userData;

    if (action.sizeChanged) {
        engine->renderer.brush.size = action.newSize;
        action.sizeChanged = false;
       // LOGI("Brush NEW size: %f", action.newSize);
    }
    if(action.colorChanged){
        engine->renderer.brush.setBrushColor(action.newColor.x, action.newColor.y, action.newColor.z, action.newColor.w);
        action.colorChanged = false;
    }
    if(action.brushChanged){
        AAssetManager* assetManager = app->activity->assetManager;
        engine->renderer.brush.setBrush(assetManager, action.BrushIndex);
    }
    if(action.savePNG){
        SaveCompositeFBOAsPNG(engine->renderer.layerManager.compositeFBO, engine->renderer.width, engine->renderer.height);
        action.savePNG = false;
    }
    if(action.ndiActive){
        engine->renderer.enableNDI(true);
        LOGI("NDI: broadcast iniciado. (MAIN)");
    }
    else {
        engine->renderer.enableNDI(false);
    }
    engine->renderer.brush.setEraser(action.eraser);

    menuAction = false;
}
void renderOnlIMGUI(android_app* app){
    Engine* engine = (Engine*)app->userData;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

// 1) Dibuja escena
    engine->renderer.renderFrame();

// 2) Dibuja tu UI
    action = uiMenu.Show();
    handleMenuAction(app);
    ImGui::Render();
}

void SaveCompositeFBOAsPNG(GLuint fbo,int width, int height) {
    // Bind al FBO compuesto
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    // Reservar buffer RGBA
    size_t pixelCount = static_cast<size_t>(width) * height;
    uint8_t* rgbaBuffer = new uint8_t[pixelCount * 4];

    // Alinear lectura de píxeles
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    // Leer los píxeles (origen en la esquina inferior izquierda)
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaBuffer);

    // Convertir RGBA a RGB y flip verticalmente
    uint8_t* rgbBuffer = new uint8_t[pixelCount * 3];
    for (int y = 0; y < height; ++y) {
        int srcRow = y;
        int dstRow = (height - 1 - y);
        for (int x = 0; x < width; ++x) {
            size_t srcIdx = (static_cast<size_t>(srcRow) * width + x) * 4;
            size_t dstIdx = (static_cast<size_t>(dstRow) * width + x) * 3;
            rgbBuffer[dstIdx + 0] = rgbaBuffer[srcIdx + 0]; // R
            rgbBuffer[dstIdx + 1] = rgbaBuffer[srcIdx + 1]; // G
            rgbBuffer[dstIdx + 2] = rgbaBuffer[srcIdx + 2]; // B
        }
    }

    // Guardar como PNG con nombre fijo
    const char* filename = "/storage/emulated/0/Download/LiveSketck.png";    int result = stbi_write_png(filename, width, height, 3, rgbBuffer, width * 3);
    if (result) {
        //LOGI("Imagen guardada como %s", filename);
    } else {
        //LOGI("Error al guardar la imagen como PNG. Código de error: %d", result);
    }

    // Limpiar memoria
    delete[] rgbaBuffer;
    delete[] rgbBuffer;

    // Restaurar el FBO default (opcional)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}


static void requestRuntimePermissions(ANativeActivity* activity) {
    JNIEnv* env = nullptr;
    activity->vm->AttachCurrentThread(&env, nullptr);

    // Preparamos el array de permisos
    const char* permList[] = {
            "android.permission.INTERNET",
            "android.permission.ACCESS_WIFI_STATE",
            "android.permission.CHANGE_WIFI_MULTICAST_STATE"
    };
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray perms = env->NewObjectArray(
            3,
            stringClass,
            nullptr
    );
    for (int i = 0; i < 3; ++i) {
        env->SetObjectArrayElement(
                perms, i,
                env->NewStringUTF(permList[i])
        );
    }

    // Obtenemos el método requestPermissions
    jclass activityClass = env->GetObjectClass(activity->clazz);
    jmethodID reqPerm = env->GetMethodID(
            activityClass,
            "requestPermissions",
            "([Ljava/lang/String;I)V"
    );

    // Lo llamamos (1001 es un requestCode cualquiera)
    env->CallVoidMethod(activity->clazz, reqPerm, perms, 1001);

    env->DeleteLocalRef(perms);
    activity->vm->DetachCurrentThread();
}
