#include "IO/ProjectFile.h"

#include "IO/FileChooser.h"
#include "IO/ImageExport.h"
#include "IO/Json.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace project {
namespace {

constexpr std::string_view kOraMimetype = "image/openraster";

std::span<const uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// PNG de 1 × 1 transparente: el de una capa vacía (OpenRaster quiere una imagen por capa).
std::vector<uint8_t> emptyPng(ColorProfile profile) {
    const uint8_t pixel[4] = {0, 0, 0, 0};
    png::Info info;
    info.profile = profile;
    std::vector<uint8_t> file;
    png::encode(pixel, 1, 1, 4, info, [&file](const uint8_t* data, size_t size) {
        file.insert(file.end(), data, data + size);
        return true;
    });
    return file;
}

} // namespace

// -----------------------------------------------------------------------------
// Guardar
// -----------------------------------------------------------------------------

Saver::~Saver() {
    if (busy()) {
        abort("se canceló", {});
    }
    wait();
}

bool Saver::begin(Document document, const Target& target, std::string app, const std::string& previous) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_busy) {
            return false;
        }
        m_busy = true;
        m_result.reset();
        m_onFinished = nullptr;
        m_entries.clear();
        m_written = 0;
        m_writing = false;
        m_finishing = false;
        m_failed = false;
        m_error.clear();
        m_pending = 0;
        m_reused = false;
    }
    m_canReuse = false;
    m_document = std::move(document);
    m_app = std::move(app);
    m_path.clear();
    m_tempPath.clear();
    m_previousSource.reset();

    if (!target.path.empty()) {
        m_path = target.path;
        m_tempPath = target.path + ".tmp";
        m_io = SDL_IOFromFile(m_tempPath.c_str(), "wb");
    } else {
        m_io = io::createFile(target.folder, target.stem, kExtension, &m_path);
    }
    if (!m_io) {
        std::lock_guard<std::mutex> lock(m_mutex);
        Result result;
        result.path = m_path;
        result.error = SDL_GetError();
        m_result = std::move(result);
        m_busy = false;
        m_document = {};
        return false;
    }

    // El archivo anterior, si se puede leer su índice: de ahí se copia lo que no cambió.
    if (!previous.empty()) {
        auto source = std::make_unique<zip::FileSource>();
        if (source->open(SDL_IOFromFile(previous.c_str(), "rb")) && m_previous.open(*source)) {
            m_previousSource = std::move(source);
            m_canReuse = true;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No se puede copiar nada de %s: se guarda entero",
                        previous.c_str());
        }
    }

    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    m_zip = std::make_unique<zip::Writer>(
        [this](const uint8_t* data, size_t size) { return SDL_WriteIO(m_io, data, size) == size; }, now);
    // OpenRaster: «mimetype» va el primero y sin comprimir.
    if (!m_zip->add("mimetype", bytesOf(kOraMimetype), zip::Method::Stored)) {
        m_failed = true;
        m_error = SDL_GetError();
    }

    // Las entradas que se escriben según estén listas (en este orden): las capas, el fondo,
    // la miniatura y el dibujo entero.
    const size_t layers = m_document.layers.size();
    m_entries.resize(layers + 3);
    for (size_t i = 0; i < layers; ++i) {
        m_entries[i].name = layerEntry(static_cast<int>(i));
        m_document.layers[i].file = m_entries[i].name;
    }
    m_entries[layers].name = kBackgroundEntry;
    m_entries[layers + 1].name = kThumbnailEntry;
    m_entries[layers + 2].name = kMergedEntry;

    if (!m_pool) {
        m_pool = std::make_unique<io::TaskPool>();
    }
    m_entries[layers].queued = true;
    m_pool->submit([this, slot = layers, width = m_document.width, height = m_document.height,
                    profile = m_document.info.profile,
                    color = std::to_array(m_document.background.color)] {
        std::string error;
        std::vector<uint8_t> file = encodeBackground(width, height, color.data(), profile, &error);
        complete(slot, std::move(file), error);
    });
    return true;
}

