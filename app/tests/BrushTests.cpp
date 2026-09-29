// Pinceles: la biblioteca y su archivo de ajustes, el recorrido del trazo (espaciado,
// afinado, estabilización, azar) y cómo pintan en la GPU (acumulación, grano, dureza).

#include "Test.h"

#include "Canvas/BrushLibrary.h"
#include "Canvas/BrushTips.h"
#include "Canvas/Canvas.h"
#include "Canvas/StrokePath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <sstream>
#include <string>

namespace {

constexpr float kPi = 3.14159265358979f;

std::vector<Dab> allDabs(StrokePath& path) {
    std::vector<Dab> dabs;
    path.takeFinal(dabs);
    return dabs;
}

BrushParams roundBrush(float spacing = 0.1f) {
    BrushParams p;
    p.tip = BrushTip::Round;
    p.spacing = spacing;
    p.minSpacing = 0.5f;
    p.pressureSize = 1.0f;
    p.pressureOpacity = 0.0f;
    p.flow = 1.0f;
    p.minRadius = 0.5f;
    p.maxRadius = 200.0f;
    return p;
}

StrokePath::Settings pathSettings(float radius, float flow = 1.0f, uint32_t seed = 7) {
    StrokePath::Settings s;
    s.radius = radius;
    s.flow = flow;
    s.pixelsPerPoint = 1.0f;
    s.seed = seed;
    return s;
}

bool sameDab(const Dab& a, const Dab& b, float tolerance = 1e-4f) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance &&
           std::fabs(a.radius - b.radius) <= tolerance && std::fabs(a.alpha - b.alpha) <= tolerance &&
           std::fabs(a.angle - b.angle) <= tolerance;
}

// --- GPU ---

std::vector<uint8_t> layerPixels(Canvas& canvas, int layer) {
    return test::readTarget(canvas.layers().at(layer).target);
}

int alphaAt(Canvas& canvas, int layer, int x, int y) {
    return test::pixelAt(layerPixels(canvas, layer), canvas.width(), x, y)[3];
}

void useBrush(Canvas& canvas, const BrushParams& params, float radius, float opacity = 1.0f, bool eraser = false) {
    BrushSettings& settings = canvas.brushSettings();
    settings.brush = params;
    settings.radius = radius;
    settings.opacity = opacity;
    settings.eraser = eraser;
    settings.color[0] = 0.0f;
    settings.color[1] = 0.0f;
    settings.color[2] = 0.0f;
}

void stroke(Canvas& canvas, std::initializer_list<std::array<float, 3>> points) {
    bool first = true;
    for (const auto& p : points) {
        if (first) {
            canvas.beginStroke(p[0], p[1], p[2]);
            first = false;
        } else {
            canvas.strokeTo(p[0], p[1], p[2]);
        }
    }
    canvas.endStroke();
    canvas.update();
}

} // namespace

// -----------------------------------------------------------------------------
// Biblioteca
// -----------------------------------------------------------------------------

TEST_CASE(brush_library_is_consistent) {
    const auto library = brushes::library();
    CHECK(library.size() >= 20);
    std::set<std::string> ids;
    int perCategory[kBrushCategoryCount] = {};
    for (const BrushPreset& preset : library) {
        CHECK(ids.insert(preset.id).second);
        CHECK(std::string(preset.name).size() > 0);
        ++perCategory[static_cast<int>(preset.category)];
        // Los valores de fábrica ya están dentro de sus márgenes.
        BrushParams copy = preset.params;
        brushes::sanitize(copy);
        if (!(copy == preset.params)) {
            test::fail(__FILE__, __LINE__, std::string("ajustes fuera de margen en ") + preset.id);
        }
        CHECK(preset.params.classic == (preset.category == BrushCategory::Classic));
    }
    for (int c = 0; c < kBrushCategoryCount; ++c) {
        CHECK(perCategory[c] >= 3);
    }
    CHECK(brushes::indexOf(brushes::kDefaultPaint) >= 0);
    CHECK(brushes::indexOf(brushes::kDefaultErase) >= 0);
    CHECK_EQ(brushes::indexOf("no-existe"), -1);
    CHECK(brushes::basic().classic);
    CHECK(brushes::basic().tip == BrushTip::Classic0);
}

