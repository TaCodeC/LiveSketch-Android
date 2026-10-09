#include "Test.h"

#include "Canvas/Canvas.h"

#include <glm/vec2.hpp>

#include <cmath>
#include <cstdlib>

namespace {

using test::Pixel;

constexpr Pixel kWhite{255, 255, 255, 255};

std::vector<uint8_t> composite(Canvas& canvas) {
    canvas.update();
    return test::readTarget(canvas.composite());
}

std::vector<uint8_t> layerPixels(Canvas& canvas, int layer) {
    return test::readTarget(canvas.layers().at(layer).target);
}

void setBrush(Canvas& canvas, float r, float g, float b, float radius = 5.0f, bool eraser = false) {
    BrushSettings& brush = canvas.brushSettings();
    brush.color[0] = r;
    brush.color[1] = g;
    brush.color[2] = b;
    brush.opacity = 1.0f;
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

} // namespace

TEST_CASE(history_stroke_undo_redo) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 48));
    CHECK(!canvas.canUndo());
    CHECK(!canvas.canRedo());
    const std::vector<uint8_t> empty = composite(canvas);

    setBrush(canvas, 0.1f, 0.2f, 0.9f);
    drawLine(canvas, {5.0f, 5.0f}, {58.0f, 40.0f});
    const std::vector<uint8_t> first = composite(canvas);
    setBrush(canvas, 0.9f, 0.1f, 0.1f, 8.0f);
    drawLine(canvas, {5.0f, 40.0f}, {58.0f, 5.0f});
    const std::vector<uint8_t> second = composite(canvas);
    const uint64_t revision = canvas.layers().at(0).revision;

    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(composite(canvas), first), 0);
    CHECK(canvas.layers().at(0).revision > revision);   // la miniatura se rehace
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(composite(canvas), empty), 0);
    CHECK(!canvas.canUndo());
    CHECK(!canvas.undo());

    REQUIRE(canvas.redo());
    CHECK_EQ(test::maxDifference(composite(canvas), first), 0);
    REQUIRE(canvas.redo());
    CHECK_EQ(test::maxDifference(composite(canvas), second), 0);
    CHECK(!canvas.canRedo());

    // Un trazo nuevo descarta lo que se podía rehacer.
    REQUIRE(canvas.undo());
    drawLine(canvas, {30.0f, 2.0f}, {30.0f, 46.0f});
    CHECK(!canvas.canRedo());
    CHECK_EQ(canvas.history().undoCount(), 2);
}

TEST_CASE(history_eraser_and_clear) {
    Canvas canvas;
    REQUIRE(canvas.init(40, 40));
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 10.0f);
    drawLine(canvas, {5.0f, 20.0f}, {35.0f, 20.0f});
    const std::vector<uint8_t> painted = layerPixels(canvas, 0);

    setBrush(canvas, 0.0f, 0.0f, 0.0f, 6.0f, true);
    drawLine(canvas, {20.0f, 5.0f}, {20.0f, 35.0f});
    CHECK(test::maxDifference(layerPixels(canvas, 0), painted) > 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), painted), 0);

    canvas.clearLayer(0);
    CHECK_EQ(test::pixelAt(layerPixels(canvas, 0), 40, 10, 20)[3], 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), painted), 0);
    REQUIRE(canvas.redo());
    CHECK_EQ(test::pixelAt(layerPixels(canvas, 0), 40, 10, 20)[3], 0);
}

TEST_CASE(history_layer_structure) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    setBrush(canvas, 1.0f, 0.0f, 0.0f, 20.0f);
    drawLine(canvas, {16.0f, 16.0f}, {17.0f, 16.0f});   // "Capa 1" roja
    const uint32_t red = layers.at(0).id;

    // Añadir y deshacer.
    REQUIRE(canvas.addLayer());
    const uint32_t added = layers.active().id;
    CHECK_EQ(layers.count(), 2);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.count(), 1);
    CHECK_EQ(layers.indexOf(added), -1);
    REQUIRE(canvas.redo());
    CHECK_EQ(layers.count(), 2);
    CHECK_EQ(layers.indexOf(added), 1);
    CHECK_EQ(layers.active().id, added);

    // Borrar la roja y recuperarla en su sitio, con su contenido.
    const std::vector<uint8_t> withRed = composite(canvas);
    CHECK(test::pixelAt(withRed, 32, 16, 16)[1] < 128);
    REQUIRE(canvas.removeLayer(0));
    CHECK_EQ(layers.indexOf(red), -1);
    CHECK_PIXEL(test::pixelAt(composite(canvas), 32, 16, 16), kWhite, 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.indexOf(red), 0);
    CHECK_EQ(layers.active().id, red);
    CHECK_EQ(test::maxDifference(composite(canvas), withRed), 0);

    // Mover.
    REQUIRE(canvas.moveLayer(0, 1));
    CHECK_EQ(layers.indexOf(red), 1);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.indexOf(red), 0);
    REQUIRE(canvas.redo());
    CHECK_EQ(layers.indexOf(red), 1);
    REQUIRE(canvas.undo());

    // Duplicar.
    REQUIRE(canvas.duplicateLayer(0));
    CHECK_EQ(layers.count(), 3);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.count(), 2);
    CHECK_EQ(layers.at(0).id, red);
}

