// Guía de dibujo y forma rápida en el lienzo: las copias de cada simetría, las líneas de la
// guía, los trazos repetidos (también húmedos) que se deshacen de una vez y el trazo que
// pasa a ser una forma sin soltarlo.

#include "Test.h"

#include "Canvas/Canvas.h"
#include "Canvas/DrawingGuide.h"
#include "Canvas/LayerStack.h"
#include "Canvas/QuickShape.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {

using test::Pixel;

constexpr float kPi = 3.14159265358979f;
constexpr Pixel kClear{0, 0, 0, 0};
constexpr Pixel kRed{255, 0, 0, 255};
constexpr Pixel kWhite{255, 255, 255, 255};

std::vector<uint8_t> layerPixels(Canvas& canvas, int layer = 1) {
    return test::readTarget(canvas.layers().at(layer).target);
}

std::vector<uint8_t> composite(Canvas& canvas) {
    canvas.update();
    return test::readTarget(canvas.composite());
}

Pixel at(const std::vector<uint8_t>& pixels, const Canvas& canvas, int x, int y) {
    return test::pixelAt(pixels, canvas.width(), x, y);
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

BrushParams hardBrush() {
    BrushParams p;
    p.tip = BrushTip::Round;
    p.hardness = 1.0f;
    p.spacing = 0.05f;
    p.minSpacing = 0.5f;
    p.pressureSize = 0.0f;
    p.pressureOpacity = 0.0f;
    p.flow = 1.0f;
    p.minRadius = 0.5f;
    p.maxRadius = 200.0f;
    return p;
}

void usePaint(Canvas& canvas, const BrushParams& params, float radius) {
    BrushSettings& settings = canvas.brushSettings();
    settings.brush = params;
    settings.radius = radius;
    settings.opacity = 1.0f;
    settings.eraser = false;
    settings.smudge = false;
    settings.color[0] = 1.0f;
    settings.color[1] = 0.0f;
    settings.color[2] = 0.0f;
}

// Puntos cada 2 px de `from` a `to` (con `to`).
std::vector<glm::vec2> points(glm::vec2 from, glm::vec2 to) {
    std::vector<glm::vec2> out;
    const int steps = std::max(1, static_cast<int>(std::ceil(glm::distance(from, to) / 2.0f)));
    for (int i = 0; i <= steps; ++i) {
        out.push_back(from + (to - from) * (static_cast<float>(i) / static_cast<float>(steps)));
    }
    return out;
}

// Empieza un trazo por `path`, sin terminarlo.
void trace(Canvas& canvas, const std::vector<glm::vec2>& path) {
    canvas.beginStroke(path.front().x, path.front().y, 1.0f);
    for (size_t i = 1; i < path.size(); ++i) {
        canvas.strokeTo(path[i].x, path[i].y, 1.0f);
        if (i % 8 == 0) {
            canvas.update();   // como al dibujar: se pinta por tandas
        }
    }
    canvas.update();
}

void stroke(Canvas& canvas, glm::vec2 from, glm::vec2 to) {
    trace(canvas, points(from, to));
    canvas.endStroke();
    canvas.update();
}

DrawingGuide symmetry(glm::vec2 center, SymmetryKind kind, bool rotational = false, float angle = 0.0f) {
    DrawingGuide g;
    g.enabled = true;
    g.kind = GuideKind::Symmetry;
    g.symmetry = kind;
    g.center = center;
    g.rotational = rotational;
    g.angle = angle;
    return g;
}

bool sameVec(glm::vec2 a, glm::vec2 b, float tolerance = 1e-4f) { return glm::distance(a, b) <= tolerance; }

// Un trazo casi recto de `from` a `to` con una joroba de `bump` px en medio (hacia abajo).
std::vector<glm::vec2> bumpyLine(glm::vec2 from, glm::vec2 to, float bump) {
    std::vector<glm::vec2> out = points(from, to);
    const glm::vec2 d = glm::normalize(to - from);
    const glm::vec2 normal(-d.y, d.x);
    for (size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(out.size() - 1);
        out[i] += normal * (bump * std::sin(t * kPi));
    }
    return out;
}

} // namespace

// -----------------------------------------------------------------------------
// Sin GL
// -----------------------------------------------------------------------------

