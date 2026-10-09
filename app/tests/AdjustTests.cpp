// Ajustes de imagen: tono, saturación y brillo, balance de color, desenfoque gaussiano (a
// tamaño completo y reduciendo), enfocar y ruido, comparados con cálculos en la CPU; la
// selección, el alfa bloqueado, la vista previa, comparar con el original, cancelar,
// deshacer y lo que se cierra antes de otra operación.

#include "Test.h"

#include "Canvas/Canvas.h"
#include "Canvas/SelectionShapes.h"

#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>

namespace {

using test::Pixel;

constexpr Pixel kClear{0, 0, 0, 0};

std::vector<uint8_t> composite(Canvas& canvas) {
    canvas.update();
    return test::readTarget(canvas.composite());
}

std::vector<uint8_t> layerPixels(Canvas& canvas, int layer) {
    return test::readTarget(canvas.layers().at(layer).target);
}

Pixel at(const std::vector<uint8_t>& pixels, const Canvas& canvas, int x, int y) {
    return test::pixelAt(pixels, canvas.width(), x, y);
}

// Color sin premultiplicar y alfa, de 0 a 1.
void fillLayer(Canvas& canvas, int layer, const IRect& rect, float r, float g, float b, float a = 1.0f) {
    test::fillRect(canvas.layers().at(layer).target, rect, r * a, g * a, b * a, a);
    canvas.layers().markDirty(rect);
}

bool near(const Pixel& a, const Pixel& b, int tolerance) {
    for (size_t i = 0; i < 4; ++i) {
        if (std::abs(a[i] - b[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

#define CHECK_PIXEL(actual, expected, tolerance)                                                  \
    do {                                                                                          \
        const Pixel actualPixel = (actual);                                                       \
        if (!near(actualPixel, (expected), (tolerance))) {                                        \
            std::ostringstream message;                                                           \
            message << #actual << " = " << actualPixel << ", se esperaba " << Pixel(expected);    \
            test::fail(__FILE__, __LINE__, message.str());                                        \
        }                                                                                         \
    } while (0)

bool selectRect(Canvas& canvas, const IRect& rect) {
    const std::vector<glm::vec2> polygon = selection::rectangle(
        {static_cast<float>(rect.x0), static_cast<float>(rect.y0)}, {static_cast<float>(rect.x1), static_cast<float>(rect.y1)});
    return canvas.selectPolygon(polygon, SelectOp::Replace);
}

// --- Referencias en la CPU (como los shaders) ---

struct Rgb {
    double r = 0.0, g = 0.0, b = 0.0;
};

Rgb toHsl(Rgb c) {
    const double high = std::max({c.r, c.g, c.b});
    const double low = std::min({c.r, c.g, c.b});
    const double l = (high + low) * 0.5;
    const double d = high - low;
    if (d <= 0.0) {
        return {0.0, 0.0, l};
    }
    const double s = l > 0.5 ? d / (2.0 - high - low) : d / (high + low);
    double h;
    if (high == c.r) {
        h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    } else if (high == c.g) {
        h = (c.b - c.r) / d + 2.0;
    } else {
        h = (c.r - c.g) / d + 4.0;
    }
    return {h / 6.0, s, l};
}

Rgb fromHsl(Rgb hsl) {
    const double chroma = (1.0 - std::abs(2.0 * hsl.b - 1.0)) * hsl.g;
    auto channel = [&](double offset) {
        const double hue = std::clamp(std::abs(std::fmod(hsl.r * 6.0 + offset, 6.0) - 3.0) - 1.0, 0.0, 1.0);
        return hsl.b + chroma * (hue - 0.5);
    };
    return {channel(0.0), channel(4.0), channel(2.0)};
}

double fract(double v) { return v - std::floor(v); }

Rgb hueSaturation(Rgb c, double hue, double saturation, double brightness) {
    Rgb hsl = toHsl(c);
    hsl.r = fract(hsl.r + hue * 0.5);
    hsl.g = std::clamp(hsl.g * (1.0 + saturation), 0.0, 1.0);
    hsl.b = brightness >= 0.0 ? hsl.b + (1.0 - hsl.b) * brightness : hsl.b * (1.0 + brightness);
    return fromHsl(hsl);
}

double balanceChannel(double value, double l, double shadows, double midtones, double highlights) {
    const double a = 0.25;
    const double b = 0.333;
    const double s = shadows * std::clamp((l - b) / -a + 0.5, 0.0, 1.0);
    const double m = midtones * std::clamp((l - b) / a + 0.5, 0.0, 1.0) * std::clamp((l + b - 1.0) / -a + 0.5, 0.0, 1.0);
    const double h = highlights * std::clamp((l + b - 1.0) / a + 0.5, 0.0, 1.0);
    return std::clamp(value + (s + m + h) * 0.7, 0.0, 1.0);
}

Rgb colorBalance(Rgb c, const float balance[3][3]) {
    const double l = (std::max({c.r, c.g, c.b}) + std::min({c.r, c.g, c.b})) * 0.5;
    const Rgb n{balanceChannel(c.r, l, balance[0][0], balance[1][0], balance[2][0]),
                balanceChannel(c.g, l, balance[0][1], balance[1][1], balance[2][1]),
                balanceChannel(c.b, l, balance[0][2], balance[1][2], balance[2][2])};
    Rgb hsl = toHsl(n);
    hsl.b = l;
    return fromHsl(hsl);
}

uint32_t scramble(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

double cell(int x, int y, uint32_t seed) {
    const uint32_t h = scramble(scramble(static_cast<uint32_t>(x) ^ seed) ^ static_cast<uint32_t>(y));
    const uint32_t h2 = scramble(h ^ 0x9e3779b9u);
    return (static_cast<double>(h >> 8) + static_cast<double>(h2 >> 8)) / 33554432.0 - 0.5;
}

double grain(int x, int y, double size, uint32_t seed) {
    if (size <= 1.0) {
        return cell(x, y, seed);
    }
    const double qx = x / size;
    const double qy = y / size;
    const double ix = std::floor(qx);
    const double iy = std::floor(qy);
    double fx = qx - ix;
    double fy = qy - iy;
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    const int cx = static_cast<int>(ix);
    const int cy = static_cast<int>(iy);
    auto mix = [](double a, double b, double t) { return a + (b - a) * t; };
    const double v = mix(mix(cell(cx, cy, seed), cell(cx + 1, cy, seed), fx),
                         mix(cell(cx, cy + 1, seed), cell(cx + 1, cy + 1, seed), fx), fy);
    return v * mix(1.0, 1.346, std::clamp(size - 1.0, 0.0, 1.0));
}

uint8_t toByte(double v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// Aplica `change` a cada píxel con alfa (sin premultiplicar) y vuelve a premultiplicar.
std::vector<uint8_t> perPixel(const std::vector<uint8_t>& source, int width,
                              const std::function<Rgb(Rgb, int, int)>& change) {
    std::vector<uint8_t> out = source;
    for (size_t i = 0; i < source.size(); i += 4) {
        const double a = source[i + 3] / 255.0;
        if (a <= 0.0) {
            continue;
        }
        const Rgb c{std::clamp(source[i] / 255.0 / a, 0.0, 1.0), std::clamp(source[i + 1] / 255.0 / a, 0.0, 1.0),
                    std::clamp(source[i + 2] / 255.0 / a, 0.0, 1.0)};
        const int x = static_cast<int>((i / 4) % static_cast<size_t>(width));
        const int y = static_cast<int>((i / 4) / static_cast<size_t>(width));
        const Rgb n = change(c, x, y);
        out[i] = toByte(n.r * a);
        out[i + 1] = toByte(n.g * a);
        out[i + 2] = toByte(n.b * a);
    }
    return out;
}

// Gaussiana separable (pesos exp(-i²/2σ²) hasta `radius`) sobre el color premultiplicado,
// con el borde del lienzo repetido. En double, de 0 a 1.
std::vector<double> gaussian(const std::vector<uint8_t>& source, int width, int height, double sigma, int radius) {
    std::vector<double> weights(static_cast<size_t>(radius) + 1);
    double total = 0.0;
    for (int i = 0; i <= radius; ++i) {
        weights[static_cast<size_t>(i)] = std::exp(-0.5 * i * i / (sigma * sigma));
        total += i == 0 ? weights[0] : 2.0 * weights[static_cast<size_t>(i)];
    }
    auto index = [&](int x, int y, int channel) {
        return (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(channel);
    };
    std::vector<double> horizontal(source.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                double sum = 0.0;
                for (int i = -radius; i <= radius; ++i) {
                    const int sx = std::clamp(x + i, 0, width - 1);
                    sum += weights[static_cast<size_t>(std::abs(i))] * source[index(sx, y, channel)] / 255.0;
                }
                horizontal[index(x, y, channel)] = sum / total;
            }
        }
    }
    std::vector<double> out(source.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                double sum = 0.0;
                for (int i = -radius; i <= radius; ++i) {
                    const int sy = std::clamp(y + i, 0, height - 1);
                    sum += weights[static_cast<size_t>(std::abs(i))] * horizontal[index(x, sy, channel)];
                }
                out[index(x, y, channel)] = sum / total;
            }
        }
    }
    return out;
}

std::vector<uint8_t> toBytes(const std::vector<double>& values) {
    std::vector<uint8_t> out(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        out[i] = toByte(values[i]);
    }
    return out;
}

// Mayor diferencia dentro de `rect` (y dónde está, para el mensaje).
int maxDifferenceIn(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int width, const IRect& rect,
                    std::string* where = nullptr) {
    int worst = 0;
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4 +
                                 static_cast<size_t>(channel);
                const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
                if (d > worst) {
                    worst = d;
                    if (where) {
                        std::ostringstream text;
                        text << "(" << x << ", " << y << ") canal " << channel << ": " << static_cast<int>(a[i]) << " vs "
                             << static_cast<int>(b[i]);
                        *where = text.str();
                    }
                }
            }
        }
    }
    return worst;
}

#define CHECK_MATCH(actual, expected, width, rect, tolerance)                                         \
    do {                                                                                              \
        std::string where;                                                                            \
        const int difference = maxDifferenceIn((actual), (expected), (width), (rect), &where);        \
        if (difference > (tolerance)) {                                                               \
            std::ostringstream message;                                                               \
            message << #actual << " difiere de " << #expected << " en " << difference << " (" << where \
                    << "), tolerancia " << (tolerance);                                               \
            test::fail(__FILE__, __LINE__, message.str());                                            \
        }                                                                                             \
    } while (0)

// Capa 1 con varios colores, opacos y semitransparentes, y zonas vacías.
void paintSwatches(Canvas& canvas) {
    fillLayer(canvas, 0, {0, 0, 8, 8}, 1.0f, 0.0f, 0.0f);
    fillLayer(canvas, 0, {8, 0, 16, 8}, 0.2f, 0.6f, 0.9f);
    fillLayer(canvas, 0, {16, 0, 24, 8}, 0.5f, 0.5f, 0.5f);
    fillLayer(canvas, 0, {24, 0, 32, 8}, 0.9f, 0.8f, 0.1f, 0.5f);
    fillLayer(canvas, 0, {0, 8, 8, 16}, 0.05f, 0.1f, 0.2f);
    fillLayer(canvas, 0, {8, 8, 16, 16}, 0.95f, 0.9f, 0.85f);
    fillLayer(canvas, 0, {16, 8, 24, 16}, 0.3f, 0.7f, 0.2f, 0.25f);
    fillLayer(canvas, 0, {24, 8, 32, 16}, 0.6f, 0.1f, 0.7f);
    // Un degradado en la mitad de abajo (todas las luminosidades).
    for (int x = 0; x < 32; ++x) {
        const float t = static_cast<float>(x) / 31.0f;
        fillLayer(canvas, 0, {x, 16, x + 1, 24}, t, t * 0.8f, 1.0f - t);
        fillLayer(canvas, 0, {x, 24, x + 1, 28}, t, t, t);
    }
}

// Aplica un ajuste y devuelve la capa 1.
std::vector<uint8_t> adjusted(Canvas& canvas, Adjustment kind, const AdjustParams& params) {
    if (canvas.beginAdjust(kind, params) != Canvas::Edit::Done) {
        return {};
    }
    canvas.endAdjust(true);
    return layerPixels(canvas, 0);
}

} // namespace

TEST_CASE(adjust_hue_saturation_matches_reference) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    paintSwatches(canvas);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    struct Case {
        float hue, saturation, brightness;
    };
    for (const Case& c : {Case{1.0f / 3.0f, 0.0f, 0.0f}, Case{-0.8f, 0.5f, -0.3f}, Case{0.1f, -0.6f, 0.4f},
                          Case{0.0f, 1.0f, 0.0f}}) {
        AdjustParams params;
        params.hue = c.hue;
        params.saturation = c.saturation;
        params.brightness = c.brightness;
        const std::vector<uint8_t> result = adjusted(canvas, Adjustment::HueSaturation, params);
        REQUIRE(!result.empty());
        const std::vector<uint8_t> expected = perPixel(before, 32, [&](Rgb color, int, int) {
            return hueSaturation(color, c.hue, c.saturation, c.brightness);
        });
        CHECK_MATCH(result, expected, 32, IRect::ofSize(32, 32), 1);
        REQUIRE(canvas.undo());
    }

