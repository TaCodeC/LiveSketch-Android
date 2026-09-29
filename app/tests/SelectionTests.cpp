// Selección (formas, automática, sumar, restar, invertir, difuminar), lo que limita,
// portapapeles y transformar, con deshacer.

#include "Test.h"

#include "Canvas/Canvas.h"
#include "Canvas/SelectionShapes.h"

#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace {

using test::Pixel;

constexpr Pixel kWhite{255, 255, 255, 255};
constexpr Pixel kClear{0, 0, 0, 0};
constexpr Pixel kRed{255, 0, 0, 255};
constexpr Pixel kGreen{0, 255, 0, 255};
constexpr Pixel kBlue{0, 0, 255, 255};
constexpr float kPi = 3.14159265358979f;

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
void fillLayer(Canvas& canvas, int layer, const IRect& rect, float r, float g, float b, float a = 1.0f) {
    test::fillRect(canvas.layers().at(layer).target, rect, r * a, g * a, b * a, a);
    canvas.layers().markDirty(rect);
}

void setBrush(Canvas& canvas, float r, float g, float b, float opacity, float radius, bool eraser = false) {
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

bool selectRect(Canvas& canvas, const IRect& rect, SelectOp op = SelectOp::Replace) {
    const std::vector<glm::vec2> polygon = selection::rectangle(
        {static_cast<float>(rect.x0), static_cast<float>(rect.y0)}, {static_cast<float>(rect.x1), static_cast<float>(rect.y1)});
    return canvas.selectPolygon(polygon, op);
}

// La máscara de la selección como RGBA (el valor en el rojo); todo 0 sin selección.
std::vector<uint8_t> maskPixels(Canvas& canvas) {
    std::vector<uint8_t> rgba(static_cast<size_t>(canvas.width()) * static_cast<size_t>(canvas.height()) * 4, 0);
    const GLuint texture = canvas.selectionMask();
    if (texture == 0) {
        return rgba;
    }
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, canvas.width(), canvas.height(), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return rgba;
}

int maskAt(Canvas& canvas, int x, int y) { return test::pixelAt(maskPixels(canvas), canvas.width(), x, y)[0]; }

int coverageAt(const std::vector<uint8_t>& coverage, const IRect& box, int x, int y) {
    if (x < box.x0 || y < box.y0 || x >= box.x1 || y >= box.y1) {
        return 0;
    }
    return coverage[static_cast<size_t>(y - box.y0) * static_cast<size_t>(box.width()) + static_cast<size_t>(x - box.x0)];
}

bool near(const Pixel& a, const Pixel& b, int tolerance) {
    for (size_t i = 0; i < 4; ++i) {
        if (std::abs(a[i] - b[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

std::string describe(const IRect& rect) {
    return "{" + std::to_string(rect.x0) + ", " + std::to_string(rect.y0) + ", " + std::to_string(rect.x1) + ", " +
           std::to_string(rect.y1) + "}";
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

#define CHECK_RECT(actual, expected)                                                                     \
    do {                                                                                                 \
        const IRect actualRect = (actual);                                                               \
        const IRect expectedRect = (expected);                                                           \
        if (actualRect.x0 != expectedRect.x0 || actualRect.y0 != expectedRect.y0 ||                      \
            actualRect.x1 != expectedRect.x1 || actualRect.y1 != expectedRect.y1) {                      \
            test::fail(__FILE__, __LINE__,                                                               \
                       std::string(#actual) + " = " + describe(actualRect) + ", se esperaba " +          \
                           describe(expectedRect));                                                      \
        }                                                                                                \
    } while (0)

// Esquinas de `rect` movidas (tx, ty).
void moved(const IRect& rect, float tx, float ty, glm::vec2 out[4]) {
    out[0] = {static_cast<float>(rect.x0) + tx, static_cast<float>(rect.y0) + ty};
    out[1] = {static_cast<float>(rect.x1) + tx, static_cast<float>(rect.y0) + ty};
    out[2] = {static_cast<float>(rect.x1) + tx, static_cast<float>(rect.y1) + ty};
    out[3] = {static_cast<float>(rect.x0) + tx, static_cast<float>(rect.y1) + ty};
}

} // namespace

// -----------------------------------------------------------------------------
// Formas y selección automática (CPU)
// -----------------------------------------------------------------------------

TEST_CASE(selection_rasterize_shapes) {
    const IRect clip = IRect::ofSize(40, 40);
    std::vector<uint8_t> coverage;
    IRect box;

    // Esquinas en píxeles enteros, en cualquier orden: todo entero.
    REQUIRE(selection::rasterize(selection::rectangle({10.0f, 8.0f}, {2.0f, 3.0f}), clip, coverage, box));
    CHECK_RECT(box, (IRect{2, 3, 10, 8}));
    CHECK(std::all_of(coverage.begin(), coverage.end(), [](uint8_t v) { return v == 255; }));

    // Un borde a medio píxel deja esa columna a medias.
    REQUIRE(selection::rasterize(selection::rectangle({2.5f, 3.0f}, {10.0f, 8.0f}), clip, coverage, box));
    CHECK_EQ(box.x0, 2);
    CHECK_NEAR(coverageAt(coverage, box, 2, 5), 128, 1);
    CHECK_EQ(coverageAt(coverage, box, 3, 5), 255);

    // Lo que se sale se recorta.
    REQUIRE(selection::rasterize(selection::rectangle({-5.0f, -5.0f}, {4.0f, 4.0f}), clip, coverage, box));
    CHECK_RECT(box, (IRect{0, 0, 4, 4}));
    CHECK(std::all_of(coverage.begin(), coverage.end(), [](uint8_t v) { return v == 255; }));

    // Un lazo que se cruza (una estrella de cinco puntas) queda relleno entero.
    std::vector<glm::vec2> star;
    for (int i = 0; i < 5; ++i) {
        const float t = -0.5f * kPi + static_cast<float>(i) * 4.0f * kPi / 5.0f;
        star.push_back({20.0f + 15.0f * std::cos(t), 20.0f + 15.0f * std::sin(t)});
    }
    REQUIRE(selection::rasterize(star, clip, coverage, box));
    CHECK_EQ(coverageAt(coverage, box, 20, 20), 255);
    CHECK_EQ(coverageAt(coverage, box, 6, 31), 0);

    // Elipse: el centro dentro, las esquinas de su caja fuera.
    REQUIRE(selection::rasterize(selection::ellipse({0.0f, 0.0f}, {20.0f, 10.0f}), clip, coverage, box));
    CHECK_EQ(coverageAt(coverage, box, 10, 5), 255);
    CHECK_EQ(coverageAt(coverage, box, 0, 0), 0);
    CHECK_EQ(coverageAt(coverage, box, 19, 9), 0);

    // Nada que rellenar.
    CHECK(!selection::rasterize(selection::rectangle({50.0f, 50.0f}, {60.0f, 60.0f}), clip, coverage, box));
    CHECK(!selection::rasterize(selection::rectangle({3.0f, 3.0f}, {3.0f, 9.0f}), clip, coverage, box));
    const std::vector<glm::vec2> line = {{1.0f, 1.0f}, {8.0f, 8.0f}};
    CHECK(!selection::rasterize(line, clip, coverage, box));
}

TEST_CASE(selection_auto_levels) {
    // Blanco, una columna gris y casi blanco detrás: lo de detrás solo entra cuando el
    // umbral deja pasar la columna.
    constexpr int kWidth = 8;
    constexpr int kHeight = 3;
    std::vector<uint8_t> image(kWidth * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const uint8_t v = x < 3 ? 255 : (x == 3 ? 200 : 250);
            uint8_t* p = image.data() + (y * kWidth + x) * 4;
            p[0] = v;
            p[1] = v;
            p[2] = v;
            p[3] = 255;
        }
    }
    selection::AutoLevels levels;
    REQUIRE(selection::autoLevels(image.data(), kWidth, kHeight, 1, 1, levels));
    CHECK_EQ(static_cast<int>(levels.levels[0]), 0);
    CHECK_EQ(static_cast<int>(levels.levels[3]), 55);
    CHECK_EQ(static_cast<int>(levels.levels[7]), 55);   // el peor escalón del camino
    CHECK_RECT(levels.bounds[0], (IRect{0, 0, 3, 3}));
    CHECK_RECT(levels.bounds[54], (IRect{0, 0, 3, 3}));
    CHECK_RECT(levels.bounds[55], (IRect{0, 0, 8, 3}));
    CHECK_RECT(levels.bounds[255], (IRect{0, 0, 8, 3}));
    CHECK(!selection::autoLevels(image.data(), kWidth, kHeight, 8, 0, levels));

    CHECK_EQ(selection::autoCutoff(0.0f), 0);
    CHECK_EQ(selection::autoCutoff(0.5f), 127);
    CHECK_EQ(selection::autoCutoff(1.0f), 254);
    CHECK_EQ(selection::autoCutoff(3.0f), 254);
}

// -----------------------------------------------------------------------------
// Crear y combinar
// -----------------------------------------------------------------------------

TEST_CASE(selection_add_subtract_invert_and_undo) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    CHECK(!canvas.hasSelection());
    // Restar sin selección no hace nada.
    CHECK(!selectRect(canvas, {0, 0, 8, 8}, SelectOp::Subtract));

    REQUIRE(selectRect(canvas, {0, 0, 8, 8}));
    CHECK(canvas.hasSelection());
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 8, 8}));
    CHECK_EQ(maskAt(canvas, 4, 4), 255);
    CHECK_EQ(maskAt(canvas, 8, 4), 0);

    REQUIRE(selectRect(canvas, {16, 16, 24, 24}, SelectOp::Add));
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 24, 24}));
    CHECK_EQ(maskAt(canvas, 4, 4), 255);
    CHECK_EQ(maskAt(canvas, 20, 20), 255);
    CHECK_EQ(maskAt(canvas, 12, 12), 0);

    // Restar ajusta la caja a lo que queda.
    REQUIRE(selectRect(canvas, {0, 0, 8, 4}, SelectOp::Subtract));
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 4, 24, 24}));
    CHECK_EQ(maskAt(canvas, 4, 2), 0);
    CHECK_EQ(maskAt(canvas, 4, 6), 255);

    canvas.invertSelection();
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 32, 32}));
    CHECK_EQ(maskAt(canvas, 12, 12), 255);
    CHECK_EQ(maskAt(canvas, 4, 6), 0);
    CHECK_EQ(maskAt(canvas, 20, 20), 0);
    CHECK_EQ(canvas.history().undoCount(), 4);

    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 8, 8}));
    CHECK_EQ(maskAt(canvas, 4, 2), 255);
    CHECK_EQ(maskAt(canvas, 20, 20), 0);
    CHECK_EQ(maskAt(canvas, 12, 12), 0);
    REQUIRE(canvas.redo());
    REQUIRE(canvas.redo());
    REQUIRE(canvas.redo());
    CHECK_EQ(maskAt(canvas, 12, 12), 255);
    CHECK_EQ(maskAt(canvas, 4, 6), 0);

    // Deseleccionar y deshacerlo.
    canvas.deselect();
    CHECK(!canvas.hasSelection());
    CHECK_EQ(canvas.selectionMask(), 0u);
    REQUIRE(canvas.undo());
    CHECK(canvas.hasSelection());
    CHECK_EQ(maskAt(canvas, 12, 12), 255);

    // Seleccionar todo; invertir sin selección también selecciona todo.
    canvas.selectAll();
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 32, 32}));
    CHECK_EQ(maskAt(canvas, 4, 6), 255);
    canvas.deselect();
    canvas.invertSelection();
    CHECK(canvas.hasSelection());
    CHECK_EQ(maskAt(canvas, 31, 31), 255);
}

