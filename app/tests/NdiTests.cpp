#include "Test.h"

#include "Canvas/Canvas.h"
#include "Gfx/Pixels.h"
#include "NDI/NdiOutput.h"

#include <SDL3/SDL_timer.h>

#include <memory>
#include <mutex>

namespace {

// Lo que ve el emisor falso. Lo comparten la prueba (hilo de GL) y el hilo de envío.
struct SinkLog {
    std::mutex mutex;
    bool openResult = true;
    bool opened = false;
    bool closed = false;
    int openWidth = 0;
    int openHeight = 0;
    int frames = 0;
    int stride = 0;
    std::vector<uint8_t> lastFrame;
};

class FakeSink : public FrameSink {
public:
    explicit FakeSink(std::shared_ptr<SinkLog> log) : m_log(std::move(log)) {}

    bool open(int width, int height) override {
        std::lock_guard<std::mutex> lock(m_log->mutex);
        m_log->opened = true;
        m_log->openWidth = width;
        m_log->openHeight = height;
        return m_log->openResult;
    }
    void send(const uint8_t* rgba, int width, int height, int stride) override {
        std::lock_guard<std::mutex> lock(m_log->mutex);
        ++m_log->frames;
        m_log->stride = stride;
        m_log->lastFrame.assign(rgba, rgba + static_cast<size_t>(height) * static_cast<size_t>(stride));
    }
    int connections() override { return 2; }
    void close() override {
        std::lock_guard<std::mutex> lock(m_log->mutex);
        m_log->closed = true;
    }

private:
    std::shared_ptr<SinkLog> m_log;
};

int frameCount(SinkLog& log) {
    std::lock_guard<std::mutex> lock(log.mutex);
    return log.frames;
}

std::vector<uint8_t> lastFrame(SinkLog& log) {
    std::lock_guard<std::mutex> lock(log.mutex);
    return log.lastFrame;
}

// Lo que debería llegar por NDI: el compuesto sin premultiplicar.
std::vector<uint8_t> expectedFrame(Canvas& canvas) {
    std::vector<uint8_t> pixels;
    canvas.readComposite(pixels);
    gfx::unpremultiply(pixels.data(), pixels.size() / 4);
    return pixels;
}

// Hace lo mismo que el bucle de la app en cada frame hasta que `done()` se cumpla.
template <typename Done>
bool pump(NdiOutput& ndi, Canvas& canvas, Done done, uint64_t timeoutMs = 3000) {
    const uint64_t end = SDL_GetTicks() + timeoutMs;
    while (SDL_GetTicks() < end) {
        canvas.update();
        ndi.capture(canvas.composite().fbo.id(), canvas.version());
        if (done()) {
            return true;
        }
        SDL_Delay(4);
    }
    return false;
}

void drawLine(Canvas& canvas, float x0, float y0, float x1, float y1) {
    canvas.beginStroke(x0, y0, 1.0f);
    canvas.strokeTo(x1, y1, 1.0f);
    canvas.endStroke();
}

} // namespace

TEST_CASE(ndi_sends_full_canvas_unpremultiplied) {
    Canvas canvas;
    REQUIRE(canvas.init(64, 48));
    canvas.layers().setVisible(0, false);   // con transparencia se nota si falta quitar el premultiplicado
    BrushSettings& brush = canvas.brushSettings();
    brush.color[0] = 1.0f;
    brush.color[1] = 0.5f;
    brush.opacity = 0.5f;
    drawLine(canvas, 5.0f, 5.0f, 60.0f, 40.0f);

    auto log = std::make_shared<SinkLog>();
    NdiOutput ndi;
    REQUIRE(ndi.start(std::make_unique<FakeSink>(log), 64, 48));
    CHECK(ndi.running());
    CHECK(pump(ndi, canvas, [&] { return frameCount(*log) >= 1 && ndi.connections() == 2; }));
    {
        std::lock_guard<std::mutex> lock(log->mutex);
        CHECK(log->opened);
        CHECK_EQ(log->openWidth, 64);
        CHECK_EQ(log->openHeight, 48);
        CHECK_EQ(log->stride, 64 * 4);
    }
    CHECK_EQ(test::maxDifference(lastFrame(*log), expectedFrame(canvas)), 0);
    CHECK(ndi.error().empty());

    ndi.stop();
    CHECK(!ndi.running());
    std::lock_guard<std::mutex> lock(log->mutex);
    CHECK(log->closed);
}

