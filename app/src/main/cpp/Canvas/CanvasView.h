#pragma once

#include "Canvas/Camera.h"
#include "Gfx/ColorSpace.h"
#include "Gfx/GLObjects.h"
#include "Gfx/Shader.h"

// Dibuja la escena: el fondo liso, la sombra del lienzo y, encima, el lienzo compuesto
// (con un damero donde es transparente) en la posición, con el zoom y con el giro y el
// volteo de la cámara.
class CanvasView {
public:
    bool init();
    void destroy();

    // Medidas de la sombra según la densidad de la interfaz (píxeles por punto).
    void setPixelsPerPoint(float pixelsPerPoint) { m_pixelsPerPoint = pixelsPerPoint; }
    // Del perfil del lienzo al de la pantalla.
    void setGamut(const colorspace::Transform& gamut) { m_gamut = gamut; }
    const colorspace::Transform& gamut() const { return m_gamut; }

    // Dibuja en `fbo` (0: la ventana), de `width`×`height` píxeles. `scale` son píxeles
    // de destino por píxel de la ventana: 1 en la ventana, menos para el fondo desenfocado
    // de la interfaz. La cámara siempre trabaja en píxeles de la ventana.
    void draw(const Camera& camera, GLuint compositeTexture, GLuint fbo, int width, int height, float scale = 1.0f);

    // Borde de la selección encima del lienzo (solo en la ventana: no sale por NDI ni en
    // el PNG): una línea discontinua blanca y negra donde la máscara `mask` (R8, del
    // tamaño del lienzo) pasa por la mitad, que avanza con `phase` (en pasos). `veil`
    // (0..1) oscurece lo que no está seleccionado.
    void drawSelection(const Camera& camera, GLuint mask, int phase, float veil, GLuint fbo, int width, int height);

private:
    // Dónde va el lienzo en la ventana (ver kCanvasVertex).
    struct Placement {
        float origin[2];
        float axes[4];
    };
    static Placement placement(const Camera& camera);
    static void setPlacement(GLint origin, GLint axes, const Placement& placement);

    gfx::Program m_shadowProgram;
    GLint m_uShadowCenter = -1;
    GLint m_uShadowSize = -1;
    GLint m_uShadowRotation = -1;
    GLint m_uShadowDown = -1;
    GLint m_uShadowViewport = -1;
    GLint m_uShadowParams = -1;   // sigma grande, desplazamiento grande, sigma pequeña, desplazamiento pequeño
    GLint m_uShadowAlpha = -1;
    GLint m_uShadowExtent = -1;

    gfx::Program m_canvasProgram;
    GLint m_uOrigin = -1;
    GLint m_uAxes = -1;
    GLint m_uViewport = -1;
    GLint m_uCanvas = -1;
    GLint m_uCell = -1;
    gfx::GamutUniforms m_uGamut;
    colorspace::Transform m_gamut;

    gfx::Program m_selectionProgram;
    GLint m_uSelectionOrigin = -1;
    GLint m_uSelectionAxes = -1;
    GLint m_uSelectionViewport = -1;
    GLint m_uSelectionMask = -1;
    GLint m_uSelectionStep = -1;
    GLint m_uSelectionPhase = -1;
    GLint m_uSelectionDash = -1;
    GLint m_uSelectionWidth = -1;
    GLint m_uSelectionVeil = -1;

    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;
    float m_pixelsPerPoint = 1.0f;
};
