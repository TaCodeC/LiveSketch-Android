// Mezcla húmeda y Difuminar: cómo arrastran lo que hay, cómo mezcla la pintura con lo que
// ya estaba (arrastre, carga y dilución), lo que limitan la selección y el alfa bloqueado,
// lo que se ve mientras se pinta, cancelar y deshacer.

#include "Test.h"

#include "Canvas/BrushLibrary.h"
#include "Canvas/Canvas.h"
#include "Canvas/SelectionShapes.h"

#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

namespace {

using test::Pixel;

constexpr Pixel kClear{0, 0, 0, 0};
constexpr Pixel kRed{255, 0, 0, 255};

std::vector<uint8_t> layerPixels(Canvas& canvas, int layer) {
    return test::readTarget(canvas.layers().at(layer).target);
}

std::vector<uint8_t> composite(Canvas& canvas) {
    canvas.update();
    return test::readTarget(canvas.composite());
}

Pixel at(const std::vector<uint8_t>& pixels, const Canvas& canvas, int x, int y) {
    return test::pixelAt(pixels, canvas.width(), x, y);
}

// Color sin premultiplicar y alfa, de 0 a 1, en la capa 1.
void fill(Canvas& canvas, const IRect& rect, float r, float g, float b, float a = 1.0f) {
    test::fillRect(canvas.layers().at(1).target, rect, r * a, g * a, b * a, a);
    canvas.layers().markDirty(rect);
}

BrushParams roundBrush(float hardness) {
    BrushParams p;
    p.tip = BrushTip::Round;
    p.hardness = hardness;
    p.spacing = 0.05f;
    p.minSpacing = 0.5f;
    p.pressureSize = 0.0f;
    p.pressureOpacity = 0.0f;
    p.flow = 1.0f;
    p.minRadius = 0.5f;
    p.maxRadius = 200.0f;
    return p;
}

// Difuminar con la fuerza `strength`. El color del pincel es azul: no tiene que aparecer.
void useSmudge(Canvas& canvas, const BrushParams& params, float radius, float strength) {
    BrushSettings& settings = canvas.brushSettings();
    settings.brush = params;
    settings.radius = radius;
    settings.opacity = strength;
    settings.eraser = false;
    settings.smudge = true;
    settings.color[0] = 0.0f;
    settings.color[1] = 0.0f;
    settings.color[2] = 1.0f;
}

void usePaint(Canvas& canvas, const BrushParams& params, float radius, float r, float g, float b,
              bool eraser = false) {
    BrushSettings& settings = canvas.brushSettings();
    settings.brush = params;
    settings.radius = radius;
    settings.opacity = 1.0f;
    settings.eraser = eraser;
    settings.smudge = false;
    settings.color[0] = r;
    settings.color[1] = g;
    settings.color[2] = b;
}

// Trazo recto con un punto cada 2 px, como llegan del lápiz.
void stroke(Canvas& canvas, glm::vec2 from, glm::vec2 to) {
    const float length = std::hypot(to.x - from.x, to.y - from.y);
    const int steps = std::max(1, static_cast<int>(std::ceil(length / 2.0f)));
    canvas.beginStroke(from.x, from.y, 1.0f);
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        canvas.strokeTo(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, 1.0f);
    }
    canvas.endStroke();
    canvas.update();
}

bool selectRect(Canvas& canvas, const IRect& rect) {
    const std::vector<glm::vec2> polygon = selection::rectangle(
        {static_cast<float>(rect.x0), static_cast<float>(rect.y0)}, {static_cast<float>(rect.x1), static_cast<float>(rect.y1)});
    return canvas.selectPolygon(polygon, SelectOp::Replace);
}

// Píxeles de `rect` que cambiaron entre dos lecturas de la misma capa.
int changedIn(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const Canvas& canvas, const IRect& rect) {
    int changed = 0;
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            changed += at(a, canvas, x, y) == at(b, canvas, x, y) ? 0 : 1;
        }
    }
    return changed;
}