TEST_CASE(selection_replace_clears_what_was_left) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    REQUIRE(selectRect(canvas, {0, 0, 8, 8}));
    canvas.deselect();
    // La máscara de antes se quedó (para deshacer); la selección nueva no la incluye.
    REQUIRE(selectRect(canvas, {16, 16, 24, 24}));
    CHECK_RECT(canvas.selectionBounds(), (IRect{16, 16, 24, 24}));
    CHECK_EQ(maskAt(canvas, 4, 4), 0);
    CHECK_EQ(maskAt(canvas, 20, 20), 255);

    REQUIRE(canvas.undo());
    CHECK(!canvas.hasSelection());
    REQUIRE(canvas.undo());
    CHECK(canvas.hasSelection());
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 8, 8}));
    CHECK_EQ(maskAt(canvas, 4, 4), 255);
    CHECK_EQ(maskAt(canvas, 20, 20), 0);
}

TEST_CASE(selection_of_layer_content) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    CHECK(canvas.selectLayerContent(1) == Canvas::Edit::Nothing);
    CHECK(!canvas.hasSelection());

    fillLayer(canvas, 1, {4, 6, 10, 12}, 1.0f, 0.0f, 0.0f, 0.5f);
    fillLayer(canvas, 1, {20, 20, 22, 30}, 0.0f, 0.0f, 1.0f, 1.0f);
    CHECK(canvas.selectLayerContent(1) == Canvas::Edit::Done);
    CHECK_RECT(canvas.selectionBounds(), (IRect{4, 6, 22, 30}));
    CHECK_NEAR(maskAt(canvas, 5, 7), 128, 1);   // con el alfa de la capa
    CHECK_EQ(maskAt(canvas, 21, 25), 255);
    CHECK_EQ(maskAt(canvas, 15, 15), 0);
    REQUIRE(canvas.undo());
    CHECK(!canvas.hasSelection());
}

