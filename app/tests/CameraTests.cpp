#include "Test.h"

#include "Canvas/Camera.h"

#include <algorithm>

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
