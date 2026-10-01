#pragma once

#include "Canvas/Bounds.h"
#include "Canvas/Brush.h"
#include "Canvas/ColorFill.h"
#include "Canvas/Compositor.h"
#include "Canvas/DrawingGuide.h"
#include "Canvas/History.h"
#include "Canvas/ImageAdjust.h"
#include "Canvas/LayerStack.h"
#include "Canvas/QuickShape.h"
#include "Canvas/Selection.h"
#include "Canvas/SelectionShapes.h"
#include "Canvas/StrokePath.h"
#include "Canvas/Warp.h"

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// El lienzo: capas, pincel, trazo en curso y compuesto. Todo en el hilo del contexto GL.
//
// El trazo en curso se pinta en un buffer aparte (tamaño del lienzo) y se ve encima de
// la capa activa. Al terminar se funde con la capa aplicando la opacidad del pincel y el
// grano del papel (si la capa tiene el alfa bloqueado, solo donde ya hay pintura), o se
// usa para borrar si el borrador está activo. Si el pincel afina el final, los sellos
// definitivos se guardan en un segundo buffer y el primero es ese más los provisionales.
//
// Un pincel húmedo y Difuminar mezclan con lo que ya hay: el buffer es entonces una copia
// de trabajo de la capa (se copia por zonas, según avanza el trazo), se pinta sobre ella
// y se ve en lugar de la capa; al terminar, lo que cambió pasa a la capa.
//
// Los trazos y las operaciones de capas se pueden deshacer: antes de cambiar píxeles se
// copia la zona afectada a una textura (en la GPU, sin pasar por la RAM).
//
// Con una selección activa, pintar, borrar, rellenar, vaciar e invertir solo cambian lo
// seleccionado, y copiar y transformar toman solo eso (ver CanvasSelection.cpp).
//
// Con la simetría de la guía de dibujo, cada sello del trazo se pinta también en sus copias
// (reflejadas o giradas alrededor del centro de la guía), y con la forma rápida el trazo se
// vuelve a pintar entero como la forma perfecta que se le parece.
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

    // El pincel se lee al empezar cada trazo.
    BrushSettings& brushSettings() { return m_settings; }
    const BrushSettings& brushSettings() const { return m_settings; }
    // Píxeles del lienzo por punto de pantalla con el zoom actual: la estabilización se
    // mide en la pantalla.
    void setViewScale(float canvasPixelsPerPoint);

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

    // --- Forma rápida ---
    // Convierte el trazo en curso en la forma perfecta que se le parece (ver QuickShape.h):
    // se vuelve a pintar como ella, con la presión que solía llevar. Desde entonces strokeTo
    // la ajusta (ver quickshape::adjust) en vez de seguir el trazo. Devuelve false si no se
    // parece a ninguna (el trazo sigue igual).
    bool snapStroke(const quickshape::Options& options);
    // La hace perfecta (ver quickshape::regular), o la deja como se reconoció.
    void setShapeRegular(bool regular);
    // La forma del trazo en curso (Kind::None si no la tiene).
    const quickshape::Shape& strokeShape() const { return m_shape; }

    // --- Guía de dibujo ---
    // Cuadrícula o simetría (ver DrawingGuide.h). Es del documento: un lienzo nuevo empieza
    // con guide::defaults(). Cambiarla no se deshace y no cambia el trazo en curso.
    const DrawingGuide& guide() const { return m_guide; }
    void setGuide(const DrawingGuide& guide);

    // Resultado de las operaciones que pueden no llegar a hacerse.
    enum class Edit {
        Done,
        Nothing,    // no había nada que hacer (nada seleccionado en la capa, portapapeles vacío...)
        Hidden,     // la capa activa no se ve
        Full,       // no caben más capas
        NoMemory,
    };

    // Operaciones de capas. Todas cierran antes lo que esté a medias (ver settle()).
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
    // Con selección, las tres solo cambian lo seleccionado.
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

    // Deshacer y rehacer. Terminan antes el trazo en curso, el cambio de propiedades, la
    // selección automática, el difuminado, el relleno y el ajuste de imagen (así deshacer
    // los quita). Con una transformación a medias, deshacer la cancela (y no toca el
    // historial).
    bool canUndo() const { return m_history.canUndo() || editPending() || livePending(); }
    bool canRedo() const { return m_history.canRedo() && !editPending() && !livePending(); }
    bool undo();
    bool redo();
    const History& history() const { return m_history; }

    // Cierra lo que esté a medias antes de otra operación: el trazo, el cambio de
    // propiedades, la selección automática, el difuminado, el relleno y el ajuste de imagen
    // (se guardan) y la transformación (se aplica).
    void settle();

    // --- Selección (CanvasSelection.cpp) ---
    bool hasSelection() const { return m_selection.active(); }
    const SelectionState& selectionState() const { return m_selection.state(); }
    // Caja de lo seleccionado (vacía sin selección).
    IRect selectionBounds() const { return m_selection.bounds(); }
    // Máscara (R8 del tamaño del lienzo) para dibujar el borde; 0 sin selección.
    GLuint selectionMask() const { return m_selection.activeMask(); }
    // Sube cada vez que cambia la selección.
    uint64_t selectionVersion() const { return m_selectionVersion; }

    // Forma cerrada (lazo, rectángulo o elipse) en píxeles del lienzo. Devuelve false si
    // no cambia nada (la forma no cubre ningún píxel, o resta sin haber selección).
    bool selectPolygon(std::span<const glm::vec2> polygon, SelectOp op);
    // Selección automática: lo parecido al color del punto (x, y) y unido a él, en lo que
    // se ve (o en la capa de referencia, si la hay). `threshold` (0..1) se puede cambiar
    // en vivo; endAutoSelect la guarda o la deshace.
    bool beginAutoSelect(float x, float y, SelectOp op, float threshold);
    void setAutoThreshold(float threshold);
    void endAutoSelect(bool apply);
    bool autoSelecting() const { return m_auto.active; }
    // Lo que tiene pintado la capa (con su alfa).
    Edit selectLayerContent(int index);
    void selectAll();
    // Sin selección, selecciona todo.
    void invertSelection();
    // Difuminar el borde en vivo: `radius` en píxeles del lienzo, de 0 a maxFeather().
    bool beginFeather();
    void setFeather(float radius);
    void endFeather(bool apply);
    bool feathering() const { return m_feather.active; }
    float featherRadius() const { return m_feather.radius; }
    float maxFeather() const;
    void deselect();

    // --- Portapapeles (CanvasSelection.cpp) ---
    // Copian lo seleccionado de la capa activa (sin selección, la capa entera). Cortar lo
    // borra además de la capa.
    Edit copySelection();
    Edit cutSelection();
    bool canPaste() const { return static_cast<bool>(m_clipboard.pixels); }
    // En una capa nueva sobre la activa, en el sitio de donde se copió. Quita la
    // selección (lo pegado no tiene por qué tener que ver con ella), en el mismo paso.
    Edit paste();
    // Lo seleccionado de la capa activa a una capa nueva (sin selección, duplica la capa).
    Edit duplicateSelection();

    // --- Transformar (CanvasSelection.cpp) ---
    // Toma lo seleccionado de la capa activa (sin selección, toda la capa). Mientras dura,
    // la capa se ve con el resultado; setTransform lleva las esquinas de
    // transformSource() (arriba izquierda, arriba derecha, abajo derecha, abajo
    // izquierda) a `corners`. Aplicar lo funde en la capa (y mueve la selección igual);
    // cancelar lo deja como estaba. `wholeLayer`: toda la capa aunque haya selección (lo
    // que se acaba de duplicar ya es solo lo seleccionado: la máscara no se aplica dos
    // veces), y la selección se mueve con ella.
    Edit beginTransform(bool wholeLayer = false);
    bool transforming() const { return m_transform.active; }
    IRect transformSource() const { return m_transform.source; }
    bool setTransform(const glm::vec2 corners[4], bool nearest);
    void applyTransform();
    void cancelTransform();

    // --- Relleno (CanvasFill.cpp) ---
    // Arrastrar el color al lienzo: la capa activa se rellena con el color del pincel en
    // lo parecido al color del punto (x, y) y unido a él, en lo que se ve (o en la capa de
    // referencia, si la hay), como la selección automática. Con selección, solo en lo
    // seleccionado; con el alfa bloqueado, solo cambia el color de lo que ya está pintado.
    // `threshold` (0..1) se puede cambiar en vivo mientras se ve el resultado; endFill lo
    // aplica (un paso de deshacer) o lo quita, y devuelve si cambió la capa (con
    // selección, la zona puede caer fuera). beginFill devuelve Hidden si la capa activa no
    // se ve y Nothing si el punto cae fuera del lienzo.
    Edit beginFill(float x, float y, float threshold);
    void setFillThreshold(float threshold);
    bool endFill(bool apply);
    bool filling() const { return m_fill.active; }

    // --- Ajustes de imagen (CanvasAdjust.cpp) ---
    // Cambia la capa activa con un ajuste (ver ImageAdjust.h) cuyos valores se pueden
    // cambiar en vivo con setAdjust mientras se ve el resultado. Con selección, solo en lo
    // seleccionado (el desenfoque también lee lo de alrededor); con el alfa bloqueado, el
    // alfa de la capa no cambia. endAdjust lo aplica (un paso de deshacer, si cambia algo)
    // o lo quita, y devuelve si cambió la capa. beginAdjust devuelve Hidden si la capa
    // activa no se ve, y NoMemory (como setAdjust, false) si no caben los intermedios del
    // desenfoque; entonces se sigue viendo lo de antes.
    Edit beginAdjust(Adjustment kind, const AdjustParams& params);
    bool setAdjust(const AdjustParams& params);
    bool endAdjust(bool apply);
    bool adjusting() const { return m_adjust.active; }
    Adjustment adjustKind() const { return m_adjust.kind; }
    const AdjustParams& adjustParams() const { return m_adjust.params; }
    // Mientras `original`, la capa se ve sin el ajuste (para comparar).
    void showAdjustOriginal(bool original);
    // Sigma máxima del desenfoque, en píxeles del lienzo.
    float maxBlur() const;

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
    // Máscara para las operaciones de píxeles: la de la selección o 0.
    GLuint operationMask() const { return m_selection.activeMask(); }
    // Zona que cambia una operación de píxeles: lo seleccionado o todo el lienzo.
    IRect operationRect() const;
    void fill(Layer& layer, float r, float g, float b, float a);
    void flushDabs();
    void commitStroke();
    // Deja el trazo terminado: sin copias, sin forma y sin nada pintado pendiente.
    void resetStroke();
    // Simetría: `dabs` como los pinta la copia `copy`.
    void transformDabs(size_t copy, std::span<const Dab> dabs, std::vector<Dab>& out) const;
    // Las copias de `dabs` en m_copyDabs (sin reflejar y reflejadas), y en `rects` la caja
    // de cada una, recortada al lienzo.
    void expandCopies(std::span<const Dab> dabs, std::vector<IRect>& rects);
    // Pinta m_copyDabs en `target`.
    void drawCopies(GLuint target);
    // Lo pintado por la copia `copy` en `rect`.
    void touch(size_t copy, const IRect& rect);
    // Lo que pintó el trazo (cada copia por su lado, juntando las que se tocan) dentro de
    // `limit`.
    std::vector<IRect> strokeRegions(const IRect& limit) const;
    // Guarda el paso de deshacer de las zonas `regions` de la capa (sin memoria, olvida el
    // historial). Hay que llamarlo antes de cambiarlas: `finish` las cambia.
    template <typename Finish>
    void changeRegions(Layer& layer, const std::vector<IRect>& regions, Finish finish);
    // Forma rápida: quita lo que ha pintado el trazo, que sigue, y lo vuelve a empezar con la
    // forma si cambió.
    void discardStroke();
    void applyShape();
    // Mezcla húmeda y Difuminar.
    void flushWet();
    void commitWet();
    // Deja los buffers de trazo transparentes y termina el trazo húmedo.
    void endWet();
    // Copia la capa a la copia de trabajo en lo que falte de `needed`.
    void prepareWet(const IRect& needed);
    // La cobertura del trazo húmedo (el segundo buffer), o nullptr si no se lleva.
    const gfx::RenderTarget* wetCoverage() const;
    // El final afinado provisional se pinta encima de la copia de trabajo: antes se guarda
    // lo que pisa (`rect`, con su cobertura) y en la tanda siguiente se devuelve. Uno por
    // copia de la simetría.
    struct WetTail {
        IRect rect;                   // lo que pisa (guardado abajo)
        gfx::RenderTarget pixels;     // lo que había ahí, desde (0, 0)
        gfx::RenderTarget coverage;
    };
    bool saveWetTail(WetTail& tail, const IRect& rect);
    IRect restoreWetTail(WetTail& tail);
    // Deja transparentes los buffers de trazo en `rect`.
    void clearStrokeBuffer(const IRect& rect);
    bool ensureStrokeBase();
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
    // Selección automática, difuminado, transformación, relleno o ajuste a medias.
    bool livePending() const {
        return m_auto.active || m_feather.active || m_transform.active || m_fill.active || m_adjust.active;
    }
    bool saveRegion(const Layer& layer, const IRect& rect, gfx::RenderTarget& out);
    void swapRegion(Layer& layer, const IRect& rect, gfx::RenderTarget& stored);
    void record(HistoryStep step);
    void applyStep(HistoryStep& step, bool undo);
    size_t layerBytes() const;

    // Selección (CanvasSelection.cpp).
    // Paso de deshacer con la máscara de `rect` tal como está ahora (antes de cambiarla).
    bool saveSelection(const IRect& rect, HistoryStep& step);
    // Igual, con la máscara de antes guardada en la copia de trabajo (cambios en vivo).
    bool saveSelectionFromScratch(const IRect& rect, HistoryStep& step);
    void recordSelection(HistoryStep step, bool saved, const SelectionState& after);
    void swapSelection(const IRect& rect, gfx::RenderTarget& stored);
    void setSelectionState(const SelectionState& state);
    // Caja exacta de la máscara dentro de `within` (vacía si no queda nada).
    IRect maskBounds(const IRect& within);
    // Niveles de la inundación desde (x, y) en lo que se ve, o en la capa de referencia si
    // la hay (el dibujo de líneas): de ahí salen la selección automática y el relleno.
    bool floodLevels(float x, float y, selection::AutoLevels& levels);
    void drawAutoLevels();
    // Lo seleccionado de `layer` (o todo) en el buffer de trazo, en `rect`. Quien lo usa
    // lo deja transparente después.
    void stageSelection(const Layer& layer, const IRect& rect, bool masked);
    // Caja de lo que hay que copiar o transformar de `layer`.
    IRect selectedContent(const Layer& layer, bool masked);
    void updateTransformPreview();
    void endTransform();

    // Relleno (CanvasFill.cpp).
    // Lo que cambia el relleno con el umbral actual.
    IRect fillReach() const;
    void drawFill();

    // Ajustes de imagen (CanvasAdjust.cpp). False: sin memoria (no cambia lo que se ve).
    bool drawAdjust();

    bool m_ready = false;
    int m_maxTextureSize = 0;          // lado máximo de una textura en esta GPU
    LayerStack m_layers;
    Compositor m_compositor;
    Brush m_brush;
    BrushSettings m_settings;
    float m_pixelsPerPoint = 1.0f;

    gfx::RenderTarget m_strokeTarget;   // lo que se ve del trazo
    gfx::RenderTarget m_strokeBase;     // sellos definitivos (con afinado); se crea al usarlo
    bool m_stroking = false;
    IRect m_strokeBounds;
    float m_strokeOpacity = 1.0f;
    StrokePreview::Mode m_strokeMode = StrokePreview::Mode::Paint;
    float m_strokeColor[3] = {0.0f, 0.0f, 0.0f};
    BrushParams m_strokeParams;
    StrokeGrain m_strokeGrain;
    StrokePath m_path;
    std::vector<Dab> m_dabs;            // tanda de sellos que se va a pintar
    bool m_useBase = false;
    uint32_t m_drawnRevision = 0;
    uint32_t m_strokeCount = 0;         // semilla del azar de cada trazo
    bool m_wet = false;                 // el trazo mezcla con la capa (húmedo o Difuminar)
    bool m_wetCoverage = false;         // m_strokeBase lleva la cobertura del trazo húmedo
    WetMix m_wetMix;
    IRect m_wetValid;                   // zona de m_strokeTarget con la copia de la capa

    // Simetría del trazo en curso: sus copias (la primera, el trazo mismo), el centro y, de
    // cada copia, lo que pintó, sus provisionales y, si es húmedo, por dónde va y su final.
    DrawingGuide m_guide;
    std::vector<guide::Copy> m_copies;
    glm::vec2 m_copyCenter{0.0f};
    std::vector<IRect> m_copyBounds;
    std::vector<IRect> m_provisionalRects;   // provisionales pintados en m_strokeTarget
    std::vector<WetCursor> m_wetCursors;     // tras el último sello definitivo
    std::vector<WetTail> m_wetTails;
    std::vector<Dab> m_copyDabs[2];          // copias de una tanda: sin reflejar y reflejadas
    std::vector<std::vector<Dab>> m_wetDabs; // húmedo: los provisionales de cada copia
    std::vector<IRect> m_rects[2];           // cajas de cada copia (intermedias)

    // Forma rápida.
    std::vector<glm::vec3> m_input;          // muestras del trazo: x, y y presión
    quickshape::Shape m_shapeRecognized;     // la reconocida
    quickshape::Shape m_shapeBase;           // esa o la perfecta
    quickshape::Shape m_shape;               // ajustada al puntero
    quickshape::Options m_shapeOptions;
    glm::vec2 m_shapeFrom{0.0f};             // el puntero al reconocerla
    glm::vec2 m_shapeTo{0.0f};               // y ahora
    float m_shapePressure = 1.0f;
    bool m_shapeDirty = false;               // hay que volver a pintarla

    uint64_t m_version = 0;
    std::vector<std::vector<uint8_t>> m_snapshot; // una entrada por capa, de abajo arriba

    History m_history;
    struct LayerEdit {
        bool active = false;
        uint32_t layerId = 0;
        LayerProperties before;
    } m_layerEdit;

    // Selección.
    Selection m_selection;
    BoundsFinder m_bounds;
    Warp m_warp;
    uint64_t m_selectionVersion = 0;
    struct AutoSelect {
        bool active = false;
        SelectOp op = SelectOp::Replace;
        SelectionState before;
        std::array<IRect, 256> levelBounds;   // caja de lo que entra con cada nivel
        int cutoff = -1;
        IRect drawn;                          // zona de la máscara cambiada en vivo
    } m_auto;
    struct Feather {
        bool active = false;
        SelectionState before;
        float radius = 0.0f;
        IRect drawn;
    } m_feather;

    // Portapapeles: se conserva al crear otro lienzo.
    struct Clipboard {
        gfx::RenderTarget pixels;   // premultiplicado, del tamaño de `rect`
        IRect rect;                 // de dónde salió, en el lienzo
    } m_clipboard;

    // Transformar.
    struct Transform {
        bool active = false;
        uint32_t layerId = 0;
        IRect source;                  // caja de lo que se transforma
        gfx::RenderTarget content;     // eso, con 1 px transparente alrededor (con mipmaps)
        int border = 1;                // ese píxel (0 si no cabe en una textura)
        bool masked = false;           // se tomó por la selección: el resto de la capa se queda
        bool movesSelection = false;   // al aplicar, la selección se mueve igual
        glm::mat3 homography{1.0f};    // fuente → destino, en píxeles del lienzo
        bool nearest = false;
        IRect drawn;                   // caja de lo dibujado en la vista previa
    } m_transform;

    // Relleno: la vista previa va en el buffer de trazo y se ve en lugar de la capa.
    ColorFill m_colorFill;
    struct Fill {
        bool active = false;
        uint32_t layerId = 0;
        std::array<IRect, 256> levelBounds;   // caja de lo que entra con cada nivel
        int cutoff = -1;
        float color[3] = {0.0f, 0.0f, 0.0f};
        bool alphaLock = false;
        IRect drawn;                          // zona del buffer de trazo con la vista previa
    } m_fill;

    // Ajuste de imagen: la vista previa va en el buffer de trazo, como la del relleno.
    ImageAdjust m_imageAdjust;
    struct Adjust {
        bool active = false;
        uint32_t layerId = 0;
        Adjustment kind = Adjustment::HueSaturation;
        AdjustParams params;
        bool alphaLock = false;
        IRect rect;              // zona que cambia: lo seleccionado o todo el lienzo
        IRect drawn;             // zona del buffer de trazo con la vista previa
        bool original = false;   // se ve la capa sin el ajuste
    } m_adjust;
    uint32_t m_adjustCount = 0;   // semilla del ruido de cada ajuste
};
