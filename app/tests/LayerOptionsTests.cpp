// Modos de fusión, máscara de recorte, bloqueo alfa, rellenar e invertir, capa de
// referencia y cómo se funden y se deshacen.

#include "Test.h"

#include "Canvas/Canvas.h"

#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace {

using test::Pixel;

constexpr Pixel kWhite{255, 255, 255, 255};
constexpr Pixel kClear{0, 0, 0, 0};

std::vector<uint8_t> composite(Canvas& canvas) {
    canvas.update();
    return test::readTarget(canvas.composite());
}

Pixel compositeAt(Canvas& canvas, int x, int y) { return test::pixelAt(composite(canvas), canvas.width(), x, y); }

std::vector<uint8_t> layerPixels(Canvas& canvas, int layer) {
    return test::readTarget(canvas.layers().at(layer).target);
}

Pixel layerAt(Canvas& canvas, int layer, int x, int y) {
    return test::pixelAt(layerPixels(canvas, layer), canvas.width(), x, y);
}

// Color sin premultiplicar y alfa, de 0 a 1.
void fillLayer(Canvas& canvas, int layer, const IRect& rect, float r, float g, float b, float a) {
    test::fillRect(canvas.layers().at(layer).target, rect, r * a, g * a, b * a, a);
    canvas.layers().markDirty(rect);
}

void setBrush(Canvas& canvas, float r, float g, float b, float opacity, float radius = 6.0f, bool eraser = false) {
    BrushSettings& brush = canvas.brushSettings();
    brush.color[0] = r;
    brush.color[1] = g;
    brush.color[2] = b;
    brush.opacity = opacity;
    brush.radius = radius;
    brush.eraser = eraser;
}

void drawLine(Canvas& canvas, glm::vec2 from, glm::vec2 to) {
    canvas.beginStroke(from.x, from.y, 1.0f);
    canvas.strokeTo(to.x, to.y, 1.0f);
    canvas.endStroke();
    canvas.update();
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

// -----------------------------------------------------------------------------
// Modos de fusión en la CPU, con las mismas fórmulas que el shader del compositor.
// -----------------------------------------------------------------------------

using Color = std::array<double, 3>;
using Rgba = std::array<double, 4>;   // premultiplicado

double lum(const Color& c) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }

Color clipColor(Color c) {
    const double l = lum(c);
    const double n = std::min({c[0], c[1], c[2]});
    const double x = std::max({c[0], c[1], c[2]});
    if (n < 0.0) {
        for (double& v : c) {
            v = l + (v - l) * l / (l - n);
        }
    }
    if (x > 1.0) {
        for (double& v : c) {
            v = l + (v - l) * (1.0 - l) / (x - l);
        }
    }
    return c;
}

Color setLum(Color c, double l) {
    const double d = l - lum(c);
    for (double& v : c) {
        v += d;
    }
    return clipColor(c);
}

