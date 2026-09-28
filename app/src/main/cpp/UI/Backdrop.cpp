#include "UI/Backdrop.h"

#include "Gfx/Shader.h"

#include <algorithm>
#include <cmath>

namespace {

// Sigma que se desenfoca en el último nivel; hasta llegar a ella se va reduciendo la
// escena a la mitad (cada mitad promedia 2×2 píxeles).
constexpr float kLevelSigma = 6.0f;
constexpr int kMinLevelSize = 8;
// Saturación extra del cristal (el "saturate(180%)" de iOS).
constexpr float kSaturation = 1.6f;

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Media ponderada gaussiana en una dirección. Con uRadius = 0 es una copia (al reducir a
// la mitad, el filtro bilineal ya promedia 2×2).
constexpr const char* kFragment = R"(
in vec2 vUV;
uniform sampler2D uSource;
uniform vec2 uStep;
uniform float uSigma;
uniform int uRadius;
uniform float uSaturation;
out vec4 fragColor;
void main() {
    vec3 sum = texture(uSource, vUV).rgb;
    float total = 1.0;
    float k = -0.5 / max(uSigma * uSigma, 0.0001);
    for (int i = 1; i <= 64; ++i) {
        if (i > uRadius) {
            break;
        }
        float fi = float(i);
        float w = exp(fi * fi * k);
        sum += (texture(uSource, vUV + uStep * fi).rgb + texture(uSource, vUV - uStep * fi).rgb) * w;
        total += 2.0 * w;
    }
    vec3 color = sum / total;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    fragColor = vec4(clamp(mix(vec3(luma), color, uSaturation), 0.0, 1.0), 1.0);
}
)";

} // namespace

bool Backdrop::init() {
    destroy();
    m_program = gfx::makeProgram("backdrop blur", kVertex, kFragment);
    if (!m_program) {
        return false;
    }
    m_uSource = glGetUniformLocation(m_program.id(), "uSource");
    m_uStep = glGetUniformLocation(m_program.id(), "uStep");
    m_uSigma = glGetUniformLocation(m_program.id(), "uSigma");
    m_uRadius = glGetUniformLocation(m_program.id(), "uRadius");
    m_uSaturation = glGetUniformLocation(m_program.id(), "uSaturation");

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

void Backdrop::destroy() {
    for (gfx::RenderTarget& level : m_levels) {
        level.destroy();
    }
    m_levelCount = 0;
    m_blurTemp.destroy();
    m_blurred.destroy();
    m_result = nullptr;
    m_vbo.reset();
    m_vao.reset();
    m_program.reset();
    m_width = 0;
    m_height = 0;
    m_valid = false;
}

bool Backdrop::resize(int width, int height, float sigma) {
    for (gfx::RenderTarget& level : m_levels) {
        level.destroy();
    }
    m_levelCount = 0;
    m_result = nullptr;
    m_valid = false;
    m_width = width;
    m_height = height;
    m_sigma = sigma;

    float scale = 0.5f;
    int w = std::max(width / 2, 1);
    int h = std::max(height / 2, 1);
    while (true) {
        if (!m_levels[m_levelCount].create(w, h)) {
            return false;
        }
        ++m_levelCount;
        const int nw = w / 2;
        const int nh = h / 2;
        if (sigma * scale <= kLevelSigma || m_levelCount == kMaxLevels || nw < kMinLevelSize || nh < kMinLevelSize) {
            break;
        }
        w = nw;
        h = nh;
        scale *= 0.5f;
    }
    m_levelSigma = std::max(sigma * scale, 0.5f);
    return m_blurTemp.create(w, h) && m_blurred.create(w, h);
}

void Backdrop::pass(const gfx::RenderTarget& target, GLuint source, float stepX, float stepY, float sigma,
                    float saturation) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glViewport(0, 0, target.width, target.height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glUniform2f(m_uStep, stepX, stepY);
    glUniform1f(m_uSigma, sigma);
    glUniform1i(m_uRadius, sigma > 0.0f ? std::min(static_cast<int>(std::ceil(sigma * 3.0f)), 64) : 0);
    glUniform1f(m_uSaturation, saturation);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Backdrop::update(CanvasView& view, const Camera& camera, GLuint compositeTexture, int width, int height,
                      float sigma, uint64_t sceneKey) {
    if (!m_program || width <= 0 || height <= 0) {
        return;
    }
    if (width != m_width || height != m_height || std::fabs(sigma - m_sigma) > 0.01f || m_levelCount == 0) {
        if (!resize(width, height, sigma)) {
            m_result = nullptr;
            return;
        }
    }
    if (m_valid && sceneKey == m_sceneKey) {
        return;
    }

    const gfx::RenderTarget& first = m_levels[0];
    view.draw(camera, compositeTexture, first.fbo.id(), first.width, first.height,
              static_cast<float>(first.width) / static_cast<float>(width));

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(m_program.id());
    glUniform1i(m_uSource, 0);
    glBindVertexArray(m_vao.id());
    for (int i = 1; i < m_levelCount; ++i) {
        pass(m_levels[i], m_levels[i - 1].texture.id(), 0.0f, 0.0f, 0.0f, 1.0f);
    }
    const gfx::RenderTarget& last = m_levels[m_levelCount - 1];
    pass(m_blurTemp, last.texture.id(), 1.0f / static_cast<float>(last.width), 0.0f, m_levelSigma, 1.0f);
    pass(m_blurred, m_blurTemp.texture.id(), 0.0f, 1.0f / static_cast<float>(last.height), m_levelSigma,
         kSaturation);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_result = &m_blurred;
    m_sceneKey = sceneKey;
    m_valid = true;
}