TEST_CASE(selection_auto_select_threshold_and_undo) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    // Rojo a la izquierda; a la derecha se ve el fondo blanco.
    fillLayer(canvas, 1, {0, 0, 16, 32}, 1.0f, 0.0f, 0.0f);
    canvas.update();

    REQUIRE(canvas.beginAutoSelect(4.0f, 4.0f, SelectOp::Replace, 0.1f));
    CHECK(canvas.autoSelecting());
    CHECK(canvas.canUndo());
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 16, 32}));
    CHECK_EQ(maskAt(canvas, 4, 4), 255);
    CHECK_EQ(maskAt(canvas, 20, 4), 0);
    // Todo el umbral: entra todo; y se puede volver atrás en vivo.
    canvas.setAutoThreshold(1.0f);
    CHECK_EQ(maskAt(canvas, 20, 4), 255);
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 32, 32}));
    canvas.setAutoThreshold(0.1f);
    CHECK_EQ(maskAt(canvas, 20, 4), 0);
    CHECK_EQ(canvas.history().undoCount(), 0);
    canvas.endAutoSelect(true);
    CHECK(!canvas.autoSelecting());
    CHECK_EQ(canvas.history().undoCount(), 1);
    CHECK_EQ(maskAt(canvas, 4, 4), 255);

    REQUIRE(canvas.undo());
    CHECK(!canvas.hasSelection());
    REQUIRE(canvas.redo());
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 16, 32}));
    CHECK_EQ(maskAt(canvas, 20, 4), 0);

    // Restar con la automática: queda la parte blanca, con la caja ajustada.
    canvas.selectAll();
    REQUIRE(canvas.beginAutoSelect(4.0f, 4.0f, SelectOp::Subtract, 0.1f));
    canvas.endAutoSelect(true);
    CHECK_RECT(canvas.selectionBounds(), (IRect{16, 0, 32, 32}));
    CHECK_EQ(maskAt(canvas, 4, 4), 0);
    CHECK_EQ(maskAt(canvas, 20, 4), 255);

    // Cancelar la deja como estaba y no guarda nada.
    const int steps = canvas.history().undoCount();
    REQUIRE(canvas.beginAutoSelect(20.0f, 4.0f, SelectOp::Add, 1.0f));
    CHECK_EQ(maskAt(canvas, 4, 4), 255);
    canvas.endAutoSelect(false);
    CHECK_EQ(maskAt(canvas, 4, 4), 0);
    CHECK_RECT(canvas.selectionBounds(), (IRect{16, 0, 32, 32}));
    CHECK_EQ(canvas.history().undoCount(), steps);

    // Deshacer en medio la quita.
    REQUIRE(canvas.beginAutoSelect(20.0f, 4.0f, SelectOp::Add, 1.0f));
    REQUIRE(canvas.undo());
    CHECK(!canvas.autoSelecting());
    CHECK_EQ(maskAt(canvas, 4, 4), 0);
    CHECK_EQ(canvas.history().undoCount(), steps);
}

