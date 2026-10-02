// Relleno arrastrando el color: la zona que se rellena, sin halo junto a las líneas
// suavizadas y sin cruzar una línea en diagonal; el umbral en vivo, la capa de referencia,
// la selección, el alfa bloqueado, la vista previa, cancelar y deshacer.

#include "Test.h"

#include "Canvas/Canvas.h"
#include "Canvas/SelectionShapes.h"

#include <glm/vec2.hpp>

#include <cstdlib>
#include <sstream>
#include <string>

namespace {

using test::Pixel;

constexpr Pixel kWhite{255, 255, 255, 255};
constexpr Pixel kBlack{0, 0, 0, 255};
constexpr Pixel kClear{0, 0, 0, 0};
constexpr Pixel kRed{255, 0, 0, 255};
constexpr Pixel kBlue{0, 0, 255, 255};

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

void setColor(Canvas& canvas, float r, float g, float b) {
    float* color = canvas.brushSettings().color;
    color[0] = r;
    color[1] = g;
    color[2] = b;
}

// Marco negro de 2 px en `outer`, con el borde de dentro suavizado: un anillo de 1 px al
// 50 % de alfa (como el borde de una línea con antialias).
void drawFrame(Canvas& canvas, int layer, const IRect& outer) {
    fillLayer(canvas, layer, outer, 0.0f, 0.0f, 0.0f);
    const IRect soft{outer.x0 + 2, outer.y0 + 2, outer.x1 - 2, outer.y1 - 2};
    test::fillRect(canvas.layers().at(layer).target, soft, 0.0f, 0.0f, 0.0f, 0.5f);
    const IRect hole{outer.x0 + 3, outer.y0 + 3, outer.x1 - 3, outer.y1 - 3};
    test::fillRect(canvas.layers().at(layer).target, hole, 0.0f, 0.0f, 0.0f, 0.0f);
    canvas.layers().markDirty(outer);
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

} // namespace

TEST_CASE(color_drop_fills_inside_the_lines_without_a_halo) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    drawFrame(canvas, 0, {4, 4, 28, 28});
    const std::vector<uint8_t> before = layerPixels(canvas, 0);
    setColor(canvas, 1.0f, 0.0f, 0.0f);

    REQUIRE(canvas.beginFill(16.0f, 16.0f, 0.3f) == Canvas::Edit::Done);
    CHECK(canvas.filling());
    CHECK(canvas.endFill(true));
    CHECK(!canvas.filling());
    CHECK_EQ(canvas.history().undoCount(), 1);

    const std::vector<uint8_t> layer = layerPixels(canvas, 0);
    CHECK_PIXEL(at(layer, canvas, 16, 16), kRed, 0);
    CHECK_PIXEL(at(layer, canvas, 7, 7), kRed, 0);         // la esquina de dentro
    // El borde suavizado queda encima del relleno: rojo oscuro, sin blanco por detrás.
    CHECK_PIXEL(at(layer, canvas, 6, 16), (Pixel{128, 0, 0, 255}), 1);
    CHECK_PIXEL(at(layer, canvas, 16, 25), (Pixel{128, 0, 0, 255}), 1);
    CHECK_PIXEL(at(layer, canvas, 5, 16), kBlack, 0);      // la línea no cambia
    CHECK_PIXEL(at(layer, canvas, 2, 2), kClear, 0);       // fuera del marco, nada
    CHECK_PIXEL(at(layer, canvas, 30, 16), kClear, 0);
    const std::vector<uint8_t> seen = composite(canvas);
    CHECK_PIXEL(at(seen, canvas, 6, 16), (Pixel{128, 0, 0, 255}), 1);
    CHECK_PIXEL(at(seen, canvas, 2, 2), kWhite, 0);

    // Un paso de deshacer, exacto.
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);
    REQUIRE(canvas.redo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), layer), 0);
}

