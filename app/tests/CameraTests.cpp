#include "Test.h"

#include "Canvas/Camera.h"

#include <algorithm>
#include <cmath>

namespace {

Camera makeCamera(glm::vec2 viewport, glm::vec2 canvas) {
    Camera camera;
    camera.setViewport(viewport);
    camera.setCanvasSize(canvas);
    return camera;
}

// Parte del lienzo (en píxeles de pantalla) que queda dentro de la ventana en cada eje.
glm::vec2 visibleCanvas(const Camera& camera) {
    const glm::vec2 a = camera.canvasToScreen({0.0f, 0.0f});
    const glm::vec2 b = camera.canvasToScreen(camera.canvasSize());
    const glm::vec2 v = camera.viewport();
    return {std::min(b.x, v.x) - std::max(a.x, 0.0f), std::min(b.y, v.y) - std::max(a.y, 0.0f)};
}

} // namespace

TEST_CASE(camera_fit_centers_canvas) {
    const Camera camera = makeCamera({1000.0f, 800.0f}, {1920.0f, 1080.0f});
    // 16 px de margen: el ancho limita, (1000 - 32) / 1920.
    CHECK_NEAR(camera.zoom(), 968.0f / 1920.0f, 1e-6f);
    const glm::vec2 center = camera.canvasToScreen({960.0f, 540.0f});
    CHECK_NEAR(center.x, 500.0f, 1e-3f);
    CHECK_NEAR(center.y, 400.0f, 1e-3f);
}

TEST_CASE(camera_screen_canvas_roundtrip) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {640.0f, 360.0f});
    camera.zoomAt({300.0f, 200.0f}, 1.7f);
    camera.pan({-40.0f, 25.0f});
    const glm::vec2 points[] = {{0.0f, 0.0f}, {639.0f, 359.0f}, {123.5f, 77.25f}};
    for (const glm::vec2 p : points) {
        const glm::vec2 back = camera.screenToCanvas(camera.canvasToScreen(p));
        CHECK_NEAR(back.x, p.x, 1e-3f);
        CHECK_NEAR(back.y, p.y, 1e-3f);
    }
}

TEST_CASE(camera_zoom_keeps_anchor) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {1920.0f, 1080.0f});
    const glm::vec2 anchor{400.0f, 300.0f};
    const glm::vec2 before = camera.screenToCanvas(anchor);
    camera.zoomAt(anchor, 2.0f);
    const glm::vec2 after = camera.screenToCanvas(anchor);
    CHECK_NEAR(after.x, before.x, 1e-3f);
    CHECK_NEAR(after.y, before.y, 1e-3f);
}

TEST_CASE(camera_zoom_limits) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {640.0f, 360.0f});
    camera.zoomAt({640.0f, 400.0f}, 1000.0f);
    CHECK_NEAR(camera.zoom(), camera.maxZoom(), 1e-5f);
    camera.zoomAt({640.0f, 400.0f}, 1e-6f);
    CHECK_NEAR(camera.zoom(), camera.minZoom(), 1e-6f);
    camera.zoomAt({640.0f, 400.0f}, 0.0f);   // se ignora
    CHECK_NEAR(camera.zoom(), camera.minZoom(), 1e-6f);
    // Un lienzo grande en una pantalla pequeña se puede ver al menos a 8 px por píxel.
    const Camera big = makeCamera({400.0f, 800.0f}, {3840.0f, 2160.0f});
    CHECK(big.maxZoom() >= 8.0f);
}

TEST_CASE(camera_pan_keeps_canvas_visible) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {1920.0f, 1080.0f});
    camera.pan({100000.0f, -100000.0f});
    const glm::vec2 visible = visibleCanvas(camera);
    const float margin = 0.2f * 800.0f;
    CHECK(visible.x >= margin - 0.01f);
    CHECK(visible.y >= margin - 0.01f);
}

TEST_CASE(camera_viewport_change) {
    // Sin tocar la vista, al girar la pantalla el lienzo se vuelve a ajustar.
    Camera camera = makeCamera({1280.0f, 800.0f}, {1920.0f, 1080.0f});
    camera.setViewport({800.0f, 1280.0f});
    CHECK_NEAR(camera.zoom(), 768.0f / 1920.0f, 1e-6f);

    // Si el usuario movió la vista, se conserva el punto del lienzo del centro.
    camera.zoomAt({400.0f, 640.0f}, 3.0f);
    camera.pan({50.0f, -30.0f});
    const glm::vec2 center = camera.screenToCanvas({400.0f, 640.0f});
    const float zoom = camera.zoom();
    camera.setViewport({1280.0f, 800.0f});
    const glm::vec2 after = camera.screenToCanvas({640.0f, 400.0f});
    CHECK_NEAR(after.x, center.x, 1e-2f);
    CHECK_NEAR(after.y, center.y, 1e-2f);
    CHECK_NEAR(camera.zoom(), zoom, 1e-6f);

    camera.fit();
    CHECK_NEAR(camera.zoom(), 1248.0f / 1920.0f, 1e-6f);
}

