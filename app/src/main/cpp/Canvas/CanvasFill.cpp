// Relleno: arrastrar el color al lienzo.
//
// La zona es la de la selección automática: la inundación desde el punto da a cada píxel
// un nivel (lo distinto que es su color del de partida, por el camino más parecido) y el
// umbral decide hasta qué nivel entra. Los niveles se calculan una vez; cambiar el umbral
// solo vuelve a dibujar el relleno en la GPU (ColorFill).
//
// Mientras se ajusta, el resultado se dibuja en el buffer de trazo, que se ve en lugar de
// la capa donde tiene la vista previa (como la copia de trabajo de un trazo húmedo). Al
// aplicarlo, lo que cambió pasa a la capa en un paso de deshacer.
#include "Canvas/Canvas.h"

#include <algorithm>
#include <cmath>

namespace {

size_t bytesOf(int width, int height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

void copyRect(GLuint source, const IRect& from, GLuint target, int toX, int toY) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(from.x0, from.y0, from.x1, from.y1, toX, toY, toX + from.width(), toY + from.height(),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

} // namespace

Canvas::Edit Canvas::beginFill(float x, float y, float threshold) {
    if (!m_ready || !(x >= 0.0f && y >= 0.0f && x < static_cast<float>(width()) && y < static_cast<float>(height()))) {
        return Edit::Nothing;
    }
    settle();
    const int index = m_layers.activeIndex();
    if (!layerShown(index)) {
        return Edit::Hidden;
    }
    selection::AutoLevels levels;
    if (!floodLevels(x, y, levels)) {
        return Edit::NoMemory;
    }
    if (!m_colorFill.upload(levels.levels, width(), height())) {
        return Edit::NoMemory;
    }
    const Layer& layer = m_layers.at(index);
    m_fill = {};
    m_fill.active = true;
    m_fill.layerId = layer.id;
    m_fill.levelBounds = levels.bounds;
    std::copy(m_settings.color, m_settings.color + 3, m_fill.color);
    m_fill.alphaLock = layer.alphaLock;
    setFillThreshold(threshold);
    return Edit::Done;
}

IRect Canvas::fillReach() const {
    // La zona y el píxel de alrededor que recibe el color por detrás; con selección, solo
    // lo seleccionado.
    const IRect& core = m_fill.levelBounds[static_cast<size_t>(std::clamp(m_fill.cutoff, 0, 255))];
    if (core.empty()) {
        return {};
    }
    const IRect grown{core.x0 - 1, core.y0 - 1, core.x1 + 1, core.y1 + 1};
    return grown.intersected(IRect::ofSize(width(), height())).intersected(operationRect());
}

void Canvas::setFillThreshold(float threshold) {
    if (!m_fill.active) {
        return;
    }
    const int cutoff = selection::autoCutoff(threshold);
    if (cutoff == m_fill.cutoff) {
        return;
    }
    m_fill.cutoff = cutoff;
    drawFill();
}

void Canvas::drawFill() {
    const int index = m_layers.indexOf(m_fill.layerId);
    if (index < 0) {
        return;
    }
    // También lo que se rellenó con el umbral anterior: ahí vuelve a estar la capa.
    IRect area = m_fill.drawn;
    area.unite(fillReach());
    if (area.empty()) {
        return;
    }
    m_colorFill.draw(m_strokeTarget.fbo.id(), width(), height(), m_layers.at(index).target.texture.id(), area,
                     m_fill.cutoff, m_fill.color, m_fill.alphaLock, operationMask());
    m_fill.drawn = area;
    m_layers.markDirty(area);
}

bool Canvas::endFill(bool apply) {
    if (!m_fill.active) {
        return false;
    }
    const int index = m_layers.indexOf(m_fill.layerId);
    const IRect bounds = fillReach();
    m_fill.active = false;
    m_colorFill.drop();
    const bool applied = apply && index >= 0 && !bounds.empty();
    if (applied) {
        // La vista previa ya tiene el resultado.
        Layer& layer = m_layers.at(index);
        HistoryStep step;
        step.kind = HistoryStep::Kind::Pixels;
        step.layerId = layer.id;
        step.rect = bounds;
        const bool saved = saveRegion(layer, bounds, step.pixels);
        glDisable(GL_SCISSOR_TEST);
        copyRect(m_strokeTarget.fbo.id(), bounds, layer.target.fbo.id(), bounds.x0, bounds.y0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ++layer.revision;
        m_layers.markDirty(bounds);
        if (saved) {
            step.bytes = bytesOf(bounds.width(), bounds.height());
            record(std::move(step));
        } else {
            dropHistory();   // sin memoria para guardarlo: lo anterior ya no se puede deshacer
        }
    }
    // El buffer de trazo vuelve a quedar transparente y la capa se ve otra vez sola.
    clearStrokeBuffer(m_fill.drawn);
    m_layers.markDirty(m_fill.drawn);
    m_fill = {};
    return applied;
}