TEST_CASE(selection_auto_select_uses_reference_layer) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    // Capa 1: un marco negro (el dibujo de líneas). Capa 2: rojo por arriba, que cruza
    // el marco.
    fillLayer(canvas, 1, {8, 8, 24, 24}, 0.0f, 0.0f, 0.0f);
    test::fillRect(canvas.layers().at(1).target, {10, 10, 22, 22}, 0.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE(canvas.addLayer());
    fillLayer(canvas, 2, {0, 0, 32, 16}, 1.0f, 0.0f, 0.0f);
    canvas.setReferenceLayer(1);
    canvas.update();

    REQUIRE(canvas.beginAutoSelect(16.0f, 16.0f, SelectOp::Replace, 0.1f));
    canvas.endAutoSelect(true);
    CHECK_RECT(canvas.selectionBounds(), (IRect{10, 10, 22, 22}));
    CHECK_EQ(maskAt(canvas, 12, 12), 255);   // dentro del marco, bajo el rojo
    CHECK_EQ(maskAt(canvas, 12, 20), 255);   // dentro del marco, sin rojo
    CHECK_EQ(maskAt(canvas, 4, 4), 0);
    CHECK_EQ(maskAt(canvas, 9, 16), 0);      // el marco
}

TEST_CASE(selection_feather_softens_the_edge) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    CHECK(!canvas.beginFeather());   // sin selección
    REQUIRE(selectRect(canvas, {16, 16, 48, 48}));
    const int steps = canvas.history().undoCount();

    REQUIRE(canvas.beginFeather());
    CHECK(canvas.feathering());
    canvas.setFeather(8.0f);
    CHECK_NEAR(canvas.featherRadius(), 8.0f, 1e-4);
    // Perfil del borde izquierdo en la fila del centro: una gaussiana de sigma 4.
    const std::vector<uint8_t> mask = maskPixels(canvas);
    for (int x = 4; x < 32; ++x) {
        const double expected = 255.0 * 0.5 * std::erfc(-(x + 0.5 - 16.0) / (4.0 * std::sqrt(2.0)));
        CHECK_NEAR(test::pixelAt(mask, 64, x, 32)[0], expected, 14);
    }
    CHECK_NEAR(maskAt(canvas, 32, 32), 255, 2);
    // Soltar a 0 vuelve al borde de antes.
    canvas.setFeather(0.0f);
    CHECK_EQ(maskAt(canvas, 16, 32), 255);
    CHECK_EQ(maskAt(canvas, 15, 32), 0);
    canvas.setFeather(8.0f);
    canvas.endFeather(true);
    CHECK(!canvas.feathering());
    CHECK_EQ(canvas.history().undoCount(), steps + 1);
    const int soft = maskAt(canvas, 16, 32);
    CHECK(soft > 90 && soft < 170);
    // La caja crece con el difuminado.
    const IRect bounds = canvas.selectionBounds();
    CHECK(bounds.x0 < 16 && bounds.y0 < 16 && bounds.x1 > 48 && bounds.y1 > 48);

    REQUIRE(canvas.undo());
    CHECK_EQ(maskAt(canvas, 16, 32), 255);
    CHECK_EQ(maskAt(canvas, 15, 32), 0);
    CHECK_RECT(canvas.selectionBounds(), (IRect{16, 16, 48, 48}));
    REQUIRE(canvas.redo());
    CHECK_EQ(maskAt(canvas, 16, 32), soft);

    // Cancelar no cambia nada.
    REQUIRE(canvas.beginFeather());
    canvas.setFeather(20.0f);
    canvas.endFeather(false);
    CHECK_EQ(maskAt(canvas, 16, 32), soft);
    CHECK_EQ(canvas.history().undoCount(), steps + 1);
    // Como mucho, maxFeather().
    REQUIRE(canvas.beginFeather());
    canvas.setFeather(1000.0f);
    CHECK_NEAR(canvas.featherRadius(), canvas.maxFeather(), 1e-4);
    canvas.endFeather(false);
}

// -----------------------------------------------------------------------------
// Lo que limita la selección
// -----------------------------------------------------------------------------

TEST_CASE(selection_limits_strokes) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    REQUIRE(selectRect(canvas, {16, 16, 48, 48}));

    setBrush(canvas, 1.0f, 0.0f, 0.0f, 1.0f, 4.0f);
    // Mientras se pinta, lo de fuera ya no se ve.
    REQUIRE(canvas.beginStroke(4.0f, 32.0f, 1.0f));
    canvas.strokeTo(60.0f, 32.0f, 1.0f);
    CHECK_PIXEL(compositeAt(canvas, 8, 32), kWhite, 0);
    CHECK(compositeAt(canvas, 32, 32)[1] <= 5);
    canvas.endStroke();
    canvas.update();
    CHECK_PIXEL(layerAt(canvas, 1, 8, 32), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 56, 32), kClear, 0);
    CHECK(layerAt(canvas, 1, 32, 32)[3] >= 250);
    CHECK_PIXEL(compositeAt(canvas, 56, 32), kWhite, 0);
    CHECK(canvas.undo());
    CHECK_PIXEL(layerAt(canvas, 1, 32, 32), kClear, 0);
    CHECK(canvas.hasSelection());   // deshacer el trazo no toca la selección

    // Borrar, también solo dentro.
    fillLayer(canvas, 1, {0, 0, 64, 64}, 0.0f, 0.0f, 1.0f);
    setBrush(canvas, 0.0f, 0.0f, 0.0f, 1.0f, 4.0f, true);
    drawLine(canvas, {4.0f, 32.0f}, {60.0f, 32.0f});
    CHECK_PIXEL(layerAt(canvas, 1, 8, 32), kBlue, 0);
    CHECK(layerAt(canvas, 1, 32, 32)[3] <= 5);
    CHECK_PIXEL(layerAt(canvas, 1, 56, 32), kBlue, 0);
}