long alphaIn(const std::vector<uint8_t>& pixels, const Canvas& canvas, const IRect& rect) {
    long sum = 0;
    for (int y = rect.y0; y < rect.y1; ++y) {
        for (int x = rect.x0; x < rect.x1; ++x) {
            sum += at(pixels, canvas, x, y)[3];
        }
    }
    return sum;
}

} // namespace

// -----------------------------------------------------------------------------
// Ajustes
// -----------------------------------------------------------------------------

TEST_CASE(wet_brush_settings) {
    BrushParams p = roundBrush(1.0f);
    CHECK(!brushes::isWet(p));
    CHECK(!brushes::isWet(brushes::basic()));
    p.wetPull = 0.2f;
    CHECK(brushes::isWet(p));
    p.wetPull = 0.0f;
    p.wetCharge = 0.9f;
    CHECK(brushes::isWet(p));
    p.wetCharge = 1.0f;
    p.wetDilution = 0.1f;
    CHECK(brushes::isWet(p));

    // Con la carga completa no se acaba; la dilución la rebaja (con toda el agua, un 5 %).
    p.wetDilution = 0.0f;
    CHECK_NEAR(brushes::wetPaint(p, 10.0f, 0.0f), 1.0f, 1e-6);
    CHECK_NEAR(brushes::wetPaint(p, 10.0f, 5000.0f), 1.0f, 1e-6);
    p.wetDilution = 1.0f;
    CHECK_NEAR(brushes::wetPaint(p, 10.0f, 0.0f), 0.05f, 1e-6);
    p.wetDilution = 0.0f;
    // Con poca carga se va acabando, y más despacio con un pincel más grande.
    p.wetCharge = 0.5f;
    CHECK_NEAR(brushes::wetPaint(p, 10.0f, 0.0f), 1.0f, 1e-6);
    const float middle = brushes::wetPaint(p, 10.0f, 100.0f);
    const float end = brushes::wetPaint(p, 10.0f, 400.0f);
    CHECK(middle < 1.0f);
    CHECK(end < middle);
    CHECK(end > 0.0f);
    CHECK(brushes::wetPaint(p, 20.0f, 400.0f) > end);
    p.wetCharge = 0.0f;
    CHECK(brushes::wetPaint(p, 10.0f, 20.0f) < 0.4f);   // un tercio en un diámetro

    // Van al archivo de ajustes como los demás.
    const BrushParams base = roundBrush(1.0f);
    BrushParams changed = base;
    changed.wetPull = 0.4f;
    changed.wetCharge = 0.6f;
    changed.wetDilution = 0.25f;
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
    CHECK_EQ(count, 3);
    CHECK(loaded == changed);

    // Los de fábrica: los húmedos lo son, y Difuminar tiene su pincel.
    for (const char* id : {"oleo-humedo", "acuarela-humeda", "gouache-humedo"}) {
        const int index = brushes::indexOf(id);
        REQUIRE(index >= 0);
        CHECK(brushes::isWet(brushes::library()[static_cast<size_t>(index)].params));
    }
    CHECK(brushes::indexOf(brushes::kDefaultSmudge) >= 0);
    CHECK(!brushes::isWet(brushes::library()[static_cast<size_t>(brushes::indexOf(brushes::kDefaultPaint))].params));
}

// -----------------------------------------------------------------------------
// Difuminar
// -----------------------------------------------------------------------------