double sat(const Color& c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

Color setSat(Color c, double s) {
    const double n = std::min({c[0], c[1], c[2]});
    const double x = std::max({c[0], c[1], c[2]});
    for (double& v : c) {
        v = x > n ? (v - n) * s / (x - n) : 0.0;
    }
    return c;
}

double screen(double b, double s) { return b + s - b * s; }
double hardLight(double b, double s) { return s < 0.5 ? 2.0 * b * s : screen(b, 2.0 * s - 1.0); }

double colorBurn(double b, double s) {
    if (b >= 1.0) {
        return 1.0;
    }
    return s <= 0.0 ? 0.0 : 1.0 - std::min(1.0, (1.0 - b) / s);
}

double colorDodge(double b, double s) {
    if (b <= 0.0) {
        return 0.0;
    }
    return s >= 1.0 ? 1.0 : std::min(1.0, b / (1.0 - s));
}

double separable(BlendMode mode, double b, double s) {
    switch (mode) {
    case BlendMode::Multiply:
        return b * s;
    case BlendMode::Darken:
        return std::min(b, s);
    case BlendMode::ColorBurn:
        return colorBurn(b, s);
    case BlendMode::LinearBurn:
        return std::max(b + s - 1.0, 0.0);
    case BlendMode::Lighten:
        return std::max(b, s);
    case BlendMode::Screen:
        return screen(b, s);
    case BlendMode::ColorDodge:
        return colorDodge(b, s);
    case BlendMode::Add:
        return std::min(b + s, 1.0);
    case BlendMode::Overlay:
        return hardLight(s, b);
    case BlendMode::SoftLight: {
        if (s <= 0.5) {
            return b - (1.0 - 2.0 * s) * b * (1.0 - b);
        }
        const double d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : std::sqrt(b);
        return b + (2.0 * s - 1.0) * (d - b);
    }
    case BlendMode::HardLight:
        return hardLight(b, s);
    case BlendMode::VividLight:
        return s <= 0.5 ? colorBurn(b, 2.0 * s) : colorDodge(b, 2.0 * s - 1.0);
    case BlendMode::LinearLight:
        return std::clamp(b + 2.0 * s - 1.0, 0.0, 1.0);
    case BlendMode::PinLight:
        return s < 0.5 ? std::min(b, 2.0 * s) : std::max(b, 2.0 * s - 1.0);
    case BlendMode::HardMix:
        return b + s >= 1.0 - 0.5 / 255.0 ? 1.0 : 0.0;
    case BlendMode::Difference:
        return std::abs(b - s);
    case BlendMode::Exclusion:
        return b + s - 2.0 * b * s;
    case BlendMode::Subtract:
        return std::max(b - s, 0.0);
    case BlendMode::Divide:
        if (s <= 0.0) {
            return b > 0.0 ? 1.0 : 0.0;
        }
        return std::min(1.0, b / s);
    default:
        return s;
    }
}

Color blendColor(BlendMode mode, const Color& b, const Color& s) {
    switch (mode) {
    case BlendMode::DarkerColor:
        return lum(s) < lum(b) ? s : b;
    case BlendMode::LighterColor:
        return lum(s) > lum(b) ? s : b;
    case BlendMode::Hue:
        return setLum(setSat(s, sat(b)), lum(b));
    case BlendMode::Saturation:
        return setLum(setSat(b, sat(s)), lum(b));
    case BlendMode::Color:
        return setLum(s, lum(b));
    case BlendMode::Luminosity:
        return setLum(b, lum(s));
    default:
        return {separable(mode, b[0], s[0]), separable(mode, b[1], s[1]), separable(mode, b[2], s[2])};
    }
}

// Casos en los que un error de redondeo cambia el resultado de golpe: no se comprueban.
bool ambiguous(BlendMode mode, const Color& b, const Color& s) {
    constexpr double kMargin = 0.02;
    switch (mode) {
    case BlendMode::DarkerColor:
    case BlendMode::LighterColor:
        return std::abs(lum(s) - lum(b)) < kMargin;
    case BlendMode::HardMix:
        for (int i = 0; i < 3; ++i) {
            if (std::abs(b[i] + s[i] - 1.0) < kMargin) {
                return true;
            }
        }
        return false;
    case BlendMode::VividLight:
        for (int i = 0; i < 3; ++i) {
            if (std::abs(s[i] - 0.5) < kMargin) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

Color unpremultiply(const Rgba& c) {
    if (c[3] <= 0.0) {
        return {0.0, 0.0, 0.0};
    }
    return {std::clamp(c[0] / c[3], 0.0, 1.0), std::clamp(c[1] / c[3], 0.0, 1.0), std::clamp(c[2] / c[3], 0.0, 1.0)};
}

// Fórmula W3C con alfa premultiplicado.
Rgba blendOver(BlendMode mode, const Rgba& dst, const Rgba& src) {
    if (src[3] <= 0.0) {
        return dst;
    }
    const Color cb = unpremultiply(dst);
    const Color cs = unpremultiply(src);
    const Color mixed = blendColor(mode, cb, cs);
    Rgba out;
    for (int i = 0; i < 3; ++i) {
        out[i] = src[i] * (1.0 - dst[3]) + dst[i] * (1.0 - src[3]) + src[3] * dst[3] * std::clamp(mixed[i], 0.0, 1.0);
    }
    out[3] = src[3] + dst[3] * (1.0 - src[3]);
    return out;
}

Rgba toRgba(const Pixel& p) { return {p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, p[3] / 255.0}; }

Pixel toPixel(const Rgba& c) {
    Pixel p{};
    for (size_t i = 0; i < 4; ++i) {
        p.channels[i] = static_cast<int>(std::lround(std::clamp(c[i], 0.0, 1.0) * 255.0));
    }
    return p;
}

Rgba scaled(const Rgba& c, double factor) { return {c[0] * factor, c[1] * factor, c[2] * factor, c[3] * factor}; }

constexpr BlendMode kAllModes[] = {
    BlendMode::Normal,     BlendMode::Multiply,     BlendMode::Darken,     BlendMode::ColorBurn,
    BlendMode::LinearBurn, BlendMode::DarkerColor,  BlendMode::Lighten,    BlendMode::Screen,
    BlendMode::ColorDodge, BlendMode::Add,          BlendMode::LighterColor, BlendMode::Overlay,
    BlendMode::SoftLight,  BlendMode::HardLight,    BlendMode::VividLight, BlendMode::LinearLight,
    BlendMode::PinLight,   BlendMode::HardMix,      BlendMode::Difference, BlendMode::Exclusion,
    BlendMode::Subtract,   BlendMode::Divide,       BlendMode::Hue,        BlendMode::Saturation,
    BlendMode::Color,      BlendMode::Luminosity,
};
static_assert(std::size(kAllModes) == kBlendModeCount);

} // namespace

// -----------------------------------------------------------------------------
// Modos de fusión
// -----------------------------------------------------------------------------

TEST_CASE(blend_modes_match_reference) {
    // Columnas: 6 colores de fondo × 4 alfas. Filas: 6 colores de capa × 3 alfas.
    const Color colors[] = {{0.90, 0.20, 0.10}, {0.15, 0.55, 0.85}, {0.35, 0.33, 0.30},
                            {1.0, 1.0, 1.0},    {0.0, 0.0, 0.0},    {0.62, 0.78, 0.12}};
    const Color sources[] = {{0.10, 0.70, 0.40}, {0.95, 0.60, 0.25}, {0.30, 0.15, 0.72},
                             {1.0, 1.0, 1.0},    {0.0, 0.0, 0.0},    {0.58, 0.44, 0.91}};
    const float backdropAlphas[] = {1.0f, 0.78f, 0.47f, 0.0f};
    const float sourceAlphas[] = {1.0f, 0.71f, 0.35f};
    constexpr int kWidth = 24;
    constexpr int kHeight = 18;

    Canvas canvas;
    REQUIRE(canvas.init(kWidth, kHeight));
    test::hideBackground(canvas);   // la capa de abajo, con sus alfas, es todo el fondo
    REQUIRE(canvas.addLayer());
    for (int x = 0; x < kWidth; ++x) {
        const Color& c = colors[x / 4];
        fillLayer(canvas, 0, {x, 0, x + 1, kHeight}, static_cast<float>(c[0]), static_cast<float>(c[1]),
                  static_cast<float>(c[2]), backdropAlphas[x % 4]);
    }
    for (int y = 0; y < kHeight; ++y) {
        const Color& c = sources[y / 3];
        fillLayer(canvas, 1, {0, y, kWidth, y + 1}, static_cast<float>(c[0]), static_cast<float>(c[1]),
                  static_cast<float>(c[2]), sourceAlphas[y % 3]);
    }
    constexpr float kOpacity = 0.8f;
    canvas.layers().setOpacity(1, kOpacity);
    const std::vector<uint8_t> backdrop = layerPixels(canvas, 0);
    const std::vector<uint8_t> source = layerPixels(canvas, 1);

    for (const BlendMode mode : kAllModes) {
        canvas.layers().setBlend(1, mode);
        const std::vector<uint8_t> result = composite(canvas);
        int worst = 0;
        Pixel worstActual{};
        Pixel worstExpected{};
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const Rgba dst = toRgba(test::pixelAt(backdrop, kWidth, x, y));
                const Rgba src = scaled(toRgba(test::pixelAt(source, kWidth, x, y)), kOpacity);
                if (ambiguous(mode, unpremultiply(dst), unpremultiply(src))) {
                    continue;
                }
                const Pixel expected = toPixel(blendOver(mode, dst, src));
                const Pixel actual = test::pixelAt(result, kWidth, x, y);
                for (size_t i = 0; i < 4; ++i) {
                    const int difference = std::abs(actual[i] - expected[i]);
                    if (difference > worst) {
                        worst = difference;
                        worstActual = actual;
                        worstExpected = expected;
                    }
                }
            }
        }
        if (worst > 2) {
            std::ostringstream message;
            message << "modo " << static_cast<int>(mode) << ": " << worstActual << ", se esperaba " << worstExpected;
            test::fail(__FILE__, __LINE__, message.str());
        }
    }
}

TEST_CASE(blend_modes_stack_and_partial_updates) {
    // Varias capas en modos distintos (el compuesto va y viene entre dos destinos) y un
    // trazo a medias en una de ellas: recomponer solo la zona sucia da lo mismo que todo.
    Canvas canvas;
    REQUIRE(canvas.init(64, 48));
    LayerStack& layers = canvas.layers();
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 0, IRect::ofSize(64, 48), 0.85f, 0.8f, 0.7f, 1.0f);
    fillLayer(canvas, 1, {0, 0, 40, 48}, 0.9f, 0.3f, 0.2f, 0.9f);
    layers.setBlend(1, BlendMode::Multiply);
    const struct {
        BlendMode mode;
        IRect rect;
        float r, g, b, a;
    } extra[] = {
        {BlendMode::Normal, {20, 0, 64, 20}, 0.1f, 0.4f, 0.9f, 0.6f},
        {BlendMode::Screen, {10, 10, 50, 40}, 0.2f, 0.8f, 0.3f, 0.8f},
        {BlendMode::Difference, {30, 5, 60, 45}, 0.7f, 0.7f, 0.1f, 1.0f},
        {BlendMode::Color, {0, 24, 64, 48}, 0.2f, 0.3f, 0.9f, 0.7f},
    };
    for (const auto& layer : extra) {
        REQUIRE(canvas.addLayer());
        const int index = layers.activeIndex();
        fillLayer(canvas, index, layer.rect, layer.r, layer.g, layer.b, layer.a);
        layers.setBlend(index, layer.mode);
    }
    layers.setOpacity(3, 0.7f);

    // Referencia en la CPU, capa a capa.
    std::vector<std::vector<uint8_t>> pixels;
    for (int i = 0; i < layers.count(); ++i) {
        pixels.push_back(layerPixels(canvas, i));
    }
    const std::vector<uint8_t> result = composite(canvas);
    int worst = 0;
    for (int y = 0; y < 48; y += 3) {
        for (int x = 0; x < 64; x += 3) {
            Rgba color{0.0, 0.0, 0.0, 0.0};
            for (int i = 0; i < layers.count(); ++i) {
                const Rgba src = scaled(toRgba(test::pixelAt(pixels[static_cast<size_t>(i)], 64, x, y)),
                                        layers.at(i).opacity);
                color = blendOver(layers.at(i).blend, color, src);
            }
            const Pixel expected = toPixel(color);
            const Pixel actual = test::pixelAt(result, 64, x, y);
            for (size_t c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(actual[c] - expected[c]));
            }
        }
    }
    CHECK(worst <= 3);

    // Trazo a medias en la capa en modo Trama.
    canvas.selectLayer(3);
    setBrush(canvas, 0.9f, 0.1f, 0.6f, 0.8f, 5.0f);
    canvas.beginStroke(5.0f, 30.0f, 1.0f);
    canvas.strokeTo(58.0f, 12.0f, 0.8f);
    const std::vector<uint8_t> partial = composite(canvas);
    layers.markAllDirty();
    CHECK_EQ(test::maxDifference(composite(canvas), partial), 0);
    canvas.endStroke();
    // Lo que se veía es lo que queda. Al fundirse, el trazo pasa a 8 bits y los modos de
    // encima (Color, sobre todo) pueden agrandar un poco esa diferencia.
    CHECK(test::maxDifference(composite(canvas), partial) <= 4);
}

