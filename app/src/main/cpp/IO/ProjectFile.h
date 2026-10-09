#pragma once

#include "IO/Project.h"
#include "IO/Tasks.h"
#include "IO/Zip.h"

#include <SDL3/SDL_iostream.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Guardar y abrir proyectos (.lvskt) sin parar la interfaz: las capas se comprimen y se
// descomprimen en hilos aparte (en la web, entre frames). La lectura y la escritura de la GPU
// las hace la app, en su hilo.
namespace project {

// Memoria que pueden ocupar los píxeles que esperan a comprimirse (o, al abrir, ya
// descomprimidos y esperando a subir a la GPU). Una capa más grande que esto va sola.
inline constexpr size_t kMemoryBudget = size_t{256} * 1024 * 1024;

// Dónde se guarda.
struct Target {
    // Un archivo nuevo en `folder` con el nombre `stem` (ver io::createFile: si ya existe,
    // «stem (2)», «stem (3)»...).
    std::string folder;
    std::string stem;
    // O este archivo, que se sustituye al terminar: se escribe al lado y se cambia de nombre,
    // así que un corte a mitad de guardar no lo estropea.
    std::string path;
};

// Guarda un proyecto. La app, en su hilo y sin soltarlo: begin(), y por cada capa reserve()
// y addLayer() con sus píxeles recién leídos de la GPU, y al final finish() con el dibujo
// entero. Desde ahí sigue solo en segundo plano: lo que se guarda es como estaba el lienzo
// en ese momento, aunque se siga dibujando.
class Saver {
public:
    struct Result {
        bool ok = false;
        std::string path;    // el archivo (o el que se intentó crear)
        std::string error;   // por qué falló
        uint64_t bytes = 0;  // lo que ocupa
    };

    Saver() = default;
    ~Saver();
    Saver(const Saver&) = delete;
    Saver& operator=(const Saver&) = delete;

    // Empieza: crea el archivo y escribe lo que no son capas. False si ya hay un guardado en
    // curso o no se pudo crear el archivo (entonces el motivo queda en takeResult()).
    // `app`: el programa y su versión, para document.json.
    bool begin(Document document, const Target& target, std::string app);
    // Antes de leer `bytes` de la GPU: espera (en la web, comprime aquí) a que quepan en
    // kMemoryBudget con lo que aún espera a comprimirse.
    void reserve(size_t bytes);
    // Los píxeles premultiplicados de la capa `index`, de su caja (vacío si no tiene nada).
    // Mejor con un byte más por fila reservado (ver project::encodeImage).
    void addLayer(int index, std::vector<uint8_t> pixels);
    // El dibujo entero, premultiplicado: con esto termina en segundo plano. `onFinished` se
    // llama desde otro hilo (o desde pump(), en la web) cuando haya resultado.
    void finish(std::vector<uint8_t> composite, std::function<void()> onFinished);
    // Algo falló antes de terminar (no se pudo leer la GPU): no se guarda nada.
    void abort(std::string reason, std::function<void()> onFinished);

    bool busy() const;
    // En la web: comprime durante `budgetMs` como mucho. Hay que llamarlo en cada frame
    // mientras busy().
    void pump(uint64_t budgetMs);
    // Lo hecho, de 0 a 1.
    float progress() const;
    // El resultado del último guardado; se entrega una sola vez.
    std::optional<Result> takeResult();
    // Espera a que termine.
    void wait();

private:
    struct Entry {
        std::string name;
        std::vector<uint8_t> data;
        bool queued = false;   // ya se encargó su trabajo
        bool ready = false;    // ya está (o falló)
    };
    // La entrada `slot` está lista (o falló, con `error`): se escribe con las anteriores, en
    // orden, y si era la última se cierra el archivo.
    void complete(size_t slot, std::vector<uint8_t> data, const std::string& error);
    // Con el cerrojo: escribe lo que esté listo y, si ya está todo, termina (devuelve true:
    // hay que llamar a m_onFinished, ya sin el cerrojo).
    bool flush();
    void close();
    void release(size_t bytes);

    std::unique_ptr<io::TaskPool> m_pool;
    mutable std::mutex m_mutex;
    std::condition_variable m_freed;   // un trabajo terminó y soltó su memoria
    bool m_busy = false;
    std::optional<Result> m_result;
    std::function<void()> m_onFinished;

    // Lo que se guarda.
    Document m_document;
    std::string m_app;
    std::string m_path;       // el archivo
    std::string m_tempPath;   // el que se escribe, si se sustituye uno (si no, vacío)
    SDL_IOStream* m_io = nullptr;
    std::unique_ptr<zip::Writer> m_zip;
    std::vector<Entry> m_entries;   // las capas, el fondo, la miniatura y el dibujo entero
    size_t m_written = 0;           // las que ya están en el archivo
    bool m_finishing = false;       // ya llegó el dibujo entero (o abort)
    bool m_failed = false;
    std::string m_error;
    size_t m_pending = 0;           // bytes de píxeles esperando a comprimirse
};

// Abre un proyecto. open() lee el índice y document.json (en este hilo: es rápido) y
// comprueba que se puede abrir aquí; start() empieza a descomprimir las capas en segundo
// plano (en la web, en pump()), y next() las va dando de abajo arriba para subirlas a la GPU.
class Loader {
public:
    Loader() = default;
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // `maxSide`: el lado máximo de textura de esta GPU (0: no se comprueba). False si no se
    // puede abrir (motivo en error()).
    bool open(const std::string& path, int maxSide);
    bool open(std::vector<uint8_t> data, int maxSide);
    const Document& document() const { return m_document; }
    // Lo que no impide abrirlo (también las capas dañadas, según se leen).
    std::vector<std::string> warnings() const;
    const std::string& error() const { return m_error; }

    void start();
    // La siguiente capa, si ya está: su índice y sus píxeles premultiplicados, del tamaño de
    // su caja (vacío si es transparente o está dañada). False si aún no está o ya no quedan.
    bool next(int* index, std::vector<uint8_t>& pixels);
    // Ya se dieron todas las capas.
    bool finished() const;
    void pump(uint64_t budgetMs);
    float progress() const;

private:
    struct Slot {
        bool ready = false;
        size_t bytes = 0;            // memoria que se cuenta mientras no se da
        std::vector<uint8_t> pixels;
    };
    bool readDocument(int maxSide);
    // Con el cerrojo: las capas siguientes que caben en la memoria (para encargarlas después
    // de soltarlo).
    std::vector<size_t> queueMore();
    void decode(size_t index);

    std::unique_ptr<zip::Source> m_source;
    std::vector<uint8_t> m_data;   // el archivo entero, si no se pudo leer por partes
    zip::Reader m_zip;
    std::mutex m_sourceMutex;      // la lectura del archivo, de una en una
    Document m_document;
    std::string m_error;
    std::unique_ptr<io::TaskPool> m_pool;

    mutable std::mutex m_mutex;
    std::vector<std::string> m_warnings;
    std::vector<Slot> m_slots;
    size_t m_queued = 0;           // capas encargadas (de abajo arriba)
    size_t m_given = 0;            // capas ya dadas
    size_t m_pending = 0;          // bytes encargados o leídos que aún no se han dado
    bool m_canceled = false;       // se destruye: lo que falta ya no se lee
};

} // namespace project