TEST_CASE(brush_settings_roundtrip) {
    const BrushParams base = brushes::library()[static_cast<size_t>(brushes::indexOf("lapiz-hb"))].params;
    BrushParams changed = base;
    changed.tip = BrushTip::Charcoal;
    changed.grain = BrushGrain::Canvas;
    changed.buildUp = BrushBuildUp::Uniform;
    changed.followStroke = true;
    changed.spacing = 0.25f;
    changed.streamline = 0.5f;
    changed.angle = 135.0f;
    changed.grainScale = 2.5f;
    changed.maxRadius = 48.0f;
    changed.count = 3.0f;

    const std::string text = brushes::describeChanges(base, changed);
    BrushParams loaded = base;
    std::istringstream lines(text);
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        const size_t space = line.find(' ');
        REQUIRE(space != std::string::npos);
        CHECK(brushes::applySetting(loaded, line.substr(0, space), line.substr(space + 1)));
        ++count;
    }
    CHECK_EQ(count, 10);
    CHECK(loaded == changed);
    CHECK(brushes::describeChanges(base, base).empty());

    // Lo que no se entiende no cambia nada.
    BrushParams untouched = base;
    CHECK(!brushes::applySetting(untouched, "punta", "estrella"));
    CHECK(!brushes::applySetting(untouched, "espaciado", "mucho"));
    CHECK(!brushes::applySetting(untouched, "brillo", "1"));
    CHECK(untouched == base);
    // Los números se quedan dentro de su margen.
    CHECK(brushes::applySetting(untouched, "dureza", "7"));
    CHECK_EQ(untouched.hardness, 1.0f);
}

TEST_CASE(brush_param_sliders_and_text) {
    for (const brushes::ParamInfo& info : brushes::params()) {
        for (const float t : {0.0f, 0.3f, 0.75f, 1.0f}) {
            const float value = brushes::fromSlider(info, t);
            CHECK(value >= info.min && value <= info.max);
            if (info.format != brushes::ParamFormat::Count) {
                CHECK_NEAR(brushes::toSlider(info, value), t, 1e-4);
            }
        }
    }
    const brushes::ParamInfo* spacing = brushes::findParam("espaciado");
    const brushes::ParamInfo* angle = brushes::findParam("angulo");
    const brushes::ParamInfo* scale = brushes::findParam("grano-escala");
    const brushes::ParamInfo* size = brushes::findParam("tamano-maximo");
    const brushes::ParamInfo* count = brushes::findParam("sellos");
    REQUIRE(spacing && angle && scale && size && count);
    CHECK_EQ(brushes::formatValue(*spacing, 0.08f), std::string("8 %"));
    CHECK_EQ(brushes::formatValue(*angle, 45.0f), std::string("45°"));
    CHECK_EQ(brushes::formatValue(*scale, 1.5f), std::string("×1,50"));
    CHECK_EQ(brushes::formatValue(*size, 12.0f), std::string("24 px"));
    CHECK_EQ(brushes::formatValue(*size, 0.75f), std::string("1,5 px"));
    CHECK_EQ(brushes::fromSlider(*count, 0.5f), 5.0f);

    // Tamaño: del mínimo al máximo del pincel, con más recorrido en los pequeños.
    const BrushParams& pen = brushes::library()[static_cast<size_t>(brushes::indexOf("pluma-estudio"))].params;
    CHECK_NEAR(brushes::radiusFor(pen, 0.0f), pen.minRadius, 1e-5);
    CHECK_NEAR(brushes::radiusFor(pen, 1.0f), pen.maxRadius, 1e-4);
    CHECK_NEAR(brushes::sizeFor(pen, brushes::radiusFor(pen, 0.4f)), 0.4f, 1e-4);
}

