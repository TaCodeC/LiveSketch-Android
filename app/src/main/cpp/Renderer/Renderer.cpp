#include "Renderer.h"
#include <android/log.h>

#define LOG_TAG "Renderer"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

Renderer::Renderer() {}

Renderer::~Renderer() {
    terminate();
}

bool Renderer::init(ANativeWindow* win){
    window = win;

    const EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE
    };

    const EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
    };

    EGLint numConfigs;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        LOGE("Failed to get or initialize EGL display");
        return false;
    }

    eglChooseConfig(display, configAttribs, &config, 1, &numConfigs);
    surface = eglCreateWindowSurface(display, config, window, nullptr);
    context = eglCreateContext(display, config, nullptr, contextAttribs);

    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT) {
        LOGE("Failed to create EGL surface or context");
        return false;
    }

    eglMakeCurrent(display, surface, surface, context);
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    createShaders();
    initBuffers();
    brush.Init();
    LOGI("Renderer initialized. Width: %d, Height: %d", width, height);
    return true;
}

void Renderer::terminate() {
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }

    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
    window = nullptr;
}
void Renderer::renderBackground(){
    if (display == EGL_NO_DISPLAY) return;
    glViewport(0, 0, width, height);
    glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(TexturesProgram);
    glBindVertexArray(vaoScreen[0]);
    glDrawArrays(GL_TRIANGLES, 0, 6);}

void Renderer::renderFrame() {
    if (display == EGL_NO_DISPLAY) return;
    glViewport(0, 0, width, height);
    glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(TexturesProgram);
    glBindVertexArray(vaoScreen[0]);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    // Renderizar las capas
    layerManager.RenderToCompositeFBO();
    layerManager.RenderLayersOnScreen();
    if(ndiRunning) {
        NotifyNDIDirty();
    }

}
void Renderer::swapBuffers() {
        eglSwapBuffers(display, surface);
}

void Renderer::createShaders() {
    char* vertexScreenShaderSource = R"( #version 300 es
        layout(location = 0) in vec2 aPos;
        out vec2 vPos;
        void main() {
            vPos = aPos;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )";

    char* fragmentScreenShaderSource = R"(#version 300 es
    precision highp float;
    // Parámetros internos (edítalos aquí)
    const vec3   BG_COLOR   = vec3(0.05, 0.05, 0.05); // Color de fondo (RGB 0..1)
    const vec3   GRID_COLOR = vec3(0.09, 0.09, 0.09); // Color de la cuadrícula
    const float  CELL_SIZE  = 35.0;                // Tamaño de cada celda en píxeles
    const float  LINE_WIDTH = 1.0;                 // Grosor de las líneas en píxeles

    out vec4 fragColor;

    void main() {
        // Coordenadas de este fragmento en píxeles
        vec2 uv_px = gl_FragCoord.xy;

        // Posición dentro de la celda
        vec2 cellPos = mod(uv_px, CELL_SIZE);

        // Detectar cercanía a línea vertical u horizontal
        float lineX = step(cellPos.x, LINE_WIDTH) + step(CELL_SIZE - cellPos.x, LINE_WIDTH);
        float lineY = step(cellPos.y, LINE_WIDTH) + step(CELL_SIZE - cellPos.y, LINE_WIDTH);

        // Combinamos para saber si es línea
        float isLine = clamp(lineX + lineY, 0.0, 1.0);

        // Mezclamos color de fondo y color de cuadrícula
        vec3 color = mix(BG_COLOR, GRID_COLOR, isLine);

        fragColor = vec4(color, 1.0);
    }


    )";

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexScreenShaderSource);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentScreenShaderSource);
    TexturesProgram = glCreateProgram();
    if (!TexturesProgram) {
        LOGE("Error creating shader program");
        return;
    }
    glAttachShader(TexturesProgram, vertexShader);
    glAttachShader(TexturesProgram, fragmentShader);
    glLinkProgram(TexturesProgram);
}

GLuint Renderer::CompileShader(GLuint shaderType, const char *source) {
    GLuint shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        LOGE("Error compiling shader: %s", infoLog);
    }
    return shader;
}

void Renderer::initBuffers() {
    float quadVertices[] = {
            -1.0f, -1.0f,
            1.0f, -1.0f,
            1.0f,  1.0f,
            -1.0f, -1.0f,
            1.0f,  1.0f,
            -1.0f,  1.0f
    };

    glGenVertexArrays(1, &vaoScreen[0]);
    glBindVertexArray(vaoScreen[0]);

    glGenBuffers(1, vboScreen);
    glBindBuffer(GL_ARRAY_BUFFER, vboScreen[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}


void Renderer::initPBO() {
    deletePBO();
    bufSize = size_t(width) * size_t(height) * 4;
    glGenBuffers(1, &pboId);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pboId);
    glBufferData(GL_PIXEL_PACK_BUFFER, bufSize, nullptr, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
void Renderer::deletePBO() {
    if (pboId) {
        glDeleteBuffers(1, &pboId);
        pboId = 0;
        bufSize = 0;
        //LOGI("PBO deleted");
    }
}
void Renderer::enableNDI(bool enable) {
    if(!ndiSender){ndiSender = new NDISender( "LiveSketch"); }
    if (enable && ndiSender && !ndiRunning) {
        // 1) Inicializa el sender (width, height, topDown)
        if (!ndiSender->initialize(layerManager.layerWidth, layerManager.layerHeight, /*topDown=*/ false)) {
            LOGE("NDISender::initialize FAILED");
            delete ndiSender;
            ndiSender = nullptr;
            return;
        }
        // 2) Prepara PBO y lanza hilo
        initPBO();
        StartNDIThread(ndiSender, layerManager.layerWidth, layerManager.layerHeight,
                       display, context, config,
                       layerManager.compositeTex, pboId);
        ndiRunning = true;
        LOGI("NDI started");
    } else if (!enable && ndiRunning) {
        deletePBO();
        StopNDIThread();
        ndiRunning = false;
        if (ndiSender) {
            delete ndiSender;
            ndiSender = nullptr;
        }
        LOGI("NDI stopped");
    }
}
void Renderer::initLayer(int lwidth, int lheight) {
    if (lwidth != 0 && lheight != 0) {
        layerWidth = lwidth;
        layerHeight = lheight;
    } else {
        layerWidth = width;
        layerHeight = height;
    }
    layerManager.Init(layerWidth, layerHeight, width, height);
}