#include "UI/Previews.h"

#include "Gfx/Shader.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Reduce la capa promediando una rejilla de uTaps×uTaps muestras bilineales dentro de la
// zona que cubre cada píxel, y la pone sobre un damero (resultado opaco).
constexpr const char* kFragment = R"(
in vec2 vUV;
uniform sampler2D uSource;
uniform vec2 uTexel;       // tamaño de un texel de la capa en uv
uniform vec2 uFootprint;   // texels de la capa por píxel de la miniatura
uniform int uTaps;
uniform float uCell;
out vec4 fragColor;
void main() {
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 16; ++y) {
        if (y >= uTaps) {
            break;
        }
        for (int x = 0; x < 16; ++x) {
            if (x >= uTaps) {
                break;
            }
            vec2 offset = (vec2(float(x), float(y)) + 0.5) / float(uTaps) - 0.5;
            sum += texture(uSource, vUV + offset * uFootprint * uTexel);
        }
    }
    vec4 color = sum / float(uTaps * uTaps);
    vec2 cell = floor(gl_FragCoord.xy / uCell);
    float checker = mix(0.84, 1.0, mod(cell.x + cell.y, 2.0));
    fragColor = vec4(color.rgb + vec3(checker) * (1.0 - color.a), 1.0);
}
)";

// Trazo de muestra: la "S" de la maqueta en una caja de 176×44.
struct Point {
    float x;
    float y;
};

Point cubic(Point a, Point b, Point c, Point d, float t) {
    const float u = 1.0f - t;
    const float w0 = u * u * u;
    const float w1 = 3.0f * u * u * t;
    const float w2 = 3.0f * u * t * t;
    const float w3 = t * t * t;
    return {a.x * w0 + b.x * w1 + c.x * w2 + d.x * w3, a.y * w0 + b.y * w1 + c.y * w2 + d.y * w3};
}

} // namespace

bool Previews::init() {
    destroy();
    m_program = gfx::makeProgram("thumbnail", kVertex, kFragment);
    if (!m_program) {
        return false;
    }
    m_uSource = glGetUniformLocation(m_program.id(), "uSource");
    m_uTexel = glGetUniformLocation(m_program.id(), "uTexel");
    m_uFootprint = glGetUniformLocation(m_program.id(), "uFootprint");
    m_uTaps = glGetUniformLocation(m_program.id(), "uTaps");
    m_uCell = glGetUniformLocation(m_program.id(), "uCell");

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

    m_brushReady = m_brush.init();
    return true;
}

void Previews::destroy() {
    m_thumbnails.clear();
    m_strokes.clear();
    m_brush.destroy();
    m_brushReady = false;
    m_vbo.reset();
    m_vao.reset();
    m_program.reset();
}

GLuint Previews::layerThumbnail(const Layer& layer, int width, int height, int checkerCell) {
    if (!m_program || width <= 0 || height <= 0 || !layer.target) {
        return 0;
    }
    Thumbnail& thumbnail = m_thumbnails[layer.id];
    const bool sized = thumbnail.target && thumbnail.target.width == width && thumbnail.target.height == height;
    if (sized && thumbnail.revision == layer.revision) {
        return thumbnail.target.texture.id();
    }
    if (!sized && !thumbnail.target.create(width, height)) {
        m_thumbnails.erase(layer.id);
        return 0;
    }

    const float footprintX = static_cast<float>(layer.target.width) / static_cast<float>(width);
    const float footprintY = static_cast<float>(layer.target.height) / static_cast<float>(height);
    const float footprint = std::max(footprintX, footprintY);
    const int taps = std::clamp(static_cast<int>(std::ceil(footprint * 0.5f)), 1, 16);

    glBindFramebuffer(GL_FRAMEBUFFER, thumbnail.target.fbo.id());
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(m_program.id());
    glUniform1i(m_uSource, 0);
    glUniform2f(m_uTexel, 1.0f / static_cast<float>(layer.target.width), 1.0f / static_cast<float>(layer.target.height));
    glUniform2f(m_uFootprint, footprintX, footprintY);
    glUniform1i(m_uTaps, taps);
    glUniform1f(m_uCell, static_cast<float>(std::max(checkerCell, 1)));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, layer.target.texture.id());
    glBindVertexArray(m_vao.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    thumbnail.revision = layer.revision;
    return thumbnail.target.texture.id();
}

void Previews::pruneThumbnails(const LayerStack& layers) {
    for (auto it = m_thumbnails.begin(); it != m_thumbnails.end();) {
        it = layers.indexOf(it->first) < 0 ? m_thumbnails.erase(it) : std::next(it);
    }
}

GLuint Previews::brushPreview(int slot, const BrushParams& params, int width, int height) {
    if (width <= 0 || height <= 0 || !m_brushReady) {
        return 0;
    }
    Stroke& stroke = m_strokes[slot];
    if (stroke.target && stroke.target.width == width && stroke.target.height == height && stroke.params == params) {
        return stroke.target.texture.id();
    }
    const bool sized = stroke.target && stroke.target.width == width && stroke.target.height == height;
    if (!sized && !stroke.target.create(width, height)) {
        m_strokes.erase(slot);
        return 0;
    }
    stroke.params = params;

    // Blanco con alfa 0 y solo se escribe el alfa: queda blanco sin premultiplicar.
    glBindFramebuffer(GL_FRAMEBUFFER, stroke.target.fbo.id());
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(1.0f, 1.0f, 1.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // La "S" de la maqueta, en una caja de 176×44 escalada a la muestra.
    const float sx = static_cast<float>(width) / 176.0f;
    const float sy = static_cast<float>(height) / 44.0f;
    const Point a0{8, 30}, a1{40, 6}, a2{70, 6}, a3{92, 22};
    const Point b1{114, 38}, b2{144, 40}, b3{168, 14};   // b1 refleja a2 respecto de a3
    constexpr int kSamples = 96;
    std::vector<Point> path;
    for (int i = 0; i <= kSamples; ++i) {
        path.push_back(cubic(a0, a1, a2, a3, static_cast<float>(i) / kSamples));
    }
    for (int i = 1; i <= kSamples; ++i) {
        path.push_back(cubic(a3, b1, b2, b3, static_cast<float>(i) / kSamples));
    }

    // El pincel a una escala que quepa en la muestra; la estabilización no hace falta.
    BrushParams sample = params;
    sample.streamline = 0.0f;
    StrokePath::Settings settings;
    settings.radius = std::max(0.75f, static_cast<float>(height) * params.previewSize);
    settings.flow = params.flow;
    settings.seed = 12345;
    const float count = static_cast<float>(path.size() - 1);
    for (size_t i = 0; i < path.size(); ++i) {
        const float t = static_cast<float>(i) / count;
        // Presión que sube y baja, como un trazo a mano.
        const float pressure = 0.25f + 0.75f * std::pow(std::sin(t * 3.14159265f), 0.6f);
        const float x = path[i].x * sx;
        const float y = path[i].y * sy;
        if (i == 0) {
            m_path.begin(sample, settings, x, y, pressure);
        } else {
            m_path.moveTo(x, y, pressure);
        }
    }
    m_path.finish();
    m_dabs.clear();
    m_path.takeFinal(m_dabs);

    const float white[3] = {1.0f, 1.0f, 1.0f};
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    const IRect touched = m_brush.draw(stroke.target.fbo.id(), width, height, m_dabs, sample, white);
    m_brush.applyGrain(stroke.target.fbo.id(), width, height, sample, touched);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    return stroke.target.texture.id();
}