TEST_CASE(blend_mode_edit_is_one_undo_step) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    LayerStack& layers = canvas.layers();
    fillLayer(canvas, 0, IRect::ofSize(16, 16), 0.5f, 0.2f, 0.9f, 1.0f);
    const std::vector<uint8_t> normal = composite(canvas);

    // Probar modos uno tras otro deja un solo paso, desde el modo de antes de empezar.
    canvas.setLayerBlend(0, BlendMode::Multiply, false);
    CHECK(canvas.canUndo());   // aunque el paso aún no se ha guardado
    canvas.setLayerBlend(0, BlendMode::Screen, false);
    canvas.setLayerBlend(0, BlendMode::Difference, false);
    CHECK_EQ(canvas.history().undoCount(), 0);
    const std::vector<uint8_t> difference = composite(canvas);
    CHECK(test::maxDifference(difference, normal) > 0);
    canvas.finishLayerEdit();
    CHECK_EQ(canvas.history().undoCount(), 1);

    REQUIRE(canvas.undo());
    CHECK(layers.at(0).blend == BlendMode::Normal);
    CHECK_EQ(test::maxDifference(composite(canvas), normal), 0);
    REQUIRE(canvas.redo());
    CHECK(layers.at(0).blend == BlendMode::Difference);
    CHECK_EQ(test::maxDifference(composite(canvas), difference), 0);

    // Volver al modo del principio no deja paso; otro cambio cierra el anterior.
    canvas.setLayerBlend(0, BlendMode::Overlay, false);
    canvas.setLayerBlend(0, BlendMode::Difference, false);
    canvas.finishLayerEdit();
    CHECK_EQ(canvas.history().undoCount(), 1);
    canvas.setLayerBlend(0, BlendMode::Overlay, false);
    canvas.setLayerOpacity(0, 0.5f, false);   // misma capa: sigue el mismo cambio
    canvas.setLayerVisible(0, false);         // otro tipo de cambio: cierra el anterior
    CHECK_EQ(canvas.history().undoCount(), 3);
    REQUIRE(canvas.undo());
    CHECK(layers.at(0).visible);
    REQUIRE(canvas.undo());
    CHECK(layers.at(0).blend == BlendMode::Difference);
    CHECK_EQ(layers.at(0).opacity, 1.0f);
}