TEST_CASE(color_drop_does_not_cross_a_diagonal_line) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    // Línea de un píxel en diagonal: por arriba a la derecha y por abajo a la izquierda
    // solo se tocan por las esquinas.
    for (int i = 0; i < 16; ++i) {
        fillLayer(canvas, 0, {i, i, i + 1, i + 1}, 0.0f, 0.0f, 0.0f);
    }
    setColor(canvas, 0.0f, 0.0f, 1.0f);
    REQUIRE(canvas.beginFill(12.5f, 2.5f, 0.3f) == Canvas::Edit::Done);
    CHECK(canvas.endFill(true));
    const std::vector<uint8_t> layer = layerPixels(canvas, 0);
    for (int i = 0; i < 15; ++i) {
        CHECK_PIXEL(at(layer, canvas, i + 1, i), kBlue, 0);    // encima de la línea
        CHECK_PIXEL(at(layer, canvas, i, i), kBlack, 0);       // la línea
        CHECK_PIXEL(at(layer, canvas, i, i + 1), kClear, 0);   // debajo: nada
    }
}

TEST_CASE(color_drop_threshold_changes_live_and_cancel_restores) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    // Rojo a la izquierda y rojo algo más claro a la derecha (60 niveles de diferencia).
    fillLayer(canvas, 0, {0, 0, 16, 32}, 1.0f, 0.0f, 0.0f);
    fillLayer(canvas, 0, {16, 0, 32, 32}, 1.0f, 60.0f / 255.0f, 60.0f / 255.0f);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);
    const std::vector<uint8_t> seenBefore = composite(canvas);
    setColor(canvas, 0.0f, 0.0f, 1.0f);

    REQUIRE(canvas.beginFill(4.0f, 4.0f, 0.1f) == Canvas::Edit::Done);
    CHECK(canvas.canUndo());
    CHECK(!canvas.canRedo());
    std::vector<uint8_t> seen = composite(canvas);
    CHECK_PIXEL(at(seen, canvas, 4, 4), kBlue, 0);
    CHECK_PIXEL(at(seen, canvas, 15, 20), kBlue, 0);
    CHECK_PIXEL(at(seen, canvas, 16, 20), (Pixel{255, 60, 60, 255}), 0);
    // Mientras se ajusta, la capa no cambia.
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);

    canvas.setFillThreshold(0.5f);
    seen = composite(canvas);
    CHECK_PIXEL(at(seen, canvas, 16, 20), kBlue, 0);
    CHECK_PIXEL(at(seen, canvas, 30, 30), kBlue, 0);
    canvas.setFillThreshold(0.1f);
    seen = composite(canvas);
    CHECK_PIXEL(at(seen, canvas, 30, 30), (Pixel{255, 60, 60, 255}), 0);
    CHECK_EQ(canvas.history().undoCount(), 0);

    // Cancelar lo deja todo como estaba, sin paso de deshacer.
    CHECK(!canvas.endFill(false));
    CHECK(!canvas.filling());
    CHECK_EQ(test::maxDifference(composite(canvas), seenBefore), 0);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);
    CHECK_EQ(canvas.history().undoCount(), 0);

    // Con el umbral alto se aplica en todo.
    REQUIRE(canvas.beginFill(4.0f, 4.0f, 0.5f) == Canvas::Edit::Done);
    CHECK(canvas.endFill(true));
    CHECK_PIXEL(at(layerPixels(canvas, 0), canvas, 30, 30), kBlue, 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);
}

TEST_CASE(color_drop_preview_matches_commit) {
    Canvas canvas;
    REQUIRE(canvas.init(40, 40));
    drawFrame(canvas, 0, {6, 6, 34, 34});
    fillLayer(canvas, 0, {10, 18, 30, 22}, 0.2f, 0.6f, 0.3f, 0.7f);
    setColor(canvas, 0.9f, 0.5f, 0.1f);
    REQUIRE(canvas.beginFill(12.0f, 12.0f, 0.4f) == Canvas::Edit::Done);
    const std::vector<uint8_t> preview = composite(canvas);
    CHECK(canvas.endFill(true));
    CHECK_EQ(test::maxDifference(composite(canvas), preview), 0);
}

