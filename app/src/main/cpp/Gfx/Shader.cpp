#include "Gfx/Shader.h"

#include <SDL3/SDL_log.h>

#include <string>

namespace gfx {

namespace {

constexpr const char* kHeader = "#version 300 es\nprecision highp float;\n";

GLuint compile(const char* name, GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    const char* sources[] = {kHeader, source};
    glShaderSource(shader, 2, sources, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(length > 1 ? static_cast<size_t>(length) : 1, '\0');
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Shader %s (%s): %s", name,
                     type == GL_VERTEX_SHADER ? "vértices" : "fragmentos", log.c_str());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

Program makeProgram(const char* name, const char* vertexSource, const char* fragmentSource) {
    const GLuint vs = compile(name, GL_VERTEX_SHADER, vertexSource);
    const GLuint fs = compile(name, GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return {};
    }

    Program program = Program::create();
    glAttachShader(program.id(), vs);
    glAttachShader(program.id(), fs);
    glLinkProgram(program.id());
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(program.id(), GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetProgramiv(program.id(), GL_INFO_LOG_LENGTH, &length);
        std::string log(length > 1 ? static_cast<size_t>(length) : 1, '\0');
        glGetProgramInfoLog(program.id(), static_cast<GLsizei>(log.size()), nullptr, log.data());
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Programa %s: %s", name, log.c_str());
        return {};
    }
    return program;
}

} // namespace gfx
