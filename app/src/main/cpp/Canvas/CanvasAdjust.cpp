// Ajustes de imagen de la capa activa: tono, saturación y brillo, balance de color,
// desenfoque, enfocar y ruido (los dibuja ImageAdjust en la GPU).
//
// Como el relleno, mientras se cambian los valores el resultado se dibuja en el buffer de
// trazo, que se ve en lugar de la capa en la zona del ajuste (lo seleccionado o todo el
// lienzo). Al aplicarlo, esa zona pasa a la capa en un paso de deshacer.
#include "Canvas/Canvas.h"

#include <algorithm>

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

Canvas::Edit Canvas::beginAdjust(Adjustment kind, const AdjustParams& params) {
    if (!m_ready) {
        return Edit::Nothing;
    }
    settle();
    const int index = m_layers.activeIndex();
    if (!layerShown(index)) {
        return Edit::Hidden;
    }
    const IRect rect = operationRect();
    if (rect.empty()) {
        return Edit::Nothing;
    }
    const Layer& layer = m_layers.at(index);
    m_adjust = {};
    m_adjust.active = true;
    m_adjust.layerId = layer.id;
    m_adjust.kind = kind;
    m_adjust.params = params;
    // Otro grano cada vez: aplicar el ruido dos veces no repite el mismo dibujo.
    m_adjust.params.seed = 0x9e3779b9u * ++m_adjustCount;
    m_adjust.alphaLock = layer.alphaLock;
    m_adjust.rect = rect;
    if (!drawAdjust()) {
        m_imageAdjust.dropBuffers();
        m_adjust = {};
        return Edit::NoMemory;
    }
    return Edit::Done;
}

bool Canvas::setAdjust(const AdjustParams& params) {
    if (!m_adjust.active) {
        return false;
    }
    AdjustParams next = params;
    next.seed = m_adjust.params.seed;
    if (next == m_adjust.params) {
        return true;
    }
    // Si no hay memoria, se sigue viendo el resultado de antes (con sus valores).
    const AdjustParams before = m_adjust.params;
    m_adjust.params = next;
    if (!drawAdjust()) {
        m_adjust.params = before;
        return false;
    }
    return true;
}

bool Canvas::drawAdjust() {
    const int index = m_layers.indexOf(m_adjust.layerId);
    if (index < 0) {
        return true;
    }
    if (!m_imageAdjust.draw(m_strokeTarget, m_layers.at(index).target, m_adjust.rect, m_adjust.kind, m_adjust.params,
                            m_adjust.alphaLock, operationMask())) {
        return false;
    }
    m_adjust.drawn = m_adjust.rect;
    m_layers.markDirty(m_adjust.rect);
    return true;
}

void Canvas::showAdjustOriginal(bool original) {
    if (!m_adjust.active || m_adjust.original == original) {
        return;
    }
    m_adjust.original = original;
    m_layers.markDirty(m_adjust.drawn);
}

float Canvas::maxBlur() const {
    return std::max(8.0f, static_cast<float>(std::max(width(), height())) / 16.0f);
}

bool Canvas::endAdjust(bool apply) {
    if (!m_adjust.active) {
        return false;
    }
    const int index = m_layers.indexOf(m_adjust.layerId);
    const IRect bounds = m_adjust.drawn;
    m_adjust.active = false;
    const bool applied = apply && index >= 0 && !bounds.empty() && !m_adjust.params.neutral(m_adjust.kind);
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
        if (saved) {
            step.bytes = bytesOf(bounds.width(), bounds.height());
            record(std::move(step));
        } else {
            dropHistory();   // sin memoria para guardarlo: lo anterior ya no se puede deshacer
        }
    }
    // El buffer de trazo vuelve a quedar transparente y la capa se ve otra vez sola.
    clearStrokeBuffer(bounds);
    m_layers.markDirty(bounds);
    m_imageAdjust.dropBuffers();
    m_adjust = {};
    return applied;
}
