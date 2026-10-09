#include "Canvas/Bounds.h"

#include "Gfx/Shader.h"

#include <algorithm>
#include <vector>

namespace {

constexpr int kBlock = 16;

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Cada píxel del destino es un bloque de 16 × 16 de la imagen: 1 si alguno no es 0.
constexpr const char* kFragment = R"(
uniform sampler2D uImage;
uniform sampler2D uMask;
uniform int uChannel;      // 0: rojo, 3: alfa
uniform int uUseMask;
uniform ivec4 uWithin;     // x0, y0, x1, y1
out vec4 fragColor;
void main() {
    ivec2 base = uWithin.xy + ivec2(gl_FragCoord.xy) * 16;
    float found = 0.0;
    for (int dy = 0; dy < 16; ++dy) {
        int y = base.y + dy;
        if (y >= uWithin.w || found > 0.0) {
            break;
        }
        for (int dx = 0; dx < 16; ++dx) {
            int x = base.x + dx;
            if (x >= uWithin.z) {
                break;
            }
            vec4 c = texelFetch(uImage, ivec2(x, y), 0);
            float v = uChannel == 0 ? c.r : c.a;
            if (uUseMask == 1) {
                v *= texelFetch(uMask, ivec2(x, y), 0).r;
            }
            if (v > 0.0) {
                found = 1.0;
                break;
            }
        }
    }
    fragColor = vec4(found);
}
)";

bool readRect(GLuint fbo, const IRect& rect, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(rect.width()) * static_cast<size_t>(rect.height()) * 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(rect.x0, rect.y0, rect.width(), rect.height(), GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

} // namespace

bool BoundsFinder::init() {
    destroy();
    m_program = gfx::makeProgram("bounds", kVertex, kFragment);
    if (!m_program) {
        return false;
    }
    const GLuint id = m_program.id();
    m_uImage = glGetUniformLocation(id, "uImage");
    m_uMask = glGetUniformLocation(id, "uMask");
    m_uChannel = glGetUniformLocation(id, "uChannel");
    m_uUseMask = glGetUniformLocation(id, "uUseMask");
    m_uWithin = glGetUniformLocation(id, "uWithin");

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

void BoundsFinder::destroy() {
    m_blocks.destroy();
    m_vbo.reset();
    m_vao.reset();
    m_program.reset();
}

bool BoundsFinder::readStrip(const gfx::RenderTarget& image, const gfx::RenderTarget* mask, const IRect& strip,
                             IRect& found) {
    if (strip.empty()) {
        return false;
    }
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> maskPixels;
    readRect(image.fbo.id(), strip, pixels);
    if (mask) {
        readRect(mask->fbo.id(), strip, maskPixels);
    }
    const int channel = image.format == gfx::RenderTarget::Format::R8 ? 0 : 3;
    bool any = false;
    for (int y = 0; y < strip.height(); ++y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(strip.width()) * 4;
        for (int x = 0; x < strip.width(); ++x) {
            const size_t i = row + static_cast<size_t>(x) * 4;
            if (pixels[i + static_cast<size_t>(channel)] == 0 || (mask && maskPixels[i] == 0)) {
                continue;
            }
            const int px = strip.x0 + x;
            const int py = strip.y0 + y;
            found.unite(IRect{px, py, px + 1, py + 1});
            any = true;
        }
    }
    return any;
}

IRect BoundsFinder::find(const gfx::RenderTarget& image, const gfx::RenderTarget* mask, const IRect& within) {
    const IRect area = within.intersected(IRect::ofSize(image.width, image.height));
    if (area.empty() || !m_program || !image) {
        return {};
    }
    const int bw = (area.width() + kBlock - 1) / kBlock;
    const int bh = (area.height() + kBlock - 1) / kBlock;
    if (m_blocks.width < bw || m_blocks.height < bh) {
        if (!m_blocks.create(std::max(bw, m_blocks.width), std::max(bh, m_blocks.height), nullptr,
                             gfx::RenderTarget::Format::R8)) {
            return area;   // sin memoria: la caja entera (sobra, pero no falta)
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_blocks.fbo.id());
    glViewport(0, 0, m_blocks.width, m_blocks.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, bw, bh);
    glDisable(GL_BLEND);
    glUseProgram(m_program.id());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image.texture.id());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mask ? mask->texture.id() : image.texture.id());
    glUniform1i(m_uImage, 0);
    glUniform1i(m_uMask, 1);
    glUniform1i(m_uChannel, image.format == gfx::RenderTarget::Format::R8 ? 0 : 3);
    glUniform1i(m_uUseMask, mask ? 1 : 0);
    glUniform4i(m_uWithin, area.x0, area.y0, area.x1, area.y1);
    glBindVertexArray(m_vao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::vector<uint8_t> blocks;
    readRect(m_blocks.fbo.id(), IRect::ofSize(bw, bh), blocks);
    int bx0 = bw;
    int by0 = bh;
    int bx1 = -1;
    int by1 = -1;
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            if (blocks[(static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x)) * 4] != 0) {
                bx0 = std::min(bx0, x);
                by0 = std::min(by0, y);
                bx1 = std::max(bx1, x);
                by1 = std::max(by1, y);
            }
        }
    }
    if (bx1 < 0) {
        return {};
    }

    // La caja exacta está dentro de los bloques de los bordes: se leen esas cuatro tiras.
    const IRect coarse = IRect{area.x0 + bx0 * kBlock, area.y0 + by0 * kBlock, area.x0 + (bx1 + 1) * kBlock,
                               area.y0 + (by1 + 1) * kBlock}
                             .intersected(area);
    IRect result;
    readStrip(image, mask, IRect{coarse.x0, coarse.y0, std::min(coarse.x0 + kBlock, coarse.x1), coarse.y1}, result);
    readStrip(image, mask, IRect{std::max(coarse.x1 - kBlock, coarse.x0), coarse.y0, coarse.x1, coarse.y1}, result);
    readStrip(image, mask, IRect{coarse.x0, coarse.y0, coarse.x1, std::min(coarse.y0 + kBlock, coarse.y1)}, result);
    readStrip(image, mask, IRect{coarse.x0, std::max(coarse.y1 - kBlock, coarse.y0), coarse.x1, coarse.y1}, result);
    return result.empty() ? coarse : result;
}