void Saver::reserve(size_t bytes) {
    if (!m_pool) {
        return;
    }
    auto fits = [this, bytes] { return m_pending == 0 || m_pending + bytes <= kMemoryBudget; };
    if (!m_pool->threaded()) {
        // Sin hilos: se comprime aquí hasta que quepa.
        while (true) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (fits()) {
                    return;
                }
            }
            if (!m_pool->runPending(0)) {
                return;
            }
        }
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_freed.wait(lock, fits);
}

void Saver::release(size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending -= std::min(bytes, m_pending);
    }
    m_freed.notify_all();
}

void Saver::addLayer(int index, std::vector<uint8_t> pixels) {
    if (!m_pool || index < 0 || static_cast<size_t>(index) >= m_document.layers.size()) {
        return;
    }
    const LayerInfo& layer = m_document.layers[static_cast<size_t>(index)];
    const ColorProfile profile = m_document.info.profile;
    const size_t bytes = pixels.capacity();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries[static_cast<size_t>(index)].queued = true;
        m_pending += bytes;
    }
    m_pool->submit([this, index, bytes, profile, width = layer.rect.width(), height = layer.rect.height(),
                    empty = layer.rect.empty(), pixels = std::move(pixels)]() mutable {
        std::string error;
        std::vector<uint8_t> file;
        if (empty) {
            file = emptyPng(profile);
        } else {
            png::Info info;
            info.profile = profile;
            file = encodeImage(pixels, width, height, info, &error);
        }
        pixels = {};
        release(bytes);
        complete(static_cast<size_t>(index), std::move(file), error);
    });
}

void Saver::reuseLayer(int index, const zip::Entry& entry) {
    if (!m_pool || index < 0 || static_cast<size_t>(index) >= m_document.layers.size()) {
        return;
    }
    const size_t maxSize = maxLayerFile(m_document.layers[static_cast<size_t>(index)]);
    bool write = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Entry& slot = m_entries[static_cast<size_t>(index)];
        slot.copy = entry;
        slot.maxSize = maxSize;
        slot.queued = true;
        slot.ready = true;
        m_reused = true;
        write = claimWriter();
    }
    if (write) {
        m_pool->submit([this] { drain(); });
    }
}

void Saver::finish(std::vector<uint8_t> composite, std::function<void()> onFinished) {
    if (!m_pool) {
        return;
    }
    const size_t layers = m_document.layers.size();
    const size_t bytes = composite.capacity();
    bool write = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_onFinished = std::move(onFinished);
        m_entries[layers + 1].queued = true;
        m_entries[layers + 2].queued = true;
        m_pending += bytes;
        m_finishing = true;
        write = skipMissingLayers() && claimWriter();
    }
    if (write) {
        m_pool->submit([this] { drain(); });
    }
    // Sin el nombre del lienzo (al contrario que el PNG que se exporta): así renombrar un
    // proyecto solo cambia document.json.
    png::Info info;
    info.ppi = m_document.info.ppi;
    info.profile = m_document.info.profile;
    m_pool->submit([this, layers, bytes, info, width = m_document.width, height = m_document.height,
                    composite = std::move(composite)]() mutable {
        // Primero la miniatura, que se hace con los colores premultiplicados.
        std::string error;
        int tw = 0;
        int th = 0;
        std::vector<uint8_t> file;
        const std::vector<uint8_t> small =
            composite.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4
                ? thumbnail(composite.data(), width, height, kThumbnailSide, &tw, &th)
                : std::vector<uint8_t>();
        png::Info smallInfo;
        smallInfo.profile = info.profile;
        if (small.empty() || !png::encode(small.data(), tw, th, static_cast<size_t>(tw) * 4, smallInfo,
                                          [&file](const uint8_t* data, size_t size) {
                                              try {
                                                  file.insert(file.end(), data, data + size);
                                              } catch (const std::bad_alloc&) {
                                                  return false;
                                              }
                                              return true;
                                          })) {
            error = "no hay memoria para guardarlo";
            file.clear();
        }
        complete(layers + 1, std::move(file), error);

        error.clear();
        file = encodeImage(composite, width, height, info, &error);
        composite = {};
        release(bytes);
        complete(layers + 2, std::move(file), error);
    });
}

