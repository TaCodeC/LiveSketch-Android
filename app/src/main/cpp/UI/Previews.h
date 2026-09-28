#pragma once

#include "Canvas/Brush.h"
#include "Canvas/LayerStack.h"
#include "Gfx/GLObjects.h"

#include <cstdint>
#include <unordered_map>

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

    // Trazo de muestra del pincel `type`: blanco con alfa (sin premultiplicar), para
    // teñirlo en ImGui.
    GLuint brushPreview(int type, int width, int height);

private:
    struct Thumbnail {
        gfx::RenderTarget target;
        uint64_t revision = 0;
    };
    struct Stroke {
        gfx::RenderTarget target;
        bool ready = false;
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
    Stroke m_strokes[BrushSettings::kTypeCount];
};
