#pragma once

#include "Canvas/BrushLibrary.h"
#include "Canvas/DrawingGuide.h"
#include "Canvas/ImageAdjust.h"
#include "Canvas/Selection.h"
#include "Tools/SelectTool.h"
#include "Tools/ToolView.h"
#include "Tools/TransformTool.h"
#include "UI/Kit.h"
#include "UI/Previews.h"

#include <cstdint>
#include <string>
#include <vector>

class Canvas;

// Con qué se dibuja el trazo en curso (la forma rápida explica cómo hacerla perfecta).
enum class StrokePointer { None, Pen, Finger, Mouse };

// Lo que la interfaz necesita saber de la app en cada frame.
struct UiStatus {
    float pointScale = 1.0f;      // unidades de ImGui (coordenadas de la ventana) por punto
    float pixelsPerUnit = 1.0f;   // píxeles físicos por unidad
    float safeTop = 0.0f;         // bordes que tapan la muesca o el sistema, en unidades
    float safeRight = 0.0f;
    float safeBottom = 0.0f;
    float safeLeft = 0.0f;
    int screenWidth = 0;          // ventana en píxeles (lienzo "Pantalla completa")
    int screenHeight = 0;
    int maxCanvasSize = 0;        // lado máximo de textura que admite la GPU
    float canvasZoom = 1.0f;      // píxeles de la pantalla por píxel del lienzo
    ToolView canvasView;          // dónde se ve el lienzo (unidades)
    float viewAngle = 0.0f;       // giro de la vista, en grados (de -180 a 180)
    bool viewFlipped = false;     // la vista está volteada en horizontal
    bool viewTurning = false;     // se está girando la vista: se muestra el ángulo
    StrokePointer strokePointer = StrokePointer::None;   // con qué se dibuja el trazo en curso

    bool ndiAvailable = false;    // la app se compiló con el SDK de NDI
    bool ndiRunning = false;
    int ndiConnections = 0;
    std::string ndiError;
    bool exporting = false;       // hay un PNG guardándose (o esperando el permiso)
};

// Lo que la interfaz pide a la app en este frame.
struct UiRequests {
    int canvasWidth = 0;    // > 0: crear un lienzo nuevo de este tamaño
    int canvasHeight = 0;
    bool fitView = false;   // centrar el lienzo (animado)
    int rotateView = 0;     // girar la vista de 15 en 15 grados (+: en el sentido de las agujas del reloj)
    bool straightenView = false;   // dejar la vista derecha
    bool flipView = false;  // voltear la vista en horizontal (o quitar el volteo)
    bool savePng = false;
    bool quit = false;
    int ndi = -1;           // 1: encender NDI, 0: apagarlo
};

// Tipo de aviso: decide el icono.
enum class Notice { Info, Success, Warning, Error, Progress, Undo, Redo };

// Herramientas de pintar, cada una con su pincel, su tamaño y su opacidad. En la barra van
// en el orden de Procreate: pincel, difuminar y borrador.
enum class Tool { Brush, Eraser, Smudge };
inline constexpr int kToolCount = 3;

// Qué hace un puntero sobre el lienzo: pintar (con el pincel o el borrador), seleccionar,
// transformar, con un ajuste de imagen, cambiar su valor principal deslizando a los lados
// o, mientras se edita la guía de dibujo, mover su centro y girarla.
enum class CanvasTool { Paint, Select, Transform, Adjust, Guide };

