#pragma once

#include "Gfx/GLObjects.h"

#include <cstdint>
#include <string>

// Modo de fusión de una capa: cómo se mezcla su color con lo que hay debajo. Son los de
// Procreate, en el mismo orden y con los mismos grupos. El valor lo usa el shader del
// compositor (y lo usarán los archivos de proyecto): no se cambia el orden.
enum class BlendMode : uint8_t {
    Normal,
    // Oscurecer
    Multiply,
    Darken,
    ColorBurn,
    LinearBurn,
    DarkerColor,
    // Aclarar
    Lighten,
    Screen,
    ColorDodge,
    Add,
    LighterColor,
    // Contraste
    Overlay,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    // Diferencia
    Difference,
    Exclusion,
    Subtract,
    Divide,
    // Color
    Hue,
    Saturation,
    Color,
    Luminosity,
};
inline constexpr int kBlendModeCount = 26;

// Una capa del lienzo. Es dueña de su textura y su FBO: al destruirse los libera.
struct Layer {
    uint32_t id = 0;        // estable mientras la capa exista (IDs de ImGui, archivos)
    std::string name;
    bool visible = true;
    float opacity = 1.0f;   // 0..1, se aplica al componer
    BlendMode blend = BlendMode::Normal;
    bool alphaLock = false; // solo se pinta donde ya hay pintura (el alfa no cambia)
    bool clipping = false;  // máscara de recorte: solo se ve donde hay pintura en la capa base
    bool reference = false; // el relleno mira esta capa (como mucho una en la pila)
    uint64_t revision = 0;  // sube cada vez que cambian sus píxeles (miniaturas)
    gfx::RenderTarget target;
};