    // Valores conocidos: el rojo con 60° más es amarillo; sin saturación, gris; con el
    // brillo al máximo, blanco. El alfa no cambia.
    AdjustParams params;
    params.hue = 1.0f / 3.0f;
    std::vector<uint8_t> result = adjusted(canvas, Adjustment::HueSaturation, params);
    CHECK_PIXEL(at(result, canvas, 4, 4), (Pixel{255, 255, 0, 255}), 1);
    CHECK_PIXEL(at(result, canvas, 4, 30), kClear, 0);
    REQUIRE(canvas.undo());
    params = {};
    params.saturation = -1.0f;
    result = adjusted(canvas, Adjustment::HueSaturation, params);
    CHECK_PIXEL(at(result, canvas, 4, 4), (Pixel{128, 128, 128, 255}), 1);
    REQUIRE(canvas.undo());
    params = {};
    params.brightness = 1.0f;
    result = adjusted(canvas, Adjustment::HueSaturation, params);
    CHECK_PIXEL(at(result, canvas, 4, 4), (Pixel{255, 255, 255, 255}), 0);
    CHECK_PIXEL(at(result, canvas, 28, 4), (Pixel{128, 128, 128, 128}), 1);
}

TEST_CASE(adjust_color_balance_matches_reference) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    paintSwatches(canvas);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    AdjustParams params;
    params.balance[0][2] = 0.6f;    // sombras hacia el azul
    params.balance[1][0] = 0.5f;    // medios tonos hacia el rojo
    params.balance[1][1] = -0.3f;   // y hacia el magenta
    params.balance[2][0] = -0.4f;   // luces hacia el cian
    const std::vector<uint8_t> result = adjusted(canvas, Adjustment::ColorBalance, params);
    REQUIRE(!result.empty());
    const std::vector<uint8_t> expected =
        perPixel(before, 32, [&](Rgb color, int, int) { return colorBalance(color, params.balance); });
    CHECK_MATCH(result, expected, 32, IRect::ofSize(32, 32), 1);

    // El gris medio se vuelve rojizo con la misma luminosidad.
    const Pixel gray = at(result, canvas, 20, 4);
    CHECK(gray[0] > gray[1] + 60);
    CHECK_NEAR((std::max(gray[0], gray[2]) + std::min(gray[1], gray[2])) / 2, 128, 2);
}