TEST_CASE(wet_smudge_drags_paint_along_the_stroke) {
    Canvas canvas;
    REQUIRE(canvas.init(128, 64));
    fill(canvas, {8, 16, 40, 48}, 1.0f, 0.0f, 0.0f);
    const std::vector<uint8_t> before = layerPixels(canvas, 1);
    useSmudge(canvas, roundBrush(0.5f), 8.0f, 0.8f);
    stroke(canvas, {24.0f, 32.0f}, {100.0f, 32.0f});
    const std::vector<uint8_t> after = layerPixels(canvas, 1);

    // El rojo sigue al trazo más allá del bloque, cada vez menos y sin oscurecerse.
    const Pixel close = at(after, canvas, 46, 32);
    const Pixel far = at(after, canvas, 64, 32);
    CHECK(close[3] > 120);
    CHECK(far[3] > 10);
    CHECK(far[3] < close[3]);
    for (const Pixel& p : {close, far}) {
        CHECK(std::abs(p[0] - p[3]) <= 1);   // rojo puro: premultiplicado, r = a
        CHECK(p[1] <= 1);
        CHECK(p[2] <= 1);
    }
    // Fuera del trazo nada cambia, y el color del pincel no aparece en ningún sitio.
    CHECK(at(after, canvas, 64, 8) == kClear);
    CHECK(at(after, canvas, 10, 18) == kRed);
    int blue = 0;
    for (size_t i = 2; i < after.size(); i += 4) {
        blue = std::max(blue, static_cast<int>(after[i]));
    }
    CHECK(blue <= 1);

    // Un solo paso de deshacer.
    CHECK_EQ(canvas.history().undoCount(), 1);
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), before), 0);
    REQUIRE(canvas.redo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), after), 0);
    CHECK(gfx::checkErrors("wet_smudge_drags_paint_along_the_stroke"));
}

TEST_CASE(wet_smudge_leaves_flat_color_alone) {
    // Arrastrar un color liso no lo cambia: ni con toda la fuerza ni sobre transparente.
    Canvas canvas;
    REQUIRE(canvas.init(96, 48));
    fill(canvas, {0, 0, 48, 48}, 0.3f, 0.6f, 0.9f, 0.8f);
    const std::vector<uint8_t> before = layerPixels(canvas, 1);
    for (const float hardness : {1.0f, 0.0f}) {
        useSmudge(canvas, roundBrush(hardness), 10.0f, 1.0f);
        stroke(canvas, {12.0f, 14.0f}, {36.0f, 34.0f});
        stroke(canvas, {62.0f, 12.0f}, {86.0f, 36.0f});
    }
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), before), 0);
}

TEST_CASE(wet_smudge_strength_sets_how_far_it_drags) {
    const float strengths[2] = {0.3f, 0.9f};
    long carried[2] = {};
    for (int i = 0; i < 2; ++i) {
        Canvas canvas;
        REQUIRE(canvas.init(128, 64));
        fill(canvas, {8, 16, 40, 48}, 1.0f, 0.0f, 0.0f);
        useSmudge(canvas, roundBrush(0.5f), 8.0f, strengths[i]);
        stroke(canvas, {24.0f, 32.0f}, {100.0f, 32.0f});
        carried[i] = alphaIn(layerPixels(canvas, 1), canvas, {41, 16, 128, 48});
    }
    CHECK(carried[0] > 0);
    CHECK(carried[1] > 3 * carried[0]);
}

TEST_CASE(wet_eraser_erases_instead_of_mixing) {
    Canvas canvas;
    REQUIRE(canvas.init(96, 48));
    fill(canvas, {0, 0, 96, 48}, 0.2f, 0.4f, 0.6f);
    // La goma del lápiz borra aunque esté Difuminar.
    useSmudge(canvas, roundBrush(1.0f), 6.0f, 1.0f);
    canvas.beginStroke(10.0f, 16.0f, 1.0f, true);
    canvas.strokeTo(86.0f, 16.0f, 1.0f);
    canvas.endStroke();
    // Un pincel húmedo en el borrador borra como uno normal.
    BrushParams wet = roundBrush(1.0f);
    wet.wetPull = 0.8f;
    wet.wetCharge = 0.2f;
    usePaint(canvas, wet, 6.0f, 1.0f, 0.0f, 0.0f, true);
    stroke(canvas, {10.0f, 34.0f}, {86.0f, 34.0f});
    const std::vector<uint8_t> pixels = layerPixels(canvas, 1);
    CHECK(at(pixels, canvas, 48, 16)[3] <= 1);
    CHECK(at(pixels, canvas, 80, 34)[3] <= 1);   // la carga no se acaba al borrar
    CHECK(at(pixels, canvas, 48, 25) == (Pixel{51, 102, 153, 255}));
}