TEST_CASE(brush_tips_and_grains_generate) {
    for (int i = 0; i < kBrushTipCount; ++i) {
        const BrushTip tip = static_cast<BrushTip>(i);
        const std::vector<uint8_t> image = brushtips::makeTip(tip, 64);
        if (i >= static_cast<int>(BrushTip::Classic0)) {
            CHECK(image.empty());   // salen de los PNG
            continue;
        }
        REQUIRE(image.size() == 64u * 64u);
        // Nada en el borde (el sello se corta en su cuadrado) y algo en el centro.
        int border = 0;
        long total = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int v = image[static_cast<size_t>(y * 64 + x)];
                total += v;
                if (x == 0 || y == 0 || x == 63 || y == 63) {
                    border = std::max(border, v);
                }
            }
        }
        if (border > 2 || total < 64 * 255) {
            test::fail(__FILE__, __LINE__,
                       std::string("punta ") + brushes::tipKey(tip) + ": borde " + std::to_string(border) +
                           ", total " + std::to_string(total));
        }
    }
    for (int g = 1; g < kBrushGrainCount; ++g) {
        const std::vector<uint8_t> image = brushtips::makeGrain(static_cast<BrushGrain>(g), 64);
        REQUIRE(image.size() == 64u * 64u);
        const auto [lo, hi] = std::minmax_element(image.begin(), image.end());
        CHECK(*hi - *lo > 100);   // tiene relieve
        // Se repite sin costura: el salto entre el último y el primer píxel de cada fila
        // es como el de dos vecinos cualquiera.
        long seam = 0;
        long inside = 0;
        for (int y = 0; y < 64; ++y) {
            seam += std::abs(image[static_cast<size_t>(y * 64 + 63)] - image[static_cast<size_t>(y * 64)]);
            inside += std::abs(image[static_cast<size_t>(y * 64 + 32)] - image[static_cast<size_t>(y * 64 + 31)]);
        }
        CHECK(seam <= inside * 3 + 64 * 8);
    }
}

// -----------------------------------------------------------------------------
// Recorrido del trazo
// -----------------------------------------------------------------------------

TEST_CASE(stroke_path_classic_matches_old_brush) {
    // El algoritmo de antes: un sello al empezar y luego uno cada 0,8 px, con la presión
    // interpolada; radio = presión × grosor (entre 1 y 200) y alfa = presión × flujo.
    struct Old {
        std::vector<Dab> dabs;
        float lastX, lastY, lastPressure, distance = 0.0f;
        void add(float x, float y, float p) {
            dabs.push_back({x, y, std::clamp(p * 10.0f, 1.0f, 200.0f), p * 0.5f, 0.0f});
        }
        void to(float x, float y, float p) {
            const float dx = x - lastX;
            const float dy = y - lastY;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length <= 0.0f) {
                lastPressure = p;
                return;
            }
            float t = 0.8f - distance;
            while (t <= length) {
                const float f = t / length;
                add(lastX + dx * f, lastY + dy * f, lastPressure + (p - lastPressure) * f);
                t += 0.8f;
            }
            distance = length - (t - 0.8f);
            lastX = x;
            lastY = y;
            lastPressure = p;
        }
    };
    const float points[][3] = {{12.0f, 8.0f, 0.6f}, {12.0f, 8.0f, 0.9f}, {30.5f, 14.2f, 1.0f},
                               {31.0f, 14.3f, 0.2f}, {18.0f, 40.0f, 0.7f}, {18.3f, 40.1f, 0.7f}};
    Old old{{}, points[0][0], points[0][1], points[0][2]};
    old.add(points[0][0], points[0][1], points[0][2]);
    StrokePath path;
    path.begin(brushes::basic(), pathSettings(10.0f, 0.5f), points[0][0], points[0][1], points[0][2]);
    for (size_t i = 1; i < std::size(points); ++i) {
        old.to(points[i][0], points[i][1], points[i][2]);
        path.moveTo(points[i][0], points[i][1], points[i][2]);
    }
    path.finish();
    const std::vector<Dab> dabs = allDabs(path);
    REQUIRE(dabs.size() == old.dabs.size());
    for (size_t i = 0; i < dabs.size(); ++i) {
        if (!sameDab(dabs[i], old.dabs[i], 2e-3f)) {
            test::fail(__FILE__, __LINE__, "sello " + std::to_string(i) + " distinto");
            break;
        }
    }
    CHECK(path.provisional().empty());
}

