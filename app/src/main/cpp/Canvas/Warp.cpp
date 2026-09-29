#include "Canvas/Warp.h"

#include "Gfx/Shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace warp {

namespace {

float cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

} // namespace

bool homography(const IRect& from, const glm::vec2 to[4], glm::mat3& out) {
    if (from.empty()) {
        return false;
    }
    // Convexo y sin esquinas en línea: los cuatro giros van hacia el mismo lado.
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 a = to[(i + 1) % 4] - to[i];
        const glm::vec2 b = to[(i + 2) % 4] - to[(i + 1) % 4];
        const float c = cross(a, b);
        if (!std::isfinite(c) || std::fabs(c) < 1e-6f) {
            return false;
        }
        if (sign == 0.0f) {
            sign = c;
        } else if ((c > 0.0f) != (sign > 0.0f)) {
            return false;
        }
    }

    // Del cuadrado unidad al cuadrilátero (Heckbert, "Fundamentals of Texture Mapping").
    const glm::vec2& q0 = to[0];
    const glm::vec2& q1 = to[1];
    const glm::vec2& q2 = to[2];
    const glm::vec2& q3 = to[3];
    const double sx = static_cast<double>(q0.x) - q1.x + q2.x - q3.x;
    const double sy = static_cast<double>(q0.y) - q1.y + q2.y - q3.y;
    double g = 0.0;
    double h = 0.0;
    if (std::fabs(sx) > 1e-9 || std::fabs(sy) > 1e-9) {
        const double dx1 = static_cast<double>(q1.x) - q2.x;
        const double dx2 = static_cast<double>(q3.x) - q2.x;
        const double dy1 = static_cast<double>(q1.y) - q2.y;
        const double dy2 = static_cast<double>(q3.y) - q2.y;
        const double det = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(det) < 1e-12) {
            return false;
        }
        g = (sx * dy2 - dx2 * sy) / det;
        h = (dx1 * sy - sx * dy1) / det;
    }
    const double a = q1.x - q0.x + g * q1.x;
    const double b = q3.x - q0.x + h * q3.x;
    const double c = q0.x;
    const double d = q1.y - q0.y + g * q1.y;
    const double e = q3.y - q0.y + h * q3.y;
    const double f = q0.y;

    // Del rectángulo al cuadrado unidad: u = (x − x0) / ancho, v = (y − y0) / alto.
    const double w = from.width();
    const double hh = from.height();
    const double x0 = from.x0;
    const double y0 = from.y0;
    // M · N, con M = [a b c; d e f; g h 1] y N = [1/w 0 −x0/w; 0 1/hh −y0/hh; 0 0 1].
    const double m[3][3] = {
        {a / w, b / hh, c - a * x0 / w - b * y0 / hh},
        {d / w, e / hh, f - d * x0 / w - e * y0 / hh},
        {g / w, h / hh, 1.0 - g * x0 / w - h * y0 / hh},
    };
    // glm guarda las matrices por columnas: out[columna][fila].
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out[column][row] = static_cast<float>(m[row][column]);
        }
    }
    return true;
}

glm::vec2 apply(const glm::mat3& h, glm::vec2 p, bool* valid) {
    const glm::vec3 q = h * glm::vec3(p, 1.0f);
    const bool ok = q.z > 1e-6f;
    if (valid) {
        *valid = ok;
    }
    return ok ? glm::vec2(q.x / q.z, q.y / q.z) : glm::vec2(0.0f);
}

IRect imageBounds(const glm::mat3& h, const IRect& rect, const IRect& clip) {
    if (rect.empty()) {
        return {};
    }
    const glm::vec2 corners[4] = {
        {static_cast<float>(rect.x0), static_cast<float>(rect.y0)},
        {static_cast<float>(rect.x1), static_cast<float>(rect.y0)},
        {static_cast<float>(rect.x1), static_cast<float>(rect.y1)},
        {static_cast<float>(rect.x0), static_cast<float>(rect.y1)},
    };
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (int i = 0; i < 4; ++i) {
        bool valid = false;
        const glm::vec2 p = apply(h, corners[i], &valid);
        if (!valid || !std::isfinite(p.x) || !std::isfinite(p.y)) {
            return clip;
        }
        minX = i == 0 ? p.x : std::min(minX, p.x);
        minY = i == 0 ? p.y : std::min(minY, p.y);
        maxX = i == 0 ? p.x : std::max(maxX, p.x);
        maxY = i == 0 ? p.y : std::max(maxY, p.y);
    }
    // Sin desbordar un entero con cajas disparatadas.
    constexpr float kLimit = 1.0e7f;
    const IRect box{static_cast<int>(std::floor(std::max(minX, -kLimit))) - 1,
                    static_cast<int>(std::floor(std::max(minY, -kLimit))) - 1,
                    static_cast<int>(std::ceil(std::min(maxX, kLimit))) + 1,
                    static_cast<int>(std::ceil(std::min(maxY, kLimit))) + 1};
    return box.intersected(clip);
}

} // namespace warp

namespace {

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Cada píxel del destino busca su punto en la fuente. La lectura va fuera de cualquier
// condición: las derivadas que eligen el nivel de mipmap tienen que ser las de todos
// los píxeles.
constexpr const char* kFragment = R"(
uniform sampler2D uSource;
uniform mat3 uToSource;    // píxel del destino → píxel de la fuente
uniform vec2 uSourceSize;
uniform int uRed;          // 1: la fuente es una máscara (el rojo en los cuatro canales)
out vec4 fragColor;
void main() {
    vec3 q = uToSource * vec3(gl_FragCoord.xy, 1.0);
    vec2 uv = q.xy / max(q.z, 1e-6) / uSourceSize;
    vec4 c = texture(uSource, uv);
    bool inside = q.z > 1e-6 && uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
    if (!inside) {
        c = vec4(0.0);
    }
    fragColor = uRed == 1 ? vec4(c.r) : c;
}
)";

} // namespace

bool Warp::init() {
    destroy();
    m_program = gfx::makeProgram("warp", kVertex, kFragment);
    if (!m_program) {
        return false;
    }
    const GLuint id = m_program.id();
    m_uSource = glGetUniformLocation(id, "uSource");
    m_uToSource = glGetUniformLocation(id, "uToSource");
    m_uSourceSize = glGetUniformLocation(id, "uSourceSize");
    m_uRed = glGetUniformLocation(id, "uRed");

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

void Warp::destroy() {
    m_vbo.reset();
    m_vao.reset();
    m_program.reset();
}

void Warp::draw(GLuint target, int targetWidth, int targetHeight, GLuint source, int sourceWidth, int sourceHeight,
                const glm::mat3& toSource, const IRect& rect, Mode mode, bool nearest) {
    const IRect area = rect.intersected(IRect::ofSize(targetWidth, targetHeight));
    if (area.empty() || !m_program || source == 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, targetWidth, targetHeight);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    if (mode == Mode::Over) {
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    GLint minFilter = GL_LINEAR;
    GLint magFilter = GL_LINEAR;
    if (nearest) {
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glUseProgram(m_program.id());
    glUniform1i(m_uSource, 0);
    glUniformMatrix3fv(m_uToSource, 1, GL_FALSE, glm::value_ptr(toSource));
    glUniform2f(m_uSourceSize, static_cast<float>(std::max(sourceWidth, 1)), static_cast<float>(std::max(sourceHeight, 1)));
    glUniform1i(m_uRed, mode == Mode::Replace ? 1 : 0);
    glBindVertexArray(m_vao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    if (nearest) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
