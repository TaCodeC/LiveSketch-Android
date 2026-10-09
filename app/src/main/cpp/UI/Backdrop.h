#pragma once

#include "Canvas/Camera.h"
#include "Canvas/CanvasView.h"
#include "Gfx/GLObjects.h"

#include <cstdint>

// Lo que se ve detrás del cristal de barras y paneles: la escena (fondo y lienzo) a
// baja resolución, con desenfoque gaussiano y algo más de saturación, como los
// materiales de iOS. Se rehace solo cuando cambia la escena.
class Backdrop {
public:
    bool init();
    void destroy();

    // `sigma`: radio del desenfoque en píxeles de la ventana. `sceneKey` resume lo que
    // cambia la escena (versión del lienzo, cámara...): si coincide con el anterior no
    // se vuelve a dibujar.
    void update(CanvasView& view, const Camera& camera, GLuint compositeTexture, int width, int height, float sigma,
                uint64_t sceneKey);
    // Textura RGBA con la fila 0 abajo (como la ventana): v = 1 - y / alto.
    GLuint texture() const { return m_result ? m_result->texture.id() : 0; }

private:
    bool resize(int width, int height, float sigma);
    void pass(const gfx::RenderTarget& target, GLuint source, float stepX, float stepY, float sigma, float saturation);

    gfx::Program m_program;
    GLint m_uSource = -1;
    GLint m_uStep = -1;
    GLint m_uSigma = -1;
    GLint m_uRadius = -1;
    GLint m_uSaturation = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;

    static constexpr int kMaxLevels = 6;
    gfx::RenderTarget m_levels[kMaxLevels];   // escena a 1/2 y mitades sucesivas
    int m_levelCount = 0;
    gfx::RenderTarget m_blurTemp;              // pasada horizontal
    gfx::RenderTarget m_blurred;               // pasada vertical: el resultado
    const gfx::RenderTarget* m_result = nullptr;
    float m_levelSigma = 0.0f;                 // sigma en píxeles del último nivel

    int m_width = 0;
    int m_height = 0;
    float m_sigma = 0.0f;
    uint64_t m_sceneKey = 0;
    bool m_valid = false;
};