void Saver::finishReusing(const zip::Entry& thumbnail, const zip::Entry& merged, std::function<void()> onFinished) {
    if (!m_pool) {
        return;
    }
    const size_t layers = m_document.layers.size();
    LayerInfo small;
    small.rect = IRect::ofSize(kThumbnailSide, kThumbnailSide);
    LayerInfo whole;
    whole.rect = IRect::ofSize(m_document.width, m_document.height);
    bool write = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_onFinished = std::move(onFinished);
        const std::pair<const zip::Entry*, size_t> copies[2] = {{&thumbnail, maxLayerFile(small)},
                                                                {&merged, maxLayerFile(whole)}};
        for (size_t i = 0; i < 2; ++i) {
            Entry& slot = m_entries[layers + 1 + i];
            slot.copy = *copies[i].first;
            slot.maxSize = copies[i].second;
            slot.queued = true;
            slot.ready = true;
        }
        m_reused = true;
        m_finishing = true;
        skipMissingLayers();
        write = claimWriter();
    }
    if (write) {
        m_pool->submit([this] { drain(); });
    }
}

bool Saver::skipMissingLayers() {
    bool skipped = false;
    for (size_t i = 0; i < m_document.layers.size(); ++i) {
        Entry& slot = m_entries[i];
        if (!slot.queued) {
            slot.queued = true;
            slot.ready = true;
            skipped = true;
            if (!m_failed) {
                m_failed = true;
                m_error = "faltó una capa por guardar";
            }
        }
    }
    return skipped;
}

void Saver::abort(std::string reason, std::function<void()> onFinished) {
    bool write = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_busy || m_finishing) {
            return;
        }
        if (!m_failed) {
            m_failed = true;
            m_error = std::move(reason);
        }
        m_onFinished = std::move(onFinished);
        // Lo que no llegó a encargarse no llegará: se da por hecho (no se escribe nada).
        for (Entry& entry : m_entries) {
            if (!entry.queued) {
                entry.queued = true;
                entry.ready = true;
            }
        }
        m_finishing = true;
        write = claimWriter();
    }
    if (write) {
        m_pool->submit([this] { drain(); });
    }
}

void Saver::complete(size_t slot, std::vector<uint8_t> data, const std::string& error) {
    bool write = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_failed && (data.empty() || !error.empty())) {
            m_failed = true;
            m_error = error.empty() ? "no hay memoria para guardarlo" : error;
        }
        m_entries[slot].data = std::move(data);
        m_entries[slot].ready = true;
        write = claimWriter();
    }
    if (write) {
        drain();   // ya en otro hilo (o en pump(), en la web)
    }
}

bool Saver::claimWriter() {
    if (m_writing || !m_busy) {
        return false;
    }
    m_writing = true;
    return true;
}

void Saver::drain() {
    std::function<void()> done;
    std::unique_lock<std::mutex> lock(m_mutex);
    // En orden: el archivo sale igual aunque las capas terminen en otro orden. Mientras se
    // escribe, el cerrojo queda libre (para la interfaz y los que terminan): lo que llegue
    // se ve al volver a mirar.
    while (m_written < m_entries.size() && m_entries[m_written].ready) {
        Entry entry = std::move(m_entries[m_written]);
        m_entries[m_written].data = {};
        const bool skip = m_failed;
        lock.unlock();
        std::string error;
        const bool ok = skip || writeEntry(entry, &error);
        entry = {};
        lock.lock();
        if (!ok && !m_failed) {
            m_failed = true;
            m_error = std::move(error);
        }
        ++m_written;
    }
    if (m_finishing && m_written == m_entries.size() && m_busy) {
        const bool failed = m_failed;
        std::string error = m_error;
        lock.unlock();
        Result result = close(failed, std::move(error));
        lock.lock();
        m_result = std::move(result);
        m_entries.clear();
        m_busy = false;
        done = std::move(m_onFinished);
        m_onFinished = nullptr;
    }
    m_writing = false;
    lock.unlock();
    if (done) {
        done();
    }
}

bool Saver::writeEntry(Entry& entry, std::string* error) {
    if (entry.copy) {
        // Del archivo anterior, si sigue como estaba (el índice lo dice) y se lee bien (su CRC
        // lo dice).
        const zip::Entry* stored = m_previousSource ? m_previous.find(entry.copy->name) : nullptr;
        if (!stored || stored->crc != entry.copy->crc || stored->size != entry.copy->size) {
            *error = "el archivo anterior cambió mientras tanto";
            return false;
        }
        if (!m_previous.read(*stored, entry.data, entry.maxSize)) {
            *error = "no se pudo copiar del archivo anterior: " + m_previous.error();
            return false;
        }
    }
    if (!m_zip->add(entry.name, entry.data, zip::Method::Stored)) {
        *error = SDL_GetError();
        return false;
    }
    return true;
}

