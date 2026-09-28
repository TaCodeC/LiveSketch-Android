#pragma once

#include "Gfx/GLObjects.h"
#include "NDI/FrameSink.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Salida del lienzo por NDI, sin contexto GL compartido:
// - En el hilo de GL, capture() copia el compuesto a un PBO (glReadPixels asíncrono) y
//   deja una fence. En un frame posterior, cuando la GPU ya terminó, mapea el PBO y pasa
//   los píxeles al hilo de envío. El hilo de GL nunca espera a la GPU.
// - El hilo de envío no toca GL: quita el premultiplicado y envía. Si el lienzo no
//   cambia, reenvía el último frame cada segundo para que un receptor que se conecte
//   reciba imagen.
class NdiOutput {
public:
    static constexpr int kMaxFps = 30;

    NdiOutput() = default;
    ~NdiOutput();
    NdiOutput(const NdiOutput&) = delete;
    NdiOutput& operator=(const NdiOutput&) = delete;

    // Hilo de GL. `width`/`height`: tamaño del lienzo.
    bool start(std::unique_ptr<FrameSink> sink, int width, int height);
    void stop();
    bool running() const { return m_running; }

    // Hilo de GL, una vez por frame, después de actualizar el compuesto.
    void capture(GLuint compositeFbo, uint64_t version);
    // Hay lecturas en curso o un cambio sin capturar: el bucle no debe dormirse.
    bool busy() const;

    // Al pasar a segundo plano (el contexto se suelta): descarta las lecturas en curso.
    void dropInFlight();
    // Tras perder el contexto: crea de nuevo los PBO en el contexto actual.
    void recreateGpu();

    int connections() const { return m_connections.load(); }
    // Mensaje de error del emisor, o vacío si va bien.
    std::string error() const;

private:
    struct Slot {
        gfx::Buffer pbo;
        GLsync fence = nullptr;
        uint32_t fenceGeneration = 0;
        uint64_t version = 0;
    };

    bool createPbos();
    void collect();
    void releaseFence(Slot& slot);
    void senderLoop();

    int m_width = 0;
    int m_height = 0;
    size_t m_frameBytes = 0;
    bool m_running = false;

    std::array<Slot, 2> m_slots;
    uint64_t m_latestVersion = 0;     // última versión del lienzo que se vio
    uint64_t m_capturedVersion = 0;   // última versión que se mandó leer
    uint64_t m_lastCaptureMs = 0;

    // Traspaso de frames al hilo de envío. Tres buffers rotan sin copias: el que llena
    // el hilo de GL, el pendiente y el que tiene el hilo de envío.
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_quit = false;
    bool m_hasPending = false;
    std::vector<uint8_t> m_scratch;   // solo hilo de GL
    std::vector<uint8_t> m_pending;   // protegido por m_mutex
    std::string m_error;              // protegido por m_mutex
    std::unique_ptr<FrameSink> m_sink; // solo hilo de envío mientras corre
    std::atomic<int> m_connections{0};
    std::atomic<bool> m_failed{false};   // el emisor no se pudo abrir
};
