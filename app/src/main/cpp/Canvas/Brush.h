#pragma once

#include "Canvas/BrushLibrary.h"
#include "Canvas/Rect.h"
#include "Canvas/StrokePath.h"
#include "Gfx/GLObjects.h"

#include <array>
#include <cstdint>
#include <span>

// Pincel con el que pinta el lienzo, como lo deja el menú.
struct BrushSettings {
    BrushParams brush = brushes::basic();   // cómo pinta (de la biblioteca, con sus ajustes)
    float radius = 10.0f;                    // radio en píxeles del lienzo con presión 1
    float color[3] = {0.0f, 0.0f, 0.0f};     // RGB 0..1
    float opacity = 1.0f;                    // opacidad del trazo completo (Difuminar: su fuerza)
    bool eraser = false;
    bool smudge = false;                     // Difuminar: arrastra lo que hay, sin poner color
};

// Cómo mezcla un trazo húmedo (o Difuminar) cada sello con lo que ya hay en la capa.
struct WetMix {
    float color[3] = {0.0f, 0.0f, 0.0f};   // color del pincel
    bool smudge = false;       // Difuminar: arrastra todo lo que hay y no pone color
    float radius = 10.0f;      // radio del pincel: la carga dura más con uno grande
    float strength = 1.0f;     // opacidad del trazo (en Difuminar, su fuerza)
    bool alphaLock = false;    // el alfa de la capa no cambia
    GLuint selection = 0;      // máscara R8 del tamaño del lienzo; 0: todo
    GLuint grain = 0;          // grano del papel (blanco con el relieve en el alfa); 0: sin grano
    float grainScale = 1.0f;   // uv del grano por píxel del lienzo
    float grainDepth = 0.0f;
    // La capa antes del trazo (no puede ser el destino): sobre lo que ya estaba pintado, el
    // arrastre mezcla en vez de tapar. 0: nada pintado.
    GLuint original = 0;
};

// Por dónde va un trazo húmedo: cada sello arrastra lo que había donde estaba el anterior.
struct WetCursor {
    float x = 0.0f;
    float y = 0.0f;
    bool started = false;
    uint32_t count = 0;        // sellos hechos (el azar del tramado)
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

    // Mezcla húmeda y Difuminar: pinta `dabs` directamente sobre `target` (RGBA
    // premultiplicado, del tamaño del lienzo) de uno en uno, porque cada sello parte de lo
    // que dejó el anterior. Cada uno mezcla lo que hay con lo que arrastra desde donde
    // estaba el sello anterior (el arrastre del pincel, o todo en Difuminar) y pone color
    // según la pintura que le queda. `coverage` (del mismo tamaño, en su rojo; a 0 al
    // empezar el trazo) guarda lo que el trazo ha cubierto en cada punto, como el alfa de un
    // trazo normal: una pasada deja esa parte de la pintura, así que la carga y la dilución
    // se notan igual con cualquier punta y espaciado. Sin él, cada sello pinta como si fuera
    // el primero (Difuminar no pinta y no lo necesita). `target` tiene que tener la capa en
    // toda la zona que leen (wetBounds). Devuelve la zona que cambió.
    IRect drawWet(const gfx::RenderTarget& target, const gfx::RenderTarget* coverage, std::span<const Dab> dabs,
                  const BrushParams& params, const WetMix& mix, WetCursor& cursor);
    // Zona que leen y escriben esos sellos (sin recortar al lienzo).
    static IRect wetBounds(std::span<const Dab> dabs, const WetCursor& cursor);

    // Texturas blancas con la forma en el alfa, para mostrarlas en la interfaz. La del
    // grano se repite (GL_REPEAT). 0 si no hay.
    GLuint tipTexture(BrushTip tip);
    GLuint grainTexture(BrushGrain grain);

    // Multiplica `target` por el grano de `params` (fijo a sus píxeles) dentro de `rect`.
    // El lienzo lo aplica al componer el trazo; esto es para las muestras de la interfaz.
    void applyGrain(GLuint target, int width, int height, const BrushParams& params, const IRect& rect);

private:
    // Texturas para la copia de la zona de un sello húmedo (color y cobertura), de al
    // menos width×height.
    bool ensureWetPatch(int width, int height);

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

    gfx::Program m_wetProgram;
    struct WetUniforms {
        GLint patchRect = -1;
        GLint patchTexel = -1;
        GLint center = -1;
        GLint radius = -1;
        GLint angle = -1;
        GLint roundness = -1;
        GLint flip = -1;
        GLint round = -1;
        GLint hardness = -1;
        GLint alpha = -1;
        GLint delta = -1;
        GLint drag = -1;
        GLint paint = -1;
        GLint pickup = -1;
        GLint originalOn = -1;
        GLint coverageOn = -1;
        GLint color = -1;
        GLint alphaLock = -1;
        GLint selectionOn = -1;
        GLint grainOn = -1;
        GLint grainScale = -1;
        GLint grainDepth = -1;
        GLint seed = -1;
    } m_wet;
    gfx::Framebuffer m_wetFbo;   // el destino y su cobertura, para pintar los dos a la vez
    gfx::Texture m_wetPatch;
    gfx::Texture m_wetCoveragePatch;
    int m_wetPatchWidth = 0;
    int m_wetPatchHeight = 0;
};