TEST_CASE(stroke_path_spacing_follows_size) {
    StrokePath path;
    path.begin(roundBrush(0.1f), pathSettings(20.0f), 0.0f, 0.0f, 1.0f);
    path.moveTo(100.0f, 0.0f, 1.0f);
    path.finish();
    const std::vector<Dab> dabs = allDabs(path);
    REQUIRE(dabs.size() == 26u);   // cada 4 px (0,1 × 40 px de diámetro)
    CHECK_NEAR(dabs[1].x - dabs[0].x, 4.0f, 1e-4);
    CHECK_NEAR(dabs.back().x, 100.0f, 1e-3);

    // Con poca presión el pincel adelgaza y los sellos se juntan.
    StrokePath light;
    light.begin(roundBrush(0.1f), pathSettings(20.0f), 0.0f, 0.0f, 0.5f);
    light.moveTo(100.0f, 0.0f, 0.5f);
    light.finish();
    const std::vector<Dab> thin = allDabs(light);
    CHECK_NEAR(thin[0].radius, 10.0f, 1e-4);
    CHECK_NEAR(thin[1].x - thin[0].x, 2.0f, 1e-4);

    // Los sellos diminutos no encogen más: se aclaran.
    StrokePath tiny;
    tiny.begin(roundBrush(0.1f), pathSettings(0.25f), 0.0f, 0.0f, 1.0f);
    tiny.finish();
    const std::vector<Dab> dot = allDabs(tiny);
    REQUIRE(dot.size() == 1u);
    CHECK_NEAR(dot[0].radius, 0.5f, 1e-6);
    CHECK_NEAR(dot[0].alpha, 0.25f, 1e-5);
}

TEST_CASE(stroke_path_tapers_with_provisional_end) {
    BrushParams p = roundBrush(0.05f);
    p.taperStart = 0.5f;
    p.taperEnd = 0.5f;
    p.taperTip = 0.1f;
    // Afinados de 0,5 × (40 + 16 × 10) = 100 px.
    StrokePath path;
    path.begin(p, pathSettings(10.0f), 0.0f, 0.0f, 1.0f);
    CHECK(path.tapered());
    std::vector<Dab> finals;
    for (int x = 10; x <= 300; x += 10) {
        path.moveTo(static_cast<float>(x), 0.0f, 1.0f);
        path.takeFinal(finals);
    }
    // Definitivos hasta 100 px antes del final; los provisionales, el resto.
    REQUIRE(!finals.empty());
    CHECK(finals.back().x <= 200.0f + 1e-3f);
    CHECK(finals.back().x > 195.0f);
    CHECK_NEAR(finals.front().radius, 1.0f, 1e-4);   // la punta
    const std::vector<Dab> provisional = path.provisional();
    REQUIRE(provisional.size() > 10u);
    CHECK(provisional.front().x > finals.back().x);
    CHECK_NEAR(provisional.back().x, 300.0f, 1.0);
    CHECK(provisional.back().radius < 1.5f);
    for (size_t i = 1; i < provisional.size(); ++i) {
        CHECK(provisional[i].radius <= provisional[i - 1].radius + 1e-4f);
    }
    // En medio, el tamaño entero.
    for (const Dab& dab : finals) {
        if (dab.x > 100.5f) {
            CHECK_NEAR(dab.radius, 10.0f, 1e-4);
        }
    }

    // Al levantar el lápiz sin moverse, los provisionales pasan tal cual a definitivos.
    path.finish();
    std::vector<Dab> rest;
    path.takeFinal(rest);
    CHECK(path.provisional().empty());
    REQUIRE(rest.size() == provisional.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        CHECK(sameDab(rest[i], provisional[i]));
    }
}

TEST_CASE(stroke_path_short_strokes_keep_their_size) {
    BrushParams p = roundBrush(0.05f);
    p.taperStart = 0.5f;
    p.taperEnd = 0.5f;
    p.taperTip = 0.0f;

    // Un toque deja un punto entero.
    StrokePath tap;
    tap.begin(p, pathSettings(10.0f), 50.0f, 50.0f, 1.0f);
    tap.finish();
    const std::vector<Dab> dot = allDabs(tap);
    REQUIRE(dot.size() == 1u);
    CHECK_NEAR(dot[0].radius, 10.0f, 1e-4);

    // Un trazo muy corto apenas se afina.
    StrokePath flick;
    flick.begin(p, pathSettings(10.0f), 0.0f, 0.0f, 1.0f);
    flick.moveTo(10.0f, 0.0f, 1.0f);
    flick.finish();
    const std::vector<Dab> dabs = allDabs(flick);
    REQUIRE(!dabs.empty());
    for (const Dab& dab : dabs) {
        CHECK(dab.radius >= 9.0f);
    }
}

