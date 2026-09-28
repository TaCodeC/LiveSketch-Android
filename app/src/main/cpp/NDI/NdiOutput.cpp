#include "NDI/NdiOutput.h"

#include "Gfx/Pixels.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

constexpr uint64_t kNoVersion = UINT64_MAX;
constexpr auto kKeepAlive = std::chrono::seconds(1);
constexpr auto kSenderTick = std::chrono::milliseconds(250);

} // namespace

NdiOutput::~NdiOutput() {
    stop();
}

bool NdiOutput::start(std::unique_ptr<FrameSink> sink, int width, int height) {
    stop();
    if (!sink || width <= 0 || height <= 0) {
        return false;
    }
    m_width = width;
    m_height = height;
    m_frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (!createPbos()) {
        return false;
    }

    m_scratch.assign(m_frameBytes, 0);
    m_pending.assign(m_frameBytes, 0);
    m_hasPending = false;
    m_quit = false;
    m_error.clear();
    m_connections = 0;
    m_failed = false;
    m_capturedVersion = kNoVersion;   // la primera captura sale siempre
    m_lastCaptureMs = 0;
    m_sink = std::move(sink);
    m_thread = std::thread(&NdiOutput::senderLoop, this);
    m_running = true;
    return true;
}

void NdiOutput::stop() {
    if (m_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    for (Slot& slot : m_slots) {
        releaseFence(slot);
        slot.pbo.reset();
    }
    m_sink.reset();
    m_scratch = {};
    m_pending = {};
    m_connections = 0;
    m_running = false;
}

bool NdiOutput::createPbos() {
    gfx::clearErrors();
    for (Slot& slot : m_slots) {
        slot.fence = nullptr;
        slot.pbo = gfx::Buffer::create();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo.id());
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(m_frameBytes), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!gfx::checkErrors("NdiOutput::createPbos")) {
        for (Slot& slot : m_slots) {
            slot.pbo.reset();
        }
        return false;
    }
    return true;
}

void NdiOutput::releaseFence(Slot& slot) {
    // Una fence del contexto perdido ya no existe: solo se olvida.
    if (slot.fence && slot.fenceGeneration == gfx::contextGeneration()) {
        glDeleteSync(slot.fence);
    }
    slot.fence = nullptr;
}

void NdiOutput::capture(GLuint compositeFbo, uint64_t version) {
    if (!m_running || m_failed) {
        return;
    }
    m_latestVersion = version;
    collect();

    if (version == m_capturedVersion) {
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (m_lastCaptureMs != 0 && now - m_lastCaptureMs < 1000 / kMaxFps) {
        return;   // se capturará en un frame posterior (busy() lo mantiene despierto)
    }
    auto free = std::find_if(m_slots.begin(), m_slots.end(), [](const Slot& slot) { return slot.fence == nullptr; });
    if (free == m_slots.end()) {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, compositeFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, free->pbo.id());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    free->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    free->fenceGeneration = gfx::contextGeneration();
    free->version = version;
    glFlush();   // para que la fence avance aunque no haya swap pronto

    m_capturedVersion = version;
    m_lastCaptureMs = now;
}

void NdiOutput::collect() {
    // La GPU termina en orden: de las lecturas listas basta con la más reciente.
    Slot* newest = nullptr;
    for (Slot& slot : m_slots) {
        if (!slot.fence) {
            continue;
        }
        const GLenum state = glClientWaitSync(slot.fence, 0, 0);
        if (state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED) {
            if (!newest || slot.version > newest->version) {
                newest = &slot;
            }
        } else if (state == GL_WAIT_FAILED) {
            releaseFence(slot);
        }
    }
    if (!newest) {
        return;
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, newest->pbo.id());
    const void* pixels =
        glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(m_frameBytes), GL_MAP_READ_BIT);
    if (pixels) {
        std::memcpy(m_scratch.data(), pixels, m_frameBytes);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.swap(m_scratch);
            m_hasPending = true;
        }
        m_wake.notify_one();
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    const uint64_t delivered = newest->version;
    for (Slot& slot : m_slots) {
        if (slot.fence && slot.version <= delivered) {
            releaseFence(slot);
        }
    }
}

bool NdiOutput::busy() const {
    if (!m_running || m_failed) {
        return false;
    }
    const bool inFlight =
        std::any_of(m_slots.begin(), m_slots.end(), [](const Slot& slot) { return slot.fence != nullptr; });
    return inFlight || m_latestVersion != m_capturedVersion;
}

void NdiOutput::dropInFlight() {
    for (Slot& slot : m_slots) {
        releaseFence(slot);
    }
    m_capturedVersion = kNoVersion;
}

void NdiOutput::recreateGpu() {
    if (!m_running) {
        return;
    }
    for (Slot& slot : m_slots) {
        releaseFence(slot);   // de otra generación: solo se olvida
    }
    if (!createPbos()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = "No se pudieron crear los buffers de captura";
    }
    m_capturedVersion = kNoVersion;
}

std::string NdiOutput::error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

void NdiOutput::senderLoop() {
    if (!m_sink->open(m_width, m_height)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = "No se pudo iniciar el emisor NDI";
        m_failed = true;
        return;
    }

    std::vector<uint8_t> frame(m_frameBytes, 0);
    bool haveFrame = false;
    auto lastSend = std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_quit) {
        m_wake.wait_for(lock, kSenderTick, [this] { return m_quit || m_hasPending; });
        if (m_quit) {
            break;
        }
        const bool fresh = m_hasPending;
        if (fresh) {
            frame.swap(m_pending);
            m_hasPending = false;
        }
        lock.unlock();

        if (fresh) {
            gfx::unpremultiply(frame.data(), frame.size() / 4);
            haveFrame = true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (haveFrame && (fresh || now - lastSend >= kKeepAlive)) {
            m_sink->send(frame.data(), m_width, m_height, m_width * 4);
            lastSend = now;
        }
        m_connections = m_sink->connections();

        lock.lock();
    }
    lock.unlock();
    m_sink->close();
}