// -----------------------------------------------------------------------------
// Máscara de recorte
// -----------------------------------------------------------------------------

TEST_CASE(clipping_mask_uses_base_alpha) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    fillLayer(canvas, 0, {4, 4, 20, 20}, 0.0f, 0.0f, 1.0f, 1.0f);    // base: azul
    fillLayer(canvas, 0, {20, 4, 28, 20}, 0.0f, 0.0f, 1.0f, 0.5f);   // y azul a medias
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 1, IRect::ofSize(32, 32), 1.0f, 0.0f, 0.0f, 1.0f);
    CHECK(canvas.canClip(1));
    canvas.setLayerClipping(1, true);
    CHECK(layers.at(1).clipping);
    CHECK_EQ(layers.clipBase(1), 0);

    CHECK_PIXEL(compositeAt(canvas, 10, 10), (Pixel{255, 0, 0, 255}), 0);
    CHECK_PIXEL(compositeAt(canvas, 24, 10), (Pixel{191, 64, 128, 255}), 1);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), kWhite, 0);

    // Una segunda capa recortada recorta con la misma base.
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 2, IRect::ofSize(32, 32), 0.0f, 1.0f, 0.0f, 0.5f);
    canvas.setLayerClipping(2, true);
    CHECK_EQ(layers.clipBase(2), 0);
    CHECK_PIXEL(compositeAt(canvas, 10, 10), (Pixel{128, 128, 0, 255}), 1);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), kWhite, 0);

    // La opacidad de la base no cambia el recorte; ocultarla oculta las recortadas.
    layers.setOpacity(0, 0.3f);
    CHECK_PIXEL(compositeAt(canvas, 10, 10), (Pixel{128, 128, 0, 255}), 1);
    canvas.setLayerVisible(0, false);
    CHECK_PIXEL(compositeAt(canvas, 10, 10), kWhite, 0);
    CHECK(!canvas.layerShown(1));
    CHECK(canvas.strokeBlock() == Canvas::StrokeBlock::ClipBaseHidden);
    CHECK(!canvas.beginStroke(10.0f, 10.0f, 1.0f));
    canvas.setLayerVisible(0, true);
    layers.setOpacity(0, 1.0f);

    // Sin recorte, la capa se ve entera.
    canvas.setLayerClipping(2, false);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), (Pixel{128, 255, 128, 255}), 1);

    // La capa de abajo del todo no puede recortar; si llega abajo recortando, se ve entera.
    CHECK(!canvas.canClip(0));
    canvas.setLayerClipping(0, true);
    CHECK(!layers.at(0).clipping);
    REQUIRE(canvas.moveLayer(1, 0));
    CHECK(layers.at(0).clipping);
    CHECK_EQ(layers.clipBase(0), -1);
    CHECK(canvas.layerShown(0));
}

