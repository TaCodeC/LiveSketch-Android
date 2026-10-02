#include "Test.h"

#include "Canvas/Canvas.h"

#include <glm/vec2.hpp>

#include <algorithm>

namespace {

using test::Pixel;

constexpr Pixel kWhite{255, 255, 255, 255};
constexpr Pixel kClear{0, 0, 0, 0};

std::vector<uint8_t> composite(Canvas& canvas) {
    canvas.update();
    return test::readTarget(canvas.composite());
}

Pixel compositeAt(Canvas& canvas, int x, int y) {
    return test::pixelAt(composite(canvas), canvas.width(), x, y);
}

Pixel layerAt(Canvas& canvas, int layer, int x, int y) {
    return test::pixelAt(test::readTarget(canvas.layers().at(layer).target), canvas.width(), x, y);
}

void fillLayer(Canvas& canvas, int layer, const IRect& rect, float r, float g, float b, float a) {
    test::fillRect(canvas.layers().at(layer).target, rect, r, g, b, a);
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

void drawLine(Canvas& canvas, glm::vec2 from, glm::vec2 to, float pressure = 1.0f) {
    canvas.beginStroke(from.x, from.y, pressure);
    canvas.strokeTo(to.x, to.y, pressure);
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

} // namespace

TEST_CASE(canvas_init_creates_layer_over_background) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 48));
    CHECK_EQ(canvas.width(), 64);
    CHECK_EQ(canvas.height(), 48);
    // El fondo no es una capa: una sola capa sobre el fondo blanco.
    CHECK_EQ(canvas.layers().count(), 1);
    CHECK_EQ(canvas.layers().at(0).name, std::string("Capa 1"));
    CHECK_EQ(canvas.layers().activeIndex(), 0);
    CHECK(canvas.background().visible);
    CHECK(canvas.background() == CanvasBackground{});
    CHECK_PIXEL(compositeAt(canvas, 0, 0), kWhite, 0);
    CHECK_PIXEL(compositeAt(canvas, 63, 47), kWhite, 0);
    CHECK_PIXEL(layerAt(canvas, 0, 10, 10), kClear, 0);
    CHECK_EQ(canvas.info().ppi, 72.0f);
    CHECK(canvas.info().profile == ColorProfile::Srgb);
    CHECK_EQ(canvas.history().undoCount(), 0);
    CHECK(!canvas.init(0, 10));
    CHECK(!canvas.ready());

    // Con lo elegido en la tarjeta de lienzo nuevo.
    CanvasSpec spec;
    spec.width = 40;
    spec.height = 30;
    spec.ppi = 300.0f;
    spec.unit = LengthUnit::Millimeters;
    spec.profile = ColorProfile::DisplayP3;
    spec.name = "Retrato";
    spec.background.color[0] = 0.2f;
    spec.background.color[1] = 0.4f;
    spec.background.color[2] = 0.6f;
    REQUIRE(canvas.init(spec));
    CHECK_EQ(canvas.info().name, std::string("Retrato"));
    CHECK_EQ(canvas.info().ppi, 300.0f);
    CHECK(canvas.info().unit == LengthUnit::Millimeters);
    CHECK(canvas.info().profile == ColorProfile::DisplayP3);
    CHECK_PIXEL(compositeAt(canvas, 20, 15), (Pixel{51, 102, 153, 255}), 0);
    spec.background.visible = false;
    REQUIRE(canvas.init(spec));
    CHECK_PIXEL(compositeAt(canvas, 20, 15), kClear, 0);
}