TEST_CASE(adjust_noise_matches_reference_and_keeps_alpha) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    paintSwatches(canvas);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    for (float size : {1.0f, 3.5f}) {
        AdjustParams params;
        params.noise = 0.6f;
        params.noiseSize = size;
        REQUIRE(canvas.beginAdjust(Adjustment::Noise, params) == Canvas::Edit::Done);
        const uint32_t seed = canvas.adjustParams().seed;
        canvas.endAdjust(true);
        const std::vector<uint8_t> result = layerPixels(canvas, 0);
        const std::vector<uint8_t> expected = perPixel(before, 32, [&](Rgb color, int x, int y) {
            const double g = grain(x, y, size, seed) * 0.6;
            return Rgb{std::clamp(color.r + g, 0.0, 1.0), std::clamp(color.g + g, 0.0, 1.0),
                       std::clamp(color.b + g, 0.0, 1.0)};
        });
        CHECK_MATCH(result, expected, 32, IRect::ofSize(32, 32), 1);
        // El alfa es el de antes y lo transparente sigue igual.
        for (size_t i = 3; i < result.size(); i += 4) {
            CHECK_EQ(static_cast<int>(result[i]), static_cast<int>(before[i]));
        }
        CHECK_PIXEL(at(result, canvas, 10, 30), kClear, 0);
        REQUIRE(canvas.undo());
    }

    // Otro ajuste de ruido, otro grano.
    AdjustParams params;
    params.noise = 0.6f;
    REQUIRE(canvas.beginAdjust(Adjustment::Noise, params) == Canvas::Edit::Done);
    const uint32_t first = canvas.adjustParams().seed;
    canvas.endAdjust(false);
    REQUIRE(canvas.beginAdjust(Adjustment::Noise, params) == Canvas::Edit::Done);
    CHECK(canvas.adjustParams().seed != first);
    canvas.endAdjust(false);
}

