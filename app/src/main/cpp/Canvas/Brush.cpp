#include "Canvas/Brush.h"

#include "Canvas/BrushTips.h"
#include "Gfx/Shader.h"
#include "IO/Assets.h"
#include "ThirdParty/stb_image.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

static_assert(sizeof(Dab) == 5 * sizeof(float), "los sellos se suben tal cual");

// Píxeles alrededor de la punta redonda para su antialias.
constexpr float kRoundPad = 1.0f;

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aCorner;   // esquina del quad, de (-1,-1) a (1,1)
layout(location = 1) in vec4 aDab;      // por sello: centro x, centro y, radio, alfa
layout(location = 2) in float aAngle;   // por sello: giro en radianes
uniform vec2 uCanvasSize;
uniform float uRoundness;   // la punta se aplana a lo alto
uniform float uPad;         // píxeles de más alrededor de la punta
uniform float uFlip;        // 1: imagen invertida (pinceles de antes)
out vec2 vLocal;            // píxeles desde el centro, sin aplanar: un círculo de radio vRadius
out vec2 vUV;
out float vAlpha;
out float vRadius;
void main() {
    float r = aDab.z;
    vec2 halfSize = vec2(r, r * uRoundness) + uPad;
    vec2 local = aCorner * halfSize;
    float c = cos(aAngle);
    float s = sin(aAngle);
    vec2 p = aDab.xy + vec2(local.x * c - local.y * s, local.x * s + local.y * c);
    gl_Position = vec4(p / uCanvasSize * 2.0 - 1.0, 0.0, 1.0);
    vLocal = vec2(local.x, local.y / uRoundness);
    vec2 uv = local / vec2(r, r * uRoundness);
    vUV = vec2(uv.x, mix(uv.y, -uv.y, uFlip)) * 0.5 + 0.5;
    vAlpha = aDab.w;
    vRadius = r;
}
)";

constexpr const char* kFragment = R"(
in vec2 vLocal;
in vec2 vUV;
in float vAlpha;
in float vRadius;
uniform int uRound;          // 1: punta redonda calculada aquí
uniform float uHardness;
uniform sampler2D uTip;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    float a;
    if (uRound == 1) {
        float d = length(vLocal);
        // Borde con antialias de un píxel de pantalla y caída suave según la dureza.
        float edge = clamp((vRadius - d) / max(fwidth(d), 1e-4) + 0.5, 0.0, 1.0);
        float soft = (1.0 - uHardness) * vRadius;
        float t = soft > 1e-3 ? clamp((vRadius - d) / soft, 0.0, 1.0) : 1.0;
        a = edge * t * t * (3.0 - 2.0 * t);
    } else {
        a = texture(uTip, vUV).a;
    }
    a *= vAlpha;
    fragColor = vec4(uColor * a, a);
}
)";

constexpr const char* kQuadVertex = R"(
layout(location = 0) in vec2 aCorner;
void main() {
    gl_Position = vec4(aCorner, 0.0, 1.0);
}
)";

// Factor del grano en cada píxel, fijo a las coordenadas del destino.
constexpr const char* kGrainFragment = R"(
uniform sampler2D uGrain;
uniform float uGrainScale;   // uv del grano por píxel
uniform float uGrainDepth;
out vec4 fragColor;
void main() {
    fragColor = vec4(0.0, 0.0, 0.0, mix(1.0, texture(uGrain, gl_FragCoord.xy * uGrainScale).a, uGrainDepth));
}
)";

// Textura blanca con `coverage` en el alfa. Con mipmaps (las puntas se ven muy pequeñas).
gfx::Texture makeAlphaTexture(const std::vector<uint8_t>& coverage, int width, int height, bool repeat,
                              bool mipmaps) {
    std::vector<uint8_t> rgba(coverage.size() * 4, 255);
    for (size_t i = 0; i < coverage.size(); ++i) {
        rgba[i * 4 + 3] = coverage[i];
    }
    gfx::Texture texture = gfx::Texture::create();
    glBindTexture(GL_TEXTURE_2D, texture.id());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

bool isClassic(BrushTip tip) {
    return tip == BrushTip::Classic0 || tip == BrushTip::Classic1 || tip == BrushTip::Classic2 ||
           tip == BrushTip::Classic3;
}

// brush0.png ... brush3.png. La cobertura es rojo × alfa (lo que leía la primera versión
// de su textura premultiplicada). Sin mipmaps: se ven como siempre.
gfx::Texture loadClassic(BrushTip tip) {
    const int index = static_cast<int>(tip) - static_cast<int>(BrushTip::Classic0);
    const std::string name = "brush" + std::to_string(index) + ".png";
    const std::vector<uint8_t> file = io::loadAsset(name.c_str());
    if (file.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo leer el pincel %s", name.c_str());
        return {};
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels,
                                            STBI_rgb_alpha);
    if (!pixels) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PNG inválido %s: %s", name.c_str(), stbi_failure_reason());
        return {};
    }
    std::vector<uint8_t> coverage(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (size_t i = 0; i < coverage.size(); ++i) {
        coverage[i] = static_cast<uint8_t>((pixels[i * 4] * pixels[i * 4 + 3] + 127) / 255);
    }
    stbi_image_free(pixels);
    return makeAlphaTexture(coverage, width, height, false, false);
}

} // namespace