TEST_CASE(canvas_background_color_is_undoable) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    fillLayer(canvas, 0, {8, 8, 24, 24}, 0.0f, 0.0f, 0.5f, 0.5f);   // azul al 50 %
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{128, 128, 255, 255}), 1);

    // Cambiarlo no toca las capas, y las de otros modos se mezclan con él.
    CanvasBackground red;
    red.color[1] = 0.0f;
    red.color[2] = 0.0f;
    canvas.setBackground(red);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), (Pixel{255, 0, 0, 255}), 0);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{128, 0, 128, 255}), 1);
    CHECK_PIXEL(layerAt(canvas, 0, 16, 16), (Pixel{0, 0, 128, 128}), 1);
    canvas.setLayerBlend(0, BlendMode::Multiply);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{128, 0, 0, 255}), 1);
    canvas.setLayerBlend(0, BlendMode::Normal);
    CHECK_EQ(canvas.history().undoCount(), 3);

    // Oculto, lo que no está pintado queda transparente.
    CanvasBackground hidden = red;
    hidden.visible = false;
    canvas.setBackground(hidden);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), kClear, 0);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{0, 0, 128, 128}), 1);

    // Elegir el color arrastrando es un solo paso, desde el de antes de empezar.
    CHECK(canvas.undo());
    CHECK(canvas.background() == red);
    const int steps = canvas.history().undoCount();
    for (int i = 1; i <= 5; ++i) {
        CanvasBackground next = red;
        next.color[1] = 0.1f * static_cast<float>(i);
        canvas.setBackground(next, false);
        CHECK(canvas.canUndo());
    }
    CHECK_PIXEL(compositeAt(canvas, 2, 2), (Pixel{255, 128, 0, 255}), 1);
    canvas.finishBackgroundEdit();
    CHECK_EQ(canvas.history().undoCount(), steps + 1);
    CHECK(canvas.undo());
    CHECK(canvas.background() == red);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), (Pixel{255, 0, 0, 255}), 0);
    CHECK(canvas.redo());
    CHECK_NEAR(canvas.background().color[1], 0.5f, 1e-6);

    // Un cambio a medias se guarda antes de otra operación, y deshacer lo quita.
    CanvasBackground green;
    green.color[0] = 0.0f;
    green.color[2] = 0.0f;
    canvas.setBackground(green, false);
    REQUIRE(canvas.addLayer());
    CHECK_EQ(canvas.history().undoCount(), steps + 2 + 1);
    CHECK(canvas.undo());
    CHECK(canvas.undo());
    CHECK_NEAR(canvas.background().color[1], 0.5f, 1e-6);
    canvas.setBackground(green, false);
    CHECK(canvas.undo());
    CHECK_NEAR(canvas.background().color[1], 0.5f, 1e-6);
    CHECK(canvas.canRedo());

    // Sin cambios no se guarda nada.
    const int before = canvas.history().undoCount();
    canvas.setBackground(canvas.background());
    canvas.setBackground(canvas.background(), false);
    canvas.finishBackgroundEdit();
    CHECK_EQ(canvas.history().undoCount(), before);
}

TEST_CASE(canvas_composite_order_visibility_opacity) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 0, IRect::ofSize(32, 32), 1.0f, 0.0f, 0.0f, 1.0f);   // capa roja
    fillLayer(canvas, 1, {8, 8, 24, 24}, 0.0f, 0.0f, 1.0f, 1.0f);          // cuadrado azul
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{0, 0, 255, 255}), 0);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), (Pixel{255, 0, 0, 255}), 0);

    layers.setOpacity(1, 0.5f);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{128, 0, 128, 255}), 1);

    layers.setVisible(1, false);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{255, 0, 0, 255}), 0);

    // Sin la capa roja ni el fondo, el compuesto es transparente y premultiplicado.
    layers.setVisible(1, true);
    layers.setVisible(0, false);
    test::hideBackground(canvas);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{0, 0, 128, 128}), 1);
    CHECK_PIXEL(compositeAt(canvas, 2, 2), kClear, 0);

    layers.setOpacity(1, 0.0f);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), kClear, 0);
}

TEST_CASE(canvas_version_changes_only_when_dirty) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    const uint64_t version = canvas.version();
    CHECK(!canvas.update());
    CHECK_EQ(canvas.version(), version);
    canvas.layers().setOpacity(0, 0.5f);
    CHECK(canvas.update());
    CHECK(canvas.version() > version);
}

TEST_CASE(canvas_partial_updates_match_full_compose) {
    Canvas canvas;
    REQUIRE(canvas.init(96, 64));
    setBrush(canvas, 0.2f, 0.6f, 0.9f, 0.7f);
    drawLine(canvas, {10.0f, 10.0f}, {80.0f, 50.0f});
    REQUIRE(canvas.addLayer());
    setBrush(canvas, 1.0f, 0.0f, 0.0f, 0.5f, 9.0f);
    drawLine(canvas, {5.0f, 60.0f}, {90.0f, 5.0f});
    canvas.layers().setOpacity(0, 0.6f);
    canvas.update();

    // Trazo a medias: el compuesto se actualiza solo en la zona que tocan los dabs.
    setBrush(canvas, 0.1f, 0.8f, 0.1f, 0.8f, 4.0f);
    canvas.beginStroke(20.0f, 30.0f, 1.0f);
    canvas.strokeTo(70.0f, 34.0f, 0.6f);
    const std::vector<uint8_t> partial = composite(canvas);

    canvas.layers().markAllDirty();
    const std::vector<uint8_t> full = composite(canvas);
    CHECK_EQ(test::maxDifference(partial, full), 0);
    canvas.endStroke();
}

