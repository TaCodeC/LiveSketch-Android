#include "Canvas/CanvasView.h"

#include "Gfx/Shader.h"

#include <algorithm>
#include <cmath>

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
//
// La sombra se calcula en el marco del lienzo en la pantalla (girado con la vista):
// vLocal va de 0 a uSize dentro del lienzo, en píxeles de la ventana.
constexpr const char* kShadowVertex = R"(
layout(location = 0) in vec2 aCorner;
uniform vec2 uCenter;     // centro del lienzo en la ventana
uniform vec2 uSize;       // tamaño del lienzo en la pantalla (sin girar)
uniform vec2 uRotation;   // coseno y seno del giro de la vista
uniform vec2 uViewport;
uniform float uExtent;
out vec2 vLocal;
void main() {
    vec2 local = -vec2(uExtent) + aCorner * (uSize + 2.0 * uExtent);
    vec2 d = local - uSize * 0.5;
    vec2 p = uCenter + vec2(uRotation.x * d.x - uRotation.y * d.y, uRotation.y * d.x + uRotation.x * d.y);
    vLocal = local;
    gl_Position = vec4(p.x / uViewport.x * 2.0 - 1.0, 1.0 - p.y / uViewport.y * 2.0, 0.0, 1.0);
}
)";

// Sombra gaussiana exacta de un rectángulo (integral separable con erf), dos a la vez. La
// luz viene de arriba de la pantalla: con la vista girada, la sombra cae hacia uDown.
constexpr const char* kShadowFragment = R"(
in vec2 vLocal;
uniform vec2 uSize;
uniform vec2 uDown;      // abajo de la pantalla, en el marco del lienzo
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
    float a1 = uAlpha.x * boxShadow(uDown * uParams.y, uSize + uDown * uParams.y, vLocal, uParams.x);
    float a2 = uAlpha.y * boxShadow(uDown * uParams.w, uSize + uDown * uParams.w, vLocal, uParams.z);
    fragColor = vec4(0.0, 0.0, 0.0, 1.0 - (1.0 - a1) * (1.0 - a2));
}
)";

// El lienzo en la ventana: uOrigin es dónde cae su esquina de arriba a la izquierda y
// uAxes, sus lados de arriba y de la izquierda (girados y volteados con la vista). La fila
// 0 del compuesto es la de arriba, así que v = 0 va arriba.
constexpr const char* kCanvasVertex = R"(
layout(location = 0) in vec2 aCorner;
uniform vec2 uOrigin;
uniform vec4 uAxes;   // xy: lado de arriba; zw: lado de la izquierda
uniform vec2 uViewport;
out vec2 vUV;
void main() {
    vec2 screen = uOrigin + aCorner.x * uAxes.xy + aCorner.y * uAxes.zw;
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

// Borde de la selección: los píxeles de dentro (la máscara, con filtro bilineal, vale 0,5
// o más) que tienen fuera algún vecino a hasta uWidth píxeles de la pantalla en horizontal
// o en vertical. Así la línea no se corta con ningún zoom (con una derivada se perdía
// cuando el borde caía entre dos bloques de 2 × 2 píxeles), y fuera del lienzo cuenta
// como no seleccionado: con todo seleccionado, la línea bordea el lienzo. El discontinuo
// es un damero en la pantalla (con las filas desplazadas medio trazo), así corta la línea
// vaya en la dirección que vaya.
constexpr const char* kSelectionFragment = R"(
in vec2 vUV;
uniform sampler2D uMask;
uniform vec2 uStep;     // un píxel de la pantalla en coordenadas de la máscara
uniform float uPhase;   // desplazamiento del discontinuo (px)
uniform float uDash;    // largo de cada trazo (px)
uniform float uWidth;   // grosor de la línea (px, de 1 a 3)
uniform float uVeil;
out vec4 fragColor;
bool outside(vec2 uv) {
    return any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || textureLod(uMask, uv, 0.0).r < 0.5;
}
void main() {
    float m = textureLod(uMask, vUV, 0.0).r;
    float line = 0.0;
    if (m >= 0.5) {
        for (int k = 1; k <= 3; ++k) {
            if (float(k) > uWidth) {
                break;
            }
            vec2 o = uStep * float(k);
            if (outside(vUV + vec2(o.x, 0.0)) || outside(vUV - vec2(o.x, 0.0)) ||
                outside(vUV + vec2(0.0, o.y)) || outside(vUV - vec2(0.0, o.y))) {
                line = 1.0;
                break;
            }
        }
    }
    vec2 p = gl_FragCoord.xy + vec2(uPhase);
    float white = mod(floor(p.x / uDash) + floor((p.y + uDash * 0.5) / uDash), 2.0);
    float veil = uVeil * (1.0 - m) * (1.0 - line);
    fragColor = vec4(vec3(white * line), line + veil);
}
)";

} // namespace

