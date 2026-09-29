#include "Canvas/Selection.h"

#include "Gfx/Shader.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// La fuente se lee en coordenadas del destino: uv = uSourceMap.xy + gl_FragCoord.xy *
// uSourceMap.zw. Con la convención de fila 0 = arriba, gl_FragCoord son píxeles del
// lienzo cuando el destino es la máscara.
constexpr const char* kFragment = R"(
uniform sampler2D uSource;
uniform vec4 uSourceMap;
uniform int uKind;         // 0 rojo, 1 alfa, 2 niveles de la selección automática, 3 uno
uniform float uCutoff;     // niveles: el borde (entre 0 y 255)
uniform float uSoftness;   // niveles: ancho del borde suave, en niveles
uniform int uInvert;       // 1: 1 - valor
out vec4 fragColor;
void main() {
    float v = 1.0;
    if (uKind != 3) {
        vec4 s = texture(uSource, uSourceMap.xy + gl_FragCoord.xy * uSourceMap.zw);
        if (uKind == 0) {
            v = s.r;
        } else if (uKind == 1) {
            v = s.a;
        } else {
            v = clamp((uCutoff - s.r * 255.0) / uSoftness + 0.5, 0.0, 1.0);
        }
    }
    if (uInvert == 1) {
        v = 1.0 - v;
    }
    fragColor = vec4(v);
}
)";

// Media gaussiana en una dirección, del canal rojo. Con uRadius = 0 es una copia (al
// reducir a la mitad, el filtro bilineal ya promedia 2×2).
constexpr const char* kBlurFragment = R"(
uniform sampler2D uSource;
uniform vec4 uSourceMap;
uniform vec2 uStep;
uniform float uSigma;
uniform int uRadius;
out vec4 fragColor;
void main() {
    vec2 uv = uSourceMap.xy + gl_FragCoord.xy * uSourceMap.zw;
    float sum = texture(uSource, uv).r;
    float total = 1.0;
    float k = -0.5 / max(uSigma * uSigma, 0.0001);
    for (int i = 1; i <= 64; ++i) {
        if (i > uRadius) {
            break;
        }
        float fi = float(i);
        float w = exp(fi * fi * k);
        sum += (texture(uSource, uv + uStep * fi).r + texture(uSource, uv - uStep * fi).r) * w;
        total += 2.0 * w;
    }
    fragColor = vec4(sum / total);
}
)";

// Sigma a la que se desenfoca en el nivel más reducido.
constexpr float kLevelSigma = 3.0f;
constexpr int kMaxBlurLevels = 6;

void setupTexture(GLuint texture) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void blitRect(GLuint source, GLuint target, const IRect& rect) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(rect.x0, rect.y0, rect.x1, rect.y1, rect.x0, rect.y0, rect.x1, rect.y1, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace

bool Selection::init(int width, int height) {
    destroy();
    m_width = width;
    m_height = height;
    m_program = gfx::makeProgram("selection", kVertex, kFragment);
    m_blurProgram = gfx::makeProgram("selection blur", kVertex, kBlurFragment);
    if (!m_program || !m_blurProgram) {
        destroy();
        return false;
    }
    const GLuint id = m_program.id();
    m_uSource = glGetUniformLocation(id, "uSource");
    m_uSourceMap = glGetUniformLocation(id, "uSourceMap");
    m_uKind = glGetUniformLocation(id, "uKind");
    m_uCutoff = glGetUniformLocation(id, "uCutoff");
    m_uSoftness = glGetUniformLocation(id, "uSoftness");
    m_uInvert = glGetUniformLocation(id, "uInvert");
    const GLuint blur = m_blurProgram.id();
    m_uBlurSource = glGetUniformLocation(blur, "uSource");
    m_uBlurMap = glGetUniformLocation(blur, "uSourceMap");
    m_uBlurStep = glGetUniformLocation(blur, "uStep");
    m_uBlurSigma = glGetUniformLocation(blur, "uSigma");
    m_uBlurRadius = glGetUniformLocation(blur, "uRadius");

    // WebGL avisa si un programa tiene un sampler sin textura, aunque no lo lea.
    m_dummy = gfx::Texture::create();
    setupTexture(m_dummy.id());
    const uint8_t zero = 0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &zero);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

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

void Selection::destroy() {
    m_state = {};
    m_mask.destroy();
    m_scratch.destroy();
    m_upload.reset();
    m_levels.reset();
    m_dummy.reset();
    m_blurLevels.clear();
    m_blurTemp[0].destroy();
    m_blurTemp[1].destroy();
    m_vbo.reset();
    m_vao.reset();
    m_blurProgram.reset();
    m_program.reset();
    m_width = 0;
    m_height = 0;
}

bool Selection::ensureMask() {
    if (m_mask) {
        return true;
    }
    if (!m_mask.create(m_width, m_height, nullptr, gfx::RenderTarget::Format::R8)) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Sin memoria para la selección");
        return false;
    }
    m_state.content = {};
    return true;
}