Saver::Result Saver::close(bool failed, std::string error) {
    if (!failed) {
        // Lo último, la descripción: un archivo cortado antes de llegar aquí no se abre a medias.
        const std::string stack = stackXml(m_document);
        const std::string document = documentJson(m_document, m_app);
        if (!m_zip->add(kStackEntry, bytesOf(stack), zip::Method::Deflate) ||
            !m_zip->add(kDocumentEntry, bytesOf(document), zip::Method::Deflate) || !m_zip->finish()) {
            failed = true;
            error = SDL_GetError();
        }
    }
    const uint64_t size = m_zip ? m_zip->size() : 0;
    if (m_io && !SDL_CloseIO(m_io) && !failed) {
        failed = true;
        error = SDL_GetError();
    }
    m_io = nullptr;
    // El anterior se suelta antes de sustituirlo (en Windows no se podría con él abierto).
    m_previous = {};
    m_previousSource.reset();
    if (!failed && !m_tempPath.empty() && !SDL_RenamePath(m_tempPath.c_str(), m_path.c_str())) {
        failed = true;
        error = SDL_GetError();
    }
    if (failed) {
        SDL_RemovePath((m_tempPath.empty() ? m_path : m_tempPath).c_str());
    }
    Result result;
    result.ok = !failed;
    result.path = m_path;
    result.error = std::move(error);
    result.bytes = result.ok ? size : 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        result.reused = m_reused;
    }
    if (result.ok && m_zip) {
        result.entries = m_zip->entries();
    }
    m_zip.reset();
    m_document = {};
    return result;
}

bool Saver::busy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_busy;
}

void Saver::pump(uint64_t budgetMs) {
    if (m_pool) {
        m_pool->runPending(budgetMs);
    }
}

float Saver::progress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_busy) {
        return m_result ? 1.0f : 0.0f;
    }
    return m_entries.empty() ? 0.0f : static_cast<float>(m_written) / static_cast<float>(m_entries.size() + 1);
}

std::optional<Saver::Result> Saver::takeResult() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::optional<Result> result = std::move(m_result);
    m_result.reset();
    return result;
}

void Saver::wait() {
    if (m_pool) {
        m_pool->waitIdle();
    }
}

// -----------------------------------------------------------------------------
// Abrir
// -----------------------------------------------------------------------------

Loader::~Loader() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_canceled = true;
    }
    m_pool.reset();   // espera a los trabajos en curso (los que faltan ya no hacen nada)
}

bool Loader::open(const std::string& path, int maxSide) {
    SDL_IOStream* io = io::openFile(path, "rb");
    if (!io) {
        m_error = "no se pudo leer el archivo";
        return false;
    }
    auto file = std::make_unique<zip::FileSource>();
    if (file->open(io)) {
        m_source = std::move(file);
        return readDocument(maxSide);
    }
    // No se puede ir de un sitio a otro del archivo (en Android, uno que llega por una
    // tubería): se lee entero.
    file.reset();
    io = io::openFile(path, "rb");
    size_t size = 0;
    void* data = io ? SDL_LoadFile_IO(io, &size, true) : nullptr;
    if (!data) {
        m_error = "no se pudo leer el archivo";
        return false;
    }
    std::vector<uint8_t> bytes;
    try {
        bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    } catch (const std::bad_alloc&) {
        SDL_free(data);
        m_error = "no hay memoria para leerlo";
        return false;
    }
    SDL_free(data);
    return open(std::move(bytes), maxSide);
}

bool Loader::open(std::vector<uint8_t> data, int maxSide) {
    m_data = std::move(data);
    m_source = std::make_unique<zip::MemorySource>(m_data);
    return readDocument(maxSide);
}