TEST_CASE(clipping_stroke_preview_matches_commit) {
    for (const int target : {0, 1}) {
        for (const bool erase : {false, true}) {
            Canvas canvas;
            REQUIRE(canvas.init(64, 64));
            fillLayer(canvas, 0, {8, 8, 40, 56}, 0.2f, 0.3f, 0.9f, 0.9f);
            REQUIRE(canvas.addLayer());
            fillLayer(canvas, 1, {0, 20, 64, 44}, 0.9f, 0.6f, 0.1f, 1.0f);
            canvas.setLayerClipping(1, true);
            canvas.layers().setBlend(1, BlendMode::Multiply);
            canvas.selectLayer(target);
            canvas.update();

            // Pintar o borrar en la base cambia también lo que se ve de la recortada.
            setBrush(canvas, 0.1f, 0.9f, 0.2f, 0.7f, 7.0f, erase);
            REQUIRE(canvas.beginStroke(4.0f, 30.0f, 1.0f));
            canvas.strokeTo(60.0f, 34.0f, 0.8f);
            const std::vector<uint8_t> preview = composite(canvas);
            canvas.endStroke();
            CHECK(test::maxDifference(composite(canvas), preview) <= 2);
        }
    }
}

// -----------------------------------------------------------------------------
// Bloqueo alfa, rellenar e invertir
// -----------------------------------------------------------------------------