TEST_CASE(adjust_blur_matches_gaussian_at_full_size) {
    Canvas canvas;
    REQUIRE(canvas.init(48, 40));
    fillLayer(canvas, 0, {12, 10, 30, 26}, 1.0f, 0.1f, 0.1f);
    fillLayer(canvas, 0, {24, 18, 40, 34}, 0.1f, 0.2f, 1.0f, 0.6f);
    fillLayer(canvas, 0, {0, 0, 6, 40}, 0.2f, 0.9f, 0.3f);   // junto al borde del lienzo
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    // Hasta 6,1 no se reduce.
    for (float sigma : {0.8f, 2.0f, 6.1f}) {
        AdjustParams params;
        params.blur = sigma;
        const std::vector<uint8_t> result = adjusted(canvas, Adjustment::Blur, params);
        REQUIRE(!result.empty());
        const int radius = std::clamp(static_cast<int>(std::ceil(sigma * 3.0f)), 1, 64);
        const std::vector<uint8_t> expected = toBytes(gaussian(before, 48, 40, sigma, radius));
        CHECK_MATCH(result, expected, 48, IRect::ofSize(48, 40), 2);
        REQUIRE(canvas.undo());
    }
}

TEST_CASE(adjust_blur_matches_gaussian_when_reduced) {
    Canvas canvas;
    REQUIRE(canvas.init(128, 112));
    fillLayer(canvas, 0, {40, 36, 88, 76}, 1.0f, 1.0f, 1.0f);
    fillLayer(canvas, 0, {60, 20, 70, 100}, 0.9f, 0.2f, 0.1f);
    fillLayer(canvas, 0, {20, 60, 110, 66}, 0.1f, 0.3f, 0.9f, 0.5f);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    // 6,2: justo al empezar a reducir; luego una, dos y tres reducciones. También junto al
    // borde del lienzo, que se repite hacia fuera.
    for (float sigma : {6.2f, 10.0f, 24.0f, 40.0f}) {
        AdjustParams params;
        params.blur = sigma;
        const std::vector<uint8_t> result = adjusted(canvas, Adjustment::Blur, params);
        REQUIRE(!result.empty());
        const std::vector<uint8_t> expected =
            toBytes(gaussian(before, 128, 112, sigma, static_cast<int>(std::ceil(sigma * 4.0f))));
        CHECK_MATCH(result, expected, 128, IRect::ofSize(128, 112), 3);
        REQUIRE(canvas.undo());
    }
}