bool Loader::readDocument(int maxSide) {
    if (!m_zip.open(*m_source)) {
        m_error = "no es un proyecto de LiveSketch o está dañado";
        return false;
    }
    const zip::Entry* entry = m_zip.find(kDocumentEntry);
    if (!entry) {
        m_error = m_zip.find(kStackEntry) ? "es un archivo OpenRaster de otro programa: por ahora solo se abren "
                                            "proyectos de LiveSketch"
                                          : "no es un proyecto de LiveSketch";
        return false;
    }
    std::vector<uint8_t> text;
    if (!m_zip.read(*entry, text, json::kMaxText)) {
        m_error = "su descripción (document.json) está dañada";
        return false;
    }
    std::vector<std::string> warnings;
    if (!project::readDocument(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), maxSide,
                               m_document, warnings, m_error)) {
        return false;
    }
    // Las capas cuya imagen falta (o dice ocupar más de lo que puede) se abren vacías.
    for (LayerInfo& layer : m_document.layers) {
        if (layer.rect.empty()) {
            continue;
        }
        const zip::Entry* file = m_zip.find(layer.file);
        if (!file || file->size > maxLayerFile(layer)) {
            warnings.push_back(damagedLayer(layer));
            layer.rect = {};
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_warnings = std::move(warnings);
    m_slots.assign(m_document.layers.size(), Slot{});
    m_queued = 0;
    m_given = 0;
    m_pending = 0;
    return true;
}

std::vector<std::string> Loader::warnings() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_warnings;
}

std::optional<zip::Entry> Loader::entry(std::string_view name) const {
    // El índice ya no cambia: se puede mirar mientras se leen las capas.
    const zip::Entry* entry = m_zip.find(name);
    return entry ? std::optional<zip::Entry>(*entry) : std::nullopt;
}

std::vector<size_t> Loader::queueMore() {
    std::vector<size_t> queued;
    while (m_queued < m_slots.size()) {
        const LayerInfo& layer = m_document.layers[m_queued];
        Slot& slot = m_slots[m_queued];
        if (layer.rect.empty()) {
            slot.ready = true;   // transparente: no hay nada que leer
            ++m_queued;
            continue;
        }
        const size_t bytes = static_cast<size_t>(layer.rect.width()) * static_cast<size_t>(layer.rect.height()) * 4;
        if (m_pending > 0 && m_pending + bytes > kMemoryBudget) {
            break;
        }
        slot.bytes = bytes;
        m_pending += bytes;
        queued.push_back(m_queued++);
    }
    return queued;
}

void Loader::start() {
    if (!m_pool) {
        m_pool = std::make_unique<io::TaskPool>();
    }
    std::vector<size_t> queued;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        queued = queueMore();
    }
    for (const size_t index : queued) {
        m_pool->submit([this, index] { decode(index); });
    }
}

void Loader::decode(size_t index) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_canceled) {
            return;
        }
    }
    // El documento ya no cambia: se puede leer desde varios hilos.
    const LayerInfo& layer = m_document.layers[index];
    std::vector<uint8_t> file;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        const zip::Entry* entry = m_zip.find(layer.file);
        ok = entry && m_zip.read(*entry, file, maxLayerFile(layer));
    }
    std::vector<uint8_t> pixels;
    if (ok) {
        ok = decodeLayer(file, layer, pixels, nullptr);
    }
    file = {};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ok) {
        pixels = {};
        m_warnings.push_back(damagedLayer(layer));
    }
    m_slots[index].pixels = std::move(pixels);
    m_slots[index].ready = true;
}

bool Loader::next(int* index, std::vector<uint8_t>& pixels) {
    std::vector<size_t> queued;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_given >= m_slots.size() || !m_slots[m_given].ready) {
            return false;
        }
        Slot& slot = m_slots[m_given];
        *index = static_cast<int>(m_given);
        pixels = std::move(slot.pixels);
        slot.pixels = {};
        m_pending -= std::min(slot.bytes, m_pending);
        slot.bytes = 0;
        ++m_given;
        queued = queueMore();
    }
    for (const size_t i : queued) {
        m_pool->submit([this, i] { decode(i); });
    }
    return true;
}

bool Loader::finished() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_given >= m_slots.size();
}

void Loader::pump(uint64_t budgetMs) {
    if (m_pool) {
        m_pool->runPending(budgetMs);
    }
}

float Loader::progress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots.empty() ? 1.0f : static_cast<float>(m_given) / static_cast<float>(m_slots.size());
}

} // namespace project