// -----------------------------------------------------------------------------
// Pinceles húmedos
// -----------------------------------------------------------------------------

TEST_CASE(wet_brush_mixes_with_the_paint_underneath) {
    // Rojo en la mitad izquierda; un trazo azul que lo cruza y sigue por lo transparente.
    auto paint = [](float pull) {
        Canvas canvas;
        canvas.init(128, 64);
        fill(canvas, {0, 0, 64, 64}, 1.0f, 0.0f, 0.0f);
        BrushParams p = roundBrush(0.8f);
        p.wetPull = pull;
        p.wetCharge = 0.999f;   // húmedo aunque no arrastre
        usePaint(canvas, p, 8.0f, 0.0f, 0.0f, 1.0f);
        stroke(canvas, {8.0f, 32.0f}, {120.0f, 32.0f});
        return layerPixels(canvas, 1);
    };
    const std::vector<uint8_t> dry = paint(0.0f);
    const std::vector<uint8_t> wet = paint(0.6f);
    auto pixel = [](const std::vector<uint8_t>& pixels, int x, int y) { return test::pixelAt(pixels, 128, x, y); };

    // Sin arrastre tapa el rojo; con arrastre lo mezcla con su color.
    CHECK(pixel(dry, 30, 32)[0] <= 3);
    CHECK(pixel(dry, 30, 32)[2] >= 250);
    CHECK(pixel(wet, 30, 32)[0] > 60);
    CHECK(pixel(wet, 30, 32)[2] > 60);
    CHECK_EQ(pixel(wet, 30, 32)[3], 255);
    // Y se lleva el rojo más allá de donde estaba.
    CHECK(pixel(wet, 70, 32)[0] > pixel(dry, 70, 32)[0] + 20);
    // Sobre lo transparente pinta como siempre: la pintura del propio trazo no cuenta.
    CHECK(pixel(wet, 110, 32)[3] > 230);
    CHECK(pixel(wet, 110, 32)[2] > 200);
    // Lejos del trazo, nada.
    CHECK(pixel(wet, 110, 8) == kClear);
    CHECK(pixel(wet, 20, 8) == kRed);
}

TEST_CASE(wet_dilution_and_charge_lighten_the_stroke) {
    auto paint = [](const BrushParams& p, float fromX, float toX) {
        Canvas canvas;
        canvas.init(160, 48);
        usePaint(canvas, p, 8.0f, 0.1f, 0.2f, 0.8f);
        stroke(canvas, {fromX, 24.0f}, {toX, 24.0f});
        return layerPixels(canvas, 1);
    };
    auto alpha = [](const std::vector<uint8_t>& pixels, int x) { return test::pixelAt(pixels, 160, x, 24)[3]; };

    // Una pasada deja la parte de la pintura que no es agua, con los sellos juntos o
    // separados: 1 - 0,95 × 0,5 = 0,525.
    for (const float spacing : {0.02f, 0.25f}) {
        BrushParams p = roundBrush(0.9f);
        p.spacing = spacing;
        p.wetDilution = 0.5f;
        CHECK_NEAR(alpha(paint(p, 16.0f, 144.0f), 80), 134, 12);
    }

    // Con cualquier punta: con cerdas (muchos huecos), la mitad de lo que pinta la normal.
    {
        BrushParams p = roundBrush(1.0f);
        p.tip = BrushTip::Bristle;
        p.followStroke = true;
        const std::vector<uint8_t> normal = paint(p, 16.0f, 144.0f);
        p.wetDilution = 0.5f;
        const std::vector<uint8_t> diluted = paint(p, 16.0f, 144.0f);
        long sumNormal = 0;
        long sumDiluted = 0;
        for (size_t i = 3; i < normal.size(); i += 4) {
            sumNormal += normal[i];
            sumDiluted += diluted[i];
        }
        REQUIRE(sumNormal > 255 * 200);
        const double ratio = static_cast<double>(sumDiluted) / static_cast<double>(sumNormal);
        CHECK_NEAR(ratio, 0.525, 0.06);
    }

    // Con poca carga, se va acabando por el camino.
    BrushParams p = roundBrush(0.9f);
    p.wetCharge = 0.3f;
    const std::vector<uint8_t> charged = paint(p, 16.0f, 144.0f);
    CHECK(alpha(charged, 24) > 200);
    CHECK(alpha(charged, 136) < 110);
    CHECK(alpha(charged, 136) > 20);
}

