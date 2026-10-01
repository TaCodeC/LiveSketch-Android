#include "Canvas/Canvas.h"

#include "Canvas/BrushTips.h"

#include <SDL3/SDL_log.h>
#include <glm/common.hpp>

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

size_t bytesOf(int width, int height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

// Las zonas de un trazo con simetría a menos de esto se guardan juntas para deshacer.
constexpr int kRegionJoin = 32;

// Copia un rectángulo entre FBO del mismo formato (sin escalar).
void copyRect(GLuint source, const IRect& from, GLuint target, int toX, int toY) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(from.x0, from.y0, from.x1, from.y1, toX, toY, toX + from.width(), toY + from.height(),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

// Deja transparente `rect` de `target` (si existe).
void clearTarget(const gfx::RenderTarget& target, const IRect& rect) {
    const IRect area = rect.intersected(IRect::ofSize(target.width, target.height));
    if (!target || area.empty()) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo.id());
    glViewport(0, 0, target.width, target.height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(area.x0, area.y0, area.width(), area.height());
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace

bool Canvas::init(int width, int height) {
    // El portapapeles pasa al lienzo nuevo.
    Clipboard clipboard = std::move(m_clipboard);
    destroy();
    m_clipboard = std::move(clipboard);
    if (width <= 0 || height <= 0) {
        return false;
    }
    m_layers.reset(width, height);
    m_guide = guide::defaults({static_cast<float>(width), static_cast<float>(height)});
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
    resetStroke();
    m_layerEdit = {};
    m_history.clear();
    m_layers.clearAll();
    destroyGpuObjects();
    m_clipboard = {};
    m_snapshot.clear();
}

bool Canvas::createGpuObjects() {
    const int w = m_layers.width();
    const int h = m_layers.height();
    m_maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
    if (!m_compositor.init(w, h) || !m_brush.init() || !m_strokeTarget.create(w, h) || !m_selection.init(w, h) ||
        !m_bounds.init() || !m_warp.init() || !m_colorFill.init() || !m_imageAdjust.init()) {
        destroyGpuObjects();
        return false;
    }
    return true;
}

void Canvas::destroyGpuObjects() {
    m_auto = {};
    m_feather = {};
    m_transform = {};
    m_fill = {};
    m_colorFill.destroy();
    m_adjust = {};
    m_imageAdjust.destroy();
    m_warp.destroy();
    m_bounds.destroy();
    m_selection.destroy();
    ++m_selectionVersion;
    m_wetTails.clear();
    m_strokeBase.destroy();
    m_strokeTarget.destroy();
    m_brush.destroy();
    m_compositor.destroy();
}

IRect Canvas::operationRect() const {
    return m_selection.active() ? m_selection.bounds() : IRect::ofSize(width(), height());
}

void Canvas::settle() {
    endStroke();
    finishLayerEdit();
    endAutoSelect(true);
    endFeather(true);
    endFill(true);
    endAdjust(true);
    applyTransform();
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

void Canvas::setViewScale(float canvasPixelsPerPoint) {
    if (std::isfinite(canvasPixelsPerPoint) && canvasPixelsPerPoint > 0.0f) {
        m_pixelsPerPoint = canvasPixelsPerPoint;
    }
}

void Canvas::setSmoothing(float amount) {
    if (std::isfinite(amount)) {
        m_smoothing = std::clamp(amount, 0.0f, 1.0f);
    }
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
    settle();
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
    m_strokeParams = m_settings.brush;
    brushes::sanitize(m_strokeParams);
    // El suavizado de Preferencias se suma a la estabilización del pincel, sin pasar del máximo.
    m_strokeParams.streamline = 1.0f - (1.0f - m_strokeParams.streamline) * (1.0f - m_smoothing);
    // Sin la textura de la punta no se pinta nada, pero el resto de la app funciona.
    m_brush.prepare(m_strokeParams);
    // Borrar no mezcla: un pincel húmedo borra como cualquier otro.
    m_wet = !erase && (m_settings.smudge || brushes::isWet(m_strokeParams));

    m_strokeGrain = {};
    if (m_strokeParams.grain != BrushGrain::None && m_strokeParams.grainDepth > 0.0f) {
        m_strokeGrain.texture = m_brush.grainTexture(m_strokeParams.grain);
        m_strokeGrain.scale = 1.0f / (static_cast<float>(brushtips::kGrainSize) * m_strokeParams.grainScale);
        m_strokeGrain.depth = m_strokeParams.grainDepth;
    }

    StrokePath::Settings path;
    path.radius = m_settings.radius;
    path.flow = m_strokeParams.flow;
    if (erase && m_strokeParams.eraseFlow > 0.0f) {
        path.flow = m_strokeParams.eraseFlow;
    }
    if (m_wet && m_settings.smudge) {
        path.flow = 1.0f;   // la fuerza de Difuminar es la opacidad
    }
    path.pixelsPerPoint = m_pixelsPerPoint;
    path.seed = ++m_strokeCount * 0x9E3779B9U;
    m_path.begin(m_strokeParams, path, x, y, pressure);
    m_drawnRevision = m_path.revision() - 1;

    // Con simetría, el trazo se repite con cada copia de la guía.
    m_copies = guide::copies(m_guide);
    m_copyCenter = m_guide.center;
    m_copyBounds.assign(m_copies.size(), IRect{});
    m_provisionalRects.assign(m_copies.size(), IRect{});
    m_input.assign(1, glm::vec3(x, y, std::clamp(pressure, 0.0f, 1.0f)));
    m_shapeRecognized = {};
    m_shapeBase = {};
    m_shape = {};
    m_shapeDirty = false;

    if (!m_wet) {
        // Sin memoria para el segundo buffer, el final afinado se ve al levantar el lápiz.
        m_useBase = m_path.tapered() && ensureStrokeBase();
    } else {
        // Pinta sobre la copia de trabajo, que se ve en lugar de la capa. El segundo buffer
        // lleva la cobertura del trazo (ver Brush::drawWet); sin memoria para él, cada
        // sello pinta como el primero. Difuminar no pinta y no lo necesita.
        m_useBase = false;
        m_wetCoverage = !m_settings.smudge && ensureStrokeBase();
        m_strokeMode = StrokePreview::Mode::Replace;
        m_wetMix = {};
        std::copy(m_strokeColor, m_strokeColor + 3, m_wetMix.color);
        m_wetMix.smudge = m_settings.smudge;
        m_wetMix.radius = m_settings.radius;
        m_wetMix.strength = m_strokeOpacity;
        m_wetMix.alphaLock = m_layers.active().alphaLock;
        m_wetMix.selection = operationMask();
        m_wetMix.grain = m_strokeGrain.texture;
        m_wetMix.grainScale = m_strokeGrain.scale;
        m_wetMix.grainDepth = m_strokeGrain.depth;
        m_wetMix.original = m_layers.active().target.texture.id();   // no cambia hasta el final
        m_wetCursors.assign(m_copies.size(), WetCursor{});
        m_wetTails.resize(m_copies.size());
        for (WetTail& tail : m_wetTails) {
            tail.rect = {};
        }
        m_wetDabs.resize(m_copies.size());
        m_wetValid = {};
    }
    return true;
}

void Canvas::strokeTo(float x, float y, float pressure) {
    if (!m_stroking) {
        return;
    }
    if (m_shape.kind != quickshape::Kind::None) {
        // Con la forma rápida, el puntero la ajusta.
        const glm::vec2 to(x, y);
        if (to != m_shapeTo) {
            m_shapeTo = to;
            m_shape = quickshape::adjust(m_shapeBase, m_shapeFrom, m_shapeTo, m_shapeOptions);
            m_shapeDirty = true;
        }
        return;
    }
    m_input.emplace_back(x, y, std::clamp(pressure, 0.0f, 1.0f));
    m_path.moveTo(x, y, pressure);
}

void Canvas::endStroke() {
    if (!m_stroking) {
        return;
    }
    applyShape();
    m_path.finish();
    flushDabs();
    commitStroke();
}

void Canvas::cancelStroke() {
    if (!m_stroking) {
        return;
    }
    m_path = {};
    if (m_wet) {
        endWet();   // la capa no ha cambiado
        return;
    }
    for (const IRect& rect : m_copyBounds) {
        clearStrokeBuffer(rect);
        m_layers.markDirty(rect);
    }
    resetStroke();
}

void Canvas::resetStroke() {
    m_stroking = false;
    m_strokeBounds = {};
    m_copies.clear();
    m_copyBounds.clear();
    m_provisionalRects.clear();
    m_wetCursors.clear();
    for (WetTail& tail : m_wetTails) {
        tail.rect = {};
    }
    m_input.clear();
    m_shapeRecognized = {};
    m_shapeBase = {};
    m_shape = {};
    m_shapeDirty = false;
    m_wetValid = {};
    m_wetCoverage = false;
    m_wet = false;
}

bool Canvas::ensureStrokeBase() {
    if (m_strokeBase) {
        return true;
    }
    if (!m_strokeBase.create(width(), height())) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Sin memoria para ver el afinado mientras se pinta");
        return false;
    }
    return true;
}

// --- Simetría ---

void Canvas::transformDabs(size_t copy, std::span<const Dab> dabs, std::vector<Dab>& out) const {
    out.clear();
    if (copy == 0) {
        out.assign(dabs.begin(), dabs.end());   // el trazo mismo
        return;
    }
    out.reserve(dabs.size());
    for (const Dab& dab : dabs) {
        out.push_back(guide::transform(dab, m_copies[copy], m_copyCenter));
    }
}

void Canvas::expandCopies(std::span<const Dab> dabs, std::vector<IRect>& rects) {
    const IRect canvas = IRect::ofSize(width(), height());
    rects.assign(m_copies.size(), IRect{});
    m_copyDabs[0].clear();
    m_copyDabs[1].clear();
    for (size_t k = 0; k < m_copies.size(); ++k) {
        std::vector<Dab>& out = m_copyDabs[m_copies[k].mirrored ? 1 : 0];
        const size_t first = out.size();
        if (k == 0) {
            out.insert(out.end(), dabs.begin(), dabs.end());
        } else {
            for (const Dab& dab : dabs) {
                out.push_back(guide::transform(dab, m_copies[k], m_copyCenter));
            }
        }
        rects[k] = Brush::bounds(std::span<const Dab>(out).subspan(first)).intersected(canvas);
    }
}

void Canvas::drawCopies(GLuint target) {
    // Un trazo pinta lo mismo en cualquier orden (un solo color: encima o el máximo).
    for (int mirrored = 0; mirrored < 2; ++mirrored) {
        if (!m_copyDabs[mirrored].empty()) {
            m_brush.draw(target, width(), height(), m_copyDabs[mirrored], m_strokeParams, m_strokeColor,
                         mirrored == 1);
        }
    }
}

void Canvas::touch(size_t copy, const IRect& rect) {
    if (rect.empty()) {
        return;
    }
    m_strokeBounds.unite(rect);
    m_copyBounds[copy].unite(rect);
    m_layers.markDirty(rect);
}

std::vector<IRect> Canvas::strokeRegions(const IRect& limit) const {
    std::vector<IRect> regions;
    for (const IRect& rect : m_copyBounds) {
        addRegion(regions, rect.intersected(limit), kRegionJoin);
    }
    return regions;
}

template <typename Finish>
void Canvas::changeRegions(Layer& layer, const std::vector<IRect>& regions, Finish finish) {
    std::vector<HistoryStep> steps;
    bool saved = true;
    size_t bytes = 0;
    for (const IRect& rect : regions) {
        HistoryStep step;
        step.kind = HistoryStep::Kind::Pixels;
        step.layerId = layer.id;
        step.rect = rect;
        saved = saved && saveRegion(layer, rect, step.pixels);
        step.bytes = bytesOf(rect.width(), rect.height());
        bytes += step.bytes;
        steps.push_back(std::move(step));
    }
    for (const IRect& rect : regions) {
        finish(rect);
        m_layers.markDirty(rect);
    }
    ++layer.revision;
    if (!saved) {
        m_history.clear();   // sin memoria para guardarlo: lo anterior ya no se puede deshacer
    } else if (steps.size() == 1) {
        record(std::move(steps.front()));
    } else {
        // Varias zonas (las copias de la simetría): se deshacen juntas.
        HistoryStep group;
        group.kind = HistoryStep::Kind::Group;
        group.bytes = bytes;
        group.children = std::move(steps);
        record(std::move(group));
    }
}

// --- Forma rápida ---

bool Canvas::snapStroke(const quickshape::Options& options) {
    if (!m_stroking || m_shape.kind != quickshape::Kind::None || m_input.size() < 2) {
        return false;
    }
    std::vector<glm::vec2> points;
    std::vector<float> pressures;
    points.reserve(m_input.size());
    pressures.reserve(m_input.size());
    for (const glm::vec3& sample : m_input) {
        points.emplace_back(sample.x, sample.y);
        pressures.push_back(sample.z);
    }
    const quickshape::Shape shape = quickshape::recognize(points, options);
    if (shape.kind == quickshape::Kind::None) {
        return false;
    }
    // La presión de la mitad del trazo: al empezar y al terminar suele ir más floja.
    const auto middle = pressures.begin() + static_cast<std::ptrdiff_t>(pressures.size() / 2);
    std::nth_element(pressures.begin(), middle, pressures.end());
    m_shapePressure = *middle;
    m_shapeRecognized = shape;
    m_shapeBase = shape;
    m_shape = shape;
    m_shapeOptions = options;
    m_shapeFrom = points.back();
    m_shapeTo = m_shapeFrom;
    m_shapeDirty = true;
    return true;
}

void Canvas::setShapeRegular(bool regular) {
    if (!m_stroking || m_shape.kind == quickshape::Kind::None || m_shapeBase.regular == regular) {
        return;
    }
    m_shapeBase = regular ? quickshape::regular(m_shapeRecognized) : m_shapeRecognized;
    m_shape = quickshape::adjust(m_shapeBase, m_shapeFrom, m_shapeTo, m_shapeOptions);
    m_shapeDirty = true;
}

void Canvas::applyShape() {
    if (!m_shapeDirty || !m_stroking) {
        return;
    }
    m_shapeDirty = false;
    const std::vector<glm::vec2> outline = quickshape::outline(m_shape);
    if (outline.size() < 2) {
        return;
    }
    discardStroke();
    m_path.reshape(outline, m_shapePressure);
    m_drawnRevision = m_path.revision() - 1;
}

void Canvas::discardStroke() {
    const GLuint layer = m_layers.active().target.fbo.id();
    for (IRect& rect : m_copyBounds) {
        if (rect.empty()) {
            continue;
        }
        if (m_wet) {
            // La copia de trabajo vuelve a ser la capa, y la cobertura, cero.
            glDisable(GL_SCISSOR_TEST);
            copyRect(layer, rect, m_strokeTarget.fbo.id(), rect.x0, rect.y0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (m_wetCoverage) {
                clearTarget(m_strokeBase, rect);
            }
        } else {
            clearStrokeBuffer(rect);
        }
        m_layers.markDirty(rect);
        rect = {};
    }
    m_strokeBounds = {};
    std::fill(m_provisionalRects.begin(), m_provisionalRects.end(), IRect{});
    std::fill(m_wetCursors.begin(), m_wetCursors.end(), WetCursor{});
    for (WetTail& tail : m_wetTails) {
        tail.rect = {};
    }
}

// --- Pintar ---

void Canvas::flushDabs() {
    if (!m_stroking) {
        return;
    }
    applyShape();
    if (m_wet) {
        flushWet();
        return;
    }
    m_dabs.clear();
    m_path.takeFinal(m_dabs);
    if (!m_useBase) {
        if (m_dabs.empty()) {
            return;
        }
        expandCopies(m_dabs, m_rects[0]);
        drawCopies(m_strokeTarget.fbo.id());
        for (size_t k = 0; k < m_copies.size(); ++k) {
            touch(k, m_rects[0][k]);
        }
        return;
    }
    if (m_dabs.empty() && m_path.revision() == m_drawnRevision) {
        return;
    }

    // Los definitivos van a la base. Lo que se ve es la base más los provisionales: se
    // copia la base sobre los provisionales de antes y lo nuevo, y encima van los de ahora.
    std::vector<IRect>& finals = m_rects[0];
    std::vector<IRect>& provisional = m_rects[1];
    expandCopies(m_dabs, finals);
    drawCopies(m_strokeBase.fbo.id());
    expandCopies(m_path.provisional(), provisional);
    glDisable(GL_SCISSOR_TEST);
    for (size_t k = 0; k < m_copies.size(); ++k) {
        IRect restore = m_provisionalRects[k];
        restore.unite(finals[k]);
        restore.unite(provisional[k]);
        if (!restore.empty()) {
            copyRect(m_strokeBase.fbo.id(), restore, m_strokeTarget.fbo.id(), restore.x0, restore.y0);
        }
        m_provisionalRects[k] = provisional[k];
        touch(k, restore);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    drawCopies(m_strokeTarget.fbo.id());
    m_drawnRevision = m_path.revision();
}

void Canvas::commitStroke() {
    if (m_wet) {
        commitWet();
        return;
    }
    // Con selección, la capa solo cambia en lo seleccionado.
    const std::vector<IRect> regions = strokeRegions(operationRect());
    if (!regions.empty()) {
        Layer& layer = m_layers.active();
        Compositor::Blend blend = Compositor::Blend::Over;
        if (m_strokeMode == StrokePreview::Mode::Erase) {
            blend = Compositor::Blend::Erase;
        } else if (m_strokeMode == StrokePreview::Mode::PaintAtop) {
            blend = Compositor::Blend::Atop;
        }
        changeRegions(layer, regions, [&](const IRect& rect) {
            m_compositor.draw(layer.target.fbo.id(), m_strokeTarget.texture.id(), m_strokeOpacity, blend, rect,
                              &m_strokeGrain, operationMask());
        });
    }
    // Lo que se pintó fuera de la selección tampoco se queda en el buffer.
    for (const IRect& rect : m_copyBounds) {
        clearStrokeBuffer(rect);
        m_layers.markDirty(rect);
    }
    resetStroke();
}

void Canvas::prepareWet(const IRect& needed) {
    const IRect all = IRect::ofSize(width(), height());
    IRect want = needed.intersected(all);
    if (want.empty() || (!m_wetValid.empty() && want.intersected(m_wetValid) == want)) {
        return;
    }
    // Con margen: el trazo sigue por ahí y así no se copia en cada tanda.
    const int margin = std::max(64, static_cast<int>(std::ceil(m_wetMix.radius * 2.0f)));
    want = IRect{want.x0 - margin, want.y0 - margin, want.x1 + margin, want.y1 + margin}.intersected(all);
    IRect grown = m_wetValid;
    grown.unite(want);
    // Solo lo que falta: lo que ya estaba tiene el trazo. Alrededor de una caja dentro de
    // otra quedan como mucho cuatro franjas.
    std::array<IRect, 4> parts{};
    if (m_wetValid.empty()) {
        parts[0] = grown;
    } else {
        const IRect& v = m_wetValid;
        parts[0] = {grown.x0, grown.y0, grown.x1, v.y0};
        parts[1] = {grown.x0, v.y1, grown.x1, grown.y1};
        parts[2] = {grown.x0, v.y0, v.x0, v.y1};
        parts[3] = {v.x1, v.y0, grown.x1, v.y1};
    }
    const GLuint layer = m_layers.active().target.fbo.id();
    glDisable(GL_SCISSOR_TEST);
    for (const IRect& part : parts) {
        if (!part.empty()) {
            copyRect(layer, part, m_strokeTarget.fbo.id(), part.x0, part.y0);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_wetValid = grown;
}

const gfx::RenderTarget* Canvas::wetCoverage() const { return m_wetCoverage ? &m_strokeBase : nullptr; }

void Canvas::flushWet() {
    m_dabs.clear();
    m_path.takeFinal(m_dabs);
    const std::vector<Dab>& provisional = m_path.provisional();
    if (m_dabs.empty() && (!m_path.tapered() || m_path.revision() == m_drawnRevision)) {
        return;
    }
    const IRect canvas = IRect::ofSize(width(), height());
    const size_t copies = m_copies.size();
    std::vector<IRect>& changed = m_rects[0];
    changed.assign(copies, IRect{});
    // El final afinado que se pintó de prueba se quita antes de seguir: los sellos
    // definitivos parten de lo que dejaron los anteriores.
    for (size_t k = 0; k < copies; ++k) {
        changed[k] = restoreWetTail(m_wetTails[k]);
    }
    // Cada copia sigue su camino y mezcla con lo que encuentra (las otras copias incluidas).
    std::vector<Dab>& dabs = m_copyDabs[0];
    for (size_t k = 0; k < copies && !m_dabs.empty(); ++k) {
        transformDabs(k, m_dabs, dabs);
        prepareWet(Brush::wetBounds(dabs, m_wetCursors[k]));
        changed[k].unite(m_brush.drawWet(m_strokeTarget, wetCoverage(), dabs, m_strokeParams, m_wetMix,
                                         m_wetCursors[k], m_copies[k].mirrored));
    }
    if (!provisional.empty()) {
        // Se guarda lo que van a pisar para quitarlos en la tanda siguiente: de todas las
        // copias antes de pintar ninguna, así se devuelve bien aunque se solapen.
        for (size_t k = 0; k < copies; ++k) {
            transformDabs(k, provisional, m_wetDabs[k]);
            prepareWet(Brush::wetBounds(m_wetDabs[k], m_wetCursors[k]));
        }
        for (size_t k = 0; k < copies; ++k) {
            saveWetTail(m_wetTails[k], Brush::bounds(m_wetDabs[k]).intersected(canvas));
        }
        for (size_t k = 0; k < copies; ++k) {
            if (m_wetTails[k].rect.empty()) {
                continue;   // sin memoria para guardarlo: el final se ve al levantar el lápiz
            }
            WetCursor tail = m_wetCursors[k];
            changed[k].unite(m_brush.drawWet(m_strokeTarget, wetCoverage(), m_wetDabs[k], m_strokeParams, m_wetMix,
                                             tail, m_copies[k].mirrored));
        }
    }
    m_drawnRevision = m_path.revision();
    for (size_t k = 0; k < copies; ++k) {
        touch(k, changed[k].intersected(canvas));
    }
}

bool Canvas::saveWetTail(WetTail& tail, const IRect& rect) {
    tail.rect = {};
    if (rect.empty()) {
        return false;
    }
    const bool coverage = wetCoverage() != nullptr;
    for (gfx::RenderTarget* copy : {&tail.pixels, &tail.coverage}) {
        if (copy == &tail.coverage && !coverage) {
            continue;
        }
        if (*copy && copy->width >= rect.width() && copy->height >= rect.height()) {
            continue;
        }
        // Con margen: el final crece y encoge con la presión.
        const int w = std::max(rect.width() + rect.width() / 2, copy->width);
        const int h = std::max(rect.height() + rect.height() / 2, copy->height);
        if (!copy->create(std::min(w, width()), std::min(h, height()))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Sin memoria para ver el afinado mientras se pinta");
            return false;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    copyRect(m_strokeTarget.fbo.id(), rect, tail.pixels.fbo.id(), 0, 0);
    if (coverage) {
        copyRect(m_strokeBase.fbo.id(), rect, tail.coverage.fbo.id(), 0, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    tail.rect = rect;
    return true;
}

IRect Canvas::restoreWetTail(WetTail& tail) {
    const IRect rect = tail.rect;
    if (rect.empty()) {
        return {};
    }
    const IRect stored = IRect::ofSize(rect.width(), rect.height());
    glDisable(GL_SCISSOR_TEST);
    copyRect(tail.pixels.fbo.id(), stored, m_strokeTarget.fbo.id(), rect.x0, rect.y0);
    if (wetCoverage()) {
        copyRect(tail.coverage.fbo.id(), stored, m_strokeBase.fbo.id(), rect.x0, rect.y0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    tail.rect = {};
    return rect;
}

void Canvas::commitWet() {
    // La copia de trabajo ya tiene el resultado (y solo cambió en lo seleccionado).
    const std::vector<IRect> regions = strokeRegions(operationRect());
    if (!regions.empty()) {
        Layer& layer = m_layers.active();
        changeRegions(layer, regions, [&](const IRect& rect) {
            glDisable(GL_SCISSOR_TEST);
            copyRect(m_strokeTarget.fbo.id(), rect, layer.target.fbo.id(), rect.x0, rect.y0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        });
    }
    endWet();
}

void Canvas::endWet() {
    // Los dos buffers vuelven a quedar transparentes (la cobertura va en el segundo).
    clearStrokeBuffer(m_wetValid);
    // Lo que se veía de la copia de trabajo vuelve a salir de la capa.
    for (const IRect& rect : m_copyBounds) {
        m_layers.markDirty(rect);
    }
    resetStroke();
}

void Canvas::clearStrokeBuffer(const IRect& rect) {
    clearTarget(m_strokeTarget, rect);
    clearTarget(m_strokeBase, rect);
}

// -----------------------------------------------------------------------------
// Guía de dibujo
// -----------------------------------------------------------------------------

void Canvas::setGuide(const DrawingGuide& guide) {
    DrawingGuide g = guide;
    const glm::vec2 size(static_cast<float>(width()), static_cast<float>(height()));
    if (!std::isfinite(g.center.x) || !std::isfinite(g.center.y)) {
        g.center = size * 0.5f;
    }
    g.center = glm::clamp(g.center, glm::vec2(0.0f), size);
    g.angle = std::isfinite(g.angle) ? std::remainder(g.angle, 2.0f * 3.14159265358979f) : 0.0f;
    g.opacity = std::isfinite(g.opacity) ? std::clamp(g.opacity, 0.0f, 1.0f) : 1.0f;
    g.gridSize = std::isfinite(g.gridSize) ? std::clamp(g.gridSize, guide::kMinGridSize, guide::kMaxGridSize)
                                           : guide::defaults(size).gridSize;
    m_guide = g;
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
    settle();
    m_layers.setActive(index);
}

bool Canvas::addLayer() {
    if (!m_ready || m_layers.count() >= maxLayers()) {
        return false;
    }
    settle();
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
    settle();

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
    settle();
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
    settle();
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
    settle();

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
    settle();
    Layer& layer = m_layers.at(index);
    // Con selección, solo cambia lo seleccionado.
    const IRect rect = operationRect();
    if (rect.empty()) {
        return;
    }
    HistoryStep step;
    step.kind = HistoryStep::Kind::Pixels;
    step.layerId = layer.id;
    step.rect = rect;
    const bool saved = saveRegion(layer, step.rect, step.pixels);

    change(layer, step.rect, operationMask());
    ++layer.revision;
    m_layers.markDirty(rect);

    if (saved) {
        step.bytes = bytesOf(rect.width(), rect.height());
        record(std::move(step));
    } else {
        m_history.clear();
    }
}

void Canvas::clearLayer(int index) {
    changePixels(index, [this](Layer& layer, const IRect& rect, GLuint mask) {
        if (mask) {
            m_compositor.clear(layer.target.fbo.id(), rect, mask);
        } else {
            fill(layer, 0.0f, 0.0f, 0.0f, 0.0f);
        }
    });
}

void Canvas::fillLayer(int index, const float rgb[3]) {
    float color[3];
    for (int i = 0; i < 3; ++i) {
        color[i] = std::clamp(rgb[i], 0.0f, 1.0f);
    }
    changePixels(index, [&](Layer& layer, const IRect& rect, GLuint mask) {
        m_compositor.fill(layer.target.fbo.id(), color,
                          layer.alphaLock ? Compositor::Fill::Atop : Compositor::Fill::Replace, rect, mask);
    });
}

void Canvas::invertLayer(int index) {
    changePixels(index, [this](Layer& layer, const IRect& rect, GLuint mask) {
        if (mask) {
            m_compositor.filter(layer.target, Compositor::Filter::Invert, mask, m_strokeTarget, rect);
        } else {
            m_compositor.invert(layer.target.fbo.id(), rect);
        }
    });
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
    settle();
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
    settle();
    const LayerProperties before = properties(index);
    m_layers.setAlphaLock(index, locked);
    recordProperties(index, before);
}

void Canvas::setLayerClipping(int index, bool clipping) {
    if (!m_ready || !m_layers.validIndex(index) || m_layers.at(index).clipping == clipping ||
        (clipping && !canClip(index))) {
        return;
    }
    settle();
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
    settle();
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

    case Kind::Selection:
        if (!step.rect.empty() && step.pixels) {
            swapSelection(step.rect, step.pixels);
        }
        setSelectionState(undo ? step.selectionBefore : step.selectionAfter);
        break;

    case Kind::Group:
        if (undo) {
            for (auto it = step.children.rbegin(); it != step.children.rend(); ++it) {
                applyStep(*it, true);
            }
        } else {
            for (HistoryStep& child : step.children) {
                applyStep(child, false);
            }
        }
        break;
    }
}

bool Canvas::undo() {
    if (!m_ready) {
        return false;
    }
    endStroke();
    finishLayerEdit();
    endAutoSelect(true);
    endFeather(true);
    endFill(true);
    endAdjust(true);
    // Una transformación a medias no está en el historial: deshacer la cancela.
    if (m_transform.active) {
        cancelTransform();
        return true;
    }
    HistoryStep* step = m_history.stepToUndo();
    if (!step) {
        return false;
    }
    applyStep(*step, true);
    return true;
}

bool Canvas::redo() {
    // Con algo a medias no hay nada que rehacer: cerrarlo guardaría un paso nuevo y eso
    // descarta lo que se podía rehacer.
    if (!m_ready || m_stroking || !canRedo()) {
        return false;
    }
    endStroke();
    finishLayerEdit();
    endAutoSelect(true);
    endFeather(true);
    endFill(true);
    endAdjust(true);
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
    if (m_layers.dirty().empty()) {
        return false;
    }

    StrokePreview preview;
    preview.texture = m_strokeTarget.texture.id();
    preview.opacity = m_strokeOpacity;
    preview.mode = m_strokeMode;
    preview.grain = m_strokeGrain;
    preview.selection = operationMask();
    // Un trazo húmedo se ve en lugar de la capa, donde ya está su copia de trabajo.
    preview.area = m_wet ? m_wetValid : IRect::ofSize(width(), height());
    const StrokePreview* shown = m_stroking ? &preview : nullptr;
    // Transformando, la capa se ve con la copia de trabajo (lo que quedaría al aplicar).
    StrokePreview working;
    if (m_transform.active && m_layers.indexOf(m_transform.layerId) == m_layers.activeIndex()) {
        working.texture = m_strokeTarget.texture.id();
        working.mode = StrokePreview::Mode::Replace;
        working.area = IRect::ofSize(width(), height());
        shown = &working;
    }
    // Rellenando, la capa se ve con el resultado donde está la vista previa.
    StrokePreview filled;
    if (m_fill.active && m_layers.indexOf(m_fill.layerId) == m_layers.activeIndex()) {
        filled.texture = m_strokeTarget.texture.id();
        filled.mode = StrokePreview::Mode::Replace;
        filled.area = m_fill.drawn;
        shown = &filled;
    }
    // Ajustando, la capa se ve con el resultado en la zona del ajuste (salvo al comparar).
    StrokePreview adjusted;
    if (m_adjust.active && !m_adjust.original && m_layers.indexOf(m_adjust.layerId) == m_layers.activeIndex()) {
        adjusted.texture = m_strokeTarget.texture.id();
        adjusted.mode = StrokePreview::Mode::Replace;
        adjusted.area = m_adjust.drawn;
        shown = &adjusted;
    }
    // Cada zona por su lado: con simetría pueden quedar lejos unas de otras.
    for (const IRect& rect : m_layers.dirtyRects()) {
        m_compositor.compose(m_layers, rect, shown);
    }

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
    settle();

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
    resetStroke();
    m_layerEdit = {};
    m_history.clear();
    m_clipboard = {};   // la selección también se pierde: vive en la GPU
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
