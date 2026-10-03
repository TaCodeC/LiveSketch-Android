// Selección, portapapeles y transformar.
//
// La selección es una máscara de un canal del tamaño del lienzo (Selection). Cada cambio
// guarda en el historial la zona de la máscara que cambia, con la caja de lo seleccionado
// de antes y de después. Los cambios en vivo (el umbral de la selección automática, el
// difuminado) parten de una copia de la máscara y se guardan al terminar.
//
// Transformar trabaja con una copia: lo que se transforma se copia a una textura aparte
// (con mipmaps, para reducir sin dientes de sierra) y la capa se ve, hasta aplicar, con lo
// que quedaría: el resto de la capa (en el segundo buffer de trazo) y encima la copia
// transformada (en el primero).
#include "Canvas/Canvas.h"

#include <SDL3/SDL_log.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <new>

namespace {

constexpr size_t kSmallStepBytes = 256;

size_t bytesOf(int width, int height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

void copyRect(GLuint source, const IRect& from, GLuint target, int toX, int toY) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(from.x0, from.y0, from.x1, from.y1, toX, toY, toX + from.width(), toY + from.height(),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

IRect expanded(const IRect& rect, int margin) {
    if (rect.empty()) {
        return {};
    }
    return {rect.x0 - margin, rect.y0 - margin, rect.x1 + margin, rect.y1 + margin};
}

// Imagen de una capa: RGBA8 premultiplicado, filas de arriba abajo.
bool readPixels(const gfx::RenderTarget& target, std::vector<uint8_t>& out) {
    try {
        out.resize(bytesOf(target.width, target.height));
    } catch (const std::bad_alloc&) {
        return false;
    }
    gfx::clearErrors();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return gfx::checkErrors("leer la capa");
}

// Radio del difuminado → sigma de la gaussiana y margen que añade alrededor.
float featherSigma(float radius) { return radius * 0.5f; }
int featherMargin(float radius) { return static_cast<int>(std::ceil(featherSigma(radius) * 3.0f)) + 2; }

} // namespace

// -----------------------------------------------------------------------------
// Historial de la selección
// -----------------------------------------------------------------------------

bool Canvas::saveSelection(const IRect& rect, HistoryStep& step) {
    step.kind = HistoryStep::Kind::Selection;
    step.rect = rect;
    if (rect.empty()) {
        return true;
    }
    if (!m_selection.ensureMask() ||
        !step.pixels.create(rect.width(), rect.height(), nullptr, gfx::RenderTarget::Format::R8)) {
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_selection.mask().fbo.id(), rect, step.pixels.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool Canvas::saveSelectionFromScratch(const IRect& rect, HistoryStep& step) {
    step.kind = HistoryStep::Kind::Selection;
    step.rect = rect;
    if (rect.empty()) {
        return true;
    }
    if (!m_selection.scratch() ||
        !step.pixels.create(rect.width(), rect.height(), nullptr, gfx::RenderTarget::Format::R8)) {
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_selection.scratch().fbo.id(), rect, step.pixels.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void Canvas::recordSelection(HistoryStep step, bool saved, const SelectionState& after) {
    step.selectionAfter = after;
    setSelectionState(after);
    if (!saved) {
        m_history.clear();   // sin memoria para guardarlo: lo anterior ya no se puede deshacer
        return;
    }
    step.bytes = step.pixels ? step.pixels.bytes() : kSmallStepBytes;
    record(std::move(step));
}

void Canvas::swapSelection(const IRect& rect, gfx::RenderTarget& stored) {
    if (!m_selection.ensureMask() || !m_selection.ensureScratch()) {
        return;
    }
    const GLuint mask = m_selection.mask().fbo.id();
    const GLuint scratch = m_selection.scratch().fbo.id();
    glDisable(GL_SCISSOR_TEST);
    copyRect(mask, rect, scratch, rect.x0, rect.y0);
    copyRect(stored.fbo.id(), IRect::ofSize(rect.width(), rect.height()), mask, rect.x0, rect.y0);
    copyRect(scratch, rect, stored.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Canvas::setSelectionState(const SelectionState& state) {
    m_selection.setState(state);
    ++m_selectionVersion;
}

IRect Canvas::maskBounds(const IRect& within) {
    if (!m_selection.mask()) {
        return {};
    }
    return m_bounds.find(m_selection.mask(), nullptr, within);
}

// -----------------------------------------------------------------------------
// Crear la selección
// -----------------------------------------------------------------------------

bool Canvas::selectPolygon(std::span<const glm::vec2> polygon, SelectOp op) {
    if (!m_ready) {
        return false;
    }
    settle();
    const SelectionState before = m_selection.state();
    if (!before.active) {
        if (op == SelectOp::Subtract) {
            return false;   // no hay nada de lo que restar
        }
        op = SelectOp::Replace;
    }
    std::vector<uint8_t> coverage;
    IRect box;
    if (!selection::rasterize(polygon, IRect::ofSize(width(), height()), coverage, box)) {
        return false;
    }
    // Zona que cambia: la de la forma y, al sustituir, lo que hubiera en la máscara.
    IRect rect = box;
    if (op == SelectOp::Replace) {
        rect.unite(before.content);
    } else if (op == SelectOp::Subtract) {
        rect = box.intersected(before.content);
        if (rect.empty()) {
            return false;
        }
    }

    HistoryStep step;
    step.selectionBefore = before;
    const bool saved = saveSelection(rect, step);
    if (!m_selection.ensureMask()) {
        return false;
    }
    SelectionState after{true, box};
    switch (op) {
    case SelectOp::Replace:
        m_selection.clear(rect);
        m_selection.drawCoverage(coverage, box, Selection::Combine::Union);
        break;
    case SelectOp::Add:
        m_selection.drawCoverage(coverage, box, Selection::Combine::Union);
        after.content.unite(before.content);
        break;
    case SelectOp::Subtract:
        m_selection.drawCoverage(coverage, box, Selection::Combine::Subtract);
        after.content = maskBounds(before.content);
        after.active = !after.content.empty();
        break;
    }
    recordSelection(std::move(step), saved, after);
    return true;
}

bool Canvas::floodLevels(float x, float y, selection::AutoLevels& levels) {
    std::vector<uint8_t> pixels;
    const int reference = m_layers.referenceIndex();
    const bool read = reference >= 0 ? readPixels(m_layers.at(reference).target, pixels) : readComposite(pixels);
    if (!read) {
        return false;
    }
    const int px = std::clamp(static_cast<int>(std::floor(x)), 0, width() - 1);
    const int py = std::clamp(static_cast<int>(std::floor(y)), 0, height() - 1);
    try {
        return selection::autoLevels(pixels.data(), width(), height(), px, py, levels);
    } catch (const std::bad_alloc&) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Sin memoria para buscar la zona del color");
        return false;
    }
}

bool Canvas::beginAutoSelect(float x, float y, SelectOp op, float threshold) {
    if (!m_ready || !(x >= 0.0f && y >= 0.0f && x < static_cast<float>(width()) && y < static_cast<float>(height()))) {
        return false;
    }
    settle();
    const SelectionState before = m_selection.state();
    if (!before.active) {
        if (op == SelectOp::Subtract) {
            return false;
        }
        op = SelectOp::Replace;
    }

    selection::AutoLevels levels;
    if (!floodLevels(x, y, levels)) {
        return false;
    }
    if (!m_selection.ensureMask() || !m_selection.ensureScratch() || !m_selection.uploadLevels(levels.levels)) {
        m_selection.dropLevels();
        return false;
    }
    // La máscara de antes: cada cambio del umbral parte de ella.
    m_selection.copyToScratch(IRect::ofSize(width(), height()));

    m_auto.active = true;
    m_auto.op = op;
    m_auto.before = before;
    m_auto.levelBounds = levels.bounds;
    m_auto.cutoff = -1;
    m_auto.drawn = {};
    setAutoThreshold(threshold);
    return true;
}

void Canvas::setAutoThreshold(float threshold) {
    if (!m_auto.active) {
        return;
    }
    const int cutoff = selection::autoCutoff(threshold);
    if (cutoff == m_auto.cutoff) {
        return;
    }
    m_auto.cutoff = cutoff;
    drawAutoLevels();
}

void Canvas::drawAutoLevels() {
    const SelectionState& before = m_auto.before;
    IRect grown = m_auto.levelBounds[static_cast<size_t>(std::clamp(m_auto.cutoff, 0, 255))];
    IRect area = m_auto.drawn;
    area.unite(grown);
    if (m_auto.op == SelectOp::Replace) {
        area.unite(before.content);
    } else if (m_auto.op == SelectOp::Subtract) {
        grown = grown.intersected(before.content);
    }
    // Desde la máscara de antes en todo lo que se ha tocado.
    m_selection.copyFromScratch(area);
    SelectionState live{true, grown};
    switch (m_auto.op) {
    case SelectOp::Replace:
        m_selection.clear(area);
        m_selection.drawLevels(grown, m_auto.cutoff, 1.0f, Selection::Combine::Union);
        break;
    case SelectOp::Add:
        m_selection.drawLevels(grown, m_auto.cutoff, 1.0f, Selection::Combine::Union);
        live.content.unite(before.content);
        break;
    case SelectOp::Subtract:
        m_selection.drawLevels(grown, m_auto.cutoff, 1.0f, Selection::Combine::Subtract);
        live.content = before.content;
        break;
    }
    m_auto.drawn = area;
    setSelectionState(live);
}

void Canvas::endAutoSelect(bool apply) {
    if (!m_auto.active) {
        return;
    }
    m_auto.active = false;
    m_selection.dropLevels();
    const SelectionState before = m_auto.before;
    if (!apply) {
        m_selection.copyFromScratch(m_auto.drawn);
        setSelectionState(before);
        return;
    }
    // Lo que cambió con el último umbral; el resto ya volvió a ser lo de antes.
    IRect rect = m_auto.levelBounds[static_cast<size_t>(std::clamp(m_auto.cutoff, 0, 255))];
    if (m_auto.op == SelectOp::Replace) {
        rect.unite(before.content);
    } else if (m_auto.op == SelectOp::Subtract) {
        rect = rect.intersected(before.content);
    }
    HistoryStep step;
    step.selectionBefore = before;
    const bool saved = saveSelectionFromScratch(rect, step);
    SelectionState after = m_selection.state();
    if (m_auto.op == SelectOp::Subtract) {
        after.content = maskBounds(before.content);
        after.active = !after.content.empty();
    }
    recordSelection(std::move(step), saved, after);
}

Canvas::Edit Canvas::selectLayerContent(int index) {
    if (!m_ready || !m_layers.validIndex(index)) {
        return Edit::Nothing;
    }
    settle();
    const Layer& layer = m_layers.at(index);
    const IRect content = m_bounds.find(layer.target, nullptr, IRect::ofSize(width(), height()));
    if (content.empty()) {
        return Edit::Nothing;
    }
    const SelectionState before = m_selection.state();
    IRect rect = content;
    rect.unite(before.content);
    HistoryStep step;
    step.selectionBefore = before;
    const bool saved = saveSelection(rect, step);
    if (!m_selection.ensureMask()) {
        return Edit::NoMemory;
    }
    m_selection.clear(rect);
    m_selection.drawAlpha(layer.target.texture.id(), content, Selection::Combine::Union);
    recordSelection(std::move(step), saved, SelectionState{true, content});
    return Edit::Done;
}

void Canvas::selectAll() {
    if (!m_ready) {
        return;
    }
    settle();
    const IRect all = IRect::ofSize(width(), height());
    HistoryStep step;
    step.selectionBefore = m_selection.state();
    const bool saved = saveSelection(all, step);
    if (!m_selection.ensureMask()) {
        return;
    }
    m_selection.fill(all);
    recordSelection(std::move(step), saved, SelectionState{true, all});
}

void Canvas::invertSelection() {
    if (!m_ready) {
        return;
    }
    if (!m_selection.active()) {
        selectAll();
        return;
    }
    settle();
    const IRect all = IRect::ofSize(width(), height());
    HistoryStep step;
    step.selectionBefore = m_selection.state();
    const bool saved = saveSelection(all, step);
    m_selection.invert(all);
    const IRect content = maskBounds(all);
    recordSelection(std::move(step), saved, SelectionState{!content.empty(), content});
}

float Canvas::maxFeather() const {
    return std::max(8.0f, static_cast<float>(std::min(width(), height())) / 8.0f);
}

bool Canvas::beginFeather() {
    if (!m_ready || !m_selection.active()) {
        return false;
    }
    settle();
    if (!m_selection.active() || !m_selection.ensureScratch()) {
        return false;
    }
    // Entera: el desenfoque lee alrededor de lo que cambia.
    m_selection.copyToScratch(IRect::ofSize(width(), height()));
    m_feather = {};
    m_feather.active = true;
    m_feather.before = m_selection.state();
    return true;
}

void Canvas::setFeather(float radius) {
    if (!m_feather.active) {
        return;
    }
    radius = std::clamp(radius, 0.0f, maxFeather());
    if (std::fabs(radius - m_feather.radius) < 0.01f) {
        return;
    }
    m_feather.radius = radius;
    const IRect all = IRect::ofSize(width(), height());
    const IRect area = expanded(m_feather.before.content, featherMargin(radius)).intersected(all);
    IRect restore = m_feather.drawn;
    restore.unite(area);
    m_selection.copyFromScratch(restore);
    if (radius > 0.0f) {
        m_selection.blurFromScratch(area, featherSigma(radius));
    }
    m_feather.drawn = restore;
    setSelectionState(SelectionState{true, radius > 0.0f ? area : m_feather.before.content});
}

void Canvas::endFeather(bool apply) {
    if (!m_feather.active) {
        return;
    }
    m_feather.active = false;
    const SelectionState before = m_feather.before;
    if (!apply || m_feather.radius <= 0.0f) {
        m_selection.copyFromScratch(m_feather.drawn);
        setSelectionState(before);
        m_feather.radius = 0.0f;
        return;
    }
    const IRect area =
        expanded(before.content, featherMargin(m_feather.radius)).intersected(IRect::ofSize(width(), height()));
    HistoryStep step;
    step.selectionBefore = before;
    const bool saved = saveSelectionFromScratch(area, step);
    recordSelection(std::move(step), saved, SelectionState{true, area});
    m_feather.radius = 0.0f;
}

void Canvas::deselect() {
    if (!m_ready || !m_selection.active()) {
        return;
    }
    settle();
    // La máscara se queda como está (deshacer solo tiene que volver a activarla).
    HistoryStep step;
    step.kind = HistoryStep::Kind::Selection;
    step.selectionBefore = m_selection.state();
    recordSelection(std::move(step), true, SelectionState{false, m_selection.state().content});
}

// -----------------------------------------------------------------------------
// Portapapeles
// -----------------------------------------------------------------------------

IRect Canvas::selectedContent(const Layer& layer, bool masked) {
    if (masked) {
        return m_bounds.find(layer.target, &m_selection.mask(), m_selection.bounds());
    }
    return m_bounds.find(layer.target, nullptr, IRect::ofSize(width(), height()));
}

void Canvas::stageSelection(const Layer& layer, const IRect& rect, bool masked) {
    // El buffer de trazo está transparente fuera de un trazo: encima queda la copia.
    m_compositor.draw(m_strokeTarget.fbo.id(), layer.target.texture.id(), 1.0f, Compositor::Blend::Over, rect,
                      nullptr, masked ? m_selection.activeMask() : 0);
}

Canvas::Edit Canvas::copySelection() {
    if (!m_ready) {
        return Edit::Nothing;
    }
    settle();
    const Layer& layer = m_layers.active();
    const bool masked = m_selection.active();
    const IRect rect = selectedContent(layer, masked);
    if (rect.empty()) {
        return Edit::Nothing;
    }
    gfx::RenderTarget pixels;
    if (!pixels.create(rect.width(), rect.height())) {
        return Edit::NoMemory;
    }
    stageSelection(layer, rect, masked);
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_strokeTarget.fbo.id(), rect, pixels.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clearStrokeBuffer(rect);
    m_clipboard.pixels = std::move(pixels);
    m_clipboard.rect = rect;
    return Edit::Done;
}

Canvas::Edit Canvas::cutSelection() {
    const Edit copied = copySelection();
    if (copied == Edit::Done) {
        clearLayer(m_layers.activeIndex());
    }
    return copied;
}

Canvas::Edit Canvas::paste() {
    if (!m_ready || !canPaste()) {
        return Edit::Nothing;
    }
    settle();
    if (m_layers.count() >= maxLayers()) {
        return Edit::Full;
    }
    const IRect all = IRect::ofSize(width(), height());
    const int w = m_clipboard.pixels.width;
    const int h = m_clipboard.pixels.height;
    // En su sitio; si en este lienzo cae fuera, centrado.
    IRect place = m_clipboard.rect;
    if (place.intersected(all).empty()) {
        const int x0 = (width() - w) / 2;
        const int y0 = (height() - h) / 2;
        place = {x0, y0, x0 + w, y0 + h};
    }
    const IRect visible = place.intersected(all);
    const int position = m_layers.activeIndex() + 1;
    Layer* layer = m_layers.insert(position, m_layers.availableName("Pegado"));
    if (!layer) {
        return Edit::NoMemory;
    }
    if (!visible.empty()) {
        const IRect from{visible.x0 - place.x0, visible.y0 - place.y0, visible.x1 - place.x0, visible.y1 - place.y0};
        glDisable(GL_SCISSOR_TEST);
        copyRect(m_clipboard.pixels.fbo.id(), from, layer->target.fbo.id(), visible.x0, visible.y0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ++layer->revision;
        m_layers.markDirty(visible);
    }
    HistoryStep step;
    step.kind = HistoryStep::Kind::AddLayer;
    step.layerId = layer->id;
    step.index = position;
    step.bytes = layerBytes();
    if (!m_selection.active()) {
        record(std::move(step));
        return Edit::Done;
    }
    // Como en Photoshop o en Procreate, pegar quita la selección; deshacer la devuelve.
    HistoryStep deselected;
    deselected.kind = HistoryStep::Kind::Selection;
    deselected.selectionBefore = m_selection.state();
    deselected.selectionAfter = SelectionState{false, m_selection.state().content};
    deselected.bytes = kSmallStepBytes;
    setSelectionState(deselected.selectionAfter);
    HistoryStep group;
    group.kind = HistoryStep::Kind::Group;
    group.bytes = step.bytes + deselected.bytes;
    group.children.push_back(std::move(step));
    group.children.push_back(std::move(deselected));
    record(std::move(group));
    return Edit::Done;
}

Canvas::Edit Canvas::duplicateSelection() {
    if (!m_ready) {
        return Edit::Nothing;
    }
    settle();
    const int index = m_layers.activeIndex();
    if (m_layers.count() >= maxLayers()) {
        return Edit::Full;
    }
    if (!m_selection.active()) {
        return duplicateLayer(index) ? Edit::Done : Edit::NoMemory;
    }
    const Layer& source = m_layers.at(index);
    const IRect rect = selectedContent(source, true);
    if (rect.empty()) {
        return Edit::Nothing;
    }
    const LayerProperties props = properties(index);
    stageSelection(source, rect, true);
    Layer* copy = m_layers.insert(index + 1, m_layers.availableName(source.name + " copia"));
    if (!copy) {
        clearStrokeBuffer(rect);
        return Edit::NoMemory;
    }
    // Se ve igual que lo de la capa original (pero no es la referencia).
    copy->visible = props.visible;
    copy->opacity = props.opacity;
    copy->blend = props.blend;
    copy->alphaLock = props.alphaLock;
    copy->clipping = props.clipping;
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_strokeTarget.fbo.id(), rect, copy->target.fbo.id(), rect.x0, rect.y0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clearStrokeBuffer(rect);
    ++copy->revision;
    m_layers.markAllDirty();

    HistoryStep step;
    step.kind = HistoryStep::Kind::AddLayer;
    step.layerId = copy->id;
    step.index = index + 1;
    step.bytes = layerBytes();
    record(std::move(step));
    return Edit::Done;
}

// -----------------------------------------------------------------------------
// Transformar
// -----------------------------------------------------------------------------

Canvas::Edit Canvas::beginTransform(bool wholeLayer) {
    if (!m_ready) {
        return Edit::Nothing;
    }
    settle();
    const int index = m_layers.activeIndex();
    if (!layerShown(index)) {
        return Edit::Hidden;
    }
    Layer& layer = m_layers.at(index);
    const bool masked = m_selection.active() && !wholeLayer;
    const IRect source = selectedContent(layer, masked);
    if (source.empty()) {
        return Edit::Nothing;
    }
    // Un píxel transparente alrededor suaviza los bordes al girar; si con él no cabe en
    // una textura (un lienzo del tamaño máximo), va sin él.
    const int border = std::max(source.width(), source.height()) + 2 <= m_maxTextureSize ? 1 : 0;
    gfx::RenderTarget content;
    if (!ensureStrokeBase() || !content.create(source.width() + 2 * border, source.height() + 2 * border)) {
        return Edit::NoMemory;
    }
    const IRect all = IRect::ofSize(width(), height());

    stageSelection(layer, source, masked);
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_strokeTarget.fbo.id(), source, content.fbo.id(), border, border);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, content.texture.id());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Lo que se queda: la capa sin eso. La copia de trabajo empieza siendo eso mismo.
    glDisable(GL_SCISSOR_TEST);
    copyRect(layer.target.fbo.id(), all, m_strokeBase.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_compositor.clear(m_strokeBase.fbo.id(), source, masked ? m_selection.activeMask() : 0);
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_strokeBase.fbo.id(), all, m_strokeTarget.fbo.id(), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_transform = {};
    m_transform.active = true;
    m_transform.layerId = layer.id;
    m_transform.source = source;
    m_transform.content = std::move(content);
    m_transform.border = border;
    m_transform.masked = masked;
    m_transform.movesSelection = m_selection.active();
    m_transform.homography = glm::mat3(1.0f);
    updateTransformPreview();
    return Edit::Done;
}

bool Canvas::setTransform(const glm::vec2 corners[4], bool nearest) {
    if (!m_transform.active) {
        return false;
    }
    glm::mat3 h(1.0f);
    if (!warp::homography(m_transform.source, corners, h)) {
        return false;
    }
    m_transform.homography = h;
    m_transform.nearest = nearest;
    updateTransformPreview();
    return true;
}

void Canvas::updateTransformPreview() {
    const IRect all = IRect::ofSize(width(), height());
    const IRect& source = m_transform.source;
    const IRect dest = warp::imageBounds(m_transform.homography, source, all);
    IRect area = m_transform.drawn;
    area.unite(dest);
    if (!area.empty()) {
        glDisable(GL_SCISSOR_TEST);
        copyRect(m_strokeBase.fbo.id(), area, m_strokeTarget.fbo.id(), area.x0, area.y0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    // Del destino a la copia: deshacer la homografía y pasar a píxeles de la copia, que
    // empieza un píxel antes que la caja de la fuente (si lleva el borde).
    glm::mat3 toContent(1.0f);
    toContent[2][0] = static_cast<float>(m_transform.border - source.x0);
    toContent[2][1] = static_cast<float>(m_transform.border - source.y0);
    const glm::mat3 toSource = toContent * glm::inverse(m_transform.homography);
    m_warp.draw(m_strokeTarget.fbo.id(), width(), height(), m_transform.content.texture.id(),
                m_transform.content.width, m_transform.content.height, toSource, dest, Warp::Mode::Over,
                m_transform.nearest);
    m_transform.drawn = dest;
    m_layers.markDirty(area);
}

void Canvas::endTransform() {
    m_transform = {};
    // La copia de trabajo ocupaba los dos buffers de trazo enteros.
    clearStrokeBuffer(IRect::ofSize(width(), height()));
}

void Canvas::cancelTransform() {
    if (!m_transform.active) {
        return;
    }
    IRect region = m_transform.source;
    region.unite(m_transform.drawn);
    endTransform();
    m_layers.markDirty(region.intersected(IRect::ofSize(width(), height())));
}

void Canvas::applyTransform() {
    if (!m_transform.active) {
        return;
    }
    const int index = m_layers.indexOf(m_transform.layerId);
    if (index < 0) {
        cancelTransform();
        return;
    }
    // Sin cambios no hay nada que guardar.
    const IRect& source = m_transform.source;
    const glm::vec2 corners[4] = {
        {static_cast<float>(source.x0), static_cast<float>(source.y0)},
        {static_cast<float>(source.x1), static_cast<float>(source.y0)},
        {static_cast<float>(source.x1), static_cast<float>(source.y1)},
        {static_cast<float>(source.x0), static_cast<float>(source.y1)},
    };
    bool moved = false;
    for (const glm::vec2& corner : corners) {
        bool valid = false;
        const glm::vec2 p = warp::apply(m_transform.homography, corner, &valid);
        if (!valid || std::fabs(p.x - corner.x) > 1e-3f || std::fabs(p.y - corner.y) > 1e-3f) {
            moved = true;
        }
    }
    if (!moved) {
        cancelTransform();
        return;
    }

    const IRect all = IRect::ofSize(width(), height());
    Layer& layer = m_layers.at(index);
    IRect region = source;
    region.unite(m_transform.drawn);
    region = region.intersected(all);

    HistoryStep group;
    group.kind = HistoryStep::Kind::Group;
    bool saved = true;
    {
        HistoryStep step;
        step.kind = HistoryStep::Kind::Pixels;
        step.layerId = layer.id;
        step.rect = region;
        saved = saveRegion(layer, region, step.pixels);
        step.bytes = bytesOf(region.width(), region.height());
        group.bytes += step.bytes;
        group.children.push_back(std::move(step));
    }
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_strokeTarget.fbo.id(), region, layer.target.fbo.id(), region.x0, region.y0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ++layer.revision;
    m_layers.markDirty(region);

    // La selección se mueve con lo que se transformó.
    if (m_transform.movesSelection && m_selection.mask() && m_selection.ensureScratch()) {
        const SelectionState before = m_selection.state();
        const IRect dest = warp::imageBounds(m_transform.homography, before.content, all);
        IRect maskRegion = before.content;
        maskRegion.unite(dest);
        HistoryStep step;
        step.selectionBefore = before;
        saved = saveSelection(maskRegion, step) && saved;
        m_selection.copyToScratch(all);
        m_selection.clear(maskRegion);
        m_warp.draw(m_selection.mask().fbo.id(), width(), height(), m_selection.scratch().texture.id(), width(),
                    height(), glm::inverse(m_transform.homography), dest, Warp::Mode::Replace, false);
        const IRect content = maskBounds(dest);
        step.selectionAfter = SelectionState{!content.empty(), content};
        setSelectionState(step.selectionAfter);
        step.bytes = step.pixels ? step.pixels.bytes() : kSmallStepBytes;
        group.bytes += step.bytes;
        group.children.push_back(std::move(step));
    }

    endTransform();
    if (saved) {
        record(std::move(group));
    } else {
        dropHistory();
    }
}