TEST_CASE(alpha_lock_paints_only_where_there_is_paint) {
    Canvas canvas;
    REQUIRE(canvas.init(40, 32));
    fillLayer(canvas, 0, {8, 8, 24, 24}, 0.0f, 0.0f, 1.0f, 1.0f);
    fillLayer(canvas, 0, {24, 8, 32, 24}, 0.0f, 0.0f, 1.0f, 0.5f);
    canvas.setLayerAlphaLock(0, true);
    CHECK(canvas.layers().at(0).alphaLock);
    CHECK_EQ(canvas.history().undoCount(), 1);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    setBrush(canvas, 1.0f, 0.0f, 0.0f, 1.0f, 6.0f);
    REQUIRE(canvas.beginStroke(2.0f, 16.0f, 1.0f));
    canvas.strokeTo(38.0f, 16.0f, 1.0f);
    const std::vector<uint8_t> preview = composite(canvas);
    canvas.endStroke();
    CHECK(test::maxDifference(composite(canvas), preview) <= 2);

    const std::vector<uint8_t> after = layerPixels(canvas, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 40; ++x) {
            if (test::pixelAt(after, 40, x, y)[3] != test::pixelAt(before, 40, x, y)[3]) {
                test::fail(__FILE__, __LINE__, "el alfa cambió en " + std::to_string(x) + "," + std::to_string(y));
                return;
            }
        }
    }
    CHECK_PIXEL(layerAt(canvas, 0, 4, 16), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 0, 16, 16), (Pixel{255, 0, 0, 255}), 6);
    CHECK_PIXEL(layerAt(canvas, 0, 28, 16), (Pixel{128, 0, 0, 128}), 6);
    CHECK_PIXEL(layerAt(canvas, 0, 16, 9), (Pixel{0, 0, 255, 255}), 0);   // fuera del trazo

    // Con el alfa bloqueado no se borra (ni con la goma del lápiz).
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 6.0f, true);
    CHECK(canvas.strokeBlock() == Canvas::StrokeBlock::AlphaLocked);
    CHECK(!canvas.beginStroke(16.0f, 16.0f, 1.0f));
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 6.0f, false);
    CHECK(canvas.strokeBlock() == Canvas::StrokeBlock::None);
    CHECK(canvas.strokeBlock(true) == Canvas::StrokeBlock::AlphaLocked);
    CHECK(!canvas.beginStroke(16.0f, 16.0f, 1.0f, true));
    CHECK(!canvas.stroking());

    canvas.setLayerAlphaLock(0, false);
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 6.0f, true);
    drawLine(canvas, {16.0f, 12.0f}, {16.0f, 20.0f});
    CHECK(layerAt(canvas, 0, 16, 16)[3] <= 5);
    REQUIRE(canvas.undo());   // el borrado
    REQUIRE(canvas.undo());   // quitar el bloqueo
    CHECK(canvas.layers().at(0).alphaLock);
}

TEST_CASE(fill_and_invert_layer) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    fillLayer(canvas, 0, {4, 4, 12, 12}, 0.2f, 0.4f, 0.6f, 0.8f);
    const std::vector<uint8_t> original = layerPixels(canvas, 0);
    const float orange[3] = {1.0f, 0.5f, 0.0f};

    canvas.fillLayer(0, orange);
    CHECK_PIXEL(layerAt(canvas, 0, 1, 1), (Pixel{255, 128, 0, 255}), 1);
    CHECK_PIXEL(layerAt(canvas, 0, 8, 8), (Pixel{255, 128, 0, 255}), 1);
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), original), 0);

    // Con el alfa bloqueado, solo donde hay pintura y con su alfa.
    canvas.setLayerAlphaLock(0, true);
    canvas.fillLayer(0, orange);
    CHECK_PIXEL(layerAt(canvas, 0, 1, 1), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 0, 8, 8), (Pixel{204, 102, 0, 204}), 1);

    // Invertir: el color pasa a 1 − c y el alfa se queda.
    canvas.invertLayer(0);
    CHECK_PIXEL(layerAt(canvas, 0, 8, 8), (Pixel{0, 102, 204, 204}), 1);
    CHECK_PIXEL(layerAt(canvas, 0, 1, 1), kClear, 0);
    canvas.invertLayer(0);
    CHECK_PIXEL(layerAt(canvas, 0, 8, 8), (Pixel{204, 102, 0, 204}), 0);

    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), original), 0);
    // El fondo no es una capa: invertir la capa no lo toca, y donde está vacía sigue blanco.
    canvas.invertLayer(0);
    CHECK_PIXEL(compositeAt(canvas, 1, 1), kWhite, 0);
}