bool Selection::ensureScratch() {
    if (m_scratch) {
        return true;
    }
    return m_scratch.create(m_width, m_height, nullptr, gfx::RenderTarget::Format::R8);
}

void Selection::bindMaskPass(GLuint fbo, int width, int height, const IRect& scissor) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(scissor.x0, scissor.y0, scissor.width(), scissor.height());
    glBindVertexArray(m_vao.id());
}

void Selection::finishPass() {
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Selection::draw(GLuint texture, int textureWidth, int textureHeight, int originX, int originY, Source source,
                     const IRect& rect, bool invertSource, float cutoff, float softness) {
    const float w = static_cast<float>(std::max(textureWidth, 1));
    const float h = static_cast<float>(std::max(textureHeight, 1));
    glUseProgram(m_program.id());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture != 0 ? texture : m_dummy.id());
    glUniform1i(m_uSource, 0);
    glUniform4f(m_uSourceMap, -static_cast<float>(originX) / w, -static_cast<float>(originY) / h, 1.0f / w, 1.0f / h);
    int kind = 0;
    switch (source) {
    case Source::Red:
        kind = 0;
        break;
    case Source::Alpha:
        kind = 1;
        break;
    case Source::Levels:
        kind = 2;
        break;
    case Source::One:
        kind = 3;
        break;
    }
    glUniform1i(m_uKind, kind);
    glUniform1f(m_uCutoff, cutoff);
    glUniform1f(m_uSoftness, std::max(softness, 0.001f));
    glUniform1i(m_uInvert, invertSource ? 1 : 0);
    (void)rect;
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

namespace {

void setCombine(Selection::Combine how) {
    glEnable(GL_BLEND);
    glBlendEquation(how == Selection::Combine::Union ? GL_MAX : GL_MIN);
}

} // namespace

bool Selection::drawCoverage(const std::vector<uint8_t>& coverage, const IRect& rect, Combine how) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || area.x0 != rect.x0 || area.y0 != rect.y0 || area.x1 != rect.x1 || area.y1 != rect.y1 ||
        coverage.size() != static_cast<size_t>(rect.width()) * static_cast<size_t>(rect.height()) || !ensureMask()) {
        return false;
    }
    if (!m_upload) {
        m_upload = gfx::Texture::create();
    }
    setupTexture(m_upload.id());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, rect.width(), rect.height(), 0, GL_RED, GL_UNSIGNED_BYTE, coverage.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    bindMaskPass(m_mask.fbo.id(), m_width, m_height, rect);
    setCombine(how);
    draw(m_upload.id(), rect.width(), rect.height(), rect.x0, rect.y0, Source::Red, rect,
         how == Combine::Subtract);
    finishPass();
    return true;
}

