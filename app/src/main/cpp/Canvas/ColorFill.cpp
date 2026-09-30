#include "Canvas/ColorFill.h"

#include "Gfx/Shader.h"

#include <SDL3/SDL_log.h>

#include <algorithm>

namespace {

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Con la convención de fila 0 = arriba, gl_FragCoord son píxeles del lienzo. Los niveles
// se leen con toda la precisión: un sampler lowp podría meter un nivel en el vecino.
//
// Dentro de la zona el color sustituye a lo que hubiera. Alrededor, un píxel (solo los
// cuatro vecinos: con las diagonales cruzaría una línea de un píxel en diagonal) recibe
// el color por detrás: el borde suavizado de una línea queda encima del relleno, sin la
// franja clara que dejaría entre los dos.
constexpr const char* kFragment = R"(
precision highp int;
precision highp sampler2D;
uniform sampler2D uLayer;
uniform sampler2D uLevels;
uniform sampler2D uMask;
uniform int uMaskOn;
uniform float uCutoff;     // nivel de corte + 0,5
uniform vec3 uColor;
uniform int uAlphaLock;
out vec4 fragColor;

float inside(ivec2 p) {
    return texelFetch(uLevels, p, 0).r * 255.0 < uCutoff ? 1.0 : 0.0;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = textureSize(uLevels, 0) - 1;
    vec4 dst = texelFetch(uLayer, p, 0);
    float core = inside(p);
    float edge = core;
    if (p.x > 0) {
        edge = max(edge, inside(p - ivec2(1, 0)));
    }
    if (p.x < last.x) {
        edge = max(edge, inside(p + ivec2(1, 0)));
    }
    if (p.y > 0) {
        edge = max(edge, inside(p - ivec2(0, 1)));
    }
    if (p.y < last.y) {
        edge = max(edge, inside(p + ivec2(0, 1)));
    }
    float m = uMaskOn == 1 ? texelFetch(uMask, p, 0).r : 1.0;
    vec4 color = vec4(uColor, 1.0);
    if (uAlphaLock == 1) {
        // Solo el color de lo pintado, con su alfa.
        fragColor = mix(dst, vec4(uColor * dst.a, dst.a), core * m);
    } else {
        vec4 behind = dst + color * (1.0 - dst.a);
        fragColor = mix(mix(dst, behind, edge * m), color, core * m);
    }
}
)";

} // namespace

bool ColorFill::init() {
    destroy();
    m_program = gfx::makeProgram("color fill", kVertex, kFragment);
    if (!m_program) {
        return false;
    }
    const GLuint id = m_program.id();
    m_uLayer = glGetUniformLocation(id, "uLayer");
    m_uLevels = glGetUniformLocation(id, "uLevels");
    m_uMask = glGetUniformLocation(id, "uMask");
    m_uMaskOn = glGetUniformLocation(id, "uMaskOn");
    m_uCutoff = glGetUniformLocation(id, "uCutoff");
    m_uColor = glGetUniformLocation(id, "uColor");
    m_uAlphaLock = glGetUniformLocation(id, "uAlphaLock");

    const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    m_vao = gfx::VertexArray::create();
    m_vbo = gfx::Buffer::create();
    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo.id());
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void ColorFill::destroy() {
    drop();
    m_vbo.reset();
    m_vao.reset();
    m_program.reset();
}

bool ColorFill::upload(const std::vector<uint8_t>& levels, int width, int height) {
    if (width <= 0 || height <= 0 || levels.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
        return false;
    }
    if (!m_levels) {
        m_levels = gfx::Texture::create();
    }
    gfx::clearErrors();
    glBindTexture(GL_TEXTURE_2D, m_levels.id());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, levels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!gfx::checkErrors("ColorFill::upload")) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Sin memoria para el relleno");
        drop();
        return false;
    }
    m_width = width;
    m_height = height;
    return true;
}

void ColorFill::drop() {
    m_levels.reset();
    m_width = 0;
    m_height = 0;
}

void ColorFill::draw(GLuint target, int width, int height, GLuint layer, const IRect& rect, int cutoff,
                     const float rgb[3], bool alphaLock, GLuint mask) {
    const IRect area = rect.intersected(IRect::ofSize(std::min(width, m_width), std::min(height, m_height)));
    if (area.empty() || !m_program || !m_levels || layer == 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    glDisable(GL_BLEND);

    // Todas las unidades llevan una textura válida (WebGL avisa si no, aunque no se lea).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, layer);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_levels.id());
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mask != 0 ? mask : m_levels.id());
    glUseProgram(m_program.id());
    glUniform1i(m_uLayer, 0);
    glUniform1i(m_uLevels, 1);
    glUniform1i(m_uMask, 2);
    glUniform1i(m_uMaskOn, mask != 0 ? 1 : 0);
    glUniform1f(m_uCutoff, static_cast<float>(cutoff) + 0.5f);
    glUniform3f(m_uColor, std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f),
                std::clamp(rgb[2], 0.0f, 1.0f));
    glUniform1i(m_uAlphaLock, alphaLock ? 1 : 0);
    glBindVertexArray(m_vao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    for (GLenum unit : {GL_TEXTURE2, GL_TEXTURE1, GL_TEXTURE0}) {
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
