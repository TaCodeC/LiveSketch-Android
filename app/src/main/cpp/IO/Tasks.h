#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace io {

// Hilos para lo que tarda (comprimir y descomprimir las capas de un proyecto), creados al
// llegar el primer trabajo. En la web no hay hilos (harían falta cabeceras COOP/COEP en el
// servidor): los trabajos esperan a runPending(), que la app llama entre frames.
class TaskPool {
public:
    // `threads`: cuántos como mucho; 0, los núcleos menos uno (entre 1 y 3), para que el hilo
    // de la interfaz siga teniendo el suyo.
    explicit TaskPool(int threads = 0);
    // Espera a que terminen los trabajos (en la web, los hace).
    ~TaskPool();
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    void submit(std::function<void()> task);
    // Sin hilos: hace trabajos pendientes durante `budgetMs` como mucho (al menos uno, si hay).
    // Con hilos no hace nada. Devuelve si hizo alguno.
    bool runPending(uint64_t budgetMs);
    // Espera a que no quede ninguno (sin hilos, los hace todos).
    void waitIdle();
    // Si hay hilos de verdad (si no, los trabajos esperan a runPending).
    bool threaded() const;

private:
    void work();

    int m_maxThreads = 1;
    std::mutex m_mutex;
    std::condition_variable m_wake;   // hay trabajo o hay que parar
    std::condition_variable m_idle;   // no queda nada
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread> m_threads;
    int m_running = 0;
    bool m_stop = false;
};

} // namespace io