TEST_CASE(adjust_blur_inside_a_selection_reads_around_it) {
    Canvas canvas;
    REQUIRE(canvas.init(128, 112));
    fillLayer(canvas, 0, {0, 0, 64, 112}, 0.0f, 0.0f, 0.0f);
    fillLayer(canvas, 0, {64, 0, 128, 112}, 1.0f, 1.0f, 1.0f);
    fillLayer(canvas, 0, {80, 70, 100, 90}, 0.9f, 0.1f, 0.2f);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    // En medio, y contra el borde del lienzo; a tamaño completo y reduciendo.
    for (const IRect& selected : {IRect{64, 30, 90, 60}, IRect{96, 80, 128, 112}}) {
        for (float sigma : {2.5f, 10.0f, 24.0f}) {
            REQUIRE(selectRect(canvas, selected));
            AdjustParams params;
            params.blur = sigma;
            const std::vector<uint8_t> result = adjusted(canvas, Adjustment::Blur, params);
            REQUIRE(!result.empty());
            const int radius = static_cast<int>(std::ceil(sigma * (sigma < 6.0f ? 3.0f : 4.0f)));
            const std::vector<uint8_t> expected = toBytes(gaussian(before, 128, 112, sigma, radius));
            CHECK_MATCH(result, expected, 128, selected, 3);
            // Fuera de la selección, nada cambia.
            std::vector<uint8_t> outside = result;
            std::vector<uint8_t> original = before;
            for (int y = selected.y0; y < selected.y1; ++y) {
                for (int x = selected.x0; x < selected.x1; ++x) {
                    const size_t i = (static_cast<size_t>(y) * 128 + static_cast<size_t>(x)) * 4;
                    std::fill(outside.begin() + static_cast<long>(i), outside.begin() + static_cast<long>(i) + 4, 0);
                    std::fill(original.begin() + static_cast<long>(i), original.begin() + static_cast<long>(i) + 4, 0);
                }
            }
            CHECK_EQ(test::maxDifference(outside, original), 0);
            // Junto al borde de la selección se mezcla con el negro de fuera.
            if (selected.x0 == 64) {
                CHECK(at(result, canvas, 64, 45)[0] < 200);
            }
            REQUIRE(canvas.undo());
        }
    }
}