TEST_CASE(stroke_path_streamline_smooths_and_reaches_the_end) {
    BrushParams p = roundBrush(0.1f);
    p.streamline = 0.5f;   // 18 puntos
    StrokePath path;
    path.begin(p, pathSettings(4.0f), 0.0f, 0.0f, 1.0f);
    CHECK_NEAR(path.pull(), 18.0f, 1e-4);
    // Zigzag de 6 px de amplitud: el trazo estabilizado casi no se entera.
    float raw = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    for (int i = 1; i <= 60; ++i) {
        const float x = static_cast<float>(i) * 4.0f;
        const float y = (i % 2 == 0) ? 0.0f : 6.0f;
        raw += std::hypot(x - lastX, y - lastY);
        lastX = x;
        lastY = y;
        path.moveTo(x, y, 1.0f);
    }
    std::vector<Dab> dabs;
    path.takeFinal(dabs);
    const float lengthBefore = path.length();
    CHECK(lengthBefore < raw * 0.75f);
    // Va por detrás del puntero...
    CHECK(dabs.back().x < 240.0f - 10.0f);
    float maxY = 0.0f;
    for (const Dab& dab : dabs) {
        if (dab.x > 40.0f) {
            maxY = std::max(maxY, std::fabs(dab.y - 3.0f));
        }
    }
    CHECK(maxY < 2.5f);
    // ...y al levantar el lápiz lo alcanza.
    path.finish();
    path.takeFinal(dabs);
    CHECK_NEAR(dabs.back().x, 240.0f, 0.5);
    CHECK_NEAR(dabs.back().y, 0.0f, 0.5);

    // Con el zoom alejado la distancia en el lienzo crece (se mide en la pantalla).
    StrokePath zoomed;
    StrokePath::Settings settings = pathSettings(4.0f);
    settings.pixelsPerPoint = 2.0f;
    zoomed.begin(p, settings, 0.0f, 0.0f, 1.0f);
    CHECK_NEAR(zoomed.pull(), 36.0f, 1e-4);
}

TEST_CASE(stroke_path_jitter_is_repeatable) {
    BrushParams p = roundBrush(0.2f);
    p.scatter = 0.5f;
    p.count = 3.0f;
    p.sizeJitter = 0.5f;
    p.opacityJitter = 0.5f;
    p.rotationJitter = 1.0f;
    auto run = [&](uint32_t seed) {
        StrokePath path;
        path.begin(p, pathSettings(10.0f, 1.0f, seed), 0.0f, 0.0f, 1.0f);
        path.moveTo(200.0f, 0.0f, 1.0f);
        path.finish();
        return allDabs(path);
    };
    const std::vector<Dab> a = run(1);
    const std::vector<Dab> b = run(1);
    const std::vector<Dab> c = run(2);
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() % 3 == 0);
    bool differs = false;
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(sameDab(a[i], b[i], 0.0f));
        differs = differs || !sameDab(a[i], c[i]);
        // Dispersión dentro de medio diámetro (0,5 × 20 px) y variación hacia abajo.
        CHECK(std::fabs(a[i].y) <= 10.0f + 1e-4f);
        CHECK(a[i].radius <= 10.0f + 1e-4f && a[i].radius >= 5.0f - 1e-4f);
        CHECK(a[i].alpha <= 1.0f && a[i].alpha >= 0.5f - 1e-4f);
        CHECK(std::fabs(a[i].angle) <= kPi + 1e-4f);
    }
    CHECK(differs);
}

