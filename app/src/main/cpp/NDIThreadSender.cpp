// NDIThreadSender.cpp
#include "NDIThreadSender.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstring>
#include <android/log.h>

#define TAG "NDIThread"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace {
    std::thread g_thread;
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_dirty{false};
    NDISender* g_sender = nullptr;
    int g_width = 0, g_height = 0;
    uint8_t* g_buffer = nullptr;
    EGLDisplay g_display = EGL_NO_DISPLAY;
    GLuint g_compositeTex = 0;
    GLuint g_pbo = 0;
}

void StartNDIThread(NDISender* sender,
                    int width, int height,
                    EGLDisplay display,
                    EGLContext sharedContext,
                    EGLConfig config,
                    GLuint compositeTex,
                    GLuint pboId) {
    if (g_running) return;
    g_sender = sender;
    g_width = width;
    g_height = height;
    g_compositeTex = compositeTex;
    g_pbo = pboId;
    g_display = display;

    size_t bufSize = size_t(width) * size_t(height) * 4;
    g_buffer = new uint8_t[bufSize];
    g_running = true;
    g_dirty = true;

    g_thread = std::thread([=]() {
        // Crear contexto compartido en este hilo
        const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        EGLint pbufAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        EGLSurface pbuffer = eglCreatePbufferSurface(display, config, pbufAttribs);
        EGLContext ctx = eglCreateContext(display, config, sharedContext, contextAttribs);
        if (pbuffer == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
            LOGI("Failed to create PBuffer surface or context");
            g_running = false;
            return;
        }
        if (!eglMakeCurrent(display, pbuffer, pbuffer, ctx)) {
            LOGI("eglMakeCurrent failed: 0x%x", eglGetError());
            g_running = false;
            eglDestroySurface(display, pbuffer);
            eglDestroyContext(display, ctx);
            return;
        }

        // Crear FBO local apuntando a la textura compartida
        GLuint threadFBO = 0;
        glGenFramebuffers(1, &threadFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, threadFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               compositeTex,
                               0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        LOGI("Thread-local FBO status: 0x%04X", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        constexpr auto delay = std::chrono::milliseconds(99);
        while (g_running) {
            if (g_dirty.exchange(false)) {
                // Capturar desde el FBO local
                glBindFramebuffer(GL_FRAMEBUFFER, threadFBO);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pboId);
                glReadPixels(0, 0, width, height,
                             GL_RGBA, GL_UNSIGNED_BYTE, 0);
                void* ptr = glMapBufferRange(GL_PIXEL_PACK_BUFFER,
                                             0, bufSize,
                                             GL_MAP_READ_BIT);
                if (ptr) {
                    memcpy(g_buffer, ptr, bufSize);
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                    sender->sendFrame(g_buffer);

                }
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            // Enviar frame NDI
            std::this_thread::sleep_for(delay);
        }

        // Limpieza
        glDeleteFramebuffers(1, &threadFBO);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, pbuffer);
        eglDestroyContext(display, ctx);
    });
}

void NotifyNDIDirty() {
    if (g_running) g_dirty = true;
}

void StopNDIThread() {
    if (!g_running) return;
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
    delete[] g_buffer;
    g_buffer = nullptr;
    g_display = EGL_NO_DISPLAY;
}
