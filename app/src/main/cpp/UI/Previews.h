#pragma once

#include "Canvas/Brush.h"
#include "Canvas/LayerStack.h"
#include "Gfx/GLObjects.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Imágenes pequeñas de la interfaz que se dibujan con la GPU: la miniatura de cada capa
// y el trazo de muestra de cada pincel. Se rehacen solo cuando cambian.
//
// Las texturas tienen la fila 0 arriba: en ImGui se dibujan con uv de (0,0) a (1,1).
class Previews {
public:
    bool init();
    void destroy();

    // Miniatura opaca de la capa (sobre un damero) de `width`×`height` píxeles.
    // `checkerCell`: lado de las casillas del damero en píxeles.
    GLuint layerThumbnail(const Layer& layer, int width, int height, int checkerCell);
    // Olvida las miniaturas de las capas que ya no están en la pila.
    void pruneThumbnails(const LayerStack& layers);

    // Trazo de muestra de `params` (una "S" con la presión subiendo y bajando): blanco con
    // alfa (sin premultiplicar), para teñirlo en ImGui. `slot` identifica la muestra (un
    // pincel de la lista, la del panel de ajustes): se rehace solo si cambian los ajustes
    // o el tamaño.
    GLuint brushPreview(int slot, const BrushParams& params, int width, int height);
    // Punta y grano para las fichas del panel de ajustes (blancas con la forma en el alfa).
    GLuint tipTexture(BrushTip tip) { return m_brushReady ? m_brush.tipTexture(tip) : 0; }
    GLuint grainTexture(BrushGrain grain) { return m_brushReady ? m_brush.grainTexture(grain) : 0; }

private:
    struct Thumbnail {
        gfx::RenderTarget target;
        uint64_t revision = 0;
    };
    struct Stroke {
        gfx::RenderTarget target;
        BrushParams params;
    };

    gfx::Program m_program;
    GLint m_uSource = -1;
    GLint m_uTexel = -1;
    GLint m_uFootprint = -1;
    GLint m_uTaps = -1;
    GLint m_uCell = -1;
    gfx::VertexArray m_vao;
    gfx::Buffer m_vbo;

    std::unordered_map<uint32_t, Thumbnail> m_thumbnails;   // por id de capa
    Brush m_brush;
    bool m_brushReady = false;
    StrokePath m_path;
    std::vector<Dab> m_dabs;
    std::unordered_map<int, Stroke> m_strokes;   // por `slot`
};