TEST_CASE(color_drop_uses_the_reference_layer) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    // Capa 1: el marco (el dibujo de líneas). Capa 2: una franja roja que lo cruza. Se
    // rellena en una capa nueva encima.
    fillLayer(canvas, 0, {8, 8, 24, 24}, 0.0f, 0.0f, 0.0f);
    test::fillRect(canvas.layers().at(0).target, {10, 10, 22, 22}, 0.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 1, {0, 0, 32, 16}, 1.0f, 0.0f, 0.0f);
    canvas.setReferenceLayer(0);
    REQUIRE(canvas.addLayer());
    REQUIRE(canvas.layers().activeIndex() == 2);
    setColor(canvas, 0.0f, 0.0f, 1.0f);

    REQUIRE(canvas.beginFill(16.0f, 16.0f, 0.1f) == Canvas::Edit::Done);
    CHECK(canvas.endFill(true));
    const std::vector<uint8_t> layer = layerPixels(canvas, 2);
    CHECK_PIXEL(at(layer, canvas, 12, 12), kBlue, 0);   // dentro del marco, bajo el rojo
    CHECK_PIXEL(at(layer, canvas, 12, 20), kBlue, 0);   // dentro, sin rojo
    CHECK_PIXEL(at(layer, canvas, 9, 16), kBlue, 0);    // bajo el borde de la línea
    CHECK_PIXEL(at(layer, canvas, 8, 16), kClear, 0);
    CHECK_PIXEL(at(layer, canvas, 4, 4), kClear, 0);
    CHECK_PIXEL(at(layer, canvas, 4, 28), kClear, 0);

    // Sin referencia cuenta lo que se ve: la franja roja corta la zona.
    REQUIRE(canvas.undo());
    canvas.setReferenceLayer(-1);
    canvas.selectLayer(2);
    REQUIRE(canvas.beginFill(16.0f, 20.0f, 0.1f) == Canvas::Edit::Done);
    CHECK(canvas.endFill(true));
    const std::vector<uint8_t> seen = layerPixels(canvas, 2);
    CHECK_PIXEL(at(seen, canvas, 12, 20), kBlue, 0);
    CHECK_PIXEL(at(seen, canvas, 12, 12), kClear, 0);
}

TEST_CASE(color_drop_respects_selection) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    REQUIRE(selectRect(canvas, {0, 0, 16, 32}));
    setColor(canvas, 0.0f, 0.0f, 1.0f);
    const int steps = canvas.history().undoCount();

    // Todo el lienzo es la misma zona blanca: solo cambia lo seleccionado.
    REQUIRE(canvas.beginFill(4.0f, 4.0f, 0.3f) == Canvas::Edit::Done);
    CHECK(canvas.endFill(true));
    const std::vector<uint8_t> layer = layerPixels(canvas, 0);
    CHECK_PIXEL(at(layer, canvas, 4, 4), kBlue, 0);
    CHECK_PIXEL(at(layer, canvas, 15, 30), kBlue, 0);
    CHECK_PIXEL(at(layer, canvas, 16, 4), kClear, 0);
    CHECK_PIXEL(at(layer, canvas, 30, 30), kClear, 0);
    CHECK_EQ(canvas.history().undoCount(), steps + 1);
    CHECK(canvas.hasSelection());

    // Una zona que cae entera fuera de la selección no cambia nada ni guarda un paso.
    fillLayer(canvas, 0, {24, 0, 32, 8}, 1.0f, 0.0f, 0.0f);
    REQUIRE(canvas.beginFill(28.0f, 4.0f, 0.1f) == Canvas::Edit::Done);
    CHECK(!canvas.endFill(true));
    CHECK_EQ(canvas.history().undoCount(), steps + 1);
    CHECK_PIXEL(at(layerPixels(canvas, 0), canvas, 28, 4), kRed, 0);
}

