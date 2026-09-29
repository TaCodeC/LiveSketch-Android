#pragma once

#include "Canvas/Brush.h"
#include "Canvas/Compositor.h"
#include "Canvas/History.h"
#include "Canvas/LayerStack.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// El lienzo: capas, pincel, trazo en curso y compuesto. Todo en el hilo del contexto GL.
//
// El trazo en curso se pinta en un buffer aparte (tamaño del lienzo) y se ve encima de
// la capa activa. Al terminar se funde con la capa aplicando la opacidad del pincel (si
// la capa tiene el alfa bloqueado, solo donde ya hay pintura), o se usa para borrar si
// el borrador está activo.
//
// Los trazos y las operaciones de capas se pueden deshacer: antes de cambiar píxeles se
// copia la zona afectada a una textura (en la GPU, sin pasar por la RAM).
class Canvas {
public:
    // Crea el lienzo con una capa "Fondo" blanca y una "Capa 1" transparente encima.
    bool init(int width, int height);
    void destroy();
    bool ready() const { return m_ready; }
    int width() const { return m_layers.width(); }
    int height() const { return m_layers.height(); }

    // Para leer la pila. Lo que se puede deshacer (estructura, píxeles y propiedades de
    // las capas) va por los métodos de abajo; cambiar la pila directamente no se guarda.
    LayerStack& layers() { return m_layers; }
    const LayerStack& layers() const { return m_layers; }

    BrushSettings& brushSettings() { return m_settings; }
    const BrushSettings& brushSettings() const { return m_settings; }
    bool setBrushType(int type);

    // Por qué no se puede pintar en la capa activa.
    enum class StrokeBlock {
        None,
        Hidden,          // la capa está oculta
        ClipBaseHidden,  // recorta con una capa oculta, así que no se ve
        AlphaLocked,     // borrar en una capa con el alfa bloqueado
    };
    StrokeBlock strokeBlock(bool eraserTip = false) const;

    // Trazo en coordenadas del lienzo, con presión de 0 a 1. `eraserTip`: se usa la goma
    // del lápiz, así que el trazo borra aunque el borrador del menú esté apagado.
    // beginStroke devuelve false (y no dibuja) si strokeBlock() lo impide.
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
    // La capa se ve en el compuesto: está visible y, si recorta, su base también.
    bool layerShown(int index) const;
    bool canMergeDown(int index) const;
    // Funde la capa con la de abajo tal como se ven (su modo de fusión y su recorte). La
    // capa resultante conserva las propiedades de la de abajo, con la opacidad horneada.
    bool mergeDown(int index);
    void clearLayer(int index);     // la deja transparente
    // Rellena la capa con un color opaco; con el alfa bloqueado, solo donde hay pintura.
    void fillLayer(int index, const float rgb[3]);
    void invertLayer(int index);    // invierte sus colores (el alfa no cambia)

    // Propiedades de capa. La opacidad y el modo de fusión se pueden cambiar de forma
    // continua (arrastrando, probando modos): con `final` = false no se guarda paso de
    // deshacer; lo guarda la llamada con `final` = true, o finishLayerEdit(), desde el
    // valor que tenía antes de empezar.
    void setLayerVisible(int index, bool visible);
    void setLayerOpacity(int index, float opacity, bool final = true);
    void setLayerBlend(int index, BlendMode blend, bool final = true);
    void finishLayerEdit();
    void setLayerAlphaLock(int index, bool locked);
    // Máscara de recorte. La capa de abajo del todo no puede recortar.
    bool canClip(int index) const { return m_layers.validIndex(index) && index > 0; }
    void setLayerClipping(int index, bool clipping);
    // Capa de referencia del relleno (como mucho una). -1 la quita.
    void setReferenceLayer(int index);
    void renameLayer(int index, std::string name);

    // Deshacer y rehacer. Terminan antes el trazo en curso y el cambio de propiedades.
    bool canUndo() const { return m_history.canUndo() || editPending(); }
    bool canRedo() const { return m_history.canRedo() && !editPending(); }
    bool undo();
    bool redo();
    const History& history() const { return m_history; }

    // Color del compuesto en un punto del lienzo, sin premultiplicar. Devuelve false si
    // el punto cae fuera del lienzo o es transparente.
    bool pickColor(float x, float y, float rgb[3]);

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
    // Cambia los píxeles de toda la capa con `change` y guarda el paso de deshacer.
    template <typename Change>
    void changePixels(int index, Change change);

    // Deshacer.
    LayerProperties properties(int index) const;
    void applyProperties(int index, const LayerProperties& properties);
    // Guarda un paso si las propiedades de la capa ya no son `before`.
    void recordProperties(int index, const LayerProperties& before);
    void beginLayerEdit(int index);
    bool editPending() const;
    bool saveRegion(const Layer& layer, const IRect& rect, gfx::RenderTarget& out);
    void swapRegion(Layer& layer, const IRect& rect, gfx::RenderTarget& stored);
    void record(HistoryStep step);
    void applyStep(HistoryStep& step, bool undo);
    size_t layerBytes() const;

    bool m_ready = false;
    LayerStack m_layers;
    Compositor m_compositor;
    Brush m_brush;
    BrushSettings m_settings;

    gfx::RenderTarget m_strokeTarget;
    bool m_stroking = false;
    IRect m_strokeBounds;
    float m_strokeOpacity = 1.0f;
    StrokePreview::Mode m_strokeMode = StrokePreview::Mode::Paint;
    float m_strokeColor[3] = {0.0f, 0.0f, 0.0f};

    uint64_t m_version = 0;
    std::vector<std::vector<uint8_t>> m_snapshot; // una entrada por capa, de abajo arriba

    History m_history;
    struct LayerEdit {
        bool active = false;
        uint32_t layerId = 0;
        LayerProperties before;
    } m_layerEdit;
};