// -----------------------------------------------------------------------------
// Capa de referencia
// -----------------------------------------------------------------------------

TEST_CASE(reference_layer_is_unique_and_undoable) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    LayerStack& layers = canvas.layers();
    REQUIRE(canvas.addLayer());
    CHECK_EQ(layers.referenceIndex(), -1);

    canvas.setReferenceLayer(0);
    CHECK_EQ(layers.referenceIndex(), 0);
    canvas.setReferenceLayer(1);
    CHECK_EQ(layers.referenceIndex(), 1);
    CHECK(!layers.at(0).reference);
    canvas.setReferenceLayer(1);   // sin cambio: no hay paso
    CHECK_EQ(canvas.history().undoCount(), 3);

    // Duplicar no copia la referencia.
    REQUIRE(canvas.duplicateLayer(1));
    CHECK_EQ(layers.referenceIndex(), 1);
    CHECK(!layers.at(2).reference);
    REQUIRE(canvas.undo());

    REQUIRE(canvas.undo());
    CHECK_EQ(layers.referenceIndex(), 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.referenceIndex(), -1);
    REQUIRE(canvas.redo());
    REQUIRE(canvas.redo());
    CHECK_EQ(layers.referenceIndex(), 1);

    canvas.setReferenceLayer(-1);
    CHECK_EQ(layers.referenceIndex(), -1);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.referenceIndex(), 1);

    // Borrar la referencia la quita; deshacer la devuelve.
    const uint32_t reference = layers.at(1).id;
    REQUIRE(canvas.removeLayer(1));
    CHECK_EQ(layers.referenceIndex(), -1);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.referenceIndex(), layers.indexOf(reference));
}

// -----------------------------------------------------------------------------
// Fundir hacia abajo
// -----------------------------------------------------------------------------

TEST_CASE(merge_down_applies_blend_mode) {
    // Con la capa de abajo opaca, fundir una capa en Multiplicar se ve igual.
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    fillLayer(canvas, 0, IRect::ofSize(32, 32), 0.9f, 0.7f, 0.4f, 1.0f);
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 1, {8, 8, 24, 24}, 0.2f, 0.5f, 0.9f, 0.8f);
    layers.setBlend(1, BlendMode::Multiply);
    layers.setOpacity(1, 0.6f);
    layers.setBlend(0, BlendMode::Screen);   // la fundida conserva el modo de la de abajo
    // Sobre el fondo negro, en Trama la capa de abajo se ve tal cual.
    canvas.setBackground(CanvasBackground{{0.0f, 0.0f, 0.0f}, true});
    const std::vector<uint8_t> look = composite(canvas);

    REQUIRE(canvas.mergeDown(1));
    CHECK(layers.at(0).blend == BlendMode::Screen);
    CHECK(test::maxDifference(composite(canvas), look) <= 2);
}

