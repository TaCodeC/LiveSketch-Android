// Pruebas de escritorio de LiveSketch. Se compilan con -DLIVESKETCH_BUILD_TESTS=ON y se
// ejecutan con ctest o directamente (`livesketch_tests [filtro]`). Usan un contexto
// OpenGL ES 3.0 de verdad (Mesa en Linux); sin pantalla se usa el driver offscreen de SDL.
// Con Emscripten salen como livesketch_tests.html y corren en el navegador, con WebGL 2.

#include "Test.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
bool g_currentFailed = false;

} // namespace

namespace test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& message) {
    g_currentFailed = true;
    std::printf("    %s:%d: %s\n", std::filesystem::path(file).filename().c_str(), line, message.c_str());
}

bool initGL() {
#ifndef SDL_PLATFORM_EMSCRIPTEN
    if (!SDL_getenv("DISPLAY") && !SDL_getenv("WAYLAND_DISPLAY")) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    g_window = SDL_CreateWindow("livesketch_tests", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        return false;
    }
    g_context = SDL_GL_CreateContext(g_window);
    return g_context && SDL_GL_MakeCurrent(g_window, g_context);
}

void shutdownGL() {
    if (g_context) {
        SDL_GL_DestroyContext(g_context);
        g_context = nullptr;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
}

bool recreateGLContext() {
    SDL_GL_MakeCurrent(g_window, nullptr);
    SDL_GL_DestroyContext(g_context);
    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context || !SDL_GL_MakeCurrent(g_window, g_context)) {
        return false;
    }
    gfx::onContextRecreated();
    return true;
}

std::vector<uint8_t> readTarget(const gfx::RenderTarget& target) {
    std::vector<uint8_t> pixels(static_cast<size_t>(target.width) * static_cast<size_t>(target.height) * 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return pixels;
}

Pixel pixelAt(const std::vector<uint8_t>& rgba, int width, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

void fillRect(const gfx::RenderTarget& target, const IRect& rect, float r, float g, float b, float a) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glViewport(0, 0, target.width, target.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect.x0, rect.y0, rect.width(), rect.height());
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) {
        return 256;
    }
    int result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result = std::max(result, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return result;
}

std::string tempFolder(const char* name) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "livesketch_tests" / name;
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder.string() + "/";
}

std::ostream& operator<<(std::ostream& out, const Pixel& pixel) {
    return out << "(" << pixel[0] << "," << pixel[1] << "," << pixel[2] << "," << pixel[3] << ")";
}

} // namespace test

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    if (!test::initGL()) {
        std::printf("No se pudo crear un contexto OpenGL ES 3.0: %s\n", SDL_GetError());
        return 2;
    }
    std::printf("GL: %s · %s\n\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    int run = 0;
    int failed = 0;
    for (const test::Case& testCase : test::registry()) {
        if (filter && !std::strstr(testCase.name, filter)) {
            continue;
        }
        g_currentFailed = false;
        testCase.run();
        if (!gfx::checkErrors(testCase.name)) {
            test::fail(__FILE__, __LINE__, "quedaron errores de OpenGL");
        }
        ++run;
        failed += g_currentFailed ? 1 : 0;
        std::printf("%s %s\n", g_currentFailed ? "FALLÓ" : "ok   ", testCase.name);
        std::fflush(stdout);
    }
    std::printf("\n%d pruebas, %d fallaron\n", run, failed);

    test::shutdownGL();
    return failed == 0 && run > 0 ? 0 : 1;
}