TEST_CASE(selection_stroke_preview_matches_commit) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    fillLayer(canvas, 1, {0, 0, 64, 64}, 0.2f, 0.5f, 0.9f, 0.7f);
    // Elipse con el borde suavizado.
    REQUIRE(canvas.selectPolygon(selection::ellipse({10.0f, 12.0f}, {54.0f, 50.0f}), SelectOp::Replace));
    setBrush(canvas, 0.9f, 0.3f, 0.1f, 0.6f, 9.0f);
    REQUIRE(canvas.beginStroke(2.0f, 30.0f, 1.0f));
    canvas.strokeTo(62.0f, 34.0f, 1.0f);
    const std::vector<uint8_t> preview = composite(canvas);
    canvas.endStroke();
    const std::vector<uint8_t> committed = composite(canvas);
    CHECK(test::maxDifference(preview, committed) <= 2);
}

TEST_CASE(selection_limits_fill_clear_and_invert) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    fillLayer(canvas, 1, {0, 0, 32, 32}, 0.0f, 0.0f, 1.0f);
    const std::vector<uint8_t> original = layerPixels(canvas, 1);
    // El borde izquierdo a medio píxel: esa columna, a medias.
    REQUIRE(canvas.selectPolygon(selection::rectangle({8.5f, 8.0f}, {24.0f, 24.0f}), SelectOp::Replace));
    const float red[3] = {1.0f, 0.0f, 0.0f};

    canvas.fillLayer(1, red);
    CHECK_PIXEL(layerAt(canvas, 1, 16, 16), kRed, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 4, 16), kBlue, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 8, 16), (Pixel{128, 0, 127, 255}), 1);

    canvas.invertLayer(1);
    CHECK_PIXEL(layerAt(canvas, 1, 16, 16), (Pixel{0, 255, 255, 255}), 0);
    CHECK_PIXEL(layerAt(canvas, 1, 4, 16), kBlue, 0);

    canvas.clearLayer(1);
    CHECK_PIXEL(layerAt(canvas, 1, 16, 16), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 4, 16), kBlue, 0);
    CHECK_NEAR(layerAt(canvas, 1, 8, 16)[3], 127, 1);

    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);

    // Con el alfa bloqueado, rellenar solo llega a lo que tiene pintura dentro.
    canvas.clearLayer(1);   // la selección
    canvas.setLayerAlphaLock(1, true);
    canvas.fillLayer(1, red);
    CHECK_PIXEL(layerAt(canvas, 1, 16, 16), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 4, 16), kBlue, 0);
}

// -----------------------------------------------------------------------------
// Portapapeles
// -----------------------------------------------------------------------------

TEST_CASE(selection_copy_cut_paste_duplicate) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    fillLayer(canvas, 1, {4, 4, 12, 12}, 1.0f, 0.0f, 0.0f);
    fillLayer(canvas, 1, {20, 20, 28, 28}, 0.0f, 1.0f, 0.0f);
    const std::vector<uint8_t> original = layerPixels(canvas, 1);
    CHECK(!canvas.canPaste());
    CHECK(canvas.paste() == Canvas::Edit::Nothing);

    // Copiar lo seleccionado y pegarlo en una capa nueva, en su sitio.
    REQUIRE(selectRect(canvas, {0, 0, 16, 16}));
    CHECK(canvas.copySelection() == Canvas::Edit::Done);
    CHECK(canvas.canPaste());
    CHECK(canvas.paste() == Canvas::Edit::Done);
    REQUIRE(canvas.layers().count() == 3);
    CHECK_EQ(canvas.layers().activeIndex(), 2);
    CHECK_EQ(canvas.layers().at(2).name, std::string("Pegado"));
    CHECK_PIXEL(layerAt(canvas, 2, 8, 8), kRed, 0);
    CHECK_PIXEL(layerAt(canvas, 2, 24, 24), kClear, 0);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);
    // Pegar quita la selección en el mismo paso: deshacer quita la capa y la devuelve.
    CHECK(!canvas.hasSelection());
    REQUIRE(canvas.undo());
    CHECK_EQ(canvas.layers().count(), 2);
    CHECK(canvas.hasSelection());
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 16, 16}));
    REQUIRE(canvas.redo());
    CHECK_EQ(canvas.layers().count(), 3);
    CHECK(!canvas.hasSelection());
    CHECK_PIXEL(layerAt(canvas, 2, 8, 8), kRed, 0);
    // Otra vez: otro nombre.
    CHECK(canvas.paste() == Canvas::Edit::Done);
    CHECK_EQ(canvas.layers().active().name, std::string("Pegado 2"));
    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());

    // Cortar: se va de la capa.
    canvas.selectLayer(1);
    CHECK(canvas.cutSelection() == Canvas::Edit::Done);
    CHECK_PIXEL(layerAt(canvas, 1, 8, 8), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 24, 24), kGreen, 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);

    // Duplicar lo seleccionado: a una capa nueva encima, con el nombre de la capa.
    REQUIRE(selectRect(canvas, {16, 16, 32, 32}));
    CHECK(canvas.duplicateSelection() == Canvas::Edit::Done);
    REQUIRE(canvas.layers().count() == 3);
    CHECK_EQ(canvas.layers().at(2).name, std::string("Capa 1 copia"));
    CHECK_PIXEL(layerAt(canvas, 2, 24, 24), kGreen, 0);
    CHECK_PIXEL(layerAt(canvas, 2, 8, 8), kClear, 0);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);
    REQUIRE(canvas.undo());
    CHECK_EQ(canvas.layers().count(), 2);

    // Nada seleccionado en la capa: nada que copiar.
    canvas.selectLayer(1);
    REQUIRE(selectRect(canvas, {0, 20, 4, 24}));
    CHECK(canvas.copySelection() == Canvas::Edit::Nothing);
    CHECK(canvas.duplicateSelection() == Canvas::Edit::Nothing);

    // Sin selección, copiar toma la capa entera y duplicar duplica la capa.
    canvas.deselect();
    CHECK(canvas.copySelection() == Canvas::Edit::Done);
    CHECK(canvas.paste() == Canvas::Edit::Done);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 2), original), 0);
    REQUIRE(canvas.undo());
    canvas.selectLayer(1);
    CHECK(canvas.duplicateSelection() == Canvas::Edit::Done);
    CHECK_EQ(canvas.layers().count(), 3);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 2), original), 0);
}