bool CanvasView::init() {
    destroy();

    m_shadowProgram = gfx::makeProgram("canvas shadow", kShadowVertex, kShadowFragment);
    m_canvasProgram = gfx::makeProgram("canvas view", kCanvasVertex, kCanvasFragment);
    m_selectionProgram = gfx::makeProgram("canvas selection", kCanvasVertex, kSelectionFragment);
    if (!m_shadowProgram || !m_canvasProgram || !m_selectionProgram) {
        destroy();
        return false;
    }
    m_uShadowCenter = glGetUniformLocation(m_shadowProgram.id(), "uCenter");
    m_uShadowSize = glGetUniformLocation(m_shadowProgram.id(), "uSize");
    m_uShadowRotation = glGetUniformLocation(m_shadowProgram.id(), "uRotation");
    m_uShadowDown = glGetUniformLocation(m_shadowProgram.id(), "uDown");
    m_uShadowViewport = glGetUniformLocation(m_shadowProgram.id(), "uViewport");
    m_uShadowParams = glGetUniformLocation(m_shadowProgram.id(), "uParams");
    m_uShadowAlpha = glGetUniformLocation(m_shadowProgram.id(), "uAlpha");
    m_uShadowExtent = glGetUniformLocation(m_shadowProgram.id(), "uExtent");
    m_uOrigin = glGetUniformLocation(m_canvasProgram.id(), "uOrigin");
    m_uAxes = glGetUniformLocation(m_canvasProgram.id(), "uAxes");
    m_uViewport = glGetUniformLocation(m_canvasProgram.id(), "uViewport");
    m_uCanvas = glGetUniformLocation(m_canvasProgram.id(), "uCanvas");
    m_uCell = glGetUniformLocation(m_canvasProgram.id(), "uCell");
    const GLuint selection = m_selectionProgram.id();
    m_uSelectionOrigin = glGetUniformLocation(selection, "uOrigin");
    m_uSelectionAxes = glGetUniformLocation(selection, "uAxes");
    m_uSelectionViewport = glGetUniformLocation(selection, "uViewport");
    m_uSelectionMask = glGetUniformLocation(selection, "uMask");
    m_uSelectionStep = glGetUniformLocation(selection, "uStep");
    m_uSelectionPhase = glGetUniformLocation(selection, "uPhase");
    m_uSelectionDash = glGetUniformLocation(selection, "uDash");
    m_uSelectionWidth = glGetUniformLocation(selection, "uWidth");
    m_uSelectionVeil = glGetUniformLocation(selection, "uVeil");

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
    m_selectionProgram.reset();
    m_canvasProgram.reset();
    m_shadowProgram.reset();
}

CanvasView::Placement CanvasView::placement(const Camera& camera) {
    const glm::vec2 origin = camera.canvasToScreen({0.0f, 0.0f});
    const glm::vec2 size = camera.canvasSize();
    const glm::vec2 top = camera.orient({size.x, 0.0f}) * camera.zoom();
    const glm::vec2 left = camera.orient({0.0f, size.y}) * camera.zoom();
    return {{origin.x, origin.y}, {top.x, top.y, left.x, left.y}};
}

void CanvasView::setPlacement(GLint origin, GLint axes, const Placement& placement) {
    glUniform2f(origin, placement.origin[0], placement.origin[1]);
    glUniform4f(axes, placement.axes[0], placement.axes[1], placement.axes[2], placement.axes[3]);
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

    const glm::vec2 center = camera.canvasToScreen(camera.canvasSize() * 0.5f);
    const glm::vec2 size = camera.canvasSize() * camera.zoom();
    const glm::vec2 viewport = camera.viewport();
    const float cosine = std::cos(camera.angle());
    const float sine = std::sin(camera.angle());
    glBindVertexArray(m_vao.id());

    // Sombra: solo alrededor, el lienzo es opaco y la tapa.
    const float ppp = m_pixelsPerPoint;
    const float sigma = kShadowSigma * ppp;
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_shadowProgram.id());
    glUniform2f(m_uShadowCenter, center.x, center.y);
    glUniform2f(m_uShadowSize, size.x, size.y);
    glUniform2f(m_uShadowRotation, cosine, sine);
    // Abajo de la pantalla, (0, 1), en el marco del lienzo girado.
    glUniform2f(m_uShadowDown, sine, cosine);
    glUniform2f(m_uShadowViewport, viewport.x, viewport.y);
    glUniform4f(m_uShadowParams, sigma, kShadowOffset * ppp, kContactSigma * ppp, kContactOffset * ppp);
    glUniform2f(m_uShadowAlpha, kShadowAlpha, kContactAlpha);
    glUniform1f(m_uShadowExtent, sigma * 3.0f + kShadowOffset * ppp);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);

    glUseProgram(m_canvasProgram.id());
    setPlacement(m_uOrigin, m_uAxes, placement(camera));
    glUniform2f(m_uViewport, viewport.x, viewport.y);
    glUniform1i(m_uCanvas, 0);
    glUniform1f(m_uCell, std::max(8.0f * scale, 1.0f));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, compositeTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}

void CanvasView::drawSelection(const Camera& camera, GLuint mask, int phase, float veil, GLuint fbo, int width,
                               int height) {
    if (mask == 0 || !m_selectionProgram) {
        return;
    }
    const glm::vec2 size = camera.canvasSize() * camera.zoom();
    const glm::vec2 viewport = camera.viewport();
    const float ppp = std::max(m_pixelsPerPoint, 1.0f);
    const float dash = std::round(4.0f * ppp);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(m_vao.id());
    glUseProgram(m_selectionProgram.id());
    setPlacement(m_uSelectionOrigin, m_uSelectionAxes, placement(camera));
    glUniform2f(m_uSelectionViewport, viewport.x, viewport.y);
    glUniform1i(m_uSelectionMask, 0);
    glUniform2f(m_uSelectionStep, 1.0f / std::max(size.x, 1.0f), 1.0f / std::max(size.y, 1.0f));
    // Cada paso avanza un cuarto de trazo; a las dos vueltas de damero se repite.
    glUniform1f(m_uSelectionPhase, static_cast<float>(phase % 8) * dash * 0.25f);
    glUniform1f(m_uSelectionDash, dash);
    glUniform1f(m_uSelectionWidth, std::min(std::round(ppp), 3.0f));
    glUniform1f(m_uSelectionVeil, veil);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mask);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}
