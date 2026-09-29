#include "Gfx/GLObjects.h"

#include <SDL3/SDL_log.h>

namespace gfx {

namespace {
uint32_t g_contextGeneration = 1;
}

uint32_t contextGeneration() { return g_contextGeneration; }
void onContextRecreated() { ++g_contextGeneration; }

GLuint TextureTraits::create() { GLuint id = 0; glGenTextures(1, &id); return id; }
void TextureTraits::destroy(GLuint id) { glDeleteTextures(1, &id); }

GLuint FramebufferTraits::create() { GLuint id = 0; glGenFramebuffers(1, &id); return id; }
void FramebufferTraits::destroy(GLuint id) { glDeleteFramebuffers(1, &id); }

GLuint BufferTraits::create() { GLuint id = 0; glGenBuffers(1, &id); return id; }
void BufferTraits::destroy(GLuint id) { glDeleteBuffers(1, &id); }

GLuint VertexArrayTraits::create() { GLuint id = 0; glGenVertexArrays(1, &id); return id; }
void VertexArrayTraits::destroy(GLuint id) { glDeleteVertexArrays(1, &id); }

GLuint ProgramTraits::create() { return glCreateProgram(); }
void ProgramTraits::destroy(GLuint id) { glDeleteProgram(id); }

bool RenderTarget::create(int w, int h, const void* pixels, Format fmt) {
    destroy();
    if (w <= 0 || h <= 0) {
        return false;
    }

    texture = Texture::create();
    glBindTexture(GL_TEXTURE_2D, texture.id());
    if (fmt == Format::R8) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    } else {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    fbo = Framebuffer::create();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.id());
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture.id(), 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE && pixels == nullptr) {
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "FBO %dx%d incompleto (0x%04X)", w, h, status);
        destroy();
        return false;
    }

    width = w;
    height = h;
    format = fmt;
    return true;
}

void RenderTarget::destroy() {
    fbo.reset();
    texture.reset();
    width = 0;
    height = 0;
    format = Format::Rgba8;
}

void clearErrors() {
    for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; ++i) {
    }
}

bool checkErrors(const char* where) {
    bool ok = true;
    for (int i = 0; i < 32; ++i) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            break;
        }
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Error GL 0x%04X en %s", error, where);
        ok = false;
    }
    return ok;
}

} // namespace gfx