TEST_CASE(canvas_stroke_preview_matches_commit) {
    for (const bool erase : {false, true}) {
        Canvas canvas;
        REQUIRE(canvas.init(64, 64));
        fillLayer(canvas, 0, IRect::ofSize(64, 64), 0.1f, 0.4f, 0.2f, 0.8f);
        canvas.update();

        setBrush(canvas, 0.9f, 0.3f, 0.1f, 0.6f, 6.0f, erase);
        canvas.beginStroke(8.0f, 8.0f, 1.0f);
        canvas.strokeTo(56.0f, 40.0f, 0.7f);
        const std::vector<uint8_t> preview = composite(canvas);
        canvas.endStroke();
        const std::vector<uint8_t> committed = composite(canvas);
        CHECK(test::maxDifference(preview, committed) <= 2);
        CHECK(!canvas.stroking());
    }
}

TEST_CASE(canvas_stroke_opacity_and_color) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    test::hideBackground(canvas);   // sin fondo: se ve el alfa del trazo

    setBrush(canvas, 1.0f, 0.5f, 0.0f, 1.0f, 8.0f);
    drawLine(canvas, {10.0f, 32.0f}, {54.0f, 32.0f});
    const Pixel full = layerAt(canvas, 0, 32, 32);
    CHECK(full[3] >= 250);
    CHECK_NEAR(full[0], full[3], 1);            // rojo 1.0 premultiplicado
    CHECK_NEAR(full[1], full[3] * 0.5, 1.5);    // verde 0.5
    CHECK_EQ(full[2], 0);

    canvas.clearLayer(0);
    setBrush(canvas, 1.0f, 0.5f, 0.0f, 0.5f, 8.0f);
    drawLine(canvas, {10.0f, 32.0f}, {54.0f, 32.0f});
    const Pixel half = layerAt(canvas, 0, 32, 32);
    CHECK_NEAR(half[3], full[3] * 0.5, 2);
    CHECK_PIXEL(compositeAt(canvas, 32, 32), half, 1);
}

TEST_CASE(canvas_stroke_coordinates_are_top_down) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 3.0f);
    drawLine(canvas, {10.0f, 5.0f}, {12.0f, 5.0f});

    // El lienzo va con la y hacia abajo y las lecturas (PNG, NDI) salen de arriba abajo.
    std::vector<uint8_t> pixels;
    REQUIRE(canvas.readComposite(pixels));
    CHECK(test::pixelAt(pixels, 64, 11, 5)[0] < 128);
    CHECK_PIXEL(test::pixelAt(pixels, 64, 11, 58), kWhite, 0);
    CHECK_PIXEL(test::pixelAt(pixels, 64, 52, 5), kWhite, 0);
}

TEST_CASE(canvas_short_moves_still_paint) {
    // El lápiz manda muchos movimientos más cortos que la separación entre dabs: lo que
    // sobra de cada uno se arrastra al siguiente y el trazo sale continuo.
    Canvas canvas;
    REQUIRE(canvas.init(64, 32));
    test::hideBackground(canvas);
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f);
    canvas.beginStroke(10.0f, 16.0f, 1.0f);
    for (int i = 1; i <= 100; ++i) {
        canvas.strokeTo(10.0f + static_cast<float>(i) * 0.3f, 16.0f, 1.0f);
    }
    canvas.endStroke();
    const std::vector<uint8_t> layer = test::readTarget(canvas.layers().at(0).target);
    for (int x = 11; x <= 39; ++x) {
        if (test::pixelAt(layer, 64, x, 16)[3] < 128) {
            test::fail(__FILE__, __LINE__, "hueco en el trazo en x=" + std::to_string(x));
            break;
        }
    }
    CHECK_EQ(test::pixelAt(layer, 64, 50, 16)[3], 0);
}