TEST_CASE(guide_copies_of_each_symmetry) {
    DrawingGuide g = symmetry({0.0f, 0.0f}, SymmetryKind::Vertical);
    g.enabled = false;
    CHECK_EQ(guide::copies(g).size(), size_t{1});
    g.enabled = true;
    g.kind = GuideKind::Grid;
    CHECK_EQ(guide::copies(g).size(), size_t{1});   // la cuadrícula no repite los trazos
    CHECK(!g.mirrors());

    const std::pair<SymmetryKind, size_t> counts[] = {
        {SymmetryKind::Vertical, 2}, {SymmetryKind::Horizontal, 2}, {SymmetryKind::Quadrant, 4}, {SymmetryKind::Radial, 8}};
    for (const auto& [kind, count] : counts) {
        for (const bool rotational : {false, true}) {
            const std::vector<guide::Copy> copies = guide::copies(symmetry({10.0f, 10.0f}, kind, rotational));
            CHECK_EQ(copies.size(), count);
            // La primera es el trazo mismo.
            CHECK(!copies.front().mirrored);
            CHECK(sameVec(guide::transform(glm::vec2(3.0f, 7.0f), copies.front(), {10.0f, 10.0f}), {3.0f, 7.0f}));
            // Giradas: ninguna refleja.
            if (rotational) {
                CHECK(std::none_of(copies.begin(), copies.end(), [](const guide::Copy& c) { return c.mirrored; }));
            }
        }
    }
}

TEST_CASE(guide_mirrors_across_its_axes) {
    const glm::vec2 c(100.0f, 50.0f);
    const glm::vec2 p(130.0f, 20.0f);
    // Vertical: al otro lado del eje vertical.
    std::vector<guide::Copy> copies = guide::copies(symmetry(c, SymmetryKind::Vertical));
    CHECK(copies[1].mirrored);
    CHECK(sameVec(guide::transform(p, copies[1], c), {70.0f, 20.0f}));
    // La punta del pincel se ve reflejada: un sello girado 0,3 rad sale girado π − 0,3.
    Dab dab;
    dab.x = p.x;
    dab.y = p.y;
    dab.angle = 0.3f;
    const Dab mirrored = guide::transform(dab, copies[1], c);
    CHECK_NEAR(mirrored.x, 70.0f, 1e-4f);
    CHECK_NEAR(std::remainder(mirrored.angle - (kPi - 0.3f), 2.0f * kPi), 0.0f, 1e-5f);

    // Horizontal: arriba y abajo.
    copies = guide::copies(symmetry(c, SymmetryKind::Horizontal));
    CHECK(sameVec(guide::transform(p, copies[1], c), {130.0f, 80.0f}));
    // Vertical girada 90°: el eje queda horizontal.
    copies = guide::copies(symmetry(c, SymmetryKind::Vertical, false, kPi * 0.5f));
    CHECK(sameVec(guide::transform(p, copies[1], c), {130.0f, 80.0f}));
    // Vertical rotacional: media vuelta.
    copies = guide::copies(symmetry(c, SymmetryKind::Vertical, true));
    CHECK(!copies[1].mirrored);
    CHECK(sameVec(guide::transform(p, copies[1], c), {70.0f, 80.0f}));
    const Dab turned = guide::transform(dab, copies[1], c);
    CHECK_NEAR(std::remainder(turned.angle - (0.3f + kPi), 2.0f * kPi), 0.0f, 1e-5f);

    // Cuadrante: los cuatro cuadrantes.
    copies = guide::copies(symmetry(c, SymmetryKind::Quadrant));
    std::vector<glm::vec2> quadrant;
    for (const guide::Copy& copy : copies) {
        quadrant.push_back(guide::transform(p, copy, c));
    }
    for (const glm::vec2 expected : {glm::vec2(130.0f, 20.0f), glm::vec2(70.0f, 20.0f), glm::vec2(130.0f, 80.0f),
                                     glm::vec2(70.0f, 80.0f)}) {
        CHECK(std::any_of(quadrant.begin(), quadrant.end(), [&](glm::vec2 q) { return sameVec(q, expected); }));
    }
}

TEST_CASE(guide_radial_copies_go_all_around) {
    const glm::vec2 c(100.0f, 100.0f);
    const glm::vec2 p(150.0f, 110.0f);
    for (const bool rotational : {false, true}) {
        const std::vector<guide::Copy> copies = guide::copies(symmetry(c, SymmetryKind::Radial, rotational));
        // Todas a la misma distancia del centro y en puntos distintos.
        std::vector<glm::vec2> out;
        for (const guide::Copy& copy : copies) {
            const glm::vec2 q = guide::transform(p, copy, c);
            CHECK_NEAR(glm::distance(q, c), glm::distance(p, c), 1e-3f);
            for (const glm::vec2& other : out) {
                CHECK(glm::distance(q, other) > 1.0f);
            }
            out.push_back(q);
        }
        // Las que giran, de 90 en 90° (sin reflejar) o de 45 en 45° (rotacional).
        const glm::vec2 quarter = c + glm::vec2(-(p.y - c.y), p.x - c.x);
        CHECK(std::any_of(out.begin(), out.end(), [&](glm::vec2 q) { return sameVec(q, quarter, 1e-3f); }));
    }
}

