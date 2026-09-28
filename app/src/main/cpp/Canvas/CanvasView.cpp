#include "Canvas/CanvasView.h"

#include "Gfx/Shader.h"

namespace {

// Esquinas de un quad de (0,0) a (1,1). El fondo lo usa como pantalla completa.
constexpr const char* kGridVertex = R"(
layout(location = 0) in vec2 aCorner;
void main() {
    gl_Position = vec4(aCorner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// La misma cuadrícula que la versión anterior.
constexpr const char* kGridFragment = R"(
const vec3 BG_COLOR = vec3(0.05, 0.05, 0.05);
const vec3 GRID_COLOR = vec3(0.09, 0.09, 0.09);
const float CELL_SIZE = 35.0;
const float LINE_WIDTH = 1.0;
out vec4 fragColor;
void main() {
    vec2 cellPos = mod(gl_FragCoord.xy, CELL_SIZE);
    float lineX = step(cellPos.x, LINE_WIDTH) + step(CELL_SIZE - cellPos.x, LINE_WIDTH);
    float lineY = step(cellPos.y, LINE_WIDTH) + step(CELL_SIZE - cellPos.y, LINE_WIDTH);
    float isLine = clamp(lineX + lineY, 0.0, 1.0);
    fragColor = vec4(mix(BG_COLOR, GRID_COLOR, isLine), 1.0);
}
)";

// uRect: x, y, ancho, alto del lienzo en píxeles de la ventana (origen arriba a la
// izquierda). La fila 0 del compuesto es la de arriba, así que v = 0 va arriba.
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
out vec4 fragColor;
void main() {
    vec4 color = texture(uCanvas, vUV);
    vec2 cell = floor(gl_FragCoord.xy / 8.0);
    float checker = mix(0.8, 1.0, mod(cell.x + cell.y, 2.0));
    fragColor = vec4(color.rgb + vec3(checker) * (1.0 - color.a), 1.0);
}
)";

} // namespace

bool CanvasView::init() {
    destroy();

    m_gridProgram = gfx::makeProgram("grid", kGridVertex, kGridFragment);
    m_canvasProgram = gfx::makeProgram("canvas view", kCanvasVertex, kCanvasFragment);
    if (!m_gridProgram || !m_canvasProgram) {
        destroy();
        return false;
    }
    m_uRect = glGetUniformLocation(m_canvasProgram.id(), "uRect");
    m_uViewport = glGetUniformLocation(m_canvasProgram.id(), "uViewport");
    m_uCanvas = glGetUniformLocation(m_canvasProgram.id(), "uCanvas");

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
    m_gridProgram.reset();
}

void CanvasView::draw(const Camera& camera, GLuint compositeTexture, int windowWidth, int windowHeight) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowWidth, windowHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    if (!m_gridProgram) {
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    glBindVertexArray(m_vao.id());
    glUseProgram(m_gridProgram.id());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (compositeTexture != 0) {
        const glm::vec2 origin = camera.offset();
        const glm::vec2 size = camera.canvasSize() * camera.zoom();
        glUseProgram(m_canvasProgram.id());
        glUniform4f(m_uRect, origin.x, origin.y, size.x, size.y);
        glUniform2f(m_uViewport, static_cast<float>(windowWidth), static_cast<float>(windowHeight));
        glUniform1i(m_uCanvas, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, compositeTexture);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glBindVertexArray(0);
}