TEST_CASE(stroke_path_follows_the_direction) {
    BrushParams p = roundBrush(0.1f);
    p.followStroke = true;
    p.angle = 90.0f;
    StrokePath path;
    path.begin(p, pathSettings(5.0f), 0.0f, 0.0f, 1.0f);
    // Sin saber hacia dónde va no hay sello.
    std::vector<Dab> dabs;
    path.takeFinal(dabs);
    CHECK(dabs.empty());
    path.moveTo(50.0f, 0.0f, 1.0f);    // a la derecha
    path.takeFinal(dabs);
    REQUIRE(!dabs.empty());
    CHECK_NEAR(dabs.front().angle, kPi * 0.5f, 1e-4);
    const size_t count = dabs.size();
    path.moveTo(50.0f, 80.0f, 1.0f);   // hacia abajo: gira poco a poco
    path.finish();
    path.takeFinal(dabs);
    REQUIRE(dabs.size() > count + 5);
    CHECK_NEAR(dabs.back().angle, kPi, 0.05);
    for (size_t i = count; i + 1 < dabs.size(); ++i) {
        CHECK(dabs[i + 1].angle >= dabs[i].angle - 1e-4f);
    }
}

// -----------------------------------------------------------------------------
// En la GPU
// -----------------------------------------------------------------------------

TEST_CASE(brush_uniform_does_not_build_up) {
    for (const BrushBuildUp mode : {BrushBuildUp::Uniform, BrushBuildUp::Glaze}) {
        Canvas canvas;
        REQUIRE(canvas.init(96, 48));
        canvas.layers().setVisible(0, false);
        BrushParams p = roundBrush(0.1f);
        p.flow = 0.5f;
        p.buildUp = mode;
        useBrush(canvas, p, 6.0f);
        // Ida y vuelta por la misma línea en un solo trazo.
        stroke(canvas, {{10.0f, 24.0f, 1.0f}, {86.0f, 24.0f, 1.0f}, {10.0f, 24.5f, 1.0f}});
        const int alpha = alphaAt(canvas, 1, 48, 24);
        if (mode == BrushBuildUp::Uniform) {
            CHECK_NEAR(alpha, 128, 3);   // lo que da un solo sello
        } else {
            CHECK(alpha > 240);          // decenas de sellos al 50 % encima
        }
    }
}

TEST_CASE(brush_hardness_shapes_the_round_tip) {
    for (const float hardness : {1.0f, 0.0f}) {
        Canvas canvas;
        REQUIRE(canvas.init(64, 64));
        canvas.layers().setVisible(0, false);
        BrushParams p = roundBrush(0.1f);
        p.hardness = hardness;
        p.buildUp = BrushBuildUp::Uniform;
        useBrush(canvas, p, 20.0f);
        stroke(canvas, {{32.0f, 32.0f, 1.0f}});   // un toque: un sello
        const std::vector<uint8_t> pixels = layerPixels(canvas, 1);
        const int center = test::pixelAt(pixels, 64, 32, 32)[3];
        const int inner = test::pixelAt(pixels, 64, 32 + 15, 32)[3];   // a 0,75 del radio
        const int outside = test::pixelAt(pixels, 64, 32 + 22, 32)[3];
        CHECK(center >= 250);
        CHECK_EQ(outside, 0);
        if (hardness >= 1.0f) {
            CHECK(inner >= 250);
            CHECK(test::pixelAt(pixels, 64, 32 + 19, 32)[3] >= 128);   // borde nítido
        } else {
            CHECK(inner < 60);
            CHECK(inner > 5);
        }
    }
}

TEST_CASE(brush_grain_is_fixed_to_the_canvas) {
    Canvas canvas;
    REQUIRE(canvas.init(96, 96));
    canvas.layers().setVisible(0, false);
    BrushParams p = roundBrush(0.05f);
    p.buildUp = BrushBuildUp::Uniform;
    p.grain = BrushGrain::Rough;
    p.grainDepth = 1.0f;
    useBrush(canvas, p, 14.0f);
    // Un trazo horizontal y, en otra capa, uno vertical: donde se cruzan el grano es el mismo.
    stroke(canvas, {{10.0f, 48.0f, 1.0f}, {86.0f, 48.0f, 1.0f}});
    REQUIRE(canvas.addLayer());
    stroke(canvas, {{48.0f, 10.0f, 1.0f}, {48.0f, 86.0f, 1.0f}});
    const std::vector<uint8_t> first = layerPixels(canvas, 1);
    const std::vector<uint8_t> second = layerPixels(canvas, 2);
    int lo = 255;
    int hi = 0;
    int worst = 0;
    for (int y = 40; y < 56; ++y) {
        for (int x = 40; x < 56; ++x) {
            const int a = test::pixelAt(first, 96, x, y)[3];
            const int b = test::pixelAt(second, 96, x, y)[3];
            lo = std::min(lo, a);
            hi = std::max(hi, a);
            worst = std::max(worst, std::abs(a - b));
        }
    }
    CHECK(hi - lo > 60);   // se nota el grano
    CHECK(worst <= 2);     // y es el mismo en los dos trazos
}