TEST_CASE(selection_pasted_and_duplicated_layers_move_whole) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    fillLayer(canvas, 1, {0, 0, 32, 32}, 1.0f, 0.0f, 0.0f);
    // Elipse de bordes suaves: lo copiado ya lleva esos bordes.
    REQUIRE(canvas.selectPolygon(selection::ellipse({4.0f, 4.0f}, {28.0f, 28.0f}), SelectOp::Replace));
    CHECK(canvas.copySelection() == Canvas::Edit::Done);

    // Pegar y mover: se mueve toda la capa nueva, sin enmascararla otra vez (ni bordes
    // más tenues ni restos en su sitio).
    REQUIRE(canvas.paste() == Canvas::Edit::Done);
    const std::vector<uint8_t> pasted = layerPixels(canvas, 2);
    REQUIRE(canvas.beginTransform(true) == Canvas::Edit::Done);
    glm::vec2 corners[4];
    moved(canvas.transformSource(), 32.0f, 32.0f, corners);
    REQUIRE(canvas.setTransform(corners, false));
    canvas.applyTransform();
    const std::vector<uint8_t> after = layerPixels(canvas, 2);
    int ghost = 0;
    int worst = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            ghost = std::max(ghost, static_cast<int>(test::pixelAt(after, 64, x, y)[3]));
            const Pixel from = test::pixelAt(pasted, 64, x, y);
            const Pixel to = test::pixelAt(after, 64, x + 32, y + 32);
            for (size_t c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(from[c] - to[c]));
            }
        }
    }
    CHECK_EQ(ghost, 0);
    CHECK(worst <= 1);
    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    CHECK_EQ(canvas.layers().count(), 2);
    REQUIRE(canvas.hasSelection());

    // Duplicar y mover: igual, y la selección se mueve con lo duplicado.
    REQUIRE(canvas.duplicateSelection() == Canvas::Edit::Done);
    const std::vector<uint8_t> duplicated = layerPixels(canvas, 2);
    const IRect selected = canvas.selectionBounds();
    REQUIRE(canvas.beginTransform(true) == Canvas::Edit::Done);
    moved(canvas.transformSource(), 32.0f, 0.0f, corners);
    REQUIRE(canvas.setTransform(corners, false));
    canvas.applyTransform();
    const std::vector<uint8_t> shifted = layerPixels(canvas, 2);
    ghost = 0;
    worst = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            ghost = std::max(ghost, static_cast<int>(test::pixelAt(shifted, 64, x, y)[3]));
            const Pixel from = test::pixelAt(duplicated, 64, x, y);
            const Pixel to = test::pixelAt(shifted, 64, x + 32, y);
            for (size_t c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(from[c] - to[c]));
            }
        }
    }
    CHECK_EQ(ghost, 0);
    CHECK(worst <= 1);
    CHECK(canvas.hasSelection());
    CHECK_RECT(canvas.selectionBounds(), (IRect{selected.x0 + 32, selected.y0, selected.x1 + 32, selected.y1}));
}

TEST_CASE(selection_redo_does_not_commit_live_edits) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    REQUIRE(selectRect(canvas, {4, 4, 28, 28}));
    setBrush(canvas, 0.0f, 0.0f, 1.0f, 1.0f, 3.0f);
    drawLine(canvas, {8.0f, 16.0f}, {24.0f, 16.0f});
    const std::vector<uint8_t> painted = layerPixels(canvas, 1);
    REQUIRE(canvas.undo());
    REQUIRE(canvas.canRedo());

    // Difuminando en vivo no hay nada que rehacer, y rehacer no guarda el difuminado
    // (eso borraría el trazo que se puede rehacer).
    REQUIRE(canvas.beginFeather());
    canvas.setFeather(2.0f);
    CHECK(!canvas.canRedo());
    CHECK(!canvas.redo());
    CHECK(canvas.feathering());
    canvas.endFeather(false);
    REQUIRE(canvas.canRedo());
    REQUIRE(canvas.redo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), painted), 0);
}

TEST_CASE(selection_copy_keeps_soft_edges_and_survives_new_canvas) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    fillLayer(canvas, 1, {0, 0, 32, 32}, 0.0f, 0.0f, 1.0f);
    REQUIRE(canvas.selectPolygon(selection::rectangle({8.5f, 8.0f}, {16.0f, 16.0f}), SelectOp::Replace));
    CHECK(canvas.copySelection() == Canvas::Edit::Done);

    // En un lienzo más pequeño cae fuera: se pega centrado.
    REQUIRE(canvas.init(8, 8));
    CHECK(canvas.canPaste());
    CHECK(canvas.paste() == Canvas::Edit::Done);
    REQUIRE(canvas.layers().count() == 3);
    // El borde a medias (x = 8 en el original) es la primera columna de lo copiado.
    CHECK_PIXEL(layerAt(canvas, 2, 0, 4), (Pixel{0, 0, 128, 128}), 1);
    CHECK_PIXEL(layerAt(canvas, 2, 4, 4), kBlue, 0);
}

// -----------------------------------------------------------------------------
// Transformar
// -----------------------------------------------------------------------------