// Interfaz al estilo de Procreate: barras flotantes de cristal arriba, barra lateral con
// tamaño, cuentagotas, opacidad, deshacer y rehacer, y paneles que se abren desde las
// barras (hojas desde abajo en un teléfono en vertical).
//
// Opera directamente sobre el lienzo; lo que no es del lienzo lo pide a la app con
// UiRequests. Implementación repartida en Ui.cpp (barras y avisos), UiPanels.cpp
// (paneles), UiBrushes.cpp (pinceles), UiTools.cpp (Selección y Transformar), UiAdjust.cpp
// (ajustes de imagen), UiGuide.cpp (guía de dibujo), UiFill.cpp (arrastrar el color para
// rellenar) y UiDialogs.cpp (alertas y lienzo nuevo).
class Ui {
public:
    // Objetos de GPU (miniaturas y trazos de muestra). Tras perder el contexto GL se
    // vuelve a llamar.
    bool init();
    void destroy();
    // Guarda ya lo que estaba pendiente de guardar (la app pasa a segundo plano y el
    // sistema puede cerrarla sin avisar).
    void saveNow() { saveBrushesIfDue(true); }

    // Escala y reloj de las animaciones. Después del NewFrame de los backends y antes de
    // ImGui::NewFrame().
    void beginFrame(const UiStatus& status);
    // Toda la interfaz del frame. `canvas` es null hasta que se crea el lienzo: entonces
    // se muestra la pantalla de inicio.
    void build(Canvas* canvas, UiRequests& requests);

    // --- Para la entrada de la app ---
    bool drawWithFinger() const { return m_prefs.drawWithFinger; }
    // Girar la vista con dos dedos.
    bool rotateWithFingers() const { return m_prefs.rotateWithFingers; }
    // Forma rápida: mantener quieto el final de un trazo lo convierte en una forma.
    bool quickShape() const { return m_prefs.quickShape; }
    // Ajustes del pincel justo antes de empezar un trazo. La goma del lápiz usa los del
    // borrador aunque la herramienta sea el pincel.
    void prepareStroke(Canvas& canvas, bool eraserTip);
    // Empezó un trazo: su color pasa a los recientes.
    void strokeStarted(const Canvas& canvas, bool erasing);

    bool eyedropperArmed() const { return m_eyedropperArmed; }
    void setEyedropperArmed(bool armed) { m_eyedropperArmed = armed; }
    // Lupa del cuentagotas en `position` (unidades). `rgb` null: fuera del lienzo.
    // `touch`: con el dedo la lupa se dibuja encima para que el dedo no la tape.
    void showPicker(ImVec2 position, const float* rgb, bool touch);
    void hidePicker() { m_picker.active = false; }
    // Aplica el color elegido con el cuentagotas.
    void pickColor(Canvas& canvas, const float rgb[3]);

    // Aviso breve bajo las barras. Progress se queda hasta que otro aviso lo sustituye
    // (o hasta `durationMs`).
    void notify(std::string text, Notice kind = Notice::Info, uint32_t durationMs = 3000);
    // Deshacer o rehacer desde un gesto o un atajo (ya hecho o no: `done`).
    void showUndo(bool redo, bool done);
    // Diálogo de salir (botón atrás de Android con todo cerrado).
    void askExit();

    // --- Selección, Transformar y Ajustes (UiTools.cpp) ---
    CanvasTool canvasTool() const { return m_canvasTool; }
    // Un puntero sobre el lienzo con la herramienta Selección, Transformar o un ajuste
    // (unidades).
    // `modifier`: Add o Subtract si los pide el teclado (Mayús o Alt); `constrain`: Mayús
    // mientras se arrastra (cuadrado o círculo).
    void toolPress(Canvas& canvas, const ToolView& view, ImVec2 position, SelectOp modifier);
    void toolDrag(Canvas& canvas, const ToolView& view, ImVec2 position, bool constrain);
    void toolRelease(Canvas& canvas, const ToolView& view, ImVec2 position);
    // Otro dedo o el sistema cancelan el gesto.
    void toolCancel(Canvas& canvas);
    // Deshacer o rehacer desde un gesto: primero lo que la herramienta tenga a medias
    // (el lazo por puntos, los pasos de la transformación).
    void undoGesture(Canvas& canvas, bool redo);
    // Hay que dibujar el siguiente frame (el borde de la selección se mueve).
    bool selectionAnimating(const Canvas& canvas) const;
    // Se creó otro lienzo: las herramientas vuelven a empezar.
    void canvasCreated();

