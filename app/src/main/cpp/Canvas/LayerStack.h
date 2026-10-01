#pragma once

#include "Canvas/Layer.h"
#include "Canvas/Rect.h"

#include <memory>
#include <string>
#include <vector>

// Pila de capas: orden, capa activa, nombres y propiedades. El índice 0 es la capa de
// abajo. Hay un único índice de capa activa (el que usan el pincel y el menú) y se
// mantiene apuntando a la misma capa cuando otras se insertan, borran o mueven.
//
// Cada cambio que afecta a la imagen marca una región sucia; Canvas la usa para
// recomponer solo lo necesario.
class LayerStack {
public:
    // Deja la pila vacía con el tamaño dado. Las capas se crean con insert().
    void reset(int width, int height);
    void clearAll();

    int width() const { return m_width; }
    int height() const { return m_height; }
    int count() const { return static_cast<int>(m_layers.size()); }
    bool validIndex(int index) const { return index >= 0 && index < count(); }

    Layer& at(int index) { return *m_layers[static_cast<size_t>(index)]; }
    const Layer& at(int index) const { return *m_layers[static_cast<size_t>(index)]; }
    int indexOf(uint32_t id) const;

    int activeIndex() const { return m_active; }
    Layer& active() { return at(m_active); }
    const Layer& active() const { return at(m_active); }
    void setActive(int index);

    // Crea una capa transparente en `position` (0..count) y la hace activa.
    Layer* insert(int position, std::string name);
    // Borra una capa. Nunca deja la pila vacía. La activa pasa a la de abajo si se borra.
    bool remove(int index);
    // Como remove(), pero devuelve la capa en vez de destruirla (para deshacer).
    std::unique_ptr<Layer> take(int index);
    // Vuelve a meter en `position` una capa sacada con take() y la hace activa.
    Layer* put(int position, std::unique_ptr<Layer> layer);
    // Mueve la capa `from` a la posición `to`. La activa sigue siendo la misma capa.
    bool move(int from, int to);

    void rename(int index, std::string name);
    void setVisible(int index, bool visible);
    void setOpacity(int index, float opacity);
    void setBlend(int index, BlendMode blend);
    void setAlphaLock(int index, bool locked);
    void setClipping(int index, bool clipping);
    // Capa de referencia del relleno: como mucho una. -1 la quita.
    void setReference(int index);
    int referenceIndex() const;

    // Base de una capa con máscara de recorte: la primera de debajo que no recorta. -1 si
    // la capa no recorta o no tiene base (está abajo del todo): entonces se ve entera.
    int clipBase(int index) const;

    // Nombre "<base> N" que no usa ninguna capa.
    std::string uniqueName(const std::string& base) const;
    // `wanted` si ninguna capa lo usa; si no, "<wanted> 2", "<wanted> 3"...
    std::string availableName(const std::string& wanted) const;

    // Región del lienzo que hay que recomponer: la caja que la cubre y las zonas sueltas que
    // la forman (que no se solapan; como mucho kMaxDirtyRects). Un trazo con simetría marca
    // zonas lejanas entre sí, y recomponer toda la caja que las abarca costaría mucho más.
    static constexpr size_t kMaxDirtyRects = 16;
    const IRect& dirty() const { return m_dirty; }
    const std::vector<IRect>& dirtyRects() const { return m_dirtyRects; }
    void markDirty(const IRect& rect);
    void markAllDirty() { markDirty(IRect::ofSize(m_width, m_height)); }
    void clearDirty() {
        m_dirty = {};
        m_dirtyRects.clear();
    }

private:
    bool nameInUse(const std::string& name) const;

    std::vector<std::unique_ptr<Layer>> m_layers;
    int m_active = 0;
    uint32_t m_nextId = 1;
    int m_width = 0;
    int m_height = 0;
    IRect m_dirty;
    std::vector<IRect> m_dirtyRects;
};
