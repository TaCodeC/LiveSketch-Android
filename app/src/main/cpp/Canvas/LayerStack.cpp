#include "Canvas/LayerStack.h"

#include <algorithm>
#include <cstdint>
#include <limits>

void LayerStack::reset(int width, int height) {
    clearAll();
    m_width = width;
    m_height = height;
}

void LayerStack::clearAll() {
    m_layers.clear();
    m_active = 0;
    clearDirty();
}

int LayerStack::indexOf(uint32_t id) const {
    for (int i = 0; i < count(); ++i) {
        if (at(i).id == id) {
            return i;
        }
    }
    return -1;
}

void LayerStack::setActive(int index) {
    if (validIndex(index)) {
        m_active = index;
    }
}

Layer* LayerStack::insert(int position, std::string name) {
    position = std::clamp(position, 0, count());

    auto layer = std::make_unique<Layer>();
    if (!layer->target.create(m_width, m_height)) {
        return nullptr;
    }
    layer->id = m_nextId++;
    layer->name = name.empty() ? uniqueName("Capa") : std::move(name);

    // Una capa transparente no cambia la imagen: no hace falta recomponer.
    Layer* raw = layer.get();
    m_layers.insert(m_layers.begin() + position, std::move(layer));
    m_active = position;
    return raw;
}

bool LayerStack::remove(int index) {
    return take(index) != nullptr;
}

std::unique_ptr<Layer> LayerStack::take(int index) {
    if (!validIndex(index) || count() <= 1) {
        return nullptr;
    }
    std::unique_ptr<Layer> layer = std::move(m_layers[static_cast<size_t>(index)]);
    m_layers.erase(m_layers.begin() + index);
    if (index < m_active || (index == m_active && m_active > 0)) {
        --m_active;
    }
    m_active = std::clamp(m_active, 0, count() - 1);
    markAllDirty();
    return layer;
}

Layer* LayerStack::put(int position, std::unique_ptr<Layer> layer) {
    if (!layer) {
        return nullptr;
    }
    position = std::clamp(position, 0, count());
    Layer* raw = layer.get();
    m_layers.insert(m_layers.begin() + position, std::move(layer));
    m_active = position;
    markAllDirty();
    return raw;
}

bool LayerStack::move(int from, int to) {
    if (!validIndex(from) || !validIndex(to) || from == to) {
        return false;
    }
    const Layer* activeLayer = m_layers[static_cast<size_t>(m_active)].get();

    auto layer = std::move(m_layers[static_cast<size_t>(from)]);
    m_layers.erase(m_layers.begin() + from);
    m_layers.insert(m_layers.begin() + to, std::move(layer));

    for (int i = 0; i < count(); ++i) {
        if (m_layers[static_cast<size_t>(i)].get() == activeLayer) {
            m_active = i;
            break;
        }
    }
    markAllDirty();
    return true;
}

void LayerStack::rename(int index, std::string name) {
    if (validIndex(index) && !name.empty()) {
        at(index).name = std::move(name);
    }
}

void LayerStack::setVisible(int index, bool visible) {
    if (validIndex(index) && at(index).visible != visible) {
        at(index).visible = visible;
        markAllDirty();
    }
}

void LayerStack::setOpacity(int index, float opacity) {
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (validIndex(index) && at(index).opacity != opacity) {
        at(index).opacity = opacity;
        markAllDirty();
    }
}

void LayerStack::setBlend(int index, BlendMode blend) {
    if (validIndex(index) && at(index).blend != blend) {
        at(index).blend = blend;
        markAllDirty();
    }
}

void LayerStack::setAlphaLock(int index, bool locked) {
    // Solo cambia cómo se pinta, no lo que se ve.
    if (validIndex(index)) {
        at(index).alphaLock = locked;
    }
}

void LayerStack::setClipping(int index, bool clipping) {
    if (validIndex(index) && at(index).clipping != clipping) {
        at(index).clipping = clipping;
        markAllDirty();
    }
}

void LayerStack::setReference(int index) {
    for (int i = 0; i < count(); ++i) {
        at(i).reference = i == index;
    }
}

int LayerStack::referenceIndex() const {
    for (int i = 0; i < count(); ++i) {
        if (at(i).reference) {
            return i;
        }
    }
    return -1;
}

int LayerStack::clipBase(int index) const {
    if (!validIndex(index) || !at(index).clipping) {
        return -1;
    }
    for (int i = index - 1; i >= 0; --i) {
        if (!at(i).clipping) {
            return i;
        }
    }
    return -1;
}

bool LayerStack::nameInUse(const std::string& name) const {
    return std::any_of(m_layers.begin(), m_layers.end(), [&](const auto& layer) { return layer->name == name; });
}

std::string LayerStack::uniqueName(const std::string& base) const {
    for (int n = 1;; ++n) {
        std::string candidate = base + " " + std::to_string(n);
        if (!nameInUse(candidate)) {
            return candidate;
        }
    }
}

std::string LayerStack::availableName(const std::string& wanted) const {
    if (!nameInUse(wanted)) {
        return wanted;
    }
    for (int n = 2;; ++n) {
        std::string candidate = wanted + " " + std::to_string(n);
        if (!nameInUse(candidate)) {
            return candidate;
        }
    }
}

namespace {

// Las zonas a menos de esto se juntan: recomponer un poco de más cuesta menos que otra pasada.
constexpr int kDirtyJoin = 16;

int64_t area(const IRect& r) { return r.empty() ? 0 : int64_t{r.width()} * int64_t{r.height()}; }

} // namespace

void LayerStack::markDirty(const IRect& rect) {
    const IRect r = rect.intersected(IRect::ofSize(m_width, m_height));
    if (r.empty()) {
        return;
    }
    m_dirty.unite(r);
    addRegion(m_dirtyRects, r, kDirtyJoin);
    // Demasiadas: se juntan las dos que menos área añaden.
    while (m_dirtyRects.size() > kMaxDirtyRects) {
        size_t bestA = 0;
        size_t bestB = 1;
        int64_t bestCost = std::numeric_limits<int64_t>::max();
        for (size_t a = 0; a < m_dirtyRects.size(); ++a) {
            for (size_t b = a + 1; b < m_dirtyRects.size(); ++b) {
                IRect u = m_dirtyRects[a];
                u.unite(m_dirtyRects[b]);
                const int64_t cost = area(u) - area(m_dirtyRects[a]) - area(m_dirtyRects[b]);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestA = a;
                    bestB = b;
                }
            }
        }
        IRect joined = m_dirtyRects[bestA];
        joined.unite(m_dirtyRects[bestB]);
        m_dirtyRects.erase(m_dirtyRects.begin() + static_cast<std::ptrdiff_t>(bestB));
        m_dirtyRects.erase(m_dirtyRects.begin() + static_cast<std::ptrdiff_t>(bestA));
        addRegion(m_dirtyRects, joined, kDirtyJoin);
    }
}