    // Bordes de la ventana que tapa la interfaz (unidades): el ajuste del lienzo los evita.
    float insetTop() const { return m_insets[0]; }
    float insetRight() const { return m_insets[1]; }
    float insetBottom() const { return m_insets[2]; }
    float insetLeft() const { return m_insets[3]; }
    // Instante (SDL_GetTicks) en que hay que volver a dibujar aunque no pase nada (se
    // oculta un aviso), o 0.
    uint64_t wakeDeadline() const;

private:
    enum class Panel { None, Actions, Ndi, Brushes, Layers, Color, Feather, Modify, Adjust };
    enum class Dialog { None, DeleteLayer, RenameLayer, Exit, NewCanvas };

    struct Layout {
        ImVec2 display;
        float safe[4] = {0, 0, 0, 0};   // arriba, derecha, abajo, izquierda
        bool compact = false;           // pantalla pequeña: medidas más justas
        bool narrow = false;            // teléfono en vertical: paneles como hojas
        float margin = 0.0f;
        float left = 0.0f;              // zona útil (sin márgenes ni zona segura)
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        ImRect leftBar;
        ImRect ndiBar;                  // vacía si NDI va dentro de la barra izquierda
        ImRect rightBar;
        ImRect sidebar;
        ImRect sizeSlider;
        ImRect eyedropper;
        ImRect opacitySlider;
        float separatorY = 0.0f;
        ImRect undo;
        ImRect redo;
        float popoverTop = 0.0f;
        float toastTop = 0.0f;
        float barButton = 0.0f;         // ancho de los botones de las barras de arriba
    };

    struct Prefs {
        bool drawWithFinger = false;
        bool sidebarRight = false;
        bool rotateWithFingers = true;
        bool quickShape = true;
        int size = 1;   // tamaño de la interfaz: 0 pequeña, 1 normal, 2 grande
    };

    // Ajustes de cada herramienta: el pincel, difuminar y el borrador guardan los suyos.
    struct ToolPreset {
        int brush = 0;          // en brushes::library()
        float size = 0.3f;      // posición del deslizador (0..1): el radio depende del pincel
        float opacity = 1.0f;
    };
    // Tamaño y opacidad que tenía cada pincel en cada herramienta.
    struct BrushMemory {
        float size = 0.0f;
        float opacity = 1.0f;
        bool set = false;
    };

    struct Toast {
        std::string text;
        Notice kind = Notice::Info;
        uint64_t until = 0;     // SDL_GetTicks
    };

    struct Picker {
        bool active = false;
        bool touch = false;
        bool valid = false;
        ImVec2 position;
        float rgb[3] = {0, 0, 0};
    };

    // Marco de un panel: popover bajo su botón o hoja desde abajo.
    struct PanelFrame {
        ImDrawList* dl = nullptr;
        ImRect rect;              // el panel en pantalla
        ImRect content;           // zona del contenido (sin el tirador de la hoja)
        float scroll = 0.0f;      // hay que restarlo a la y del contenido
        bool scrolls = false;
        bool open = false;        // abierto (si no, se está cerrando)
        bool sheet = false;
        float presence = 0.0f;
        ImVec2 anchor;
        ui::DrawMark mark;
    };

    // --- Ui.cpp ---
    void computeLayout();
    void loadPrefs();
    void savePrefs() const;
    void syncBrush(Canvas& canvas);
    void applyPreset(Canvas& canvas, Tool tool);
    void selectTool(Tool tool);
    // Posición del botón de la herramienta en la barra de la derecha.
    static int toolSlot(Tool tool);
    void togglePanel(Panel panel);
    void closePanels();
    // Cierra lo último que se abrió (el botón atrás o Escape). Con `canvas`, también sale
    // de la herramienta Selección o Transformar, o cancela el ajuste de imagen.
    bool closeTopmost(Canvas* canvas);
    void handleKeys(Canvas& canvas, UiRequests& requests);
    void undo(Canvas& canvas, bool redo);
    bool canUndo(const Canvas& canvas) const;
    void setColor(Canvas& canvas, const float rgb[3]);
    void pushRecent(const float rgb[3]);
    float ndiCapsuleWidth() const;

