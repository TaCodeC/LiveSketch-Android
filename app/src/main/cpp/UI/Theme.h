#pragma once

#include <imgui.h>

// Tema de la interfaz: oscuro. De iOS toma los colores de sistema y el cristal
// esmerilado; los menús tienen un aspecto propio, de paleta de herramientas: esquinas
// más rectas, cabeceras separadas con una línea, rótulos de sección pequeños y controles
// cuadrados. Las medidas van en puntos (pt); ui::pt() las pasa a coordenadas de ImGui.
namespace ui::theme {

// --- Colores ---
inline constexpr ImU32 kAccent = IM_COL32(10, 132, 255, 255);
inline constexpr ImU32 kAccentBorder = IM_COL32(61, 155, 255, 255);   // borde de un interruptor encendido
inline constexpr ImU32 kAccentText = IM_COL32(90, 176, 255, 255);     // texto de acento sobre fondo oscuro
inline constexpr ImU32 kGreen = IM_COL32(48, 209, 88, 255);           // avisos de éxito
inline constexpr ImU32 kRed = IM_COL32(255, 69, 58, 255);             // destructivo
inline constexpr ImU32 kLive = IM_COL32(255, 59, 48, 230);            // cápsula "EN VIVO"
inline constexpr ImU32 kOrange = IM_COL32(255, 159, 10, 255);

// --- Texto ---
inline constexpr ImU32 kLabel = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kSecondaryLabel = IM_COL32(235, 235, 245, 158);   // 62 %
inline constexpr ImU32 kTertiaryLabel = IM_COL32(235, 235, 245, 82);     // 32 %
inline constexpr ImU32 kDisabledLabel = IM_COL32(255, 255, 255, 82);
inline constexpr ImU32 kSectionLabel = IM_COL32(235, 235, 245, 102);     // rótulos de sección
inline constexpr ImU32 kMutedIcon = IM_COL32(235, 235, 245, 191);        // iconos de las filas

// --- Superficies ---
inline constexpr ImU32 kBackground = IM_COL32(21, 21, 23, 255);           // alrededor del lienzo
inline constexpr ImU32 kBarTint = IM_COL32(40, 40, 44, 158);              // cápsulas de las herramientas
inline constexpr ImU32 kPanelTint = IM_COL32(24, 24, 28, 219);            // paneles
inline constexpr ImU32 kDialogTint = IM_COL32(30, 30, 34, 240);           // diálogos
inline constexpr ImU32 kGlassBorder = IM_COL32(255, 255, 255, 26);
inline constexpr ImU32 kGlassHighlight = IM_COL32(255, 255, 255, 20);     // brillo del borde superior
inline constexpr ImU32 kControl = IM_COL32(255, 255, 255, 15);            // fondo de botones, fichas y campos
inline constexpr ImU32 kControlSoft = IM_COL32(255, 255, 255, 10);        // fichas sin elegir
inline constexpr ImU32 kControlBorder = IM_COL32(255, 255, 255, 26);
inline constexpr ImU32 kControlBorderStrong = IM_COL32(255, 255, 255, 46);
inline constexpr ImU32 kAccentSoft = IM_COL32(10, 132, 255, 38);          // ficha o pestaña elegida
inline constexpr ImU32 kRule = IM_COL32(255, 255, 255, 20);               // líneas de cabeceras y secciones
inline constexpr ImU32 kFill = IM_COL32(120, 120, 128, 77);               // deslizadores de la barra lateral
inline constexpr ImU32 kSeparator = IM_COL32(255, 255, 255, 26);
inline constexpr ImU32 kPressed = IM_COL32(255, 255, 255, 36);            // botón abierto o pulsado
inline constexpr ImU32 kDim = IM_COL32(0, 0, 0, 115);                     // detrás de un diálogo
inline constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 255);

// --- Medidas (pt) ---
inline constexpr float kMargin = 16.0f;          // de las barras al borde de la pantalla
inline constexpr float kCompactMargin = 12.0f;
inline constexpr float kBarHeight = 48.0f;
inline constexpr float kBarButtonWidth = 44.0f;
inline constexpr float kBarButtonHeight = 40.0f;
inline constexpr float kBarPadding = 4.0f;
inline constexpr float kIconSize = 22.0f;
inline constexpr float kSidebarWidth = 52.0f;
inline constexpr float kPopoverGap = 8.0f;       // entre la barra y el panel
inline constexpr float kPanelRadius = 16.0f;
inline constexpr float kSheetRadius = 20.0f;
inline constexpr float kDialogRadius = 18.0f;
inline constexpr float kHeaderHeight = 48.0f;    // cabecera de un panel
inline constexpr float kRowRadius = 12.0f;       // filas de capas y pinceles
inline constexpr float kControlRadius = 10.0f;   // botones, fichas y filas de menú

// --- Tipografía (pt) ---
inline constexpr float kTitle = 24.0f;
inline constexpr float kHeadline = 17.0f;
inline constexpr float kPanelTitle = 16.0f;
inline constexpr float kBody = 16.0f;
inline constexpr float kSubhead = 15.0f;
inline constexpr float kCallout = 14.0f;
inline constexpr float kFootnote = 13.0f;
inline constexpr float kCaption = 12.0f;
inline constexpr float kMicro = 11.0f;           // rótulos de sección y de pestaña

} // namespace ui::theme