TEST_CASE(guide_lines_cover_the_canvas) {
    const glm::vec2 size(400.0f, 300.0f);
    DrawingGuide grid = guide::defaults(size);
    CHECK(!grid.enabled);
    CHECK(sameVec(grid.center, {200.0f, 150.0f}));
    grid.enabled = true;
    grid.gridSize = 100.0f;
    std::vector<guide::Segment> lines = guide::lines(grid, size);
    // Verticales en x = 0, 100, 200, 300 y 400; horizontales en y = 50, 150 y 250.
    CHECK_EQ(lines.size(), size_t{8});
    int vertical = 0;
    for (const guide::Segment& s : lines) {
        if (std::fabs(s.a.x - s.b.x) < 1e-3f) {
            ++vertical;
            CHECK_NEAR(std::fmod(s.a.x, 100.0f), 0.0f, 1e-3f);
            CHECK_NEAR(std::min(s.a.y, s.b.y), 0.0f, 1e-3f);
            CHECK_NEAR(std::max(s.a.y, s.b.y), 300.0f, 1e-3f);
        } else {
            CHECK_NEAR(std::fmod(s.a.y - 50.0f, 100.0f), 0.0f, 1e-3f);
        }
    }
    CHECK_EQ(vertical, 5);
    // Muy juntas en la pantalla: una de cada dos.
    lines = guide::lines(grid, size, 150.0f);
    CHECK_EQ(lines.size(), size_t{4});
    // Girada: siempre dentro del lienzo.
    grid.angle = 0.4f;
    for (const guide::Segment& s : guide::lines(grid, size)) {
        for (const glm::vec2 q : {s.a, s.b}) {
            CHECK(q.x >= -1e-3f && q.x <= 400.001f && q.y >= -1e-3f && q.y <= 300.001f);
        }
    }

    // Simetría: sus ejes, de borde a borde.
    lines = guide::lines(symmetry({200.0f, 150.0f}, SymmetryKind::Vertical), size);
    REQUIRE(lines.size() == 1);
    CHECK_NEAR(lines[0].a.x, 200.0f, 1e-3f);
    CHECK_NEAR(std::fabs(lines[0].a.y - lines[0].b.y), 300.0f, 1e-3f);
    CHECK_EQ(guide::lines(symmetry({200.0f, 150.0f}, SymmetryKind::Quadrant), size).size(), size_t{2});
    CHECK_EQ(guide::lines(symmetry({200.0f, 150.0f}, SymmetryKind::Radial), size).size(), size_t{4});
}

TEST_CASE(layers_dirty_regions_stay_apart) {
    LayerStack stack;
    stack.reset(400, 400);
    stack.insert(0, "Fondo");
    stack.clearDirty();
    // Dos zonas lejanas: dos regiones, y la caja que las cubre.
    stack.markDirty({10, 10, 30, 30});
    stack.markDirty({300, 300, 320, 320});
    CHECK_EQ(stack.dirtyRects().size(), size_t{2});
    CHECK(stack.dirty() == (IRect{10, 10, 320, 320}));
    // Una cerca de la primera se junta con ella.
    stack.markDirty({35, 12, 50, 28});
    CHECK_EQ(stack.dirtyRects().size(), size_t{2});
    // Muchas: no pasan del máximo y siguen cubriendo todo lo marcado.
    stack.clearDirty();
    std::vector<IRect> marked;
    for (int i = 0; i < 40; ++i) {
        const IRect r{(i % 8) * 50, (i / 8) * 80, (i % 8) * 50 + 5, (i / 8) * 80 + 5};
        stack.markDirty(r);
        marked.push_back(r);
    }
    CHECK(stack.dirtyRects().size() <= LayerStack::kMaxDirtyRects);
    for (const IRect& r : marked) {
        const bool covered = std::any_of(stack.dirtyRects().begin(), stack.dirtyRects().end(), [&](const IRect& d) {
            return d.x0 <= r.x0 && d.y0 <= r.y0 && d.x1 >= r.x1 && d.y1 >= r.y1;
        });
        CHECK(covered);
    }
    stack.clearDirty();
    CHECK(stack.dirtyRects().empty());
}