    void beginBar(const char* name, const ImRect& rect, float radius);
    bool barButton(const char* id, const ImRect& rect, const char* glyph, bool open, bool selected,
                   bool enabled = true);
    void drawScrim();
    void drawTopBars(Canvas& canvas, UiRequests& requests);
    void drawNdiCapsule();
    void drawSidebar(Canvas& canvas);
    void drawHud(Canvas& canvas);
    void drawPicker(Canvas& canvas);
    void drawToast();
    // Cápsula de arriba (donde salen los avisos) mientras se gira la vista o se ajusta una
    // forma rápida. Va antes que los avisos: mientras se ve, los oculta.
    void drawCapsules(const Canvas& canvas);

    // --- UiPanels.cpp ---
    bool beginPanel(PanelFrame& frame, Panel panel, const char* name, float x, float width, float contentHeight,
                    float anchorX, bool scrollBody);
    void endPanel(PanelFrame& frame);
    void drawPanels(Canvas& canvas, UiRequests& requests);
    void actionsPanel(Canvas& canvas, UiRequests& requests);
    void ndiPanel(Canvas& canvas, UiRequests& requests);
    void layersPanel(Canvas& canvas);
    void blendList(Canvas& canvas, ImDrawList* dl, const ImRect& view);
    void layerMenu(Canvas& canvas);
    void colorPanel(Canvas& canvas);
    void syncHsv(const float rgb[3]);
    void applyHsv(Canvas& canvas);

    // --- UiBrushes.cpp ---
    void initBrushes();
    void loadBrushes();
    void saveBrushes();
    void scheduleBrushSave();
    void saveBrushesIfDue(bool force);
    // Elige el pincel `index` de la biblioteca para `tool`: recupera el tamaño y la
    // opacidad que tenía (o los suyos por defecto).
    void selectBrush(Tool tool, int index);
    const BrushParams& toolBrush(Tool tool) const;
    float toolRadius(Tool tool) const;
    void brushesPanel();
    void brushList(ImDrawList* dl, const ImRect& view);
    void brushSettings(ImDrawList* dl, const ImRect& view);

    // --- UiTools.cpp ---
    // Cambia de herramienta del lienzo: al salir de Transformar se aplica y al entrar se
    // empieza (si hay algo que transformar).
    void setCanvasTool(Canvas& canvas, CanvasTool tool);
    // Vuelve a pintar con el pincel o el borrador.
    void paintWith(Canvas& canvas, Tool tool);
    // Cada frame: lo que cambió fuera de la herramienta (otra operación aplicó la
    // transformación, deshacer quitó el difuminado...).
    void toolFrame(Canvas& canvas);
    // Deshace o rehace un paso (con lo de la herramienta primero). Devuelve si hizo algo.
    bool undoStep(Canvas& canvas, bool redo);
    bool toolKeys(Canvas& canvas);
    void selectionNotice(SelectTool::Result result);
    // Empieza a transformar. `quiet`: sin avisar si no hay nada que transformar.
    // `wholeLayer`: toda la capa activa aunque haya selección (ver Canvas::beginTransform).
    bool enterTransform(Canvas& canvas, bool quiet, bool wholeLayer = false);
    void copySelection(Canvas& canvas, bool cut);
    void pasteClipboard(Canvas& canvas);
    void duplicateSelection(Canvas& canvas);
    void clearSelected(Canvas& canvas);
    void fillSelected(Canvas& canvas);
    void drawToolOverlay(const Canvas& canvas);
    void drawDock(Canvas& canvas);
    void selectDock(Canvas& canvas);
    void transformDock(Canvas& canvas);
    void drawPolygonBar(Canvas& canvas);
    // Indicador de arriba mientras se ajusta algo arrastrando en el lienzo.
    void drawThreshold(const Canvas& canvas);
    void featherPanel(Canvas& canvas);
    void modifyPanel(Canvas& canvas);