TEST_CASE(adjust_blur_stays_clean_on_transparent_edges) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    fillLayer(canvas, 0, {20, 20, 44, 44}, 1.0f, 1.0f, 1.0f);
    for (float sigma : {1.5f, 8.0f}) {
        AdjustParams params;
        params.blur = sigma;
        const std::vector<uint8_t> result = adjusted(canvas, Adjustment::Blur, params);
        REQUIRE(!result.empty());
        // Blanco premultiplicado: el color es igual al alfa en todas partes (sin halo oscuro).
        int worst = 0;
        for (size_t i = 0; i < result.size(); i += 4) {
            for (size_t c = 0; c < 3; ++c) {
                worst = std::max(worst, std::abs(static_cast<int>(result[i + c]) - static_cast<int>(result[i + 3])));
            }
        }
        CHECK(worst <= 1);
        CHECK(at(result, canvas, 19, 32)[3] > 40);   // se extiende fuera del cuadrado
        REQUIRE(canvas.undo());
    }
}

TEST_CASE(adjust_blur_with_alpha_lock_keeps_alpha) {
    Canvas canvas;
    REQUIRE(canvas.init(40, 40));
    fillLayer(canvas, 0, {8, 8, 20, 32}, 1.0f, 0.0f, 0.0f);
    fillLayer(canvas, 0, {20, 8, 32, 32}, 0.0f, 0.0f, 1.0f);
    fillLayer(canvas, 0, {8, 32, 32, 34}, 0.0f, 1.0f, 0.0f, 0.5f);
    canvas.setLayerAlphaLock(0, true);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    for (float sigma : {2.0f, 6.0f}) {
        AdjustParams params;
        params.blur = sigma;
        const std::vector<uint8_t> result = adjusted(canvas, Adjustment::Blur, params);
        REQUIRE(!result.empty());
        for (size_t i = 3; i < result.size(); i += 4) {
            CHECK_EQ(static_cast<int>(result[i]), static_cast<int>(before[i]));
        }
        CHECK_PIXEL(at(result, canvas, 2, 2), kClear, 0);
        // En la unión, rojo y azul se mezclan; junto al borde transparente, el rojo sigue
        // entero (lo de fuera no tiene color que aportar).
        const Pixel middle = at(result, canvas, 19, 20);
        CHECK(middle[0] > 60 && middle[2] > 60);
        const Pixel edge = at(result, canvas, 8, 20);
        CHECK(edge[0] > 200 && edge[3] == 255);
        REQUIRE(canvas.undo());
    }
}