// -----------------------------------------------------------------------------
// Con GL
// -----------------------------------------------------------------------------

TEST_CASE(symmetry_repeats_the_stroke_and_undoes_it_at_once) {
    Canvas canvas;
    REQUIRE(canvas.init(240, 100));
    usePaint(canvas, hardBrush(), 4.0f);
    canvas.setGuide(symmetry({120.0f, 50.0f}, SymmetryKind::Vertical));
    stroke(canvas, {20.0f, 30.0f}, {60.0f, 30.0f});
    std::vector<uint8_t> pixels = layerPixels(canvas);
    CHECK_PIXEL(at(pixels, canvas, 40, 30), kRed, 2);
    CHECK_PIXEL(at(pixels, canvas, 200, 30), kRed, 2);   // reflejado
    CHECK_PIXEL(at(pixels, canvas, 120, 30), kClear, 0);
    CHECK_PIXEL(at(pixels, canvas, 40, 70), kClear, 0);
    // Las dos copias van en un solo paso.
    REQUIRE(canvas.undo());
    pixels = layerPixels(canvas);
    CHECK_PIXEL(at(pixels, canvas, 40, 30), kClear, 0);
    CHECK_PIXEL(at(pixels, canvas, 200, 30), kClear, 0);
    CHECK(!canvas.canUndo());
    REQUIRE(canvas.redo());
    pixels = layerPixels(canvas);
    CHECK_PIXEL(at(pixels, canvas, 40, 30), kRed, 2);
    CHECK_PIXEL(at(pixels, canvas, 200, 30), kRed, 2);

    // Cuadrante: las cuatro esquinas. El compuesto también las muestra.
    canvas.setGuide(symmetry({120.0f, 50.0f}, SymmetryKind::Quadrant));
    stroke(canvas, {20.0f, 10.0f}, {40.0f, 10.0f});
    const std::vector<uint8_t> shown = composite(canvas);
    for (const glm::ivec2 q : {glm::ivec2(30, 10), glm::ivec2(210, 10), glm::ivec2(30, 90), glm::ivec2(210, 90)}) {
        CHECK_PIXEL(at(shown, canvas, q.x, q.y), kRed, 2);
    }
    // Sin simetría vuelve a pintar una vez.
    DrawingGuide off = canvas.guide();
    off.enabled = false;
    canvas.setGuide(off);
    stroke(canvas, {20.0f, 70.0f}, {40.0f, 70.0f});
    pixels = layerPixels(canvas);
    CHECK_PIXEL(at(pixels, canvas, 30, 70), kRed, 2);
    CHECK_PIXEL(at(pixels, canvas, 210, 70), kClear, 0);
    CHECK(gfx::checkErrors("symmetry_repeats_the_stroke_and_undoes_it_at_once"));
}

TEST_CASE(symmetry_matches_a_stroke_drawn_by_hand_on_the_other_side) {
    // Lo que pinta la copia es igual que el trazo reflejado dibujado a mano (con un pincel
    // con el final afinado, que pinta provisionales mientras se dibuja).
    BrushParams tapered = hardBrush();
    tapered.taperEnd = 0.6f;
    tapered.taperStart = 0.3f;
    Canvas mirrored;
    REQUIRE(mirrored.init(200, 80));
    usePaint(mirrored, tapered, 5.0f);
    mirrored.setGuide(symmetry({100.0f, 40.0f}, SymmetryKind::Vertical));
    stroke(mirrored, {20.0f, 40.0f}, {80.0f, 20.0f});

    Canvas byHand;
    REQUIRE(byHand.init(200, 80));
    usePaint(byHand, tapered, 5.0f);
    stroke(byHand, {20.0f, 40.0f}, {80.0f, 20.0f});
    stroke(byHand, {180.0f, 40.0f}, {120.0f, 20.0f});
    CHECK(test::maxDifference(layerPixels(mirrored), layerPixels(byHand)) <= 3);
}

