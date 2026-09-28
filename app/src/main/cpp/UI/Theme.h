#pragma once

#include <imgui.h>

// Tema de la interfaz: oscuro, con los colores de sistema de iOS en modo oscuro. Las
// medidas van en puntos (pt); ui::pt() las pasa a coordenadas de ImGui.
namespace ui::theme {

// --- Colores de sistema ---
inline constexpr ImU32 kAccent = IM_COL32(10, 132, 255, 255);      // systemBlue
inline constexpr ImU32 kGreen = IM_COL32(48, 209, 88, 255);        // systemGreen (interruptores)
inline constexpr ImU32 kRed = IM_COL32(255, 69, 58, 255);          // systemRed (destructivo)
inline constexpr ImU32 kLive = IM_COL32(255, 59, 48, 230);         // cápsula "EN VIVO"
inline constexpr ImU32 kOrange = IM_COL32(255, 159, 10, 255);
inline constexpr ImU32 kIndigo = IM_COL32(94, 92, 230, 255);
inline constexpr ImU32 kGray = IM_COL32(142, 142, 147, 255);
inline constexpr ImU32 kGray2 = IM_COL32(99, 99, 102, 255);
inline constexpr ImU32 kGray3 = IM_COL32(72, 72, 74, 255);

// --- Texto ---
inline constexpr ImU32 kLabel = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 kSecondaryLabel = IM_COL32(235, 235, 245, 158);   // 62 %
inline constexpr ImU32 kTertiaryLabel = IM_COL32(235, 235, 245, 82);     // 32 %
inline constexpr ImU32 kDisabledLabel = IM_COL32(255, 255, 255, 82);

// --- Superficies ---
inline constexpr ImU32 kBackground = IM_COL32(21, 21, 23, 255);           // alrededor del lienzo
inline constexpr ImU32 kBarTint = IM_COL32(40, 40, 44, 158);              // cápsulas de cristal
inline constexpr ImU32 kPopoverTint = IM_COL32(30, 30, 34, 218);          // paneles
inline constexpr ImU32 kDialogTint = IM_COL32(44, 44, 48, 226);           // alertas
inline constexpr ImU32 kGlassBorder = IM_COL32(255, 255, 255, 26);
inline constexpr ImU32 kGlassHighlight = IM_COL32(255, 255, 255, 20);     // brillo del borde superior
inline constexpr ImU32 kGroupFill = IM_COL32(255, 255, 255, 15);          // grupos de filas
inline constexpr ImU32 kFill = IM_COL32(120, 120, 128, 77);               // pistas de deslizadores
inline constexpr ImU32 kFillStrong = IM_COL32(120, 120, 128, 92);
inline constexpr ImU32 kFillSoft = IM_COL32(118, 118, 128, 61);           // controles segmentados
inline constexpr ImU32 kSeparator = IM_COL32(255, 255, 255, 26);
inline constexpr ImU32 kPressed = IM_COL32(255, 255, 255, 36);            // botón abierto o pulsado
inline constexpr ImU32 kHover = IM_COL32(255, 255, 255, 18);              // puntero encima (ratón)
inline constexpr ImU32 kDim = IM_COL32(0, 0, 0, 102);                     // detrás de una alerta
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
inline constexpr float kPopoverRadius = 22.0f;
inline constexpr float kRowRadius = 14.0f;
inline constexpr float kGroupRadius = 14.0f;

// --- Tipografía (pt) ---
inline constexpr float kTitle = 28.0f;
inline constexpr float kHeadline = 17.0f;
inline constexpr float kBody = 16.0f;
inline constexpr float kSubhead = 15.0f;
inline constexpr float kFootnote = 13.0f;
inline constexpr float kCaption = 12.0f;

} // namespace ui::theme
