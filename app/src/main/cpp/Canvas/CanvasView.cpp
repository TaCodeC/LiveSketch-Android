#include "Canvas/CanvasView.h"

#include "Gfx/Shader.h"

#include <algorithm>

namespace {

// Gris de fondo alrededor del lienzo (el mismo que ui::theme::kBackground).
constexpr float kBackground[3] = {21.0f / 255.0f, 21.0f / 255.0f, 23.0f / 255.0f};

// Sombra del lienzo, en puntos: una grande y difusa y otra corta de contacto.
constexpr float kShadowSigma = 34.0f;
constexpr float kShadowOffset = 24.0f;
constexpr float kShadowAlpha = 0.5f;
constexpr float kContactSigma = 4.0f;
constexpr float kContactOffset = 2.0f;
constexpr float kContactAlpha = 0.35f;

// Un quad de (0,0) a (1,1) que cada programa coloca en píxeles de la ventana (origen
// arriba a la izquierda).
constexpr const char* kShadowVertex = R"(
layout(location = 0) in vec2 aCorner;
uniform vec4 uRect;
uniform vec2 uViewport;
uniform float uExtent;
out vec2 vPos;
void main() {
    vec2 p = uRect.xy - vec2(uExtent) + aCorner * (uRect.zw + 2.0 * uExtent);
    vPos = p;
    gl_Position = vec4(p.x / uViewport.x * 2.0 - 1.0, 1.0 - p.y / uViewport.y * 2.0, 0.0, 1.0);
}
)";

// Sombra gaussiana exacta de un rectángulo (integral separable con erf), dos a la vez.
constexpr const char* kShadowFragment = R"(
in vec2 vPos;
uniform vec4 uRect;
uniform vec4 uParams;
uniform vec2 uAlpha;
out vec4 fragColor;
vec4 erf4(vec4 x) {
    vec4 s = sign(x);
    vec4 a = abs(x);
    x = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    x *= x;
    return s - s / (x * x);
}
float boxShadow(vec2 lower, vec2 upper, vec2 point, float sigma) {
    vec4 query = vec4(point - lower, point - upper);
    vec4 integral = 0.5 + 0.5 * erf4(query * (0.70710678 / sigma));
    return (integral.z - integral.x) * (integral.w - integral.y);
}
void main() {
    vec2 lower = uRect.xy;
    vec2 upper = uRect.xy + uRect.zw;
    float a1 = uAlpha.x * boxShadow(lower + vec2(0.0, uParams.y), upper + vec2(0.0, uParams.y), vPos, uParams.x);
    float a2 = uAlpha.y * boxShadow(lower + vec2(0.0, uParams.w), upper + vec2(0.0, uParams.w), vPos, uParams.z);
    fragColor = vec4(0.0, 0.0, 0.0, 1.0 - (1.0 - a1) * (1.0 - a2));
}
)";

// uRect: x, y, ancho, alto del lienzo en píxeles de la ventana. La fila 0 del compuesto
// es la de arriba, así que v = 0 va arriba.
constexpr const char* kCanvasVertex = R"(
layout(location = 0) in vec2 aCorner;
uniform vec4 uRect;
uniform vec2 uViewport;
out vec2 vUV;
void main() {
    vec2 screen = uRect.xy + aCorner * uRect.zw;
    gl_Position = vec4(screen.x / uViewport.x * 2.0 - 1.0, 1.0 - screen.y / uViewport.y * 2.0, 0.0, 1.0);
    vUV = aCorner;
}
)";

// Lo transparente del lienzo se ve sobre un damero, como en cualquier editor.
constexpr const char* kCanvasFragment = R"(
in vec2 vUV;
uniform sampler2D uCanvas;
uniform float uCell;
out vec4 fragColor;
void main() {
    vec4 color = texture(uCanvas, vUV);
    vec2 cell = floor(gl_FragCoord.xy / uCell);
    float checker = mix(0.8, 1.0, mod(cell.x + cell.y, 2.0));
    fragColor = vec4(color.rgb + vec3(checker) * (1.0 - color.a), 1.0);
}
)";

} // namespace

bool CanvasView::init() {
    destroy();

    m_shadowProgram = gfx::makeProgram("canvas shadow", kShadowVertex, kShadowFragment);
    m_canvasProgram = gfx::makeProgram("canvas view", kCanvasVertex, kCanvasFragment);
    if (!m_shadowProgram || !m_canvasProgram) {
        destroy();
        return false;
    }
    m_uShadowRect = glGetUniformLocation(m_shadowProgram.id(), "uRect");
    m_uShadowViewport = glGetUniformLocation(m_shadowProgram.id(), "uViewport");
    m_uShadowParams = glGetUniformLocation(m_shadowProgram.id(), "uParams");
    m_uShadowAlpha = glGetUniformLocation(m_shadowProgram.id(), "uAlpha");
    m_uShadowExtent = glGetUniformLocation(m_shadowProgram.id(), "uExtent");
    m_uRect = glGetUniformLocation(m_canvasProgram.id(), "uRect");
    m_uViewport = glGetUniformLocation(m_canvasProgram.id(), "uViewport");
    m_uCanvas = glGetUniformLocation(m_canvasProgram.id(), "uCanvas");
    m_uCell = glGetUniformLocation(m_canvasProgram.id(), "uCell");

    const float corners[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    m_vao = gfx::VertexArray::create();
    m_vbo = gfx::Buffer::create();
    glBindVertexArray(m_vao.id());
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo.id());
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void CanvasView::destroy() {
    m_vbo.reset();
    m_vao.reset();
    m_canvasProgram.reset();
    m_shadowProgram.reset();
}

void CanvasView::draw(const Camera& camera, GLuint compositeTexture, GLuint fbo, int width, int height, float scale) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(kBackground[0], kBackground[1], kBackground[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (compositeTexture == 0 || !m_canvasProgram) {
        return;
    }

    const glm::vec2 origin = camera.offset();
    const glm::vec2 size = camera.canvasSize() * camera.zoom();
    const glm::vec2 viewport = camera.viewport();
    glBindVertexArray(m_vao.id());

    // Sombra: solo alrededor, el lienzo es opaco y la tapa.
    const float ppp = m_pixelsPerPoint;
    const float sigma = kShadowSigma * ppp;
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_shadowProgram.id());
    glUniform4f(m_uShadowRect, origin.x, origin.y, size.x, size.y);
    glUniform2f(m_uShadowViewport, viewport.x, viewport.y);
    glUniform4f(m_uShadowParams, sigma, kShadowOffset * ppp, kContactSigma * ppp, kContactOffset * ppp);
    glUniform2f(m_uShadowAlpha, kShadowAlpha, kContactAlpha);
    glUniform1f(m_uShadowExtent, sigma * 3.0f + kShadowOffset * ppp);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);

    glUseProgram(m_canvasProgram.id());
    glUniform4f(m_uRect, origin.x, origin.y, size.x, size.y);
    glUniform2f(m_uViewport, viewport.x, viewport.y);
    glUniform1i(m_uCanvas, 0);
    glUniform1f(m_uCell, std::max(8.0f * scale, 1.0f));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, compositeTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}