TEST_CASE(ndi_sends_changes_and_keepalive) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    auto log = std::make_shared<SinkLog>();
    NdiOutput ndi;
    REQUIRE(ndi.start(std::make_unique<FakeSink>(log), 32, 32));
    REQUIRE(pump(ndi, canvas, [&] { return frameCount(*log) >= 1; }));

    // Un cambio en el lienzo sale en un frame nuevo.
    drawLine(canvas, 2.0f, 16.0f, 30.0f, 16.0f);
    const std::vector<uint8_t> expected = expectedFrame(canvas);
    CHECK(pump(ndi, canvas, [&] { return test::maxDifference(lastFrame(*log), expected) == 0; }));

    // Sin cambios no se captura más, pero el último frame se reenvía cada segundo.
    CHECK(pump(ndi, canvas, [&] { return !ndi.busy(); }));
    const int before = frameCount(*log);
    pump(ndi, canvas, [] { return false; }, 1400);
    const int after = frameCount(*log);
    CHECK(after >= before + 1);
    CHECK(after <= before + 2);
    ndi.stop();
}

TEST_CASE(ndi_limits_capture_rate) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    auto log = std::make_shared<SinkLog>();
    NdiOutput ndi;
    REQUIRE(ndi.start(std::make_unique<FakeSink>(log), 32, 32));

    // El lienzo cambia en cada frame durante 600 ms: como mucho 30 capturas por segundo.
    float opacity = 0.0f;
    const uint64_t start = SDL_GetTicks();
    pump(ndi, canvas, [&] {
        opacity = opacity > 0.5f ? 0.0f : 1.0f;
        canvas.layers().setOpacity(0, opacity);
        return false;
    }, 600);
    const uint64_t elapsed = SDL_GetTicks() - start;
    ndi.stop();
    const int frames = frameCount(*log);
    CHECK(frames >= 3);
    CHECK(frames <= static_cast<int>(elapsed * NdiOutput::kMaxFps / 1000) + 3);
}

TEST_CASE(ndi_open_failure_reports_error) {
    Canvas canvas;
    REQUIRE(canvas.init(16, 16));
    auto log = std::make_shared<SinkLog>();
    log->openResult = false;
    NdiOutput ndi;
    REQUIRE(ndi.start(std::make_unique<FakeSink>(log), 16, 16));
    CHECK(pump(ndi, canvas, [&] { return !ndi.error().empty(); }));
    CHECK(!ndi.busy());   // no mantiene despierto el bucle
    ndi.stop();
    CHECK_EQ(frameCount(*log), 0);

    CHECK(!ndi.start(nullptr, 16, 16));
    CHECK(!ndi.start(std::make_unique<FakeSink>(log), 0, 16));
    CHECK(!ndi.running());
}

TEST_CASE(ndi_survives_context_loss) {
    Canvas canvas;
    REQUIRE(canvas.init(32, 32));
    auto log = std::make_shared<SinkLog>();
    NdiOutput ndi;
    REQUIRE(ndi.start(std::make_unique<FakeSink>(log), 32, 32));
    REQUIRE(pump(ndi, canvas, [&] { return frameCount(*log) >= 1; }));

    // Lo mismo que hace la app: segundo plano, contexto perdido y vuelta.
    drawLine(canvas, 2.0f, 2.0f, 30.0f, 30.0f);
    canvas.update();
    ndi.capture(canvas.composite().fbo.id(), canvas.version());   // deja una lectura en curso
    ndi.dropInFlight();
    REQUIRE(canvas.takeSnapshot(size_t{1} << 30));
    REQUIRE(test::recreateGLContext());
    ndi.recreateGpu();
    bool restored = false;
    REQUIRE(canvas.recreateGpu(&restored));
    CHECK(restored);

    drawLine(canvas, 2.0f, 30.0f, 30.0f, 2.0f);
    const std::vector<uint8_t> expected = expectedFrame(canvas);
    CHECK(pump(ndi, canvas, [&] { return test::maxDifference(lastFrame(*log), expected) == 0; }));
    CHECK(ndi.error().empty());
    ndi.stop();
}