TEST_CASE(merge_down_with_clipping) {
    const auto build = [](Canvas& canvas) {
        LayerStack& layers = canvas.layers();
        // 0: base con zona opaca y zona transparente; 1 y 2 recortan con ella; 3 normal.
        fillLayer(canvas, 0, {0, 0, 20, 32}, 0.2f, 0.3f, 0.9f, 1.0f);
        canvas.addLayer();
        fillLayer(canvas, 1, {10, 0, 32, 32}, 0.9f, 0.2f, 0.1f, 0.7f);
        layers.setClipping(1, true);
        canvas.addLayer();
        fillLayer(canvas, 2, {0, 10, 32, 22}, 0.1f, 0.8f, 0.2f, 0.9f);
        layers.setClipping(2, true);
        canvas.addLayer();
        fillLayer(canvas, 3, {0, 0, 32, 6}, 0.9f, 0.9f, 0.1f, 0.8f);
    };

    // Las dos recortan con la misma base: la fundida sigue recortando.
    {
        Canvas canvas;
        REQUIRE(canvas.init(32, 32));
        build(canvas);
        const std::vector<uint8_t> look = composite(canvas);
        REQUIRE(canvas.mergeDown(2));
        CHECK(canvas.layers().at(1).clipping);
        CHECK(test::maxDifference(composite(canvas), look) <= 2);
    }
    // La de arriba recorta con la de abajo (con opacidad): el recorte se aplica al fundir.
    {
        Canvas canvas;
        REQUIRE(canvas.init(32, 32));
        build(canvas);
        canvas.layers().setOpacity(0, 0.5f);
        fillLayer(canvas, 0, {0, 0, 20, 32}, 0.2f, 0.3f, 0.9f, 0.6f);
        // La 2 pasará a recortar con la fundida, que tiene otro alfa (lleva la opacidad
        // horneada): esa sí cambia, así que no cuenta para comparar.
        canvas.layers().setVisible(2, false);
        const std::vector<uint8_t> look = composite(canvas);
        REQUIRE(canvas.mergeDown(1));
        CHECK(!canvas.layers().at(0).clipping);
        CHECK_EQ(canvas.layers().at(0).opacity, 1.0f);
        CHECK_EQ(canvas.layers().clipBase(1), 0);   // la otra recortada sigue con la fundida
        CHECK(test::maxDifference(composite(canvas), look) <= 2);
    }
    // Solo recorta la de abajo: su recorte se hornea y la fundida deja de recortar.
    {
        Canvas canvas;
        REQUIRE(canvas.init(32, 32));
        build(canvas);
        const std::vector<uint8_t> look = composite(canvas);
        const LayerProperties before{canvas.layers().at(2).name, true, 1.0f, BlendMode::Normal, false, true};
        REQUIRE(canvas.mergeDown(3));
        CHECK(!canvas.layers().at(2).clipping);
        CHECK(test::maxDifference(composite(canvas), look) <= 2);

        REQUIRE(canvas.undo());
        CHECK_EQ(canvas.layers().count(), 4);
        CHECK(canvas.layers().at(2).clipping);
        CHECK_EQ(canvas.layers().at(2).name, before.name);
        CHECK_EQ(test::maxDifference(composite(canvas), look), 0);
        REQUIRE(canvas.redo());
        CHECK(!canvas.layers().at(2).clipping);
        CHECK(test::maxDifference(composite(canvas), look) <= 2);
    }
    // No se funde con una capa que no se ve (ni con una recortada cuya base está oculta).
    {
        Canvas canvas;
        REQUIRE(canvas.init(32, 32));
        build(canvas);
        canvas.setLayerVisible(0, false);
        CHECK(!canvas.canMergeDown(2));
        CHECK(!canvas.canMergeDown(3));
        CHECK(!canvas.mergeDown(3));
    }
}

// -----------------------------------------------------------------------------
// Propiedades nuevas: deshacer, duplicar y pérdida de contexto
// -----------------------------------------------------------------------------

TEST_CASE(layer_options_undo_and_duplicate) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    LayerStack& layers = canvas.layers();
    REQUIRE(canvas.addLayer());
    canvas.setLayerBlend(1, BlendMode::Overlay);
    canvas.setLayerAlphaLock(1, true);
    canvas.setLayerClipping(1, true);
    canvas.setLayerClipping(1, true);   // sin cambio: no hay paso
    CHECK_EQ(canvas.history().undoCount(), 4);

    REQUIRE(canvas.duplicateLayer(1));
    const Layer& copy = layers.at(2);
    CHECK(copy.blend == BlendMode::Overlay);
    CHECK(copy.alphaLock);
    CHECK(copy.clipping);
    CHECK_EQ(layers.clipBase(2), 0);
    REQUIRE(canvas.undo());

    REQUIRE(canvas.undo());
    CHECK(!layers.at(1).clipping);
    REQUIRE(canvas.undo());
    CHECK(!layers.at(1).alphaLock);
    REQUIRE(canvas.undo());
    CHECK(layers.at(1).blend == BlendMode::Normal);
    REQUIRE(canvas.redo());
    REQUIRE(canvas.redo());
    REQUIRE(canvas.redo());
    CHECK(layers.at(1).blend == BlendMode::Overlay);
    CHECK(layers.at(1).alphaLock);
    CHECK(layers.at(1).clipping);
}

TEST_CASE(layer_options_survive_context_loss) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    fillLayer(canvas, 0, {4, 4, 28, 28}, 0.3f, 0.6f, 0.9f, 0.9f);
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 1, {0, 12, 32, 20}, 0.9f, 0.4f, 0.2f, 1.0f);
    canvas.setLayerBlend(1, BlendMode::HardLight);
    canvas.setLayerClipping(1, true);
    canvas.setReferenceLayer(0);
    const std::vector<uint8_t> before = composite(canvas);

    REQUIRE(canvas.takeSnapshot(size_t{1} << 30));
    REQUIRE(test::recreateGLContext());
    REQUIRE(canvas.recreateGpu(nullptr));
    CHECK(layers.at(1).blend == BlendMode::HardLight);
    CHECK(layers.at(1).clipping);
    CHECK_EQ(layers.referenceIndex(), 0);
    CHECK_EQ(test::maxDifference(composite(canvas), before), 0);
}