bool Brush::init() {
    destroy();

    m_program = gfx::makeProgram("brush", kVertex, kFragment);
    m_grainProgram = gfx::makeProgram("brush-grain", kQuadVertex, kGrainFragment);
    if (!m_program || !m_grainProgram) {
        destroy();
        return false;
    }
    const GLuint id = m_program.id();
    m_uCanvasSize = glGetUniformLocation(id, "uCanvasSize");
    m_uRoundness = glGetUniformLocation(id, "uRoundness");
    m_uPad = glGetUniformLocation(id, "uPad");
    m_uFlip = glGetUniformLocation(id, "uFlip");
    m_uRound = glGetUniformLocation(id, "uRound");
    m_uHardness = glGetUniformLocation(id, "uHardness");
    m_uColor = glGetUniformLocation(id, "uColor");
    m_uTip = glGetUniformLocation(id, "uTip");
    m_uGrain = glGetUniformLocation(m_grainProgram.id(), "uGrain");
    m_uGrainScale = glGetUniformLocation(m_grainProgram.id(), "uGrainScale");
    m_uGrainDepth = glGetUniformLocation(m_grainProgram.id(), "uGrainDepth");

    const float corners[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    m_vao = gfx::VertexArray::create();
    m_quadVao = gfx::VertexArray::create();
    m_cornerVbo = gfx::Buffer::create();
    m_instanceVbo = gfx::Buffer::create();

    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_cornerVbo.id());
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo.id());
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Dab), nullptr);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(Dab), reinterpret_cast<const void*>(4 * sizeof(float)));
    glVertexAttribDivisor(2, 1);

    glBindVertexArray(m_quadVao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_cornerVbo.id());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void Brush::destroy() {
    for (auto& tip : m_tips) {
        tip.reset();
    }
    m_tipFailed.fill(false);
    for (auto& grain : m_grains) {
        grain.reset();
    }
    m_instanceVbo.reset();
    m_cornerVbo.reset();
    m_quadVao.reset();
    m_vao.reset();
    m_grainProgram.reset();
    m_program.reset();
}

GLuint Brush::tipTexture(BrushTip tip) {
    const auto index = static_cast<size_t>(tip);
    if (index >= m_tips.size()) {
        return 0;
    }
    if (!m_tips[index] && !m_tipFailed[index]) {
        if (isClassic(tip)) {
            m_tips[index] = loadClassic(tip);
        } else {
            m_tips[index] = makeAlphaTexture(brushtips::makeTip(tip), brushtips::kTipSize, brushtips::kTipSize, false,
                                             true);
        }
        m_tipFailed[index] = !m_tips[index];
    }
    return m_tips[index].id();
}

GLuint Brush::grainTexture(BrushGrain grain) {
    const auto index = static_cast<size_t>(grain);
    if (index >= m_grains.size() || grain == BrushGrain::None) {
        return 0;
    }
    if (!m_grains[index]) {
        m_grains[index] = makeAlphaTexture(brushtips::makeGrain(grain), brushtips::kGrainSize, brushtips::kGrainSize,
                                           true, true);
    }
    return m_grains[index].id();
}

bool Brush::prepare(const BrushParams& params) {
    if (!m_program) {
        return false;
    }
    if (params.grain != BrushGrain::None) {
        grainTexture(params.grain);
    }
    return params.tip == BrushTip::Round || tipTexture(params.tip) != 0;
}

IRect Brush::bounds(std::span<const Dab> dabs) {
    IRect rect;
    for (const Dab& dab : dabs) {
        // Un círculo del radio cubre la punta con cualquier giro (la redondez solo la
        // aplana), más el margen del antialias de la redonda.
        rect.unite(IRect::around(dab.x, dab.y, dab.radius + kRoundPad));
    }
    return rect;
}

IRect Brush::draw(GLuint target, int width, int height, std::span<const Dab> dabs, const BrushParams& params,
                  const float color[3]) {
    if (dabs.empty() || !m_program || !prepare(params)) {
        return {};
    }
    const bool round = params.tip == BrushTip::Round;

    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);   // viewport del lienzo, no el de la ventana
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    if (params.buildUp == BrushBuildUp::Uniform) {
        // Cada píxel se queda con el sello más opaco: repasar no oscurece.
        glBlendEquation(GL_MAX);
    } else {
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    glUseProgram(m_program.id());
    glUniform2f(m_uCanvasSize, static_cast<float>(width), static_cast<float>(height));
    glUniform1f(m_uRoundness, std::clamp(params.roundness, 0.1f, 1.0f));
    glUniform1f(m_uPad, round ? kRoundPad : 0.0f);
    glUniform1f(m_uFlip, isClassic(params.tip) ? 1.0f : 0.0f);
    glUniform1i(m_uRound, round ? 1 : 0);
    glUniform1f(m_uHardness, std::clamp(params.hardness, 0.0f, 1.0f));
    glUniform3f(m_uColor, color[0], color[1], color[2]);
    glUniform1i(m_uTip, 0);
    glActiveTexture(GL_TEXTURE0);
    // La redonda no lee la textura, pero la unidad necesita una válida.
    glBindTexture(GL_TEXTURE_2D, tipTexture(round ? BrushTip::Round : params.tip));

    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVbo.id());
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(dabs.size_bytes()), dabs.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(dabs.size()));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return bounds(dabs).intersected(IRect::ofSize(width, height));
}

void Brush::applyGrain(GLuint target, int width, int height, const BrushParams& params, const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(width, height));
    const GLuint grain = grainTexture(params.grain);
    if (area.empty() || !grain || params.grainDepth <= 0.0f || !m_grainProgram) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ZERO, GL_SRC_ALPHA);   // destino *= factor del grano

    glUseProgram(m_grainProgram.id());
    glUniform1i(m_uGrain, 0);
    glUniform1f(m_uGrainScale, 1.0f / (static_cast<float>(brushtips::kGrainSize) * std::max(params.grainScale, 0.01f)));
    glUniform1f(m_uGrainDepth, std::clamp(params.grainDepth, 0.0f, 1.0f));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, grain);
    glBindVertexArray(m_quadVao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