TEST_CASE(transform_moves_the_layer) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    CHECK(canvas.beginTransform() == Canvas::Edit::Nothing);   // capa vacía
    fillLayer(canvas, 1, {8, 8, 24, 24}, 1.0f, 0.0f, 0.0f);
    const std::vector<uint8_t> original = layerPixels(canvas, 1);

    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    CHECK(canvas.transforming());
    CHECK_RECT(canvas.transformSource(), (IRect{8, 8, 24, 24}));
    glm::vec2 corners[4];
    moved(canvas.transformSource(), 20.0f, 10.0f, corners);
    REQUIRE(canvas.setTransform(corners, false));
    // Se ve movido, pero la capa no cambia hasta aplicar.
    CHECK_PIXEL(compositeAt(canvas, 16, 16), kWhite, 0);
    CHECK_PIXEL(compositeAt(canvas, 36, 26), kRed, 1);
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);

    canvas.applyTransform();
    CHECK(!canvas.transforming());
    CHECK_PIXEL(layerAt(canvas, 1, 16, 16), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 36, 26), kRed, 1);
    CHECK_PIXEL(layerAt(canvas, 1, 28, 18), kRed, 1);
    CHECK_PIXEL(layerAt(canvas, 1, 27, 18), kClear, 1);
    CHECK_PIXEL(layerAt(canvas, 1, 43, 33), kRed, 1);
    CHECK_PIXEL(layerAt(canvas, 1, 44, 33), kClear, 1);
    CHECK_PIXEL(compositeAt(canvas, 16, 16), kWhite, 0);
    const std::vector<uint8_t> after = layerPixels(canvas, 1);

    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);
    REQUIRE(canvas.redo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), after), 0);

    // Una capa oculta no se transforma.
    canvas.setLayerVisible(1, false);
    CHECK(canvas.beginTransform() == Canvas::Edit::Hidden);
}

TEST_CASE(transform_cancel_undo_and_settle) {
    Canvas canvas;
    REQUIRE(canvas.init(48, 48));
    fillLayer(canvas, 1, {8, 8, 16, 16}, 0.0f, 1.0f, 0.0f);
    const std::vector<uint8_t> original = layerPixels(canvas, 1);
    const std::vector<uint8_t> image = composite(canvas);
    glm::vec2 corners[4];

    // Cancelar.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    moved(canvas.transformSource(), 16.0f, 0.0f, corners);
    REQUIRE(canvas.setTransform(corners, false));
    canvas.cancelTransform();
    CHECK(!canvas.transforming());
    CHECK_EQ(test::maxDifference(composite(canvas), image), 0);
    CHECK_EQ(canvas.history().undoCount(), 0);

    // Deshacer durante la transformación la cancela sin tocar el historial.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    CHECK(canvas.canUndo());
    CHECK(!canvas.canRedo());
    REQUIRE(canvas.setTransform(corners, false));
    REQUIRE(canvas.undo());
    CHECK(!canvas.transforming());
    CHECK_EQ(test::maxDifference(composite(canvas), image), 0);
    CHECK_EQ(canvas.history().undoCount(), 0);

    // Sin mover nada, aplicar no guarda nada.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    canvas.applyTransform();
    CHECK_EQ(canvas.history().undoCount(), 0);

    // Otra operación aplica antes la transformación.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    REQUIRE(canvas.setTransform(corners, false));
    REQUIRE(canvas.addLayer());
    CHECK(!canvas.transforming());
    CHECK_PIXEL(layerAt(canvas, 1, 28, 12), kGreen, 1);
    CHECK_PIXEL(layerAt(canvas, 1, 12, 12), kClear, 0);
    REQUIRE(canvas.undo());   // la capa nueva
    REQUIRE(canvas.undo());   // la transformación
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);

    // Un cuadrilátero que se cruza no vale.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    const glm::vec2 crossed[4] = {{8.0f, 8.0f}, {16.0f, 8.0f}, {8.0f, 16.0f}, {16.0f, 16.0f}};
    CHECK(!canvas.setTransform(crossed, false));
    canvas.cancelTransform();
}