    // --- UiAdjust.cpp ---
    // Deslizador de la barra de un ajuste: su valor (0..1), su nombre, el texto del valor
    // y su aspecto.
    struct AdjustSlider {
        float* t = nullptr;
        const char* text = "";
        char value[24] = {};
        ui::SliderStyle style;
    };
    // Empieza el ajuste `kind` en la capa activa, con los valores a cero. El que hubiera a
    // medias se aplica antes, y la transformación también.
    void startAdjust(Canvas& canvas, Adjustment kind);
    // Termina el ajuste (aplicándolo o no) y vuelve a la herramienta de antes.
    void finishAdjust(Canvas& canvas, bool apply);
    AdjustParams adjustParams(const Canvas& canvas) const;
    // Pasa los deslizadores al lienzo (que vuelve a dibujar el resultado).
    void updateAdjust(Canvas& canvas);
    // Los deslizadores del ajuste que se está haciendo (el principal, primero). Devuelve
    // cuántos hay.
    int adjustSliders(const Canvas& canvas, AdjustSlider out[3]);
    // El valor que cambia deslizando en el lienzo (desenfoque, enfocar y ruido), o null.
    float* adjustMain();
    void adjustPress(ImVec2 position);
    void adjustDrag(Canvas& canvas, ImVec2 position);
    void adjustRelease();
    // Otro dedo o el sistema cancelan el arrastre: el valor vuelve al de antes.
    void adjustCancel(Canvas& canvas);
    // Lo que muestra el indicador de arriba mientras se desliza en el lienzo.
    void adjustPill(const Canvas& canvas, const char** glyph, const char** title, char* value, size_t size, float* t);
    void adjustPanel(Canvas& canvas);
    void adjustDock(Canvas& canvas);

    // --- UiGuide.cpp ---
    // Edita la guía de dibujo: la activa y muestra su barra y sus tiradores (el centro y el
    // giro). Al terminar se vuelve a la herramienta de antes; sin `keep`, la guía vuelve a
    // como estaba.
    void startGuide(Canvas& canvas);
    void finishGuide(Canvas& canvas, bool keep);
    // Un puntero sobre el lienzo mientras se edita: arrastra el tirador que toque.
    void guidePress(const Canvas& canvas, const ToolView& view, ImVec2 position);
    void guideDrag(Canvas& canvas, const ToolView& view, ImVec2 position);
    void guideRelease();
    // Otro dedo o el sistema cancelan el arrastre: el tirador vuelve a donde estaba.
    void guideCancel(Canvas& canvas);
    // Las líneas de la guía, si está activa, y al editarla sus tiradores (debajo de la
    // interfaz).
    void drawGuide(const Canvas& canvas);
    void guideDock(Canvas& canvas);

    // --- UiFill.cpp ---
    // El botón del color (`button`) está pulsado o se acaba de soltar: arrastrarlo lleva
    // el color al lienzo, y soltarlo rellena la zona. Quieto un momento sobre el lienzo,
    // el relleno se ve y deslizar en horizontal ajusta el umbral hasta soltar.
    void colorDrop(Canvas& canvas, ImGuiID button);
    void finishDrop(Canvas& canvas);
    // Escape o deshacer mientras se arrastra: no se rellena nada (hasta soltar no hace nada).
    void cancelDrop(Canvas& canvas);
    // Empieza el relleno donde está el puntero; si no se puede, avisa por qué.
    bool startFill(Canvas& canvas, ImVec2 position);
    void fillApplied(Canvas& canvas, bool changed);
    // El puntero está sobre el lienzo y no sobre la interfaz.
    bool overCanvas(const Canvas& canvas, ImVec2 position) const;
    // El círculo del color que va con el puntero.
    void drawDrop(Canvas& canvas);

