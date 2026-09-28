#include "Canvas/Canvas.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <new>
#include <string>

namespace {

// Memoria de GPU que pueden ocupar las capas. En un móvil la GPU usa la RAM del sistema:
// pasarse no da un error limpio, el sistema mata la app y se pierde el dibujo.
constexpr size_t kLayerMemoryBudget = size_t{768} * 1024 * 1024;
constexpr int kMinLayerLimit = 4;
constexpr int kMaxLayerLimit = 64;

// Alfa de cada dab con presión 1 (los valores de la versión anterior).
constexpr float kPaintFlow = 0.5f;
constexpr float kEraseFlow = 1.0f;

size_t layerBytes(int width, int height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

} // namespace

bool Canvas::init(int width, int height) {
    destroy();
    if (width <= 0 || height <= 0) {
        return false;
    }
    m_layers.reset(width, height);
    if (!createGpuObjects()) {
        destroy();
        return false;
    }

    Layer* background = m_layers.insert(0, "Fondo");
    Layer* first = background ? m_layers.insert(1, "") : nullptr;
    if (!first) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudieron crear las capas de %dx%d", width, height);
        destroy();
        return false;
    }
    fill(*background, 1.0f, 1.0f, 1.0f, 1.0f);

    m_layers.markAllDirty();
    m_ready = true;
    update();
    return true;
}

void Canvas::destroy() {
    m_ready = false;
    m_stroking = false;
    m_strokeBounds = {};
    m_layers.clearAll();
    destroyGpuObjects();
    m_snapshot.clear();
}

bool Canvas::createGpuObjects() {
    const int w = m_layers.width();
    const int h = m_layers.height();
    if (!m_compositor.init(w, h) || !m_brush.init() || !m_strokeTarget.create(w, h)) {
        destroyGpuObjects();
        return false;
    }
    // Sin la textura del pincel no se puede pintar, pero el resto de la app funciona.
    m_brush.setType(m_settings.type);
    return true;
}

void Canvas::destroyGpuObjects() {
    m_strokeTarget.destroy();
    m_brush.destroy();
    m_compositor.destroy();
}

void Canvas::fill(Layer& layer, float r, float g, float b, float a) {
    glBindFramebuffer(GL_FRAMEBUFFER, layer.target.fbo.id());
    glViewport(0, 0, layer.target.width, layer.target.height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Canvas::setBrushType(int type) {
    endStroke();
    if (!m_brush.setType(type)) {
        return false;
    }
    m_settings.type = type;
    return true;
}

// -----------------------------------------------------------------------------
// Trazo
// -----------------------------------------------------------------------------

bool Canvas::beginStroke(float x, float y, float pressure, bool eraserTip) {
    if (!m_ready) {
        return false;
    }
    endStroke();
    if (!m_layers.active().visible) {
        return false;
    }

    m_stroking = true;
    m_strokeErase = m_settings.eraser || eraserTip;
    m_strokeOpacity = std::clamp(m_settings.opacity, 0.0f, 1.0f);
    std::copy(m_settings.color, m_settings.color + 3, m_strokeColor);
    m_brush.beginStroke(x, y, pressure, m_settings.radius, m_strokeErase ? kEraseFlow : kPaintFlow);
    return true;
}

void Canvas::strokeTo(float x, float y, float pressure) {
    if (m_stroking) {
        m_brush.strokeTo(x, y, pressure);
    }
}

void Canvas::endStroke() {
    if (!m_stroking) {
        return;
    }
    flushDabs();
    commitStroke();
}

void Canvas::cancelStroke() {
    if (!m_stroking) {
        return;
    }
    m_brush.discardPending();
    clearStrokeBuffer(m_strokeBounds);
    m_layers.markDirty(m_strokeBounds);
    m_strokeBounds = {};
    m_stroking = false;
}

void Canvas::flushDabs() {
    if (!m_brush.hasPendingDabs()) {
        return;
    }
    const IRect touched = m_brush.flush(m_strokeTarget.fbo.id(), width(), height(), m_strokeColor);
    m_strokeBounds.unite(touched);
    m_layers.markDirty(touched);
}

void Canvas::commitStroke() {
    const IRect bounds = m_strokeBounds.intersected(IRect::ofSize(width(), height()));
    if (!bounds.empty()) {
        Layer& layer = m_layers.active();
        m_compositor.draw(layer.target.fbo.id(), m_strokeTarget.texture.id(), m_strokeOpacity,
                          m_strokeErase ? Compositor::Blend::Erase : Compositor::Blend::Over, bounds);
        clearStrokeBuffer(bounds);
        m_layers.markDirty(bounds);
    }
    m_strokeBounds = {};
    m_stroking = false;
}

void Canvas::clearStrokeBuffer(const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(width(), height()));
    if (area.empty()) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_strokeTarget.fbo.id());
    glViewport(0, 0, width(), height());
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// -----------------------------------------------------------------------------
// Capas
// -----------------------------------------------------------------------------

int Canvas::maxLayers() const {
    const size_t bytes = layerBytes(width(), height());
    const size_t byBudget = bytes > 0 ? kLayerMemoryBudget / bytes : static_cast<size_t>(kMaxLayerLimit);
    const int limit = static_cast<int>(std::min(byBudget, static_cast<size_t>(kMaxLayerLimit)));
    return std::max(limit, kMinLayerLimit);
}

void Canvas::selectLayer(int index) {
    if (!m_layers.validIndex(index) || index == m_layers.activeIndex()) {
        return;
    }
    endStroke();
    m_layers.setActive(index);
}

bool Canvas::addLayer() {
    if (!m_ready || m_layers.count() >= maxLayers()) {
        return false;
    }
    endStroke();
    return m_layers.insert(m_layers.activeIndex() + 1, "") != nullptr;
}

bool Canvas::duplicateLayer(int index) {
    if (!m_ready || !m_layers.validIndex(index) || m_layers.count() >= maxLayers()) {
        return false;
    }
    endStroke();

    const Layer& source = m_layers.at(index);
    const GLuint sourceFbo = source.target.fbo.id();
    const bool visible = source.visible;
    const float opacity = source.opacity;
    Layer* copy = m_layers.insert(index + 1, m_layers.availableName(source.name + " copia"));
    if (!copy) {
        return false;
    }
    copy->visible = visible;
    copy->opacity = opacity;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy->target.fbo.id());
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, width(), height(), 0, 0, width(), height(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_layers.markAllDirty();
    return true;
}

bool Canvas::removeLayer(int index) {
    if (!m_ready) {
        return false;
    }
    endStroke();
    return m_layers.remove(index);
}

bool Canvas::moveLayer(int from, int to) {
    if (!m_ready) {
        return false;
    }
    endStroke();
    return m_layers.move(from, to);
}

bool Canvas::canMergeDown(int index) const {
    // Solo si las dos se ven: fundir con una capa oculta cambiaría lo que se ve.
    return m_layers.validIndex(index) && index > 0 && m_layers.at(index).visible && m_layers.at(index - 1).visible;
}

bool Canvas::mergeDown(int index) {
    if (!m_ready || !canMergeDown(index)) {
        return false;
    }
    endStroke();

    const Layer& upper = m_layers.at(index);
    Layer& lower = m_layers.at(index - 1);
    m_compositor.draw(lower.target.fbo.id(), upper.target.texture.id(), upper.opacity, Compositor::Blend::Over,
                      IRect::ofSize(width(), height()));
    m_layers.remove(index);
    m_layers.setActive(index - 1);
    return true;
}

void Canvas::clearLayer(int index) {
    if (!m_ready || !m_layers.validIndex(index)) {
        return;
    }
    endStroke();
    fill(m_layers.at(index), 0.0f, 0.0f, 0.0f, 0.0f);
    m_layers.markAllDirty();
}

// -----------------------------------------------------------------------------
// Compuesto
// -----------------------------------------------------------------------------

bool Canvas::update() {
    if (!m_ready) {
        return false;
    }
    flushDabs();
    const IRect dirty = m_layers.dirty();
    if (dirty.empty()) {
        return false;
    }

    StrokePreview preview;
    preview.texture = m_strokeTarget.texture.id();
    preview.opacity = m_strokeOpacity;
    preview.erase = m_strokeErase;
    m_compositor.compose(m_layers, dirty, m_stroking ? &preview : nullptr);

    m_layers.clearDirty();
    ++m_version;
    return true;
}

bool Canvas::readComposite(std::vector<uint8_t>& pixels) {
    if (!m_ready) {
        return false;
    }
    update();
    const gfx::RenderTarget& target = m_compositor.composite();
    try {
        pixels.resize(layerBytes(target.width, target.height));
    } catch (const std::bad_alloc&) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Sin memoria para leer el lienzo");
        return false;
    }

    gfx::clearErrors();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return gfx::checkErrors("Canvas::readComposite");
}

