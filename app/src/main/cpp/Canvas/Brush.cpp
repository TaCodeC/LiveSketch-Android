#include "Canvas/Brush.h"

#include "Gfx/Shader.h"
#include "IO/Assets.h"
#include "ThirdParty/stb_image.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

// Distancia entre dabs en píxeles del lienzo (la misma que la versión anterior).
constexpr float kSpacing = 0.8f;

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aCorner;   // esquina del quad, de (-1,-1) a (1,1)
layout(location = 1) in vec4 aDab;      // por instancia: centro x, centro y, radio, alfa
uniform vec2 uCanvasSize;
out vec2 vUV;
out float vAlpha;
void main() {
    vec2 p = aDab.xy + aCorner * aDab.z;
    gl_Position = vec4(p / uCanvasSize * 2.0 - 1.0, 0.0, 1.0);
    // v invertida: conserva la orientación con la que se veían los pinceles antes.
    vUV = vec2(aCorner.x, -aCorner.y) * 0.5 + 0.5;
    vAlpha = aDab.w;
}
)";

constexpr const char* kFragment = R"(
in vec2 vUV;
in float vAlpha;
uniform sampler2D uMask;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    float a = vAlpha * texture(uMask, vUV).r;
    fragColor = vec4(uColor * a, a);
}
)";

} // namespace

bool Brush::init() {
    destroy();

    m_program = gfx::makeProgram("brush", kVertex, kFragment);
    if (!m_program) {
        return false;
    }
    m_uCanvasSize = glGetUniformLocation(m_program.id(), "uCanvasSize");
    m_uColor = glGetUniformLocation(m_program.id(), "uColor");
    m_uMask = glGetUniformLocation(m_program.id(), "uMask");

    const float corners[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    m_vao = gfx::VertexArray::create();
    m_cornerVbo = gfx::Buffer::create();
    m_instanceVbo = gfx::Buffer::create();

    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_cornerVbo.id());
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo.id());
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribDivisor(1, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void Brush::destroy() {
    discardPending();
    for (auto& mask : m_masks) {
        mask.reset();
    }
    m_type = -1;
    m_instanceVbo.reset();
    m_cornerVbo.reset();
    m_vao.reset();
    m_program.reset();
}

bool Brush::setType(int type) {
    if (type < 0 || type >= BrushSettings::kTypeCount) {
        return false;
    }
    if (!m_masks[static_cast<size_t>(type)]) {
        const std::string name = "brush" + std::to_string(type) + ".png";
        const std::vector<uint8_t> file = io::loadAsset(name.c_str());
        if (file.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo leer el pincel %s", name.c_str());
            return false;
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height,
                                                &channels, STBI_rgb_alpha);
        if (!pixels) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PNG inválido %s: %s", name.c_str(), stbi_failure_reason());
            return false;
        }

        // Cobertura del pincel = rojo × alfa (lo que la versión anterior leía de su
        // textura premultiplicada). Se guarda en un solo canal.
        std::vector<uint8_t> mask(static_cast<size_t>(width) * static_cast<size_t>(height));
        for (size_t i = 0; i < mask.size(); ++i) {
            mask[i] = static_cast<uint8_t>((pixels[i * 4] * pixels[i * 4 + 3] + 127) / 255);
        }
        stbi_image_free(pixels);

        gfx::Texture texture = gfx::Texture::create();
        glBindTexture(GL_TEXTURE_2D, texture.id());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, mask.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_masks[static_cast<size_t>(type)] = std::move(texture);
    }
    m_type = type;
    return true;
}

void Brush::beginStroke(float x, float y, float pressure, float radius, float flow) {
    m_radius = radius;
    m_flow = flow;
    m_lastX = x;
    m_lastY = y;
    m_lastPressure = pressure;
    m_distanceSinceDab = 0.0f;
    addDab(x, y, pressure);
}

void Brush::strokeTo(float x, float y, float pressure) {
    const float dx = x - m_lastX;
    const float dy = y - m_lastY;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0f) {
        m_lastPressure = pressure;
        return;
    }

    // Un dab cada kSpacing píxeles a lo largo del segmento. Lo que sobra se arrastra al
    // siguiente segmento, así los movimientos cortos del lápiz no se pierden.
    float t = kSpacing - m_distanceSinceDab;
    while (t <= length) {
        const float f = t / length;
        addDab(m_lastX + dx * f, m_lastY + dy * f, m_lastPressure + (pressure - m_lastPressure) * f);
        t += kSpacing;
    }
    m_distanceSinceDab = length - (t - kSpacing);
    m_lastX = x;
    m_lastY = y;
    m_lastPressure = pressure;
}

void Brush::discardPending() {
    m_dabs.clear();
    m_pendingBounds = {};
}

void Brush::addDab(float x, float y, float pressure) {
    pressure = std::clamp(pressure, 0.0f, 1.0f);
    // Mismo mapeo que la versión anterior: la presión escala el radio y el alfa del dab.
    const float radius = std::clamp(pressure * m_radius, kMinRadius, kMaxRadius);
    const float alpha = pressure * m_flow;
    m_dabs.insert(m_dabs.end(), {x, y, radius, alpha});
    m_pendingBounds.unite(IRect::around(x, y, radius));
}

IRect Brush::flush(GLuint target, int width, int height, const float color[3]) {
    IRect bounds = m_pendingBounds.intersected(IRect::ofSize(width, height));
    const GLsizei count = static_cast<GLsizei>(m_dabs.size() / 4);
    if (count == 0 || !m_program || m_type < 0) {
        discardPending();
        return {};
    }

    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);   // viewport del lienzo, no el de la ventana
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_program.id());
    glUniform2f(m_uCanvasSize, static_cast<float>(width), static_cast<float>(height));
    glUniform3f(m_uColor, color[0], color[1], color[2]);
    glUniform1i(m_uMask, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_masks[static_cast<size_t>(m_type)].id());

    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo.id());
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_dabs.size() * sizeof(float)), m_dabs.data(),
                 GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    discardPending();
    return bounds;
}
