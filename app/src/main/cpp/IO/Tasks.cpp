#include "IO/Tasks.h"

#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace io {

TaskPool::TaskPool(int threads) {
    m_maxThreads = threads > 0 ? threads : std::clamp(SDL_GetNumLogicalCPUCores() - 1, 1, 3);
}

TaskPool::~TaskPool() {
    waitIdle();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

bool TaskPool::threaded() const {
#ifdef SDL_PLATFORM_EMSCRIPTEN
    return false;
#else
    return true;
#endif
}

void TaskPool::submit(std::function<void()> task) {
    if (!task) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(task));
#ifndef SDL_PLATFORM_EMSCRIPTEN
    // Un hilo más mientras haya más trabajos esperando que hilos libres.
    const int busy = m_running + static_cast<int>(m_queue.size());
    if (static_cast<int>(m_threads.size()) < std::min(m_maxThreads, busy)) {
        try {
            m_threads.emplace_back([this] { work(); });
        } catch (const std::system_error&) {
            // Sin hilos nuevos, lo harán los que ya hay; sin ninguno, se hace aquí mismo.
            if (m_threads.empty()) {
                std::function<void()> inline_ = std::move(m_queue.back());
                m_queue.pop_back();
                lock.unlock();
                inline_();
                return;
            }
        }
    }
    lock.unlock();
    m_wake.notify_one();
#endif
}

void TaskPool::work() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty()) {
            return;   // m_stop
        }
        std::function<void()> task = std::move(m_queue.front());
        m_queue.pop_front();
        ++m_running;
        lock.unlock();
        task();
        task = nullptr;   // lo que capturó se libera fuera del cerrojo
        lock.lock();
        --m_running;
        if (m_queue.empty() && m_running == 0) {
            m_idle.notify_all();
        }
    }
}

bool TaskPool::runPending(uint64_t budgetMs) {
#ifdef SDL_PLATFORM_EMSCRIPTEN
    const uint64_t start = SDL_GetTicks();
    bool ran = false;
    while (true) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty()) {
                return ran;
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
        ran = true;
        if (SDL_GetTicks() - start >= budgetMs) {
            return true;
        }
    }
#else
    (void)budgetMs;
    return false;
#endif
}

void TaskPool::waitIdle() {
#ifdef SDL_PLATFORM_EMSCRIPTEN
    while (runPending(UINT64_MAX)) {
    }
#else
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_queue.empty() && m_running == 0; });
#endif
}

} // namespace io
