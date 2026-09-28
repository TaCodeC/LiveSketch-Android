#pragma once

#include "Canvas/Brush.h"
#include "Canvas/Compositor.h"
#include "Canvas/LayerStack.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// El lienzo: capas, pincel, trazo en curso y compuesto. Todo en el hilo del contexto GL.
//
// El trazo en curso se pinta en un buffer aparte (tamaño del lienzo) y se ve encima de
// la capa activa. Al terminar se funde con la capa aplicando la opacidad del pincel, o
// se usa para borrar si el borrador está activo.
class Canvas {
public:
    // Crea el lienzo con una capa "Fondo" blanca y una "Capa 1" transparente encima.
    bool init(int width, int height);
    void destroy();
    bool ready() const { return m_ready; }
    int width() const { return m_layers.width(); }
    int height() const { return m_layers.height(); }

    // Para leer la pila y cambiar nombre, visibilidad u opacidad. Lo que cambia la
    // estructura (añadir, borrar, mover, capa activa) va por los métodos de abajo.
    LayerStack& layers() { return m_layers; }
    const LayerStack& layers() const { return m_layers; }

    BrushSettings& brushSettings() { return m_settings; }
    const BrushSettings& brushSettings() const { return m_settings; }
    bool setBrushType(int type);

    // Trazo en coordenadas del lienzo, con presión de 0 a 1. `eraserTip`: se usa la goma
    // del lápiz, así que el trazo borra aunque el borrador del menú esté apagado.
    // beginStroke devuelve false (y no dibuja) si la capa activa está oculta.
    bool beginStroke(float x, float y, float pressure, bool eraserTip = false);
    void strokeTo(float x, float y, float pressure);
    void endStroke();
    void cancelStroke();
    bool stroking() const { return m_stroking; }

    // Operaciones de capas. Todas cierran antes el trazo en curso.
    int maxLayers() const;
    void selectLayer(int index);
    bool addLayer();                // encima de la activa
    bool duplicateLayer(int index); // la copia queda encima y activa
    bool removeLayer(int index);
    bool moveLayer(int from, int to);
    bool canMergeDown(int index) const;
    bool mergeDown(int index);      // funde la capa con la de abajo
    void clearLayer(int index);     // la deja transparente

    // Pinta los dabs pendientes y recompone la región sucia. Devuelve true si el
    // compuesto cambió.
    bool update();
    const gfx::RenderTarget& composite() const { return m_compositor.composite(); }
    // Sube cada vez que cambia el compuesto.
    uint64_t version() const { return m_version; }

    // Lee el compuesto: RGBA8 premultiplicado, filas de arriba abajo.
    bool readComposite(std::vector<uint8_t>& pixels);

    // Pérdida del contexto GL. takeSnapshot copia todas las capas a RAM si ocupan como
    // mucho `maxBytes`; recreateGpu crea de nuevo todos los objetos GL y restaura las
    // capas desde esa copia. Sin copia, las capas quedan vacías (el fondo, blanco).
    bool takeSnapshot(size_t maxBytes);
    void dropSnapshot();
    bool hasSnapshot() const { return !m_snapshot.empty(); }
    bool recreateGpu(bool* restored);

private:
    bool createGpuObjects();
    void destroyGpuObjects();
    void fill(Layer& layer, float r, float g, float b, float a);
    void flushDabs();
    void commitStroke();
    void clearStrokeBuffer(const IRect& rect);

    bool m_ready = false;
    LayerStack m_layers;
    Compositor m_compositor;
    Brush m_brush;
    BrushSettings m_settings;

    gfx::RenderTarget m_strokeTarget;
    bool m_stroking = false;
    IRect m_strokeBounds;
    float m_strokeOpacity = 1.0f;
    bool m_strokeErase = false;
    float m_strokeColor[3] = {0.0f, 0.0f, 0.0f};

    uint64_t m_version = 0;
    std::vector<std::vector<uint8_t>> m_snapshot; // una entrada por capa, de abajo arriba
};