TEST_CASE(canvas_eraser_removes_paint) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    fillLayer(canvas, 0, IRect::ofSize(64, 64), 0.0f, 0.0f, 1.0f, 1.0f);
    canvas.update();

    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 8.0f, true);
    drawLine(canvas, {10.0f, 32.0f}, {54.0f, 32.0f});
    CHECK(layerAt(canvas, 0, 32, 32)[3] <= 5);
    CHECK_PIXEL(compositeAt(canvas, 32, 32), kWhite, 5);   // se ve el fondo
    CHECK_PIXEL(compositeAt(canvas, 32, 5), (Pixel{0, 0, 255, 255}), 0);

    // La goma del lápiz borra aunque el borrador del menú esté apagado.
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 8.0f, false);
    REQUIRE(canvas.beginStroke(20.0f, 4.0f, 1.0f, true));
    canvas.strokeTo(20.0f, 60.0f, 1.0f);
    canvas.endStroke();
    CHECK(layerAt(canvas, 0, 20, 12)[3] <= 5);

    // Borrador a media opacidad: deja la mitad.
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 0.5f, 8.0f, true);
    drawLine(canvas, {44.0f, 4.0f}, {44.0f, 20.0f});
    CHECK_NEAR(layerAt(canvas, 0, 44, 12)[3], 128, 3);
}

TEST_CASE(canvas_hidden_layer_blocks_strokes) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    canvas.layers().setVisible(0, false);
    CHECK(!canvas.beginStroke(10.0f, 10.0f, 1.0f));
    CHECK(!canvas.stroking());
    canvas.strokeTo(20.0f, 20.0f, 1.0f);
    canvas.endStroke();
    CHECK_PIXEL(layerAt(canvas, 0, 15, 15), kClear, 0);
}

TEST_CASE(canvas_cancel_stroke_restores) {
    Canvas canvas;
    REQUIRE(canvas.init(48, 48));
    const std::vector<uint8_t> before = composite(canvas);
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 5.0f);
    canvas.beginStroke(5.0f, 5.0f, 1.0f);
    canvas.strokeTo(40.0f, 40.0f, 1.0f);
    CHECK(test::maxDifference(composite(canvas), before) > 0);   // se ve el trazo a medias
    canvas.cancelStroke();
    CHECK_EQ(test::maxDifference(composite(canvas), before), 0);
    CHECK_PIXEL(layerAt(canvas, 0, 20, 20), kClear, 0);
    // Un trazo nuevo no arrastra restos del cancelado.
    drawLine(canvas, {40.0f, 5.0f}, {41.0f, 5.0f});
    CHECK_PIXEL(layerAt(canvas, 0, 20, 20), kClear, 0);
}

TEST_CASE(canvas_merge_down_preserves_look) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    fillLayer(canvas, 0, {0, 0, 20, 32}, 0.0f, 0.0f, 0.8f, 0.8f);
    layers.setOpacity(0, 0.5f);
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 1, {10, 0, 32, 32}, 0.6f, 0.0f, 0.0f, 0.6f);
    layers.setOpacity(1, 0.7f);
    const std::vector<uint8_t> before = composite(canvas);

    CHECK(canvas.canMergeDown(1));
    REQUIRE(canvas.mergeDown(1));
    CHECK_EQ(layers.count(), 1);
    CHECK_EQ(layers.activeIndex(), 0);
    CHECK_EQ(layers.at(0).opacity, 1.0f);
    CHECK(test::maxDifference(composite(canvas), before) <= 2);

    CHECK(!canvas.canMergeDown(0));   // no hay capa debajo (el fondo no es una capa)
    REQUIRE(canvas.addLayer());
    layers.setVisible(0, false);
    CHECK(!canvas.canMergeDown(1));   // no se funde con una capa oculta
    CHECK(!canvas.mergeDown(1));
}

TEST_CASE(canvas_duplicate_layer) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    fillLayer(canvas, 0, {4, 4, 20, 12}, 1.0f, 0.0f, 0.0f, 1.0f);
    layers.rename(0, "Tinta");
    layers.setOpacity(0, 0.5f);

    REQUIRE(canvas.duplicateLayer(0));
    CHECK_EQ(layers.count(), 2);
    CHECK_EQ(layers.activeIndex(), 1);
    CHECK_EQ(layers.at(1).name, std::string("Tinta copia"));
    CHECK_EQ(layers.at(1).opacity, 0.5f);
    CHECK_EQ(test::maxDifference(test::readTarget(layers.at(0).target), test::readTarget(layers.at(1).target)), 0);
    // Dos capas rojas al 50 %: 75 % de rojo sobre blanco.
    CHECK_PIXEL(compositeAt(canvas, 10, 8), (Pixel{255, 64, 64, 255}), 1);

    REQUIRE(canvas.duplicateLayer(0));
    CHECK_EQ(layers.at(1).name, std::string("Tinta copia 2"));
    CHECK_EQ(layers.at(2).name, std::string("Tinta copia"));
}

