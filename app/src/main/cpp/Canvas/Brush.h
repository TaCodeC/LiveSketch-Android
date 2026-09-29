#pragma once

#include "Canvas/BrushLibrary.h"
#include "Canvas/Rect.h"
#include "Canvas/StrokePath.h"
#include "Gfx/GLObjects.h"

#include <array>
#include <span>

// Pincel con el que pinta el lienzo, como lo deja el menú.
struct BrushSettings {
    BrushParams brush = brushes::basic();   // cómo pinta (de la biblioteca, con sus ajustes)
    float radius = 10.0f;                    // radio en píxeles del lienzo con presión 1
    float color[3] = {0.0f, 0.0f, 0.0f};     // RGB 0..1
    float opacity = 1.0f;                    // opacidad del trazo completo (se aplica al fundirlo)
    bool eraser = false;
};

// Dibuja sellos con la GPU: todos los de una tanda con un único draw call instanciado. La
// punta redonda se calcula en el shader (con su dureza); las demás son texturas generadas
// al usarlas por primera vez (o las imágenes de los pinceles de antes).
class Brush {
public:
    bool init();
    void destroy();

    // Prepara las texturas que usa `params`. Devuelve false si falta la punta: con ese
    // pincel no se puede pintar.
    bool prepare(const BrushParams& params);

    // Pinta `dabs` en `target` (un FBO de width×height, el tamaño del lienzo) con `color`,
    // en alfa premultiplicado. Devuelve la zona que pueden haber tocado.
    IRect draw(GLuint target, int width, int height, std::span<const Dab> dabs, const BrushParams& params,
               const float color[3]);
    // Caja que cubre los sellos, en píxeles del lienzo.
    static IRect bounds(std::span<const Dab> dabs);

    // Texturas blancas con la forma en el alfa, para mostrarlas en la interfaz. La del
    // grano se repite (GL_REPEAT). 0 si no hay.
    GLuint tipTexture(BrushTip tip);
    GLuint grainTexture(BrushGrain grain);

    // Multiplica `target` por el grano de `params` (fijo a sus píxeles) dentro de `rect`.
    // El lienzo lo aplica al componer el trazo; esto es para las muestras de la interfaz.
    void applyGrain(GLuint target, int width, int height, const BrushParams& params, const IRect& rect);

private:
    std::array<gfx::Texture, kBrushTipCount> m_tips;
    std::array<bool, kBrushTipCount> m_tipFailed{};
    std::array<gfx::Texture, kBrushGrainCount> m_grains;

    gfx::Program m_program;
    GLint m_uCanvasSize = -1;
    GLint m_uRoundness = -1;
    GLint m_uPad = -1;
    GLint m_uFlip = -1;
    GLint m_uRound = -1;
    GLint m_uHardness = -1;
    GLint m_uColor = -1;
    GLint m_uTip = -1;
    gfx::Program m_grainProgram;
    GLint m_uGrain = -1;
    GLint m_uGrainScale = -1;
    GLint m_uGrainDepth = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_cornerVbo;
    gfx::Buffer m_instanceVbo;
    gfx::VertexArray m_quadVao;
};
