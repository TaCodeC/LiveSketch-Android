#include "Gfx/ColorSpace.h"

#include <algorithm>
#include <cmath>

namespace colorspace {

namespace {

// Matrices en luz lineal, sacadas de los primarios de cada perfil con el mismo blanco D65
// (sin adaptación cromática). Cada fila suma 1: un gris no cambia.
constexpr float kSrgbToP3[9] = {
    0.8224619687f, 0.1775380313f, 0.0f,
    0.0331941989f, 0.9668058011f, 0.0f,
    0.0170826307f, 0.0723974407f, 0.9105199286f,
};
constexpr float kP3ToSrgb[9] = {
    1.2249401763f, -0.2249401763f, 0.0f,
    -0.0420569547f, 1.0420569547f, 0.0f,
    -0.0196375546f, -0.0786360456f, 1.0982736001f,
};

} // namespace

const char* name(ColorProfile profile) {
    return profile == ColorProfile::DisplayP3 ? "Display P3" : "sRGB";
}

float toLinear(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float toEncoded(float linear) {
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

Transform between(ColorProfile from, ColorProfile to) {
    Transform transform;
    transform.from = from;
    transform.to = to;
    if (from != to) {
        const float* matrix = from == ColorProfile::Srgb ? kSrgbToP3 : kP3ToSrgb;
        std::copy(matrix, matrix + 9, transform.matrix);
    }
    return transform;
}

void convert(const Transform& transform, float rgb[3]) {
    for (int i = 0; i < 3; ++i) {
        rgb[i] = std::clamp(rgb[i], 0.0f, 1.0f);
    }
    if (transform.identity() || (rgb[0] == rgb[1] && rgb[1] == rgb[2])) {
        return;
    }
    const float linear[3] = {toLinear(rgb[0]), toLinear(rgb[1]), toLinear(rgb[2])};
    const float* m = transform.matrix;
    for (int i = 0; i < 3; ++i) {
        const float value = m[i * 3] * linear[0] + m[i * 3 + 1] * linear[1] + m[i * 3 + 2] * linear[2];
        rgb[i] = std::clamp(toEncoded(std::clamp(value, 0.0f, 1.0f)), 0.0f, 1.0f);
    }
}

Converter8::Converter8(const Transform& transform) : m_transform(transform) {
    for (int i = 0; i < 256; ++i) {
        m_linear[i] = toLinear(static_cast<float>(i) / 255.0f);
    }
    // El valor codificado redondeado es i mientras la luz quede por debajo de la de
    // (i + 0,5) / 255: la curva solo sube, así que basta buscar el umbral.
    for (int i = 0; i < 255; ++i) {
        m_thresholds[i] = toLinear((static_cast<float>(i) + 0.5f) / 255.0f);
    }
}

uint8_t Converter8::encode(float linear) const {
    if (!(linear > 0.0f)) {
        return 0;
    }
    if (linear >= 1.0f) {
        return 255;
    }
    return static_cast<uint8_t>(std::upper_bound(m_thresholds, m_thresholds + 255, linear) - m_thresholds);
}

void Converter8::apply(uint8_t& r, uint8_t& g, uint8_t& b) const {
    if (m_transform.identity() || (r == g && g == b)) {
        return;
    }
    const float lr = m_linear[r];
    const float lg = m_linear[g];
    const float lb = m_linear[b];
    const float* m = m_transform.matrix;
    r = encode(m[0] * lr + m[1] * lg + m[2] * lb);
    g = encode(m[3] * lr + m[4] * lg + m[5] * lb);
    b = encode(m[6] * lr + m[7] * lg + m[8] * lb);
}

const Converter8& converter8(ColorProfile from, ColorProfile to) {
    static const Converter8 converters[kColorProfileCount * kColorProfileCount] = {
        Converter8(between(ColorProfile::Srgb, ColorProfile::Srgb)),
        Converter8(between(ColorProfile::Srgb, ColorProfile::DisplayP3)),
        Converter8(between(ColorProfile::DisplayP3, ColorProfile::Srgb)),
        Converter8(between(ColorProfile::DisplayP3, ColorProfile::DisplayP3)),
    };
    return converters[between(from, to).key()];
}

const char* const kGlsl = R"(
uniform mat3 uGamut;
uniform int uGamutOn;
vec3 gamutToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 gamutToEncoded(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
vec3 gamutConvert(vec3 c) {
    if (uGamutOn == 0) {
        return c;
    }
    return gamutToEncoded(clamp(uGamut * gamutToLinear(clamp(c, 0.0, 1.0)), 0.0, 1.0));
}
vec4 gamutConvertPremultiplied(vec4 c) {
    if (uGamutOn == 0 || c.a <= 0.0) {
        return c;
    }
    return vec4(gamutConvert(c.rgb / c.a) * c.a, c.a);
}
)";

} // namespace colorspace