TEST_CASE(adjust_sharpen_overshoots_edges) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 24));
    fillLayer(canvas, 0, {0, 0, 16, 24}, 0.25f, 0.25f, 0.25f);
    fillLayer(canvas, 0, {16, 0, 32, 24}, 0.75f, 0.75f, 0.75f);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    AdjustParams params;
    params.sharpen = 0.5f;
    const std::vector<uint8_t> result = adjusted(canvas, Adjustment::Sharpen, params);
    REQUIRE(!result.empty());
    const std::vector<double> blurred = gaussian(before, 32, 24, 1.2, 4);
    std::vector<uint8_t> expected(before.size());
    for (size_t i = 0; i < before.size(); i += 4) {
        const double dst = before[i + 3] / 255.0;
        const double a = std::clamp(dst + (dst - blurred[i + 3]) * 1.5, 0.0, 1.0);
        for (size_t c = 0; c < 3; ++c) {
            const double v = before[i + c] / 255.0;
            expected[i + c] = toByte(std::clamp(v + (v - blurred[i + c]) * 1.5, 0.0, a));
        }
        expected[i + 3] = toByte(a);
    }
    CHECK_MATCH(result, expected, 32, IRect::ofSize(32, 24), 2);
    // Más oscuro junto al borde por el lado oscuro y más claro por el claro.
    CHECK(at(result, canvas, 15, 12)[0] < 50);
    CHECK(at(result, canvas, 16, 12)[0] > 205);
    CHECK_PIXEL(at(result, canvas, 4, 12), (Pixel{64, 64, 64, 255}), 1);
}

TEST_CASE(adjust_selection_limits_color_changes) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    fillLayer(canvas, 0, IRect::ofSize(32, 32), 1.0f, 0.0f, 0.0f);
    REQUIRE(selectRect(canvas, {8, 8, 24, 24}));
    AdjustParams params;
    params.hue = 2.0f / 3.0f;   // 120°: verde
    const std::vector<uint8_t> result = adjusted(canvas, Adjustment::HueSaturation, params);
    REQUIRE(!result.empty());
    CHECK_PIXEL(at(result, canvas, 8, 8), (Pixel{0, 255, 0, 255}), 1);
    CHECK_PIXEL(at(result, canvas, 23, 23), (Pixel{0, 255, 0, 255}), 1);
    CHECK_PIXEL(at(result, canvas, 7, 16), (Pixel{255, 0, 0, 255}), 0);
    CHECK_PIXEL(at(result, canvas, 24, 16), (Pixel{255, 0, 0, 255}), 0);
    CHECK_PIXEL(at(result, canvas, 2, 2), (Pixel{255, 0, 0, 255}), 0);
}