TEST_CASE(transform_flip_rotate_scale_and_distort) {
    Canvas canvas;
    REQUIRE(canvas.init(40, 40));
    // Mitad izquierda roja, mitad derecha verde.
    fillLayer(canvas, 1, {8, 12, 16, 20}, 1.0f, 0.0f, 0.0f);
    fillLayer(canvas, 1, {16, 12, 24, 20}, 0.0f, 1.0f, 0.0f);

    // Voltear en horizontal: las esquinas de la izquierda van a la derecha.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    const glm::vec2 flipped[4] = {{24.0f, 12.0f}, {8.0f, 12.0f}, {8.0f, 20.0f}, {24.0f, 20.0f}};
    REQUIRE(canvas.setTransform(flipped, false));
    canvas.applyTransform();
    CHECK_PIXEL(layerAt(canvas, 1, 10, 16), kGreen, 1);
    CHECK_PIXEL(layerAt(canvas, 1, 21, 16), kRed, 1);
    REQUIRE(canvas.undo());

    // Girar 90° a la derecha alrededor del centro (16, 16).
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    const glm::vec2 rotated[4] = {{20.0f, 8.0f}, {20.0f, 24.0f}, {12.0f, 24.0f}, {12.0f, 8.0f}};
    REQUIRE(canvas.setTransform(rotated, false));
    canvas.applyTransform();
    CHECK_PIXEL(layerAt(canvas, 1, 16, 10), kRed, 1);     // lo de la izquierda, arriba
    CHECK_PIXEL(layerAt(canvas, 1, 16, 21), kGreen, 1);   // lo de la derecha, abajo
    CHECK_PIXEL(layerAt(canvas, 1, 22, 16), kClear, 1);
    REQUIRE(canvas.undo());

    // Escalar al doble: suave en el borde, o nítido.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    const glm::vec2 scaled[4] = {{8.0f, 12.0f}, {40.0f, 12.0f}, {40.0f, 28.0f}, {8.0f, 28.0f}};
    REQUIRE(canvas.setTransform(scaled, false));
    canvas.update();
    const Pixel softCorner = compositeAt(canvas, 8, 12);
    CHECK(softCorner[1] > 0 && softCorner[1] < 255);   // el rojo se mezcla con el blanco
    REQUIRE(canvas.setTransform(scaled, true));
    CHECK_PIXEL(compositeAt(canvas, 8, 12), kRed, 0);
    canvas.applyTransform();
    CHECK_PIXEL(layerAt(canvas, 1, 8, 12), kRed, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 20, 20), kRed, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 30, 20), kGreen, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 30, 30), kClear, 0);
    REQUIRE(canvas.undo());

    // Reducir a la mitad (con mipmaps): el centro sigue siendo del color.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    const glm::vec2 half[4] = {{8.0f, 12.0f}, {16.0f, 12.0f}, {16.0f, 16.0f}, {8.0f, 16.0f}};
    REQUIRE(canvas.setTransform(half, false));
    canvas.applyTransform();
    CHECK_PIXEL(layerAt(canvas, 1, 9, 14), kRed, 2);
    CHECK_PIXEL(layerAt(canvas, 1, 14, 14), kGreen, 2);
    CHECK_PIXEL(layerAt(canvas, 1, 20, 14), kClear, 0);
    REQUIRE(canvas.undo());

    // Distorsionar: un trapecio, más estrecho arriba. Con perspectiva, la mitad de arriba
    // de la fuente ocupa más de la mitad de la altura: (12, 9) y (19, 9) vienen de su
    // centro, (11, 16) y (21, 16).
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    const glm::vec2 trapezoid[4] = {{12.0f, 4.0f}, {20.0f, 4.0f}, {24.0f, 20.0f}, {8.0f, 20.0f}};
    REQUIRE(canvas.setTransform(trapezoid, false));
    canvas.applyTransform();
    CHECK_PIXEL(layerAt(canvas, 1, 12, 9), kRed, 2);
    CHECK_PIXEL(layerAt(canvas, 1, 19, 9), kGreen, 2);
    CHECK_PIXEL(layerAt(canvas, 1, 9, 5), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 22, 5), kClear, 0);
}

TEST_CASE(transform_selection_moves_part_and_selection) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    fillLayer(canvas, 1, {8, 8, 24, 24}, 1.0f, 0.0f, 0.0f);
    fillLayer(canvas, 1, {40, 40, 56, 56}, 0.0f, 0.0f, 1.0f);
    const std::vector<uint8_t> original = layerPixels(canvas, 1);
    REQUIRE(selectRect(canvas, {0, 0, 32, 32}));

    // Solo lo seleccionado, con la caja de lo que tiene pintado.
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    CHECK_RECT(canvas.transformSource(), (IRect{8, 8, 24, 24}));
    glm::vec2 corners[4];
    moved(canvas.transformSource(), 24.0f, 0.0f, corners);
    REQUIRE(canvas.setTransform(corners, false));
    canvas.applyTransform();
    CHECK_PIXEL(layerAt(canvas, 1, 12, 12), kClear, 0);
    CHECK_PIXEL(layerAt(canvas, 1, 36, 12), kRed, 1);
    CHECK_PIXEL(layerAt(canvas, 1, 48, 48), kBlue, 0);
    // La selección se ha movido igual.
    CHECK(canvas.hasSelection());
    CHECK_RECT(canvas.selectionBounds(), (IRect{24, 0, 56, 32}));
    CHECK_EQ(maskAt(canvas, 30, 16), 255);
    CHECK_EQ(maskAt(canvas, 20, 16), 0);

    // Un paso deshace las dos cosas.
    REQUIRE(canvas.undo());
    CHECK_EQ(test::maxDifference(layerPixels(canvas, 1), original), 0);
    CHECK_RECT(canvas.selectionBounds(), (IRect{0, 0, 32, 32}));
    CHECK_EQ(maskAt(canvas, 20, 16), 255);
    CHECK_EQ(maskAt(canvas, 40, 16), 0);
    REQUIRE(canvas.redo());
    CHECK_RECT(canvas.selectionBounds(), (IRect{24, 0, 56, 32}));
    CHECK_PIXEL(layerAt(canvas, 1, 36, 12), kRed, 1);

    // Selección sin nada pintado debajo: nada que transformar.
    REQUIRE(selectRect(canvas, {0, 40, 16, 56}));
    CHECK(canvas.beginTransform() == Canvas::Edit::Nothing);
}

TEST_CASE(selection_lost_with_context) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    fillLayer(canvas, 1, {4, 4, 12, 12}, 1.0f, 0.0f, 0.0f);
    REQUIRE(selectRect(canvas, {0, 0, 16, 16}));
    CHECK(canvas.copySelection() == Canvas::Edit::Done);
    REQUIRE(canvas.beginTransform() == Canvas::Edit::Done);
    REQUIRE(canvas.takeSnapshot(size_t{1} << 30));   // aplica la transformación
    CHECK(!canvas.transforming());
    REQUIRE(test::recreateGLContext());
    REQUIRE(canvas.recreateGpu(nullptr));
    // La selección y el portapapeles vivían en la GPU; lo demás sigue.
    CHECK(!canvas.hasSelection());
    CHECK(!canvas.canPaste());
    CHECK_PIXEL(layerAt(canvas, 1, 8, 8), kRed, 0);
    REQUIRE(selectRect(canvas, {0, 0, 8, 8}));
    CHECK_EQ(maskAt(canvas, 4, 4), 255);
    CHECK(canvas.beginTransform() == Canvas::Edit::Done);
    canvas.cancelTransform();
}