TEST_CASE(brush_taper_preview_matches_commit) {
    for (const bool erase : {false, true}) {
        Canvas canvas;
        REQUIRE(canvas.init(128, 64));
        if (erase) {
            test::fillRect(canvas.layers().at(1).target, IRect::ofSize(128, 64), 0.2f, 0.1f, 0.6f, 1.0f);
            canvas.layers().markDirty(IRect::ofSize(128, 64));
        }
        BrushParams p = roundBrush(0.05f);
        p.taperStart = 0.4f;
        p.taperEnd = 0.6f;
        p.taperTip = 0.05f;
        p.hardness = 0.8f;
        p.grain = BrushGrain::Paper;
        p.grainDepth = 0.5f;
        useBrush(canvas, p, 3.0f, 0.8f, erase);
        canvas.beginStroke(8.0f, 32.0f, 0.8f);
        for (int i = 1; i <= 24; ++i) {
            const float t = static_cast<float>(i) / 24.0f;
            canvas.strokeTo(8.0f + 110.0f * t, 32.0f + 18.0f * std::sin(t * 6.0f), 0.6f + 0.4f * t);
            canvas.update();   // como en la app: se ve a medias
        }
        canvas.update();
        const std::vector<uint8_t> preview = test::readTarget(canvas.composite());
        canvas.endStroke();
        canvas.update();
        const std::vector<uint8_t> committed = test::readTarget(canvas.composite());
        CHECK(test::maxDifference(preview, committed) <= 2);
        CHECK(test::maxDifference(preview, std::vector<uint8_t>(preview.size(), 0)) > 0);
    }
}

TEST_CASE(brush_every_preset_paints_and_erases) {
    Canvas canvas;
    REQUIRE(canvas.init(160, 64));
    canvas.layers().setVisible(0, false);
    const auto library = brushes::library();
    for (const BrushPreset& preset : library) {
        canvas.clearLayer(1);
        const float radius = brushes::radiusFor(preset.params, preset.params.size);
        useBrush(canvas, preset.params, std::min(radius, 20.0f));
        stroke(canvas, {{20.0f, 32.0f, 0.7f}, {80.0f, 28.0f, 1.0f}, {140.0f, 36.0f, 0.8f}});
        const std::vector<uint8_t> painted = layerPixels(canvas, 1);
        long sum = 0;
        for (size_t i = 3; i < painted.size(); i += 4) {
            sum += painted[i];
        }
        if (sum < 255 * 40) {
            test::fail(__FILE__, __LINE__, std::string("apenas pinta: ") + preset.id);
        }

        test::fillRect(canvas.layers().at(1).target, IRect::ofSize(160, 64), 0.0f, 0.0f, 0.0f, 1.0f);
        canvas.layers().markDirty(IRect::ofSize(160, 64));
        useBrush(canvas, preset.params, std::min(radius, 20.0f), 1.0f, true);
        stroke(canvas, {{20.0f, 32.0f, 0.7f}, {80.0f, 28.0f, 1.0f}, {140.0f, 36.0f, 0.8f}});
        const std::vector<uint8_t> erased = layerPixels(canvas, 1);
        long removed = 0;
        for (size_t i = 3; i < erased.size(); i += 4) {
            removed += 255 - erased[i];
        }
        if (removed < 255 * 40) {
            test::fail(__FILE__, __LINE__, std::string("apenas borra: ") + preset.id);
        }
    }
    CHECK(gfx::checkErrors("brush_every_preset_paints_and_erases"));
}