TEST_CASE(symmetry_works_with_wet_brushes) {
    // Un pincel húmedo (que mezcla con el azul de la capa y afina el final): la copia pinta
    // lo mismo que el trazo reflejado dibujado a mano.
    BrushParams wet = hardBrush();
    wet.wetPull = 0.5f;
    wet.wetCharge = 0.7f;
    wet.taperEnd = 0.5f;
    auto prepare = [&](Canvas& canvas) {
        test::fillRect(canvas.layers().at(1).target, IRect::ofSize(200, 80), 0.0f, 0.0f, 1.0f, 1.0f);
        canvas.layers().markDirty(IRect::ofSize(200, 80));
        usePaint(canvas, wet, 6.0f);
    };
    Canvas canvas;
    REQUIRE(canvas.init(200, 80));
    prepare(canvas);
    const std::vector<uint8_t> before = layerPixels(canvas);
    canvas.setGuide(symmetry({100.0f, 40.0f}, SymmetryKind::Vertical));
    stroke(canvas, {20.0f, 40.0f}, {70.0f, 40.0f});
    const std::vector<uint8_t> pixels = layerPixels(canvas);
    CHECK(at(pixels, canvas, 35, 40)[0] > 40);
    CHECK(at(pixels, canvas, 165, 40)[0] > 40);
    CHECK_PIXEL(at(pixels, canvas, 100, 40), at(before, canvas, 100, 40), 0);
    CHECK_PIXEL(at(pixels, canvas, 35, 70), at(before, canvas, 35, 70), 0);

    Canvas byHand;
    REQUIRE(byHand.init(200, 80));
    prepare(byHand);
    stroke(byHand, {20.0f, 40.0f}, {70.0f, 40.0f});
    stroke(byHand, {180.0f, 40.0f}, {130.0f, 40.0f});
    CHECK(test::maxDifference(pixels, layerPixels(byHand)) <= 2);

    REQUIRE(canvas.undo());
    CHECK(test::maxDifference(layerPixels(canvas), before) == 0);
    CHECK(gfx::checkErrors("symmetry_works_with_wet_brushes"));
}

TEST_CASE(quick_shape_redraws_the_stroke) {
    Canvas canvas;
    REQUIRE(canvas.init(200, 120));
    usePaint(canvas, hardBrush(), 2.0f);
    // Un trazo con una joroba de 7 px: se parece a una línea.
    const std::vector<glm::vec2> path = bumpyLine({20.0f, 60.0f}, {180.0f, 60.0f}, 7.0f);
    trace(canvas, path);
    // Mientras se dibuja, el trazo se ve en el compuesto (sobre el fondo blanco).
    CHECK_PIXEL(at(composite(canvas), canvas, 100, 67), kRed, 2);
    quickshape::Options options;
    options.minLength = 24.0f;
    REQUIRE(canvas.snapStroke(options));
    CHECK(canvas.strokeShape().kind == quickshape::Kind::Line);
    CHECK(!canvas.snapStroke(options));   // ya es una forma
    std::vector<uint8_t> pixels = composite(canvas);
    CHECK_PIXEL(at(pixels, canvas, 100, 60), kRed, 2);
    CHECK_PIXEL(at(pixels, canvas, 100, 67), kWhite, 0);

    // Sin soltar, el final sigue al puntero (y no cuenta como trazo a mano).
    canvas.strokeTo(180.0f, 100.0f, 1.0f);
    pixels = composite(canvas);
    CHECK_PIXEL(at(pixels, canvas, 179, 60), kWhite, 0);
    CHECK_PIXEL(at(pixels, canvas, 100, 80), kRed, 2);   // a mitad de (20, 60)–(180, 100)
    canvas.endStroke();
    canvas.update();
    pixels = layerPixels(canvas);
    CHECK_PIXEL(at(pixels, canvas, 100, 80), kRed, 2);
    CHECK_PIXEL(at(pixels, canvas, 100, 67), kClear, 0);
    CHECK(canvas.strokeShape().kind == quickshape::Kind::None);
    // Un solo paso de deshacer.
    REQUIRE(canvas.undo());
    CHECK(test::maxDifference(layerPixels(canvas), std::vector<uint8_t>(pixels.size(), 0)) == 0);
    CHECK(gfx::checkErrors("quick_shape_redraws_the_stroke"));
}