bool Selection::uploadLevels(const std::vector<uint8_t>& levels) {
    if (levels.size() != static_cast<size_t>(m_width) * static_cast<size_t>(m_height)) {
        return false;
    }
    if (!m_levels) {
        m_levels = gfx::Texture::create();
    }
    setupTexture(m_levels.id());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, m_width, m_height, 0, GL_RED, GL_UNSIGNED_BYTE, levels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void Selection::dropLevels() { m_levels.reset(); }

void Selection::drawLevels(const IRect& rect, int cutoff, float softness, Combine how) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !m_levels || !ensureMask()) {
        return;
    }
    bindMaskPass(m_mask.fbo.id(), m_width, m_height, area);
    setCombine(how);
    // Con el borde en `cutoff` + 0,5, el nivel `cutoff` todavía entra entero.
    draw(m_levels.id(), m_width, m_height, 0, 0, Source::Levels, area, how == Combine::Subtract,
         static_cast<float>(cutoff) + 0.5f, softness);
    finishPass();
}

void Selection::drawAlpha(GLuint texture, const IRect& rect, Combine how) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !ensureMask()) {
        return;
    }
    bindMaskPass(m_mask.fbo.id(), m_width, m_height, area);
    setCombine(how);
    draw(texture, m_width, m_height, 0, 0, Source::Alpha, area, how == Combine::Subtract);
    finishPass();
}

void Selection::clear(const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !m_mask) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_mask.fbo.id());
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Selection::fill(const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !ensureMask()) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_mask.fbo.id());
    glViewport(0, 0, m_width, m_height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Selection::invert(const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !ensureMask()) {
        return;
    }
    bindMaskPass(m_mask.fbo.id(), m_width, m_height, area);
    // destino = 1 · (1 − destino)
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
    draw(0, 1, 1, 0, 0, Source::One, area, false);
    finishPass();
}

void Selection::copyToScratch(const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !ensureMask() || !ensureScratch()) {
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    blitRect(m_mask.fbo.id(), m_scratch.fbo.id(), area);
}

void Selection::copyFromScratch(const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !m_scratch || !ensureMask()) {
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    blitRect(m_scratch.fbo.id(), m_mask.fbo.id(), area);
}