TEST_CASE(adjust_preview_compare_cancel_and_apply) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    paintSwatches(canvas);
    const std::vector<uint8_t> layerBefore = layerPixels(canvas, 0);
    const std::vector<uint8_t> seenBefore = composite(canvas);

    AdjustParams params;
    params.hue = 0.5f;
    REQUIRE(canvas.beginAdjust(Adjustment::HueSaturation, params) == Canvas::Edit::Done);
    CHECK(canvas.adjusting());
    CHECK(canvas.adjustKind() == Adjustment::HueSaturation);
    CHECK(canvas.canUndo());
    // La capa no cambia hasta aplicar; se ve el resultado.
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), layerBefore), 0);
    const std::vector<uint8_t> seenFirst = composite(canvas);
    CHECK(test::maxDifference(seenFirst, seenBefore) > 50);
    params.hue = -0.25f;
    params.saturation = 0.3f;
    REQUIRE(canvas.setAdjust(params));
    const std::vector<uint8_t> seen = composite(canvas);
    CHECK(test::maxDifference(seen, seenFirst) > 20);
    // Comparar: mientras se mantiene, se ve la capa como era.
    canvas.showAdjustOriginal(true);
    CHECK_EQ(test::maxDifference(composite(canvas), seenBefore), 0);
    canvas.showAdjustOriginal(false);
    CHECK_EQ(test::maxDifference(composite(canvas), seen), 0);

    // Cancelar: todo como estaba, sin paso de deshacer.
    CHECK(!canvas.endAdjust(false));
    CHECK(!canvas.adjusting());
    CHECK_EQ(canvas.history().undoCount(), 0);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), layerBefore), 0);
    CHECK_EQ(test::maxDifference(composite(canvas), seenBefore), 0);

    // Aplicar: la capa queda como se veía, en un paso.
    REQUIRE(canvas.beginAdjust(Adjustment::HueSaturation, params) == Canvas::Edit::Done);
    CHECK_EQ(test::maxDifference(composite(canvas), seen), 0);
    CHECK(canvas.endAdjust(true));
    CHECK_EQ(canvas.history().undoCount(), 1);
    CHECK_EQ(test::maxDifference(composite(canvas), seen), 0);
    const std::vector<uint8_t> layerAfter = layerPixels(canvas, 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), layerBefore), 0);
    REQUIRE(canvas.redo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), layerAfter), 0);
}

TEST_CASE(adjust_undo_while_adjusting_and_settle) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    paintSwatches(canvas);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    // Deshacer a medias lo quita (y se puede rehacer).
    AdjustParams params;
    params.blur = 3.0f;
    REQUIRE(canvas.beginAdjust(Adjustment::Blur, params) == Canvas::Edit::Done);
    REQUIRE(canvas.undo());
    CHECK(!canvas.adjusting());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);
    REQUIRE(canvas.redo());
    const std::vector<uint8_t> blurred = layerPixels(canvas, 0);
    CHECK(test::maxDifference(blurred, before) > 50);
    REQUIRE(canvas.undo());

    // Otra operación lo aplica antes.
    REQUIRE(canvas.beginAdjust(Adjustment::Blur, params) == Canvas::Edit::Done);
    REQUIRE(canvas.addLayer());
    CHECK(!canvas.adjusting());
    CHECK_EQ(canvas.history().undoCount(), 2);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), blurred), 0);
}

TEST_CASE(adjust_refuses_hidden_layer_and_skips_neutral_values) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    paintSwatches(canvas);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    const int steps = canvas.history().undoCount();
    canvas.setLayerVisible(0, false);
    CHECK(canvas.beginAdjust(Adjustment::Noise, AdjustParams{}) == Canvas::Edit::Hidden);
    CHECK(!canvas.adjusting());
    canvas.setLayerVisible(0, true);

    // Sin cambiar nada (o volviendo a cero), aplicar no guarda nada.
    for (Adjustment kind : {Adjustment::HueSaturation, Adjustment::ColorBalance, Adjustment::Blur,
                            Adjustment::Sharpen, Adjustment::Noise}) {
        const int count = canvas.history().undoCount();
        REQUIRE(canvas.beginAdjust(kind, AdjustParams{}) == Canvas::Edit::Done);
        CHECK(!canvas.endAdjust(true));
        CHECK_EQ(canvas.history().undoCount(), count);
    }
    AdjustParams params;
    params.brightness = 0.5f;
    REQUIRE(canvas.beginAdjust(Adjustment::HueSaturation, params) == Canvas::Edit::Done);
    REQUIRE(canvas.setAdjust(AdjustParams{}));
    CHECK(!canvas.endAdjust(true));
    params = {};
    params.blur = 0.2f;   // no se notaría
    REQUIRE(canvas.beginAdjust(Adjustment::Blur, params) == Canvas::Edit::Done);
    CHECK(!canvas.endAdjust(true));
    CHECK_EQ(canvas.history().undoCount(), steps + 2);   // mostrar y ocultar la capa
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);
}