TEST_CASE(camera_insets_center_in_free_area) {
    // Barras arriba (72 px) y a la izquierda (76 px): el lienzo se centra en lo que queda.
    Camera camera = makeCamera({1280.0f, 800.0f}, {1920.0f, 1080.0f});
    camera.setInsets(72.0f, 0.0f, 0.0f, 76.0f);
    const float zoom = std::min((1204.0f - 32.0f) / 1920.0f, (728.0f - 32.0f) / 1080.0f);
    CHECK_NEAR(camera.zoom(), zoom, 1e-6f);
    const glm::vec2 center = camera.canvasToScreen({960.0f, 540.0f});
    CHECK_NEAR(center.x, 76.0f + 1204.0f * 0.5f, 1e-3f);
    CHECK_NEAR(center.y, 72.0f + 728.0f * 0.5f, 1e-3f);

    // Con la vista movida, cambiar los bordes no la toca.
    camera.pan({30.0f, 0.0f});
    const glm::vec2 offset = camera.offset();
    camera.setInsets(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK_NEAR(camera.offset().x, offset.x, 1e-4f);
    camera.fit();
    CHECK_NEAR(camera.zoom(), 1248.0f / 1920.0f, 1e-6f);

    // Si la interfaz taparía casi toda la ventana, se ignora.
    Camera small = makeCamera({300.0f, 300.0f}, {100.0f, 100.0f});
    small.setInsets(200.0f, 0.0f, 0.0f, 0.0f);
    CHECK_NEAR(small.zoom(), (300.0f - 32.0f) / 100.0f, 1e-5f);
}

TEST_CASE(camera_fit_view_for_animation) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {640.0f, 360.0f});
    camera.zoomAt({100.0f, 100.0f}, 2.5f);
    camera.rotateAt({300.0f, 200.0f}, 0.7f);
    CHECK(camera.userMoved());
    float zoom = 0.0f;
    glm::vec2 center{0.0f};
    camera.fitView(zoom, center);
    camera.place(zoom, 0.0f, center);
    const glm::vec2 middle = camera.canvasToScreen({320.0f, 180.0f});
    CHECK_NEAR(middle.x, 640.0f, 1e-3f);
    CHECK_NEAR(middle.y, 400.0f, 1e-3f);
    camera.fit();
    CHECK(!camera.userMoved());
    CHECK_EQ(camera.angle(), 0.0f);
}

TEST_CASE(camera_rotation_keeps_anchor) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {640.0f, 360.0f});
    const glm::vec2 anchor(500.0f, 300.0f);
    const glm::vec2 before = camera.screenToCanvas(anchor);
    camera.rotateAt(anchor, 0.5f);
    CHECK_NEAR(camera.angle(), 0.5f, 1e-6f);
    const glm::vec2 after = camera.screenToCanvas(anchor);
    CHECK_NEAR(after.x, before.x, 1e-2f);
    CHECK_NEAR(after.y, before.y, 1e-2f);
    // En el sentido de las agujas del reloj (la y hacia abajo).
    const glm::vec2 right = camera.orient({1.0f, 0.0f});
    CHECK_NEAR(right.x, std::cos(0.5f), 1e-6f);
    CHECK_NEAR(right.y, std::sin(0.5f), 1e-6f);
    const glm::vec2 p(123.0f, 45.0f);
    const glm::vec2 back = camera.screenToCanvas(camera.canvasToScreen(p));
    CHECK_NEAR(back.x, p.x, 1e-3f);
    CHECK_NEAR(back.y, p.y, 1e-3f);
    // Da la vuelta entera: el ángulo va de -180° a 180°.
    camera.rotateAt(anchor, 3.0f);
    CHECK_NEAR(camera.angle(), 3.5f - 2.0f * 3.14159265f, 1e-5f);
}

TEST_CASE(camera_quarter_turns_are_exact) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {640.0f, 360.0f});
    camera.rotateAt({640.0f, 400.0f}, 0.25f);
    camera.rotateAt({640.0f, 400.0f}, 3.14159265f * 0.5f - 0.25f);
    const glm::vec2 right = camera.orient({1.0f, 0.0f});
    const glm::vec2 down = camera.orient({0.0f, 1.0f});
    CHECK_EQ(right.x, 0.0f);
    CHECK_EQ(right.y, 1.0f);
    CHECK_EQ(down.x, -1.0f);
    CHECK_EQ(down.y, 0.0f);
}

TEST_CASE(camera_flip_mirrors_the_view) {
    Camera camera = makeCamera({1280.0f, 800.0f}, {640.0f, 360.0f});
    const glm::vec2 corner = camera.canvasToScreen({0.0f, 0.0f});
    camera.setFlipped(true);
    CHECK(camera.flipped());
    // Ajustado, el lienzo se queda donde estaba con la izquierda a la derecha.
    const glm::vec2 mirrored = camera.canvasToScreen({0.0f, 0.0f});
    CHECK_NEAR(mirrored.x, 1280.0f - corner.x, 1e-3f);
    CHECK_NEAR(mirrored.y, corner.y, 1e-3f);
    const glm::vec2 p(200.0f, 100.0f);
    const glm::vec2 back = camera.screenToCanvas(camera.canvasToScreen(p));
    CHECK_NEAR(back.x, p.x, 1e-3f);
    CHECK_NEAR(back.y, p.y, 1e-3f);
    // Girada y volteada: el giro cambia de sentido y se ve igual que en un espejo.
    camera.setFlipped(false);
    camera.rotateAt({640.0f, 400.0f}, 0.3f);
    const glm::vec2 turned = camera.canvasToScreen(p);
    camera.setFlipped(true);
    CHECK_NEAR(camera.angle(), -0.3f, 1e-6f);
    const glm::vec2 seen = camera.canvasToScreen(p);
    CHECK_NEAR(seen.x, 1280.0f - turned.x, 1e-2f);
    CHECK_NEAR(seen.y, turned.y, 1e-2f);
    // Un lienzo nuevo empieza sin voltear.
    camera.setCanvasSize({300.0f, 200.0f});
    CHECK(!camera.flipped());
}