TEST_CASE(canvas_clear_and_move_layers) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    fillLayer(canvas, 0, IRect::ofSize(32, 32), 1.0f, 0.0f, 0.0f, 1.0f);
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 1, IRect::ofSize(32, 32), 0.0f, 1.0f, 0.0f, 1.0f);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{0, 255, 0, 255}), 0);

    const uint32_t green = canvas.layers().active().id;
    REQUIRE(canvas.moveLayer(1, 0));
    CHECK_EQ(canvas.layers().active().id, green);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{255, 0, 0, 255}), 0);

    canvas.clearLayer(1);
    CHECK_PIXEL(layerAt(canvas, 1, 16, 16), kClear, 0);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), (Pixel{0, 255, 0, 255}), 0);

    REQUIRE(canvas.removeLayer(0));
    CHECK_PIXEL(compositeAt(canvas, 16, 16), kWhite, 0);
    // La última capa no se puede quitar.
    CHECK(!canvas.removeLayer(0));
    CHECK_EQ(canvas.layers().count(), 1);
}

TEST_CASE(canvas_layer_limit) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    CHECK_EQ(canvas.maxLayers(), 64);
    while (canvas.layers().count() < canvas.maxLayers()) {
        REQUIRE(canvas.addLayer());
    }
    CHECK(!canvas.addLayer());
    CHECK(!canvas.duplicateLayer(0));

    // Con un lienzo 4K el límite lo pone la memoria: 768 MB / 33 MB por capa.
    REQUIRE(canvas.init(3840, 2160));
    CHECK_EQ(canvas.maxLayers(), 24);
    canvas.destroy();
}

TEST_CASE(canvas_context_loss_restores_snapshot) {
    Canvas canvas;
    REQUIRE(canvas.init(48, 32));
    setBrush(canvas, 0.2f, 0.4f, 0.9f, 0.8f, 5.0f);
    drawLine(canvas, {4.0f, 4.0f}, {44.0f, 28.0f});
    REQUIRE(canvas.addLayer());
    setBrush(canvas, 0.9f, 0.1f, 0.1f, 0.5f, 7.0f);
    drawLine(canvas, {4.0f, 28.0f}, {44.0f, 4.0f});
    canvas.layers().rename(1, "Rojo");
    canvas.layers().setOpacity(1, 0.7f);
    canvas.layers().setVisible(0, false);
    CanvasBackground background;
    background.color[0] = 0.9f;
    background.color[1] = 0.8f;
    background.color[2] = 0.6f;
    canvas.setBackground(background);
    const std::vector<uint8_t> before = composite(canvas);

    REQUIRE(canvas.takeSnapshot(size_t{1} << 30));
    CHECK(canvas.hasSnapshot());
    REQUIRE(test::recreateGLContext());

    bool restored = false;
    REQUIRE(canvas.recreateGpu(&restored));
    CHECK(restored);
    CHECK(!canvas.hasSnapshot());
    CHECK(canvas.ready());
    CHECK_EQ(canvas.layers().count(), 2);
    CHECK_EQ(canvas.layers().at(1).name, std::string("Rojo"));
    CHECK_EQ(canvas.layers().at(1).opacity, 0.7f);
    CHECK(!canvas.layers().at(0).visible);
    CHECK(canvas.background() == background);
    CHECK_EQ(test::maxDifference(composite(canvas), before), 0);

    // Se puede seguir pintando en el contexto nuevo.
    drawLine(canvas, {24.0f, 2.0f}, {24.0f, 30.0f});
    CHECK(test::maxDifference(composite(canvas), before) > 0);
}

TEST_CASE(canvas_context_loss_without_snapshot) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 5.0f);
    drawLine(canvas, {4.0f, 16.0f}, {28.0f, 16.0f});
    REQUIRE(test::recreateGLContext());

    bool restored = true;
    REQUIRE(canvas.recreateGpu(&restored));
    CHECK(!restored);
    CHECK_EQ(canvas.layers().count(), 1);
    // El fondo (que no está en la GPU) y capas vacías: el lienzo sigue siendo usable.
    CHECK_PIXEL(compositeAt(canvas, 16, 16), kWhite, 0);
    CHECK_PIXEL(layerAt(canvas, 0, 16, 16), kClear, 0);
}

TEST_CASE(canvas_snapshot_respects_budget) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    CHECK(!canvas.takeSnapshot(100));
    CHECK(!canvas.hasSnapshot());
    CHECK(canvas.takeSnapshot(32 * 32 * 4));
    CHECK(canvas.hasSnapshot());
    canvas.dropSnapshot();
    CHECK(!canvas.hasSnapshot());
}