TEST_CASE(wet_mix_respects_selection) {
    Canvas canvas;
    REQUIRE(canvas.init(128, 64));
    fill(canvas, {8, 16, 40, 48}, 1.0f, 0.0f, 0.0f);
    REQUIRE(selectRect(canvas, {0, 0, 128, 32}));   // solo la mitad de arriba
    const std::vector<uint8_t> before = layerPixels(canvas, 1);
    useSmudge(canvas, roundBrush(0.5f), 8.0f, 0.9f);
    stroke(canvas, {24.0f, 32.0f}, {100.0f, 32.0f});
    BrushParams wet = roundBrush(0.7f);
    wet.wetPull = 0.5f;
    wet.wetDilution = 0.2f;
    usePaint(canvas, wet, 6.0f, 0.0f, 0.8f, 0.2f);
    stroke(canvas, {10.0f, 44.0f}, {120.0f, 20.0f});
    const std::vector<uint8_t> after = layerPixels(canvas, 1);

    CHECK_EQ(changedIn(before, after, canvas, {0, 32, 128, 64}), 0);
    CHECK(changedIn(before, after, canvas, {0, 0, 128, 32}) > 100);
    CHECK(at(after, canvas, 50, 28)[0] > 30);   // el rojo arrastrado, dentro
}

TEST_CASE(wet_mix_respects_alpha_lock) {
    Canvas canvas;
    REQUIRE(canvas.init(96, 48));
    fill(canvas, {16, 8, 48, 40}, 1.0f, 0.0f, 0.0f, 0.5f);   // rojo a medias
    fill(canvas, {48, 8, 64, 40}, 0.0f, 1.0f, 0.0f);         // verde opaco
    canvas.setLayerAlphaLock(1, true);
    const std::vector<uint8_t> before = layerPixels(canvas, 1);

    BrushParams wet = roundBrush(0.8f);
    wet.wetPull = 0.5f;
    usePaint(canvas, wet, 8.0f, 0.0f, 0.0f, 1.0f);
    stroke(canvas, {4.0f, 24.0f}, {92.0f, 24.0f});
    useSmudge(canvas, roundBrush(0.5f), 8.0f, 1.0f);
    stroke(canvas, {60.0f, 30.0f}, {20.0f, 30.0f});
    const std::vector<uint8_t> after = layerPixels(canvas, 1);

    // El alfa no cambia en ningún píxel; el color sí, donde había pintura.
    int alphaChanges = 0;
    for (size_t i = 3; i < after.size(); i += 4) {
        alphaChanges += after[i] != before[i] ? 1 : 0;
    }
    CHECK_EQ(alphaChanges, 0);
    CHECK(at(after, canvas, 32, 22)[2] > 20);   // azul sobre el rojo
    CHECK(at(after, canvas, 56, 22)[2] > 20);   // y sobre el verde
    CHECK(at(after, canvas, 80, 24) == kClear); // donde no había nada, nada
}

// -----------------------------------------------------------------------------
// Mientras se pinta
// -----------------------------------------------------------------------------

