#pragma once

#include <cstdint>

// Perfiles de color y el paso de uno a otro. Los dos perfiles de LiveSketch tienen el blanco
// D65 y la curva de transferencia de sRGB; solo cambian los primarios: Display P3 llega a
// rojos, naranjas y verdes más intensos que sRGB, que cabe entero dentro de P3. Un gris es el
// mismo número en los dos.

// Perfil de color del lienzo: el espacio de sus colores. NDI recibe siempre sRGB.
enum class ColorProfile : uint8_t { Srgb, DisplayP3 };
inline constexpr int kColorProfileCount = 2;

namespace colorspace {

// Nombre para la interfaz: "sRGB" o "Display P3".
const char* name(ColorProfile profile);

// Curva de transferencia de sRGB (la misma en Display P3): de un valor codificado (0..1) a
// luz lineal y al revés.
float toLinear(float encoded);
float toEncoded(float linear);

// Paso de los colores de un perfil a otro: una matriz 3×3 en luz lineal.
struct Transform {
    ColorProfile from = ColorProfile::Srgb;
    ColorProfile to = ColorProfile::Srgb;
    float matrix[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};   // por filas: destino = matrix · origen

    bool identity() const { return from == to; }
    // Distingue las transformaciones (para las cachés que dependen de ella).
    int key() const { return static_cast<int>(from) * kColorProfileCount + static_cast<int>(to); }
};
Transform between(ColorProfile from, ColorProfile to);

// Convierte un color codificado (0..1, sin premultiplicar). Lo que el destino no tiene se
// recorta a su borde canal a canal, como hacen los navegadores.
void convert(const Transform& transform, float rgb[3]);

// Lo mismo con 8 bits por canal, para muchos colores seguidos (los vértices de la interfaz):
// con tablas y redondeando al valor más cercano, como convert().
class Converter8 {
public:
    explicit Converter8(const Transform& transform);
    void apply(uint8_t& r, uint8_t& g, uint8_t& b) const;
    const Transform& transform() const { return m_transform; }

private:
    uint8_t encode(float linear) const;

    Transform m_transform;
    float m_linear[256];        // luz lineal de cada valor codificado
    float m_thresholds[255];    // luz lineal a medio camino entre cada valor y el siguiente
};

// El de cada par de perfiles, creado la primera vez que se pide.
const Converter8& converter8(ColorProfile from, ColorProfile to);

// Código GLSL que se pone delante del main() de un fragment shader: declara los uniforms
// uGamut (mat3) y uGamutOn (int; 0: mismo perfil) y las funciones gamutConvert(vec3), para
// un color sin premultiplicar, y gamutConvertPremultiplied(vec4). Ver gfx::GamutUniforms.
extern const char* const kGlsl;

} // namespace colorspace
