#ifndef RENDERER_H
#define RENDERER_H


#pragma once
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android_native_app_glue.h>
#include <vector>
#include "../layers/LayerManager.h"
#include "../Brush/Brush.h"
#include "../NDI/NDISender.h"
#include "../NDIThreadSender.h"

class Renderer {
public:
    Renderer();
    ~Renderer();
    void createShaders();
    void initPBO();
    void deletePBO();
    bool init(ANativeWindow* window);
    void initLayer(int lwidth, int lheight);
    void terminate();
    void renderFrame();
    void initBuffers();
    void swapBuffers();
    void enableNDI(bool enable);
    void renderBackground();
    bool layerInit = false;
    LayerManager layerManager;
    Brush brush;
    int width = 0;
    int height = 0;
    int layerWidth = 0;
    int layerHeight = 0;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig  config;
    GLuint pboId = 0;
    size_t bufSize;
    EGLContext context = EGL_NO_CONTEXT;
private:
    GLuint CompileShader(GLuint shader, const char* source);
    GLuint TexturesProgram;
    GLuint ScreenID;

    GLuint vaoScreen[1];
    GLuint vboScreen[1];
    ANativeWindow* window = nullptr;

    NDISender* ndiSender = nullptr;
    bool       ndiRunning = false;
};

#endif // RENDERER_H