// -----------------------------------------------------------------------------
// Pérdida de contexto
// -----------------------------------------------------------------------------

bool Canvas::takeSnapshot(size_t maxBytes) {
    m_snapshot.clear();
    if (!m_ready) {
        return false;
    }
    endStroke();

    const size_t bytes = layerBytes(width(), height());
    const size_t total = bytes * static_cast<size_t>(m_layers.count());
    if (total > maxBytes) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Copia de seguridad omitida: %zu MB superan el límite de %zu MB",
                    total >> 20, maxBytes >> 20);
        return false;
    }

    try {
        m_snapshot.resize(static_cast<size_t>(m_layers.count()));
        for (auto& pixels : m_snapshot) {
            pixels.resize(bytes);
        }
    } catch (const std::bad_alloc&) {
        m_snapshot.clear();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Copia de seguridad omitida: sin memoria");
        return false;
    }

    gfx::clearErrors();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int i = 0; i < m_layers.count(); ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_layers.at(i).target.fbo.id());
        glReadPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, m_snapshot[static_cast<size_t>(i)].data());
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!gfx::checkErrors("Canvas::takeSnapshot")) {
        m_snapshot.clear();
        return false;
    }
    return true;
}

void Canvas::dropSnapshot() {
    m_snapshot.clear();
    m_snapshot.shrink_to_fit();
}

bool Canvas::recreateGpu(bool* restored) {
    if (restored) {
        *restored = false;
    }
    if (m_layers.count() == 0) {
        return false;
    }

    // Los objetos del contexto anterior ya no existen: se olvidan sin borrarlos.
    m_ready = false;
    m_stroking = false;
    m_strokeBounds = {};
    destroyGpuObjects();
    if (!createGpuObjects()) {
        return false;
    }

    const bool fromSnapshot = m_snapshot.size() == static_cast<size_t>(m_layers.count());
    for (int i = 0; i < m_layers.count(); ++i) {
        Layer& layer = m_layers.at(i);
        const void* pixels = fromSnapshot ? m_snapshot[static_cast<size_t>(i)].data() : nullptr;
        if (!layer.target.create(width(), height(), pixels)) {
            return false;
        }
        if (!fromSnapshot && i == 0) {
            fill(layer, 1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
    dropSnapshot();

    m_layers.markAllDirty();
    m_ready = true;
    update();
    if (restored) {
        *restored = fromSnapshot;
    }
    return true;
}