    // --- UiDialogs.cpp ---
    void drawDialogs(Canvas* canvas, UiRequests& requests);
    void openDialog(Dialog dialog, const Canvas* canvas);
    // Tarjeta de lienzo nuevo: pantalla de inicio (`modal` false) o desde Acciones.
    void newCanvasCard(bool modal, float presence, bool interactive, UiRequests& requests);
    void startScreen(UiRequests& requests);

    Previews m_previews;
    UiStatus m_status;
    Layout m_layout;
    Prefs m_prefs;
    bool m_prefsLoaded = false;
    float m_insets[4] = {0, 0, 0, 0};

    Tool m_tool = Tool::Brush;
    ToolPreset m_presets[kToolCount];

    // Pinceles: la biblioteca con los cambios del usuario, lo que recuerda cada uno y el
    // guardado (se escribe un momento después del último cambio).
    std::vector<BrushParams> m_brushParams;
    std::vector<BrushMemory> m_brushMemory[kToolCount];
    uint64_t m_brushSaveAt = 0;     // SDL_GetTicks; 0: nada pendiente
    int m_brushCategory = 0;        // la que muestra la lista
    bool m_brushPage = false;       // el panel muestra los ajustes del pincel elegido
    bool m_brushScroll = false;     // al abrir la lista, mostrar el pincel elegido
    Panel m_panel = Panel::None;
    int m_actionsTab = 0;           // Acciones: 0 lienzo, 1 compartir, 2 preferencias, 3 ayuda
    bool m_layerMenu = false;
    bool m_blendPage = false;       // el panel de capas muestra la lista de modos de fusión
    bool m_blendEditing = false;    // se han probado modos: al salir de la lista se guarda el paso
    bool m_blendScroll = false;     // al abrir la lista, mostrar el modo de la capa
    ImRect m_activeRow;             // fila de la capa activa (para situar su menú)
    ImRect m_layersRect;            // panel de capas en pantalla
    bool m_layerOpacityDragging = false;
    int m_scrollToLayer = -1;       // id de una capa nueva que hay que mostrar

    // Color: HSV propio (para no perder el tono con grises), recientes y el anterior.
    float m_hsv[3] = {0.6f, 0.8f, 0.9f};
    float m_hsvSource[3] = {-1.0f, -1.0f, -1.0f};   // RGB del que salió m_hsv
    float m_recent[6][3] = {};
    int m_recentCount = 0;
    float m_previousColor[3] = {0, 0, 0};
    bool m_hexEditing = false;
    int m_hexFocusFrames = 0;       // frames en los que el campo aún está cogiendo el foco
    char m_hexBuffer[16] = {};

    bool m_eyedropperArmed = false;
    Picker m_picker;
    Toast m_toast;

    // Selección y Transformar.
    CanvasTool m_canvasTool = CanvasTool::Paint;
    CanvasTool m_toolBeforeTransform = CanvasTool::Paint;   // adónde vuelve al aplicar
    SelectTool m_select;
    TransformTool m_transform;
    ImRect m_dock;                  // barra de opciones de la herramienta, en pantalla
    ImRect m_featherButton;         // botón de difuminar (para situar su panel)
    ImRect m_modifyButton;          // "Modificar" en un teléfono
    float m_featherValue = 0.0f;    // posición del deslizador de difuminar (0..1)
    bool m_autoHint = false;        // ya se explicó la selección automática
    // Lo que muestra el indicador de arriba (o lo que mostraba, mientras se oculta).
    enum class Pill { Select, Fill, Adjust };
    Pill m_pill = Pill::Select;
    // Cápsula del giro de la vista o de la forma rápida (lo que muestra o, mientras se
    // oculta, lo que mostraba).
    struct Capsule {
        const char* glyph = nullptr;
        std::string title;
        std::string hint;           // cómo hacer perfecta la forma ("" si no hay)
        bool flipped = false;       // la vista está volteada
    } m_capsule;