TEST_CASE(history_merge_down) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    LayerStack& layers = canvas.layers();
    setBrush(canvas, 0.0f, 0.0f, 1.0f, 12.0f);
    drawLine(canvas, {10.0f, 16.0f}, {11.0f, 16.0f});
    canvas.setLayerOpacity(0, 0.5f);
    REQUIRE(canvas.addLayer());
    setBrush(canvas, 1.0f, 0.0f, 0.0f, 12.0f);
    drawLine(canvas, {22.0f, 16.0f}, {23.0f, 16.0f});
    const uint32_t lower = layers.at(0).id;
    const uint32_t upper = layers.at(1).id;
    const std::vector<uint8_t> lowerBefore = layerPixels(canvas, 0);
    const std::vector<uint8_t> upperBefore = layerPixels(canvas, 1);
    const std::vector<uint8_t> look = composite(canvas);

    REQUIRE(canvas.mergeDown(1));
    CHECK_EQ(layers.count(), 1);
    CHECK_EQ(layers.at(0).opacity, 1.0f);
    const std::vector<uint8_t> merged = layerPixels(canvas, 0);

    REQUIRE(canvas.undo());
    CHECK_EQ(layers.count(), 2);
    CHECK_EQ(layers.at(0).id, lower);
    CHECK_EQ(layers.at(1).id, upper);
    CHECK_EQ(layers.at(0).opacity, 0.5f);
    CHECK_EQ(layers.active().id, upper);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), lowerBefore), 0);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), upperBefore), 0);
    CHECK_EQ(test::maxDifference(composite(canvas), look), 0);

    REQUIRE(canvas.redo());
    CHECK_EQ(layers.count(), 1);
    CHECK_EQ(layers.at(0).opacity, 1.0f);
    CHECK_EQ(layers.active().id, lower);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), merged), 0);
}

TEST_CASE(history_layer_properties) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    LayerStack& layers = canvas.layers();

    canvas.setLayerVisible(0, false);
    CHECK(!layers.at(0).visible);
    canvas.renameLayer(0, "Tinta");
    // Arrastrar la opacidad deja un solo paso, desde el valor de antes de empezar.
    canvas.setLayerOpacity(0, 0.8f, false);
    canvas.setLayerOpacity(0, 0.5f, false);
    canvas.setLayerOpacity(0, 0.3f, true);
    CHECK_EQ(canvas.history().undoCount(), 3);

    REQUIRE(canvas.undo());
    CHECK_EQ(layers.at(0).opacity, 1.0f);
    REQUIRE(canvas.undo());
    CHECK_EQ(layers.at(0).name, std::string("Capa 1"));
    REQUIRE(canvas.undo());
    CHECK(layers.at(0).visible);
    REQUIRE(canvas.redo());
    REQUIRE(canvas.redo());
    REQUIRE(canvas.redo());
    CHECK(!layers.at(0).visible);
    CHECK_EQ(layers.at(0).name, std::string("Tinta"));
    CHECK_EQ(layers.at(0).opacity, 0.3f);

    // Sin cambio real no hay paso.
    canvas.setLayerOpacity(0, 0.3f, true);
    canvas.renameLayer(0, "Tinta");
    CHECK_EQ(canvas.history().undoCount(), 3);
}

TEST_CASE(history_limits_drop_oldest) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 2.0f);
    for (int i = 0; i < 120; ++i) {
        const float x = static_cast<float>(4 + i % 24);
        drawLine(canvas, {x, 4.0f}, {x, 28.0f});
    }
    CHECK_EQ(canvas.history().undoCount(), 100);
    int undone = 0;
    while (canvas.undo()) {
        ++undone;
    }
    CHECK_EQ(undone, 100);
}

TEST_CASE(history_cleared_by_new_canvas_and_context_loss) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    drawLine(canvas, {4.0f, 4.0f}, {28.0f, 28.0f});
    CHECK(canvas.canUndo());
    REQUIRE(canvas.init(24, 24));
    CHECK(!canvas.canUndo());

    drawLine(canvas, {4.0f, 4.0f}, {20.0f, 20.0f});
    REQUIRE(canvas.takeSnapshot(size_t{1} << 30));
    REQUIRE(test::recreateGLContext());
    REQUIRE(canvas.recreateGpu(nullptr));
    CHECK(!canvas.canUndo());
    // El buffer de trazo del contexto nuevo sirve para seguir deshaciendo.
    drawLine(canvas, {4.0f, 20.0f}, {20.0f, 4.0f});
    REQUIRE(canvas.undo());
}

TEST_CASE(canvas_pick_color) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    test::fillRect(canvas.layers().at(0).target, {0, 0, 16, 32}, 0.25f, 0.0f, 0.0f, 0.5f);   // rojo al 50 %
    canvas.layers().markAllDirty();

    float rgb[3] = {0.0f, 0.0f, 0.0f};
    REQUIRE(canvas.pickColor(24.0f, 10.0f, rgb));   // fondo blanco
    CHECK_NEAR(rgb[0], 1.0f, 0.01f);
    CHECK_NEAR(rgb[2], 1.0f, 0.01f);
    REQUIRE(canvas.pickColor(4.0f, 10.0f, rgb));    // rojo 0,5 sobre blanco: (1, 0,5, 0,5)
    CHECK_NEAR(rgb[0], 0.75f, 0.01f);
    CHECK_NEAR(rgb[1], 0.5f, 0.01f);

    test::hideBackground(canvas);
    REQUIRE(canvas.pickColor(4.0f, 10.0f, rgb));    // sin fondo: el color de la capa sin premultiplicar
    CHECK_NEAR(rgb[0], 0.5f, 0.01f);
    CHECK_NEAR(rgb[1], 0.0f, 0.01f);
    CHECK(!canvas.pickColor(24.0f, 10.0f, rgb));    // transparente
    CHECK(!canvas.pickColor(-1.0f, 10.0f, rgb));    // fuera del lienzo
    CHECK(!canvas.pickColor(10.0f, 32.0f, rgb));
}
