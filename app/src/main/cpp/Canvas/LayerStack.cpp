#include "Canvas/LayerStack.h"

#include <algorithm>

void LayerStack::reset(int width, int height) {
    clearAll();
    m_width = width;
    m_height = height;
}

void LayerStack::clearAll() {
    m_layers.clear();
    m_active = 0;
    m_dirty = {};
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

void LayerStack::markDirty(const IRect& rect) {
    m_dirty.unite(rect.intersected(IRect::ofSize(m_width, m_height)));
}
