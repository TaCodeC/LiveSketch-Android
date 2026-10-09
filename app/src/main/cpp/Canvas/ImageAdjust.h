#pragma once

#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

#include <cstdint>
#include <vector>

// Ajustes de imagen de una capa.
enum class Adjustment {
    HueSaturation,   // tono, saturación y brillo
    ColorBalance,    // balance de color en sombras, medios tonos y luces
    Blur,            // desenfoque gaussiano
    Sharpen,         // enfocar (máscara de enfoque)
    Noise,           // ruido: grano monocromo
};

// Valores de un ajuste; cada uno usa los suyos. Todos a cero: la capa no cambia.
struct AdjustParams {
    // Tono, saturación y brillo, de -1 a 1 (el tono, de -180° a 180°).
    float hue = 0.0f;
    float saturation = 0.0f;
    float brightness = 0.0f;
    // Balance de color, de -1 a 1: [rango][eje] con los rangos sombras, medios tonos y
    // luces, y los ejes cian-rojo, magenta-verde y amarillo-azul.
    float balance[3][3] = {};
    // Desenfoque: sigma de la gaussiana en píxeles del lienzo.
    float blur = 0.0f;
    // Enfocar, de 0 a 1.
    float sharpen = 0.0f;
    // Ruido: cantidad de 0 a 1 y tamaño del grano en píxeles del lienzo (1: un píxel).
    float noise = 0.0f;
    float noiseSize = 1.0f;
    uint32_t seed = 0;     // el mismo grano mientras se ajusta

    bool operator==(const AdjustParams&) const = default;
    // El ajuste `kind` con estos valores deja la capa igual.
    bool neutral(Adjustment kind) const;
};

// Ajustes de imagen en la GPU. El resultado se dibuja en un destino del tamaño del lienzo
// (el buffer de trazo), dentro de un rectángulo y mezclado con la capa según la máscara de
// la selección; el lienzo lo muestra en lugar de la capa mientras se ajusta.
//
// Los de color (tono, balance, ruido) cambian cada píxel sin premultiplicar y conservan su
// alfa. El desenfoque es una gaussiana separable sobre el color premultiplicado (sin halos
// oscuros en los bordes): con una sigma grande se reduce antes la zona a la mitad las veces
// que haga falta, se desenfoca ahí y se amplía con un filtro bicúbico. Enfocar suma a la
// capa su diferencia con una versión desenfocada. Leen también fuera del rectángulo (lo
// que rodea a una selección), hasta el borde del lienzo.
class ImageAdjust {
public:
    bool init();
    void destroy();

    // Dibuja en `target` (FBO del tamaño del lienzo) dentro de `rect` la capa `layer`
    // (textura del mismo tamaño, premultiplicada) con el ajuste. Con `alphaLock`, el alfa
    // de la capa no cambia (el desenfoque solo mueve el color entre lo pintado). Con
    // `mask` (R8 del tamaño del lienzo), solo en lo seleccionado y según su valor.
    // Devuelve false si no hay memoria para los intermedios del desenfoque.
    bool draw(const gfx::RenderTarget& target, const gfx::RenderTarget& layer, const IRect& rect, Adjustment kind,
              const AdjustParams& params, bool alphaLock, GLuint mask);
    // Libera los intermedios del desenfoque (al terminar de ajustar).
    void dropBuffers();

private:
    // Intermedio que se reutiliza mientras quepa; `used` es la parte con datos.
    struct Buffer {
        gfx::RenderTarget target;
        int usedWidth = 0;
        int usedHeight = 0;
    };
    bool ensure(Buffer& buffer, int width, int height);
    void drawColor(const gfx::RenderTarget& target, const gfx::RenderTarget& layer, const IRect& rect, Adjustment kind,
                   const AdjustParams& params, GLuint mask);
    bool drawBlurred(const gfx::RenderTarget& target, const gfx::RenderTarget& layer, const IRect& rect, float sigma,
                     bool sharpen, float amount, bool alphaLock, GLuint mask);
    // Una pasada del desenfoque en `buffer`: lee `source` (textura de `sourceWidth` ×
    // `sourceHeight`, de la que vale la parte `limitWidth` × `limitHeight`).
    void blurPass(Buffer& buffer, GLuint source, int sourceWidth, int sourceHeight, int limitWidth, int limitHeight,
                  const float map[4], float stepX, float stepY, float sigma);

    gfx::Program m_colorProgram;
    GLint m_uColorLayer = -1;
    GLint m_uColorMask = -1;
    GLint m_uColorMaskOn = -1;
    GLint m_uColorKind = -1;
    GLint m_uHsb = -1;
    GLint m_uBalance = -1;
    GLint m_uNoise = -1;
    GLint m_uSeed = -1;

    gfx::Program m_blurProgram;
    GLint m_uBlurSource = -1;
    GLint m_uBlurMap = -1;
    GLint m_uBlurLimit = -1;
    GLint m_uBlurStep = -1;
    GLint m_uBlurSigma = -1;
    GLint m_uBlurRadius = -1;

    gfx::Program m_finalProgram;
    GLint m_uFinalLayer = -1;
    GLint m_uFinalBlurred = -1;
    GLint m_uFinalMask = -1;
    GLint m_uFinalMaskOn = -1;
    GLint m_uFinalMode = -1;
    GLint m_uFinalLevel = -1;
    GLint m_uFinalMap = -1;
    GLint m_uFinalLimit = -1;
    GLint m_uFinalStep = -1;
    GLint m_uFinalSigma = -1;
    GLint m_uFinalRadius = -1;
    GLint m_uFinalAmount = -1;
    GLint m_uFinalAlphaLock = -1;

    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;

    Buffer m_temp;                  // pasada horizontal a tamaño completo (sigma pequeña)
    std::vector<Buffer> m_levels;   // reducciones a la mitad (sigma grande)
    Buffer m_levelTemp;             // pasada horizontal en el nivel más reducido
};
