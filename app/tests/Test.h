#pragma once

// Mini marco de pruebas: sin dependencias, para compilar igual en cualquier máquina.
//
//   TEST_CASE(nombre) { CHECK(a == b); CHECK_EQ(a, b); CHECK_NEAR(a, b, tol); REQUIRE(ok); }
//
// CHECK sigue con la prueba aunque falle; REQUIRE sale de la prueba.

#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*run)();
};

std::vector<Case>& registry();
void fail(const char* file, int line, const std::string& message);

struct Register {
    Register(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

template <typename A, typename B>
bool checkEqual(const A& a, const B& b, const char* expression, const char* file, int line) {
    if (a == b) {
        return true;
    }
    std::ostringstream message;
    message << expression << "  (" << a << " != " << b << ")";
    fail(file, line, message.str());
    return false;
}

template <typename A, typename B, typename T>
bool checkNear(const A& a, const B& b, const T& tolerance, const char* expression, const char* file, int line) {
    const double difference = static_cast<double>(a) - static_cast<double>(b);
    if (difference <= static_cast<double>(tolerance) && -difference <= static_cast<double>(tolerance)) {
        return true;
    }
    std::ostringstream message;
    message << expression << "  (" << a << " vs " << b << ", tolerancia " << tolerance << ")";
    fail(file, line, message.str());
    return false;
}

// --- OpenGL (TestMain.cpp) ---
// Contexto OpenGL ES 3.0 en una ventana oculta, creado antes de las pruebas.
bool initGL();
void shutdownGL();
// Simula que el driver pierde el contexto: destruye el actual y crea otro, como hace SDL
// en Android antes de mandar SDL_EVENT_RENDER_DEVICE_RESET. También avisa a gfx.
bool recreateGLContext();

// --- Utilidades de píxeles ---
struct Pixel {   // RGBA 0..255
    std::array<int, 4> channels;
    int operator[](size_t i) const { return channels[i]; }
    bool operator==(const Pixel&) const = default;
};
std::ostream& operator<<(std::ostream& out, const Pixel& pixel);

// Lee un FBO completo (RGBA8, filas de arriba abajo).
std::vector<uint8_t> readTarget(const gfx::RenderTarget& target);
Pixel pixelAt(const std::vector<uint8_t>& rgba, int width, int x, int y);
// Rellena `rect` de un FBO con un color premultiplicado (0..1).
void fillRect(const gfx::RenderTarget& target, const IRect& rect, float r, float g, float b, float a);
// Mayor diferencia entre dos imágenes del mismo tamaño, canal a canal.
int maxDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
// Carpeta temporal vacía para la prueba, terminada en '/'.
std::string tempFolder(const char* name);

} // namespace test

#define TEST_CASE(name)                                            \
    static void name();                                            \
    static const test::Register name##_registration(#name, name); \
    static void name()

#define CHECK(condition)                                         \
    do {                                                         \
        if (!(condition)) {                                      \
            test::fail(__FILE__, __LINE__, #condition);          \
        }                                                        \
    } while (0)

#define REQUIRE(condition)                                       \
    do {                                                         \
        if (!(condition)) {                                      \
            test::fail(__FILE__, __LINE__, "REQUIRE " #condition); \
            return;                                              \
        }                                                        \
    } while (0)

#define CHECK_EQ(a, b) test::checkEqual((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tolerance) test::checkNear((a), (b), (tolerance), #a " ≈ " #b, __FILE__, __LINE__)