TEST_CASE(quick_shape_can_be_made_regular_and_back) {
    Canvas canvas;
    REQUIRE(canvas.init(300, 200));
    usePaint(canvas, hardBrush(), 2.0f);
    std::vector<glm::vec2> oval;
    for (int i = 0; i <= 400; ++i) {
        const float t = 0.2f + (2.0f * kPi + 0.1f) * static_cast<float>(i) / 400.0f;
        oval.emplace_back(150.0f + 110.0f * std::cos(t), 100.0f + 60.0f * std::sin(t));
    }
    trace(canvas, oval);
    REQUIRE(canvas.snapStroke({}));
    CHECK(canvas.strokeShape().kind == quickshape::Kind::Ellipse);
    canvas.setShapeRegular(true);
    CHECK(canvas.strokeShape().kind == quickshape::Kind::Circle);
    CHECK(canvas.strokeShape().regular);
    std::vector<uint8_t> pixels = composite(canvas);
    CHECK_PIXEL(at(pixels, canvas, 150, 15), kRed, 2);     // el círculo de radio 85
    CHECK_PIXEL(at(pixels, canvas, 150, 40), kWhite, 0);   // por donde pasaba la elipse
    canvas.setShapeRegular(false);
    CHECK(canvas.strokeShape().kind == quickshape::Kind::Ellipse);
    pixels = composite(canvas);
    CHECK_PIXEL(at(pixels, canvas, 150, 40), kRed, 2);
    CHECK_PIXEL(at(pixels, canvas, 150, 15), kWhite, 0);
    // Cancelar no deja nada.
    canvas.cancelStroke();
    pixels = composite(canvas);
    CHECK_PIXEL(at(pixels, canvas, 150, 40), kWhite, 0);
    CHECK(test::maxDifference(layerPixels(canvas), std::vector<uint8_t>(pixels.size(), 0)) == 0);
    CHECK(!canvas.canUndo());
}

TEST_CASE(quick_shape_with_symmetry_and_wet_brushes) {
    // La forma se repite con la simetría, y con un pincel húmedo lo que pintó el trazo a mano
    // vuelve a ser la capa.
    for (const bool wet : {false, true}) {
        Canvas canvas;
        REQUIRE(canvas.init(240, 120));
        test::fillRect(canvas.layers().at(1).target, IRect::ofSize(240, 120), 0.0f, 0.0f, 1.0f, 1.0f);
        canvas.layers().markDirty(IRect::ofSize(240, 120));
        const std::vector<uint8_t> before = layerPixels(canvas);
        BrushParams params = hardBrush();
        if (wet) {
            params.wetPull = 0.3f;
            params.wetCharge = 0.8f;
            params.taperEnd = 0.4f;
        }
        usePaint(canvas, params, 2.5f);
        canvas.setGuide(symmetry({120.0f, 60.0f}, SymmetryKind::Vertical));
        trace(canvas, bumpyLine({20.0f, 40.0f}, {100.0f, 40.0f}, 4.0f));
        REQUIRE(canvas.snapStroke({}));
        canvas.endStroke();
        canvas.update();
        const std::vector<uint8_t> pixels = layerPixels(canvas);
        // La línea recta y su reflejo.
        CHECK(at(pixels, canvas, 60, 40)[0] > 100);
        CHECK(at(pixels, canvas, 180, 40)[0] > 100);
        // Donde solo pasó la joroba, la capa está como antes.
        CHECK_PIXEL(at(pixels, canvas, 60, 44), at(before, canvas, 60, 44), wet ? 1 : 0);
        CHECK_PIXEL(at(pixels, canvas, 180, 44), at(before, canvas, 180, 44), wet ? 1 : 0);
        REQUIRE(canvas.undo());
        CHECK(test::maxDifference(layerPixels(canvas), before) == 0);
        CHECK(gfx::checkErrors("quick_shape_with_symmetry_and_wet_brushes"));
    }
}

TEST_CASE(guide_is_part_of_each_canvas) {
    Canvas canvas;
    REQUIRE(canvas.init(300, 200));
    CHECK(!canvas.guide().enabled);
    CHECK(sameVec(canvas.guide().center, {150.0f, 100.0f}));
    DrawingGuide g = symmetry({1000.0f, -50.0f}, SymmetryKind::Radial);
    g.opacity = 7.0f;
    g.gridSize = 0.0f;
    canvas.setGuide(g);
    // Se queda dentro del lienzo y con valores válidos.
    CHECK(sameVec(canvas.guide().center, {300.0f, 0.0f}));
    CHECK_NEAR(canvas.guide().opacity, 1.0f, 1e-6f);
    CHECK_NEAR(canvas.guide().gridSize, guide::kMinGridSize, 1e-6f);
    // Cambiarla no se deshace.
    CHECK(!canvas.canUndo());
    // Un lienzo nuevo empieza con la suya.
    REQUIRE(canvas.init(100, 60));
    CHECK(!canvas.guide().enabled);
    CHECK(sameVec(canvas.guide().center, {50.0f, 30.0f}));
}