void Selection::blurPass(const gfx::RenderTarget& target, GLuint source, const float map[4], float stepX, float stepY,
                         float sigma) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glViewport(0, 0, target.width, target.height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glUniform4f(m_uBlurMap, map[0], map[1], map[2], map[3]);
    glUniform2f(m_uBlurStep, stepX, stepY);
    glUniform1f(m_uBlurSigma, sigma);
    glUniform1i(m_uBlurRadius, sigma > 0.0f ? std::min(static_cast<int>(std::ceil(sigma * 3.0f)), 64) : 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Selection::blurFromScratch(const IRect& rect, float sigma) {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !m_scratch || !ensureMask()) {
        return;
    }
    if (sigma < 0.3f) {
        copyFromScratch(area);
        return;
    }

    // Se reduce a la mitad hasta que la sigma que queda es pequeña; se desenfoca ahí y se
    // amplía al dibujarlo en la máscara (con el filtro bilineal queda suave).
    int levels = 0;
    float levelSigma = sigma;
    int w = area.width();
    int h = area.height();
    while (levelSigma > kLevelSigma && levels < kMaxBlurLevels && w > 8 && h > 8) {
        levelSigma *= 0.5f;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        ++levels;
    }
    auto sized = [](gfx::RenderTarget& target, int tw, int th) {
        if (target.width == tw && target.height == th && target) {
            return true;
        }
        if (!target.create(tw, th, nullptr, gfx::RenderTarget::Format::R8)) {
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, target.texture.id());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        return true;
    };
    if (static_cast<int>(m_blurLevels.size()) < levels) {
        m_blurLevels.resize(static_cast<size_t>(levels));
    }
    {
        int lw = area.width();
        int lh = area.height();
        for (int i = 0; i < levels; ++i) {
            lw = (lw + 1) / 2;
            lh = (lh + 1) / 2;
            if (!sized(m_blurLevels[static_cast<size_t>(i)], lw, lh)) {
                copyFromScratch(area);
                return;
            }
        }
    }
    if (!sized(m_blurTemp[0], w, h) || (levels > 0 && !sized(m_blurTemp[1], w, h))) {
        copyFromScratch(area);
        return;
    }

    const float cw = static_cast<float>(m_width);
    const float ch = static_cast<float>(m_height);
    const float x0 = static_cast<float>(area.x0);
    const float y0 = static_cast<float>(area.y0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(m_blurProgram.id());
    glUniform1i(m_uBlurSource, 0);
    glBindVertexArray(m_vao.id());

    // Reducciones: cada píxel lee la esquina común de 2×2 de la fuente.
    GLuint source = m_scratch.texture.id();
    int sourceW = m_width;
    int sourceH = m_height;
    for (int i = 0; i < levels; ++i) {
        const gfx::RenderTarget& target = m_blurLevels[static_cast<size_t>(i)];
        float map[4];
        if (i == 0) {
            map[0] = x0 / cw;
            map[1] = y0 / ch;
            map[2] = 2.0f / cw;
            map[3] = 2.0f / ch;
        } else {
            map[0] = 0.0f;
            map[1] = 0.0f;
            map[2] = 2.0f / static_cast<float>(sourceW);
            map[3] = 2.0f / static_cast<float>(sourceH);
        }
        blurPass(target, source, map, 0.0f, 0.0f, 0.0f);
        source = target.texture.id();
        sourceW = target.width;
        sourceH = target.height;
    }

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    if (levels == 0) {
        // Horizontal leyendo la copia de trabajo; vertical directamente en la máscara.
        const float mapH[4] = {x0 / cw, y0 / ch, 1.0f / cw, 1.0f / ch};
        blurPass(m_blurTemp[0], m_scratch.texture.id(), mapH, 1.0f / cw, 0.0f, levelSigma);
        glEnable(GL_SCISSOR_TEST);
        glScissor(area.x0, area.y0, area.width(), area.height());
        const float mapV[4] = {-x0 / fw, -y0 / fh, 1.0f / fw, 1.0f / fh};
        blurPass(m_mask, m_blurTemp[0].texture.id(), mapV, 0.0f, 1.0f / fh, levelSigma);
    } else {
        const float map[4] = {0.0f, 0.0f, 1.0f / fw, 1.0f / fh};
        blurPass(m_blurTemp[0], source, map, 1.0f / fw, 0.0f, levelSigma);
        blurPass(m_blurTemp[1], m_blurTemp[0].texture.id(), map, 0.0f, 1.0f / fh, levelSigma);
        // Ampliación: el píxel j del nivel cubre los píxeles [j·2^n, (j+1)·2^n) de la zona.
        const float scale = static_cast<float>(1 << levels);
        glEnable(GL_SCISSOR_TEST);
        glScissor(area.x0, area.y0, area.width(), area.height());
        const float mapUp[4] = {-x0 / (scale * fw), -y0 / (scale * fh), 1.0f / (scale * fw), 1.0f / (scale * fh)};
        blurPass(m_mask, m_blurTemp[1].texture.id(), mapUp, 0.0f, 0.0f, 0.0f);
    }
    finishPass();
}

bool Selection::read(const IRect& rect, std::vector<uint8_t>& rgba) const {
    const IRect area = rect.intersected(IRect::ofSize(m_width, m_height));
    if (area.empty() || !m_mask) {
        rgba.clear();
        return false;
    }
    rgba.resize(static_cast<size_t>(area.width()) * static_cast<size_t>(area.height()) * 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_mask.fbo.id());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(area.x0, area.y0, area.width(), area.height(), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}
