#pragma once

#include "Gfx/GL.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace gfx {

// Generación del contexto GL. Sube cuando SDL tiene que crear un contexto nuevo porque
// el driver perdió el anterior (SDL_EVENT_RENDER_DEVICE_RESET). Los objetos creados en
// una generación anterior ya no existen: se olvidan sin llamar a glDelete*, porque sus
// nombres podrían coincidir con objetos del contexto nuevo.
uint32_t contextGeneration();
void onContextRecreated();

// Dueño único (RAII) de un objeto GL. Movible, no copiable.
template <typename Traits>
class Object {
public:
    Object() = default;
    ~Object() { reset(); }

    Object(Object&& other) noexcept
        : m_id(std::exchange(other.m_id, 0u)), m_generation(other.m_generation) {}

    Object& operator=(Object&& other) noexcept {
        if (this != &other) {
            reset();
            m_id = std::exchange(other.m_id, 0u);
            m_generation = other.m_generation;
        }
        return *this;
    }

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    static Object create() {
        Object object;
        object.m_id = Traits::create();
        object.m_generation = contextGeneration();
        return object;
    }

    GLuint id() const { return m_id; }
    explicit operator bool() const { return m_id != 0; }

    void reset() {
        if (m_id != 0 && m_generation == contextGeneration()) {
            Traits::destroy(m_id);
        }
        m_id = 0;
    }

private:
    GLuint m_id = 0;
    uint32_t m_generation = 0;
};

struct TextureTraits {
    static GLuint create();
    static void destroy(GLuint id);
};
struct FramebufferTraits {
    static GLuint create();
    static void destroy(GLuint id);
};
struct BufferTraits {
    static GLuint create();
    static void destroy(GLuint id);
};
struct VertexArrayTraits {
    static GLuint create();
    static void destroy(GLuint id);
};
struct ProgramTraits {
    static GLuint create();
    static void destroy(GLuint id);
};

using Texture = Object<TextureTraits>;
using Framebuffer = Object<FramebufferTraits>;
using Buffer = Object<BufferTraits>;
using VertexArray = Object<VertexArrayTraits>;
using Program = Object<ProgramTraits>;

// Textura RGBA8 con premultiplicado de alfa + FBO que la usa como color. Las máscaras
// (la selección) son de un solo canal, R8.
// Convención de todo el lienzo: la fila 0 de la textura es la parte de ARRIBA de la
// imagen, así que las lecturas con glReadPixels ya salen de arriba abajo (PNG, NDI).
struct RenderTarget {
    enum class Format { Rgba8, R8 };

    Texture texture;
    Framebuffer fbo;
    int width = 0;
    int height = 0;
    Format format = Format::Rgba8;

    // `pixels` (opcional): RGBA8 premultiplicado (o un byte por píxel con R8), filas de
    // arriba abajo, sin relleno.
    bool create(int w, int h, const void* pixels = nullptr, Format fmt = Format::Rgba8);
    void destroy();
    explicit operator bool() const { return static_cast<bool>(fbo); }
    // Bytes que ocupa en la GPU.
    size_t bytes() const {
        return static_cast<size_t>(width) * static_cast<size_t>(height) * (format == Format::R8 ? 1u : 4u);
    }
};

// Descarta los errores GL pendientes.
void clearErrors();
// Registra en el log los errores GL pendientes. Devuelve false si había alguno.
bool checkErrors(const char* where);

} // namespace gfx
