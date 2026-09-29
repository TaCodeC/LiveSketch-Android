#pragma once

#include "Canvas/BrushLibrary.h"
#include "UI/Kit.h"
#include "UI/Previews.h"

#include <cstdint>
#include <string>
#include <vector>

class Canvas;

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
    bool savePng = false;
    bool quit = false;
    int ndi = -1;           // 1: encender NDI, 0: apagarlo
};

// Tipo de aviso: decide el icono.
enum class Notice { Info, Success, Warning, Error, Progress, Undo, Redo };

enum class Tool { Brush, Eraser };

// Interfaz al estilo de Procreate: barras flotantes de cristal arriba, barra lateral con
// tamaño, cuentagotas, opacidad, deshacer y rehacer, y paneles que se abren desde las
// barras (hojas desde abajo en un teléfono en vertical).
//
// Opera directamente sobre el lienzo; lo que no es del lienzo lo pide a la app con
// UiRequests. Implementación repartida en Ui.cpp (barras y avisos), UiPanels.cpp
// (paneles), UiBrushes.cpp (pinceles) y UiDialogs.cpp (alertas y lienzo nuevo).
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

    // Bordes de la ventana que tapa la interfaz (unidades): el ajuste del lienzo los evita.
    float insetTop() const { return m_insets[0]; }
    float insetRight() const { return m_insets[1]; }
    float insetBottom() const { return m_insets[2]; }
    float insetLeft() const { return m_insets[3]; }
    // Instante (SDL_GetTicks) en que hay que volver a dibujar aunque no pase nada (se
    // oculta un aviso), o 0.
    uint64_t wakeDeadline() const;

private:
    enum class Panel { None, Actions, Ndi, Brushes, Layers, Color };
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
    };

    struct Prefs {
        bool drawWithFinger = false;
        bool sidebarRight = false;
        int size = 1;   // tamaño de la interfaz: 0 pequeña, 1 normal, 2 grande
    };

    // Ajustes de cada herramienta: el pincel y el borrador guardan los suyos.
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
    void togglePanel(Panel panel);
    void closePanels();
    bool closeTopmost();
    void handleKeys(Canvas& canvas);
    void undo(Canvas& canvas, bool redo);
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
    ToolPreset m_presets[2];

    // Pinceles: la biblioteca con los cambios del usuario, lo que recuerda cada uno y el
    // guardado (se escribe un momento después del último cambio).
    std::vector<BrushParams> m_brushParams;
    std::vector<BrushMemory> m_brushMemory[2];
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
