#include "Canvas/Canvas.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <new>
#include <string>

namespace {

// Memoria de GPU que pueden ocupar las capas. En un móvil la GPU usa la RAM del sistema:
// pasarse no da un error limpio, el sistema mata la app y se pierde el dibujo.
constexpr size_t kLayerMemoryBudget = size_t{768} * 1024 * 1024;
constexpr int kMinLayerLimit = 4;
constexpr int kMaxLayerLimit = 64;

// Deshacer: hasta 100 pasos y, de memoria de GPU, lo que ocupan 6 capas enteras (entre
// 48 y 192 MB). Un trazo guarda solo la zona que tocó.
constexpr int kMaxUndoSteps = 100;
constexpr size_t kMinUndoBytes = size_t{48} * 1024 * 1024;
constexpr size_t kMaxUndoBytes = size_t{192} * 1024 * 1024;
constexpr size_t kUndoLayers = 6;
// Lo que se cuenta por un paso sin píxeles.
constexpr size_t kSmallStepBytes = 256;

// Alfa de cada dab con presión 1 (los valores de la versión anterior).
constexpr float kPaintFlow = 0.5f;
constexpr float kEraseFlow = 1.0f;

size_t bytesOf(int width, int height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

// Copia un rectángulo entre FBO del mismo formato (sin escalar).
void copyRect(GLuint source, const IRect& from, GLuint target, int toX, int toY) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(from.x0, from.y0, from.x1, from.y1, toX, toY, toX + from.width(), toY + from.height(),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
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

    const size_t undoBytes = std::clamp(layerBytes() * kUndoLayers, kMinUndoBytes, kMaxUndoBytes);
    m_history.setLimits(undoBytes, kMaxUndoSteps);

    m_layers.markAllDirty();
    m_ready = true;
    update();
    return true;
}

void Canvas::destroy() {
    m_ready = false;
    m_stroking = false;
    m_strokeBounds = {};
    m_layerEdit = {};
    m_history.clear();
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

size_t Canvas::layerBytes() const { return bytesOf(width(), height()); }

void Canvas::fill(Layer& layer, float r, float g, float b, float a) {
    glBindFramebuffer(GL_FRAMEBUFFER, layer.target.fbo.id());
    glViewport(0, 0, layer.target.width, layer.target.height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++layer.revision;
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

Canvas::StrokeBlock Canvas::strokeBlock(bool eraserTip) const {
    if (!m_ready) {
        return StrokeBlock::None;
    }
    const int active = m_layers.activeIndex();
    if (!m_layers.at(active).visible) {
        return StrokeBlock::Hidden;
    }
    if (!layerShown(active)) {
        return StrokeBlock::ClipBaseHidden;
    }
    if ((m_settings.eraser || eraserTip) && m_layers.at(active).alphaLock) {
        return StrokeBlock::AlphaLocked;
    }
    return StrokeBlock::None;
}

bool Canvas::beginStroke(float x, float y, float pressure, bool eraserTip) {
    if (!m_ready) {
        return false;
    }
    endStroke();
    finishLayerEdit();
    if (strokeBlock(eraserTip) != StrokeBlock::None) {
        return false;
    }

    const bool erase = m_settings.eraser || eraserTip;
    m_stroking = true;
    if (erase) {
        m_strokeMode = StrokePreview::Mode::Erase;
    } else {
        m_strokeMode = m_layers.active().alphaLock ? StrokePreview::Mode::PaintAtop : StrokePreview::Mode::Paint;
    }
    m_strokeOpacity = std::clamp(m_settings.opacity, 0.0f, 1.0f);
    std::copy(m_settings.color, m_settings.color + 3, m_strokeColor);
    m_brush.beginStroke(x, y, pressure, m_settings.radius, erase ? kEraseFlow : kPaintFlow);
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
        HistoryStep step;
        step.kind = HistoryStep::Kind::Pixels;
        step.layerId = layer.id;
        step.rect = bounds;
        const bool saved = saveRegion(layer, bounds, step.pixels);

        Compositor::Blend blend = Compositor::Blend::Over;
        if (m_strokeMode == StrokePreview::Mode::Erase) {
            blend = Compositor::Blend::Erase;
        } else if (m_strokeMode == StrokePreview::Mode::PaintAtop) {
            blend = Compositor::Blend::Atop;
        }
        m_compositor.draw(layer.target.fbo.id(), m_strokeTarget.texture.id(), m_strokeOpacity, blend, bounds);
        clearStrokeBuffer(bounds);
        ++layer.revision;
        m_layers.markDirty(bounds);

        if (saved) {
            step.bytes = bytesOf(bounds.width(), bounds.height());
            record(std::move(step));
        } else {
            m_history.clear();   // sin memoria para guardarlo: lo anterior ya no se puede deshacer
        }
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
    const size_t bytes = layerBytes();
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
    finishLayerEdit();
    const int position = m_layers.activeIndex() + 1;
    Layer* layer = m_layers.insert(position, "");
    if (!layer) {
        return false;
    }
    HistoryStep step;
    step.kind = HistoryStep::Kind::AddLayer;
    step.layerId = layer->id;
    step.index = position;
    step.bytes = layerBytes();
    record(std::move(step));
    return true;
}

bool Canvas::duplicateLayer(int index) {
    if (!m_ready || !m_layers.validIndex(index) || m_layers.count() >= maxLayers()) {
        return false;
    }
    endStroke();
    finishLayerEdit();

    const Layer& source = m_layers.at(index);
    const GLuint sourceFbo = source.target.fbo.id();
    // La copia se ve igual que el original, pero no es la referencia (solo hay una).
    const LayerProperties props = properties(index);
    Layer* copy = m_layers.insert(index + 1, m_layers.availableName(source.name + " copia"));
    if (!copy) {
        return false;
    }
    copy->visible = props.visible;
    copy->opacity = props.opacity;
    copy->blend = props.blend;
    copy->alphaLock = props.alphaLock;
    copy->clipping = props.clipping;

    glDisable(GL_SCISSOR_TEST);
    copyRect(sourceFbo, IRect::ofSize(width(), height()), copy->target.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++copy->revision;

    HistoryStep step;
    step.kind = HistoryStep::Kind::AddLayer;
    step.layerId = copy->id;
    step.index = index + 1;
    step.bytes = layerBytes();
    record(std::move(step));

    m_layers.markAllDirty();
    return true;
}

bool Canvas::removeLayer(int index) {
    if (!m_ready || !m_layers.validIndex(index)) {
        return false;
    }
    endStroke();
    finishLayerEdit();
    const uint32_t id = m_layers.at(index).id;
    std::unique_ptr<Layer> removed = m_layers.take(index);
    if (!removed) {
        return false;
    }
    HistoryStep step;
    step.kind = HistoryStep::Kind::RemoveLayer;
    step.layerId = id;
    step.index = index;
    step.layer = std::move(removed);
    step.bytes = layerBytes();
    record(std::move(step));
    return true;
}

bool Canvas::moveLayer(int from, int to) {
    if (!m_ready || !m_layers.validIndex(from)) {
        return false;
    }
    endStroke();
    finishLayerEdit();
    const uint32_t id = m_layers.at(from).id;
    if (!m_layers.move(from, to)) {
        return false;
    }
    HistoryStep step;
    step.kind = HistoryStep::Kind::MoveLayer;
    step.layerId = id;
    step.index = from;
    step.target = to;
    step.bytes = kSmallStepBytes;
    record(std::move(step));
    return true;
}

bool Canvas::layerShown(int index) const {
    if (!m_layers.validIndex(index) || !m_layers.at(index).visible) {
        return false;
    }
    const int base = m_layers.clipBase(index);
    return base < 0 || m_layers.at(base).visible;
}

bool Canvas::canMergeDown(int index) const {
    // Solo si las dos se ven: fundir con una capa oculta cambiaría lo que se ve.
    return m_layers.validIndex(index) && index > 0 && layerShown(index) && layerShown(index - 1);
}

bool Canvas::mergeDown(int index) {
    if (!m_ready || !canMergeDown(index)) {
        return false;
    }
    endStroke();
    finishLayerEdit();

    const Layer& upper = m_layers.at(index);
    Layer& lower = m_layers.at(index - 1);
    const IRect all = IRect::ofSize(width(), height());
    // La de arriba recorta con la de abajo si esta es su base. Si recortan las dos, lo
    // hacen con la misma base y la capa fundida sigue recortando con ella. Si solo
    // recorta la de abajo, la fundida deja de recortar y su recorte se hornea.
    const bool clipToLower = upper.clipping && !lower.clipping;
    const bool unclipLower = !upper.clipping && lower.clipping;
    const int lowerBase = unclipLower ? m_layers.clipBase(index - 1) : -1;

    HistoryStep step;
    step.kind = HistoryStep::Kind::MergeDown;
    step.layerId = lower.id;
    step.otherId = upper.id;
    step.index = index;
    step.rect = all;
    step.before = properties(index - 1);
    const bool saved = saveRegion(lower, all, step.pixels);

    // Lo que la capa de abajo aplica al componer y no está en sus píxeles (opacidad y
    // recorte) se hornea en ellos para que el resultado se vea igual.
    if (lower.opacity < 1.0f) {
        m_compositor.scale(lower.target.fbo.id(), lower.opacity, all);
        m_layers.setOpacity(index - 1, 1.0f);
    }
    if (lowerBase >= 0) {
        m_compositor.draw(lower.target.fbo.id(), m_layers.at(lowerBase).target.texture.id(), 1.0f,
                          Compositor::Blend::Mask, all);
    }
    if (unclipLower) {
        m_layers.setClipping(index - 1, false);
    }
    // El recorte usa el alfa de la capa de abajo sin su opacidad: el de la copia guardada.
    GLuint clip = 0;
    if (clipToLower) {
        clip = saved ? step.pixels.texture.id() : lower.target.texture.id();
    }
    m_compositor.merge(lower.target, upper, clip, m_strokeTarget, all);
    ++lower.revision;
    std::unique_ptr<Layer> merged = m_layers.take(index);
    m_layers.setActive(index - 1);
    m_layers.markAllDirty();
    step.after = properties(index - 1);

    if (saved) {
        step.layer = std::move(merged);
        step.bytes = 2 * layerBytes();
        record(std::move(step));
    } else {
        m_history.clear();
    }
    return true;
}

template <typename Change>
void Canvas::changePixels(int index, Change change) {
    if (!m_ready || !m_layers.validIndex(index)) {
        return;
    }
    endStroke();
    finishLayerEdit();
    Layer& layer = m_layers.at(index);
    HistoryStep step;
    step.kind = HistoryStep::Kind::Pixels;
    step.layerId = layer.id;
    step.rect = IRect::ofSize(width(), height());
    const bool saved = saveRegion(layer, step.rect, step.pixels);

    change(layer, step.rect);
    ++layer.revision;
    m_layers.markAllDirty();

    if (saved) {
        step.bytes = layerBytes();
        record(std::move(step));
    } else {
        m_history.clear();
    }
}

void Canvas::clearLayer(int index) {
    changePixels(index, [this](Layer& layer, const IRect&) { fill(layer, 0.0f, 0.0f, 0.0f, 0.0f); });
}

void Canvas::fillLayer(int index, const float rgb[3]) {
    float color[3];
    for (int i = 0; i < 3; ++i) {
        color[i] = std::clamp(rgb[i], 0.0f, 1.0f);
    }
    changePixels(index, [&](Layer& layer, const IRect& all) {
        m_compositor.fill(layer.target.fbo.id(), color,
                          layer.alphaLock ? Compositor::Fill::Atop : Compositor::Fill::Replace, all);
    });
}

void Canvas::invertLayer(int index) {
    changePixels(index, [this](Layer& layer, const IRect& all) { m_compositor.invert(layer.target.fbo.id(), all); });
}

LayerProperties Canvas::properties(int index) const {
    const Layer& layer = m_layers.at(index);
    return {layer.name, layer.visible, layer.opacity, layer.blend, layer.alphaLock, layer.clipping};
}

void Canvas::applyProperties(int index, const LayerProperties& p) {
    m_layers.rename(index, p.name);
    m_layers.setVisible(index, p.visible);
    m_layers.setOpacity(index, p.opacity);
    m_layers.setBlend(index, p.blend);
    m_layers.setAlphaLock(index, p.alphaLock);
    m_layers.setClipping(index, p.clipping);
}

void Canvas::recordProperties(int index, const LayerProperties& before) {
    LayerProperties after = properties(index);
    if (after == before) {
        return;
    }
    HistoryStep step;
    step.kind = HistoryStep::Kind::Properties;
    step.layerId = m_layers.at(index).id;
    step.before = before;
    step.after = std::move(after);
    step.bytes = kSmallStepBytes;
    record(std::move(step));
}

void Canvas::setLayerVisible(int index, bool visible) {
    if (!m_ready || !m_layers.validIndex(index) || m_layers.at(index).visible == visible) {
        return;
    }
    endStroke();
    finishLayerEdit();
    const LayerProperties before = properties(index);
    m_layers.setVisible(index, visible);
    recordProperties(index, before);
}

void Canvas::beginLayerEdit(int index) {
    const uint32_t id = m_layers.at(index).id;
    if (!m_layerEdit.active || m_layerEdit.layerId != id) {
        endStroke();
        finishLayerEdit();
        m_layerEdit = {true, id, properties(index)};
    }
}

bool Canvas::editPending() const {
    if (!m_layerEdit.active) {
        return false;
    }
    const int index = m_layers.indexOf(m_layerEdit.layerId);
    return index >= 0 && !(properties(index) == m_layerEdit.before);
}

void Canvas::setLayerOpacity(int index, float opacity, bool final) {
    if (!m_ready || !m_layers.validIndex(index)) {
        return;
    }
    beginLayerEdit(index);
    m_layers.setOpacity(index, opacity);
    if (final) {
        finishLayerEdit();
    }
}

void Canvas::setLayerBlend(int index, BlendMode blend, bool final) {
    if (!m_ready || !m_layers.validIndex(index)) {
        return;
    }
    beginLayerEdit(index);
    m_layers.setBlend(index, blend);
    if (final) {
        finishLayerEdit();
    }
}

void Canvas::finishLayerEdit() {
    if (!m_layerEdit.active) {
        return;
    }
    m_layerEdit.active = false;
    const int index = m_layers.indexOf(m_layerEdit.layerId);
    if (index >= 0) {
        recordProperties(index, m_layerEdit.before);
    }
}

void Canvas::setLayerAlphaLock(int index, bool locked) {
    if (!m_ready || !m_layers.validIndex(index) || m_layers.at(index).alphaLock == locked) {
        return;
    }
    endStroke();
    finishLayerEdit();
    const LayerProperties before = properties(index);
    m_layers.setAlphaLock(index, locked);
    recordProperties(index, before);
}

void Canvas::setLayerClipping(int index, bool clipping) {
    if (!m_ready || !m_layers.validIndex(index) || m_layers.at(index).clipping == clipping ||
        (clipping && !canClip(index))) {
        return;
    }
    endStroke();
    finishLayerEdit();
    const LayerProperties before = properties(index);
    m_layers.setClipping(index, clipping);
    recordProperties(index, before);
}

void Canvas::setReferenceLayer(int index) {
    if (!m_ready || (index != -1 && !m_layers.validIndex(index))) {
        return;
    }
    const int previous = m_layers.referenceIndex();
    if (previous == index) {
        return;
    }
    endStroke();
    finishLayerEdit();
    HistoryStep step;
    step.kind = HistoryStep::Kind::Reference;
    step.layerId = index >= 0 ? m_layers.at(index).id : 0;
    step.otherId = previous >= 0 ? m_layers.at(previous).id : 0;
    step.bytes = kSmallStepBytes;
    m_layers.setReference(index);
    record(std::move(step));
}

void Canvas::renameLayer(int index, std::string name) {
    if (!m_ready || !m_layers.validIndex(index) || name.empty() || m_layers.at(index).name == name) {
        return;
    }
    finishLayerEdit();
    const LayerProperties before = properties(index);
    m_layers.rename(index, std::move(name));
    recordProperties(index, before);
}

// -----------------------------------------------------------------------------
// Deshacer
// -----------------------------------------------------------------------------

bool Canvas::saveRegion(const Layer& layer, const IRect& rect, gfx::RenderTarget& out) {
    if (rect.empty() || !out.create(rect.width(), rect.height())) {
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    copyRect(layer.target.fbo.id(), rect, out.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void Canvas::swapRegion(Layer& layer, const IRect& rect, gfx::RenderTarget& stored) {
    // El buffer de trazo (vacío fuera de un trazo) sirve de intermedio.
    glDisable(GL_SCISSOR_TEST);
    copyRect(layer.target.fbo.id(), rect, m_strokeTarget.fbo.id(), rect.x0, rect.y0);
    copyRect(stored.fbo.id(), IRect::ofSize(rect.width(), rect.height()), layer.target.fbo.id(), rect.x0, rect.y0);
    copyRect(m_strokeTarget.fbo.id(), rect, stored.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clearStrokeBuffer(rect);
    ++layer.revision;
    m_layers.markDirty(rect);
}

void Canvas::record(HistoryStep step) { m_history.push(std::move(step)); }

void Canvas::applyStep(HistoryStep& step, bool undo) {
    using Kind = HistoryStep::Kind;
    const int index = m_layers.indexOf(step.layerId);
    switch (step.kind) {
    case Kind::Pixels:
        if (index >= 0) {
            swapRegion(m_layers.at(index), step.rect, step.pixels);
            m_layers.setActive(index);
        }
        break;

    case Kind::AddLayer:
        if (undo) {
            step.layer = m_layers.take(index);
        } else {
            m_layers.put(step.index, std::move(step.layer));
        }
        break;

    case Kind::RemoveLayer:
        if (undo) {
            m_layers.put(step.index, std::move(step.layer));
        } else {
            step.layer = m_layers.take(index);
        }
        break;

    case Kind::MoveLayer:
        if (index >= 0) {
            m_layers.move(index, undo ? step.index : step.target);
            m_layers.setActive(m_layers.indexOf(step.layerId));
        }
        break;

    case Kind::MergeDown:
        if (index >= 0) {
            Layer& lower = m_layers.at(index);
            swapRegion(lower, step.rect, step.pixels);
            applyProperties(index, undo ? step.before : step.after);
            if (undo) {
                m_layers.put(step.index, std::move(step.layer));
            } else {
                step.layer = m_layers.take(m_layers.indexOf(step.otherId));
                m_layers.setActive(m_layers.indexOf(step.layerId));
            }
            m_layers.markAllDirty();
        }
        break;

    case Kind::Properties:
        if (index >= 0) {
            applyProperties(index, undo ? step.before : step.after);
        }
        break;

    case Kind::Reference:
        m_layers.setReference(m_layers.indexOf(undo ? step.otherId : step.layerId));
        break;
    }
}

bool Canvas::undo() {
    if (!m_ready) {
        return false;
    }
    endStroke();
    finishLayerEdit();
    HistoryStep* step = m_history.stepToUndo();
    if (!step) {
        return false;
    }
    applyStep(*step, true);
    return true;
}

bool Canvas::redo() {
    if (!m_ready) {
        return false;
    }
    endStroke();
    finishLayerEdit();
    HistoryStep* step = m_history.stepToRedo();
    if (!step) {
        return false;
    }
    applyStep(*step, false);
    return true;
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
    preview.mode = m_strokeMode;
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
        pixels.resize(bytesOf(target.width, target.height));
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

bool Canvas::pickColor(float x, float y, float rgb[3]) {
    if (!m_ready || !(x >= 0.0f && y >= 0.0f && x < static_cast<float>(width()) && y < static_cast<float>(height()))) {
        return false;
    }
    update();
    const int px = std::clamp(static_cast<int>(std::floor(x)), 0, width() - 1);
    const int py = std::clamp(static_cast<int>(std::floor(y)), 0, height() - 1);
    uint8_t pixel[4] = {0, 0, 0, 0};
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_compositor.composite().fbo.id());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (pixel[3] == 0) {
        return false;
    }
    const float alpha = static_cast<float>(pixel[3]);
    for (int i = 0; i < 3; ++i) {
        rgb[i] = std::clamp(static_cast<float>(pixel[i]) / alpha, 0.0f, 1.0f);
    }
    return true;
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

    const size_t bytes = layerBytes();
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

    // Los objetos del contexto anterior ya no existen: se olvidan sin borrarlos. Lo que
    // guardaba el historial estaba en la GPU y se pierde con ellos.
    m_ready = false;
    m_stroking = false;
    m_strokeBounds = {};
    m_layerEdit = {};
    m_history.clear();
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
        ++layer.revision;
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
