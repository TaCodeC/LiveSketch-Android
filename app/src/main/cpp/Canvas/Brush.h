#pragma once

#include "Canvas/Rect.h"
#include "Gfx/GLObjects.h"

#include <array>
#include <vector>

// Ajustes del pincel que controla el menú.
struct BrushSettings {
    static constexpr int kTypeCount = 4;   // assets brush0.png .. brush3.png

    int type = 0;
    float radius = 10.0f;                  // "Grosor": radio en píxeles del lienzo con presión 1
    float color[3] = {0.0f, 0.0f, 0.0f};   // RGB 0..1
    float opacity = 1.0f;                  // opacidad del trazo completo (se aplica al fundirlo)
    bool eraser = false;
};

// Genera los "dabs" (sellos de la textura del pincel) a lo largo de un trazo y los dibuja
// en lote: todos los dabs acumulados se pintan con un único draw call instanciado.
class Brush {
public:
    static constexpr float kMinRadius = 1.0f;
    static constexpr float kMaxRadius = 200.0f;

    bool init();
    void destroy();

    // Carga (una vez) la textura brushN.png y la deja como pincel actual.
    bool setType(int type);
    int type() const { return m_type; }

    // Trazo en coordenadas del lienzo. La presión va de 0 a 1. `flow` es el alfa de cada
    // dab con presión 1 (0.5 al pintar y 1 al borrar, como en la versión anterior).
    void beginStroke(float x, float y, float pressure, float radius, float flow);
    void strokeTo(float x, float y, float pressure);
    bool hasPendingDabs() const { return !m_dabs.empty(); }
    void discardPending();

    // Pinta los dabs pendientes sobre `target` (buffer de trazo) y devuelve la zona tocada.
    IRect flush(GLuint target, int width, int height, const float color[3]);

private:
    void addDab(float x, float y, float pressure);

    // Cada dab: centro x, centro y, radio, alfa.
    std::vector<float> m_dabs;
    IRect m_pendingBounds;

    float m_radius = 10.0f;
    float m_flow = 0.5f;
    float m_lastX = 0.0f;
    float m_lastY = 0.0f;
    float m_lastPressure = 1.0f;
    float m_distanceSinceDab = 0.0f;

    int m_type = -1;
    std::array<gfx::Texture, BrushSettings::kTypeCount> m_masks;
    gfx::Program m_program;
    GLint m_uCanvasSize = -1;
    GLint m_uColor = -1;
    GLint m_uMask = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_cornerVbo;
    gfx::Buffer m_instanceVbo;
};