TEST_CASE(wet_preview_matches_commit) {
    for (const bool tapered : {false, true}) {
        for (const bool smudge : {false, true}) {
            Canvas canvas;
            REQUIRE(canvas.init(128, 64));
            // Franjas por toda la capa: fuera de la copia de trabajo se tiene que ver la capa.
            for (int x = 0; x < 128; x += 16) {
                fill(canvas, {x, 0, x + 8, 64}, 0.2f, 0.5f, 0.9f);
            }
            const std::vector<uint8_t> original = composite(canvas);
            BrushParams p = roundBrush(0.7f);
            p.wetPull = 0.5f;
            p.wetCharge = 0.6f;
            p.wetDilution = 0.2f;
            p.grain = BrushGrain::Paper;
            p.grainDepth = 0.4f;
            if (tapered) {
                p.taperStart = 0.3f;
                p.taperEnd = 0.6f;
                p.taperTip = 0.1f;
            }
            if (smudge) {
                useSmudge(canvas, p, 5.0f, 0.8f);
            } else {
                usePaint(canvas, p, 5.0f, 0.9f, 0.3f, 0.1f);
            }
            canvas.beginStroke(8.0f, 32.0f, 0.8f);
            for (int i = 1; i <= 30; ++i) {
                const float t = static_cast<float>(i) / 30.0f;
                canvas.strokeTo(8.0f + 110.0f * t, 32.0f + 18.0f * std::sin(t * 6.0f), 0.6f + 0.4f * t);
                canvas.update();   // como en la app: se ve a medias
            }
            const std::vector<uint8_t> preview = composite(canvas);
            canvas.endStroke();
            const std::vector<uint8_t> committed = composite(canvas);
            const int difference = test::maxDifference(preview, committed);
            if (difference > 2) {
                test::fail(__FILE__, __LINE__,
                           std::string("la vista no es lo que queda (") + (tapered ? "afinado" : "sin afinar") +
                               (smudge ? ", difuminar): " : ", húmedo): ") + std::to_string(difference));
            }
            CHECK(test::maxDifference(committed, original) > 30);
        }
    }
    CHECK(gfx::checkErrors("wet_preview_matches_commit"));
}

TEST_CASE(wet_cancel_leaves_everything_as_it_was) {
    // Un trazo normal afinado (usa el segundo buffer) en un lienzo nuevo.
    auto normalStroke = [](Canvas& canvas) {
        BrushParams p = roundBrush(0.8f);
        p.taperStart = 0.3f;
        p.taperEnd = 0.5f;
        p.taperTip = 0.2f;
        usePaint(canvas, p, 6.0f, 0.1f, 0.8f, 0.3f);
        stroke(canvas, {10.0f, 20.0f}, {110.0f, 44.0f});
    };
    Canvas fresh;
    REQUIRE(fresh.init(128, 64));
    normalStroke(fresh);
    const std::vector<uint8_t> expected = layerPixels(fresh, 1);

    Canvas canvas;
    REQUIRE(canvas.init(128, 64));
    fill(canvas, {0, 0, 64, 64}, 1.0f, 0.0f, 0.0f);
    const std::vector<uint8_t> original = layerPixels(canvas, 1);
    const std::vector<uint8_t> shown = composite(canvas);
    BrushParams wet = roundBrush(0.6f);
    wet.wetPull = 0.5f;
    wet.wetDilution = 0.3f;
    wet.taperEnd = 0.5f;
    usePaint(canvas, wet, 8.0f, 0.0f, 0.0f, 1.0f);
    canvas.beginStroke(10.0f, 30.0f, 1.0f);
    for (int i = 1; i <= 20; ++i) {
        canvas.strokeTo(10.0f + 5.0f * static_cast<float>(i), 30.0f, 1.0f);
        canvas.update();
    }
    CHECK(test::maxDifference(composite(canvas), shown) > 30);   // se ve mientras se pinta
    canvas.cancelStroke();
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);
    CHECK_EQ(test::maxDifference(composite(canvas), shown), 0);
    CHECK_EQ(canvas.history().undoCount(), 0);

    // Uno terminado tampoco deja nada en los buffers de trazo: después, un trazo normal
    // sale igual que en un lienzo nuevo.
    stroke(canvas, {10.0f, 50.0f}, {110.0f, 50.0f});
    CHECK_EQ(canvas.history().undoCount(), 1);
    REQUIRE(canvas.addLayer());
    normalStroke(canvas);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 2), expected), 0);
    CHECK(gfx::checkErrors("wet_cancel_leaves_everything_as_it_was"));
}