    // Guía de dibujo mientras se edita.
    CanvasTool m_toolBeforeGuide = CanvasTool::Paint;   // adónde vuelve al terminar
    DrawingGuide m_guideBefore;     // la de antes de editarla (para cancelar)
    struct GuideDrag {
        int handle = 0;             // 0 ninguno, 1 el centro, 2 el giro
        glm::vec2 grab{0.0f};       // del puntero al centro, en el lienzo
        float spin = 0.0f;          // del puntero al tirador del giro (radianes)
        DrawingGuide before;        // la guía al empezar el arrastre
    } m_guideDrag;

    // Ajustes de imagen: los deslizadores de cada uno (0..1) y el arrastre en el lienzo.
    CanvasTool m_toolBeforeAdjust = CanvasTool::Paint;   // adónde vuelve al terminar
    Adjustment m_adjustKind = Adjustment::HueSaturation;
    struct AdjustSliders {
        float hsb[3] = {0.5f, 0.5f, 0.5f};   // el centro es el cero
        float balance[3][3] = {{0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}};
        float blur = 0.0f;
        float sharpen = 0.0f;
        float noise = 0.0f;
        float noiseSize = 0.0f;
    } m_adjust;
    int m_balanceRange = 1;         // sombras, medios tonos o luces
    struct AdjustDrag {
        bool pressed = false;
        bool active = false;        // ya se movió: cambia el valor
        float startX = 0.0f;
        float startT = 0.0f;
    } m_adjustDrag;
    ImRect m_adjustButton;          // botón de Ajustes en la barra de una tableta
    bool m_adjustHint = false;      // ya se explicó que se puede deslizar en el lienzo

    // Arrastrar el color al lienzo para rellenar.
    struct ColorDrop {
        bool dragging = false;      // el color va con el puntero
        bool filling = false;       // se paró sobre el lienzo: se ve el relleno y se ajusta el umbral
        bool finished = false;      // ya terminó (deshacer lo quitó): no hace nada hasta soltar
        bool refused = false;       // no se pudo rellenar aquí: no se vuelve a probar sin moverse
        ImVec2 position;            // del puntero (unidades)
        ImVec2 stillAt;             // dónde se quedó quieto
        double stillSince = 0.0;
        ImVec2 fillAt;              // dónde empezó el relleno
        float startThreshold = 0.0f;
    } m_drop;
    float m_fillThreshold = 0.3f;   // umbral del relleno (0..1); se recuerda para la siguiente vez
    struct DropMark {               // lo que se dibuja del color (se queda al soltar, mientras se desvanece)
        ImVec2 center;
        bool small = false;         // el punto de donde sale el relleno
    } m_dropMark;

    // Deshacer mantenido pulsado: se repite.
    ImGuiID m_repeatId = 0;
    double m_repeatStart = 0.0;
    double m_repeatLast = 0.0;
    bool m_repeated = false;

    // HUD del tamaño o la opacidad mientras se arrastra su deslizador.
    int m_hudKind = 0;              // 0 tamaño, 1 opacidad
    bool m_hudActive = false;

    // Diálogos.
    Dialog m_dialog = Dialog::None;   // el que está abierto
    Dialog m_dialogShown = Dialog::None;   // el que se dibuja (sigue al cerrarse)
    uint32_t m_dialogLayerId = 0;
    std::string m_dialogTitle;
    char m_renameBuffer[64] = {};
    bool m_dialogFocus = false;
    int m_preset = 0;               // lienzo nuevo: tamaño elegido
    int m_orientation = 0;          // 0 horizontal, 1 vertical
};
