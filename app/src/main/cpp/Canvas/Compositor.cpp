#include "Canvas/Compositor.h"

#include "Gfx/Shader.h"

namespace {

// Quad que cubre todo el FBO. Fila 0 de la textura = fila 0 del FBO: no hay volteos.
constexpr const char* kVertex = R"(
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Todo en alfa premultiplicado. uStrokeMode: 0 sin trazo, 1 pintar, 2 borrar.
constexpr const char* kFragment = R"(
in vec2 vUV;
uniform sampler2D uLayer;
uniform sampler2D uStroke;
uniform float uOpacity;
uniform float uStrokeOpacity;
uniform int uStrokeMode;
out vec4 fragColor;
void main() {
    vec4 color = texture(uLayer, vUV);
    if (uStrokeMode == 1) {
        vec4 stroke = texture(uStroke, vUV) * uStrokeOpacity;
        color = stroke + color * (1.0 - stroke.a);
    } else if (uStrokeMode == 2) {
        color *= 1.0 - texture(uStroke, vUV).a * uStrokeOpacity;
    }
    fragColor = color * uOpacity;
}
)";

} // namespace

bool Compositor::init(int width, int height) {
    destroy();

    m_program = gfx::makeProgram("compositor", kVertex, kFragment);
    if (!m_program) {
        return false;
    }
    m_uLayer = glGetUniformLocation(m_program.id(), "uLayer");
    m_uStroke = glGetUniformLocation(m_program.id(), "uStroke");
    m_uOpacity = glGetUniformLocation(m_program.id(), "uOpacity");
    m_uStrokeOpacity = glGetUniformLocation(m_program.id(), "uStrokeOpacity");
    m_uStrokeMode = glGetUniformLocation(m_program.id(), "uStrokeMode");

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

    return m_composite.create(width, height);
}

void Compositor::destroy() {
    m_composite.destroy();
    m_vbo.reset();
    m_vao.reset();
    m_program.reset();
}

void Compositor::bindCanvasPass(GLuint target, const IRect& rect) {
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, m_composite.width, m_composite.height);
    // Con la convención de fila 0 = arriba, las coordenadas del FBO coinciden con las del lienzo.
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect.x0, rect.y0, rect.width(), rect.height());
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glUseProgram(m_program.id());
    glUniform1i(m_uLayer, 0);
    glUniform1i(m_uStroke, 1);
    glBindVertexArray(m_vao.id());
}

void Compositor::drawQuad(GLuint layerTexture, float opacity, const StrokePreview* preview) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, layerTexture);
    if (preview) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, preview->texture);
        glUniform1f(m_uStrokeOpacity, preview->opacity);
        glUniform1i(m_uStrokeMode, preview->erase ? 2 : 1);
    } else {
        glUniform1i(m_uStrokeMode, 0);
    }
    glUniform1f(m_uOpacity, opacity);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::compose(const LayerStack& layers, const IRect& rect, const StrokePreview* preview) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(m_composite.fbo.id(), area);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < layers.count(); ++i) {
        const Layer& layer = layers.at(i);
        if (!layer.visible || layer.opacity <= 0.0f) {
            continue;
        }
        const bool withStroke = preview && preview->texture && i == layers.activeIndex();
        drawQuad(layer.target.texture.id(), layer.opacity, withStroke ? preview : nullptr);
    }

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Compositor::draw(GLuint target, GLuint source, float opacity, Blend blend, const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(target, area);
    if (blend == Blend::Over) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        // destino *= 1 - alfa de la fuente
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    }
    drawQuad(source, opacity, nullptr);

    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Compositor::scale(GLuint target, float factor, const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(m_composite.width, m_composite.height));
    if (area.empty() || !m_program) {
        return;
    }

    bindCanvasPass(target, area);
    // destino *= factor. La salida del shader no cuenta (GL_ZERO); se muestrea la textura
    // 0 para no leer del FBO en el que se escribe.
    glBlendColor(factor, factor, factor, factor);
    glBlendFunc(GL_ZERO, GL_CONSTANT_COLOR);
    drawQuad(0, 1.0f, nullptr);

    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