TEST_CASE(color_drop_with_alpha_lock_recolors_the_paint) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    // Rojo al 50 % sobre el fondo blanco; alrededor la capa está vacía.
    fillLayer(canvas, 0, {8, 8, 24, 24}, 1.0f, 0.0f, 0.0f, 0.5f);
    canvas.setLayerAlphaLock(0, true);
    setColor(canvas, 0.0f, 0.0f, 1.0f);

    REQUIRE(canvas.beginFill(16.0f, 16.0f, 0.3f) == Canvas::Edit::Done);
    CHECK(canvas.endFill(true));
    const std::vector<uint8_t> layer = layerPixels(canvas, 0);
    CHECK_PIXEL(at(layer, canvas, 16, 16), (Pixel{0, 0, 128, 128}), 1);   // azul, con su alfa
    CHECK_PIXEL(at(layer, canvas, 8, 8), (Pixel{0, 0, 128, 128}), 1);
    CHECK_PIXEL(at(layer, canvas, 7, 16), kClear, 0);   // sin el píxel por detrás
    CHECK_PIXEL(at(layer, canvas, 2, 2), kClear, 0);
}

TEST_CASE(color_drop_refuses_hidden_layers_and_points_outside) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    CHECK(canvas.beginFill(-1.0f, 4.0f, 0.3f) == Canvas::Edit::Nothing);
    CHECK(canvas.beginFill(4.0f, 16.0f, 0.3f) == Canvas::Edit::Nothing);
    canvas.setLayerVisible(0, false);
    CHECK(canvas.beginFill(4.0f, 4.0f, 0.3f) == Canvas::Edit::Hidden);
    CHECK(!canvas.filling());
    // Una capa que recorta con una base oculta tampoco se ve.
    canvas.setLayerVisible(0, true);
    REQUIRE(canvas.addLayer());
    canvas.setLayerClipping(1, true);
    canvas.setLayerVisible(0, false);
    canvas.selectLayer(1);
    CHECK(canvas.beginFill(4.0f, 4.0f, 0.3f) == Canvas::Edit::Hidden);
    CHECK(!canvas.filling());
}

TEST_CASE(color_drop_ends_before_other_edits_and_undo_removes_it) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    setColor(canvas, 1.0f, 0.0f, 0.0f);
    const std::vector<uint8_t> before = layerPixels(canvas, 0);

    // Deshacer en medio lo quita (y se puede rehacer, como la selección automática).
    REQUIRE(canvas.beginFill(4.0f, 4.0f, 0.3f) == Canvas::Edit::Done);
    REQUIRE(canvas.undo());
    CHECK(!canvas.filling());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);
    CHECK_PIXEL(at(composite(canvas), canvas, 4, 4), kWhite, 0);
    CHECK_EQ(canvas.history().undoCount(), 0);

    // Otra operación lo aplica antes de hacer la suya.
    REQUIRE(canvas.beginFill(4.0f, 4.0f, 0.3f) == Canvas::Edit::Done);
    REQUIRE(canvas.addLayer());
    CHECK(!canvas.filling());
    CHECK_PIXEL(at(layerPixels(canvas, 0), canvas, 4, 4), kRed, 0);
    CHECK_EQ(canvas.history().undoCount(), 2);
    // El buffer de trazo vuelve a quedar transparente: mientras se borra en otro sitio,
    // la vista previa del trazo no se lleva el relleno.
    canvas.selectLayer(0);
    BrushSettings& brush = canvas.brushSettings();
    brush.eraser = true;
    brush.radius = 1.5f;
    brush.opacity = 1.0f;
    REQUIRE(canvas.beginStroke(12.0f, 12.0f, 1.0f));
    canvas.strokeTo(13.0f, 12.0f, 1.0f);
    CHECK_PIXEL(at(composite(canvas), canvas, 4, 4), kRed, 0);
    canvas.cancelStroke();
    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 0), before), 0);
}
