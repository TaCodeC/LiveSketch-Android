#include "IO/Zip.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>
#include <libdeflate.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace zip {
namespace {

constexpr uint32_t kLocalSignature = 0x04034b50;
constexpr uint32_t kCentralSignature = 0x02014b50;
constexpr uint32_t kEndSignature = 0x06054b50;
constexpr size_t kLocalHeader = 30;
constexpr size_t kCentralHeader = 46;
constexpr size_t kEndRecord = 22;
constexpr uint16_t kUtf8Flag = 1 << 11;
constexpr uint16_t kEncryptedFlag = 1;
// Hecho en Unix con la versión 2.0 de la especificación (deflate); un archivo normal con
// permisos rw-r--r--.
constexpr uint16_t kMadeBy = (3 << 8) | 20;
constexpr uint32_t kFileAttributes = 0100644u << 16;
// Directorio central como mucho (unas pocas entradas por capa ocupan muy poco).
constexpr uint64_t kMaxDirectory = uint64_t{16} << 20;

void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool ascii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// Fecha y hora de MS-DOS (con segundos pares, desde 1980) en hora local.
void dosTime(int64_t time, uint16_t* dosTime, uint16_t* dosDate) {
    SDL_DateTime date;
    SDL_zero(date);
    if (!SDL_TimeToDateTime(time, &date, true) || date.year < 1980) {
        *dosTime = 0;
        *dosDate = (1 << 5) | 1;   // 1 de enero de 1980
        return;
    }
    const int year = std::min(date.year, 2107);
    *dosTime = static_cast<uint16_t>((date.hour << 11) | (date.minute << 5) | (date.second / 2));
    *dosDate = static_cast<uint16_t>(((year - 1980) << 9) | (date.month << 5) | date.day);
}

} // namespace

// -----------------------------------------------------------------------------
// Writer
// -----------------------------------------------------------------------------

Writer::Writer(Sink sink, int64_t time) : m_sink(std::move(sink)) {
    dosTime(time, &m_dosTime, &m_dosDate);
}

bool Writer::write(const void* data, size_t size) {
    if (m_failed) {
        return false;
    }
    if (size > 0 && !m_sink(static_cast<const uint8_t*>(data), size)) {
        m_failed = true;
        return false;
    }
    m_offset += size;
    return true;
}

bool Writer::add(std::string_view name, std::span<const uint8_t> data, Method method, int level) {
    if (m_failed) {
        return false;
    }
    if (name.empty() || name.size() > 0xFFFF || m_entries.size() >= 0xFFFF) {
        SDL_SetError("entrada de ZIP no válida");
        return false;
    }
    if (data.size() >= 0xFFFFFFFFu || m_offset >= 0xFFFFFFFFu) {
        SDL_SetError("el archivo pasa de 4 GB");
        return false;
    }

    // Comprimir, si se pide y gana algo.
    std::vector<uint8_t> compressed;
    if (method == Method::Deflate && !data.empty()) {
        libdeflate_compressor* compressor = libdeflate_alloc_compressor(std::clamp(level, 1, 12));
        if (!compressor) {
            SDL_SetError("no hay memoria para comprimir");
            return false;
        }
        try {
            compressed.resize(libdeflate_deflate_compress_bound(compressor, data.size()));
        } catch (const std::bad_alloc&) {
            libdeflate_free_compressor(compressor);
            SDL_SetError("no hay memoria para comprimir");
            return false;
        }
        const size_t size =
            libdeflate_deflate_compress(compressor, data.data(), data.size(), compressed.data(), compressed.size());
        libdeflate_free_compressor(compressor);
        if (size == 0 || size >= data.size()) {
            method = Method::Stored;
            compressed = {};
        } else {
            compressed.resize(size);
        }
    } else {
        method = Method::Stored;
    }
    const std::span<const uint8_t> stored = method == Method::Deflate ? std::span<const uint8_t>(compressed) : data;

    Entry entry;
    entry.name = std::string(name);
    entry.method = static_cast<uint16_t>(method);
    entry.flags = ascii(name) ? 0 : kUtf8Flag;
    entry.crc = libdeflate_crc32(0, data.data(), data.size());
    entry.compressedSize = static_cast<uint32_t>(stored.size());
    entry.size = static_cast<uint32_t>(data.size());
    entry.offset = static_cast<uint32_t>(m_offset);

    std::vector<uint8_t> header;
    header.reserve(kLocalHeader + name.size());
    put32(header, kLocalSignature);
    put16(header, method == Method::Deflate ? 20 : 10);   // versión necesaria
    put16(header, entry.flags);
    put16(header, entry.method);
    put16(header, m_dosTime);
    put16(header, m_dosDate);
    put32(header, entry.crc);
    put32(header, entry.compressedSize);
    put32(header, entry.size);
    put16(header, static_cast<uint16_t>(name.size()));
    put16(header, 0);   // sin campo extra (OpenRaster lo pide así para «mimetype»)
    header.insert(header.end(), name.begin(), name.end());
    if (!write(header.data(), header.size()) || !write(stored.data(), stored.size())) {
        return false;
    }
    m_entries.push_back(std::move(entry));
    return true;
}

bool Writer::finish() {
    if (m_failed) {
        return false;
    }
    const uint64_t directoryOffset = m_offset;
    std::vector<uint8_t> directory;
    for (const Entry& entry : m_entries) {
        put32(directory, kCentralSignature);
        put16(directory, kMadeBy);
        put16(directory, entry.method == static_cast<uint16_t>(Method::Deflate) ? 20 : 10);
        put16(directory, entry.flags);
        put16(directory, entry.method);
        put16(directory, m_dosTime);
        put16(directory, m_dosDate);
        put32(directory, entry.crc);
        put32(directory, entry.compressedSize);
        put32(directory, entry.size);
        put16(directory, static_cast<uint16_t>(entry.name.size()));
        put16(directory, 0);   // extra
        put16(directory, 0);   // comentario
        put16(directory, 0);   // disco
        put16(directory, 0);   // atributos internos
        put32(directory, kFileAttributes);
        put32(directory, entry.offset);
        directory.insert(directory.end(), entry.name.begin(), entry.name.end());
    }
    const size_t directorySize = directory.size();
    if (directoryOffset + directorySize >= 0xFFFFFFFFu) {
        SDL_SetError("el archivo pasa de 4 GB");
        m_failed = true;
        return false;
    }
    put32(directory, kEndSignature);
    put16(directory, 0);   // este disco
    put16(directory, 0);   // disco del directorio
    put16(directory, static_cast<uint16_t>(m_entries.size()));
    put16(directory, static_cast<uint16_t>(m_entries.size()));
    put32(directory, static_cast<uint32_t>(directorySize));
    put32(directory, static_cast<uint32_t>(directoryOffset));
    put16(directory, 0);   // sin comentario
    return write(directory.data(), directory.size());
}

// -----------------------------------------------------------------------------
// Fuentes
// -----------------------------------------------------------------------------

bool MemorySource::read(uint64_t offset, void* data, size_t size) {
    if (offset > m_data.size() || size > m_data.size() - offset) {
        return false;
    }
    if (size > 0) {
        std::memcpy(data, m_data.data() + offset, size);
    }
    return true;
}

FileSource::~FileSource() {
    if (m_io) {
        SDL_CloseIO(m_io);
    }
}

bool FileSource::open(SDL_IOStream* io) {
    if (m_io) {
        SDL_CloseIO(m_io);
    }
    m_io = io;
    m_size = 0;
    if (!m_io) {
        return false;
    }
    const Sint64 size = SDL_GetIOSize(m_io);
    if (size < 0 || SDL_SeekIO(m_io, 0, SDL_IO_SEEK_SET) < 0) {
        return false;
    }
    m_size = static_cast<uint64_t>(size);
    return true;
}

bool FileSource::read(uint64_t offset, void* data, size_t size) {
    if (!m_io || offset > m_size || size > m_size - offset) {
        return false;
    }
    if (offset > static_cast<uint64_t>(std::numeric_limits<Sint64>::max()) ||
        SDL_SeekIO(m_io, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0) {
        return false;
    }
    auto* out = static_cast<uint8_t*>(data);
    while (size > 0) {
        const size_t got = SDL_ReadIO(m_io, out, size);
        if (got == 0) {
            return false;
        }
        out += got;
        size -= got;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Reader
// -----------------------------------------------------------------------------

bool Reader::fail(std::string message) {
    m_error = std::move(message);
    return false;
}

bool Reader::open(Source& source) {
    m_source = &source;
    m_entries.clear();
    m_error.clear();
    const uint64_t size = source.size();
    if (size < kEndRecord) {
        return fail("no es un archivo ZIP");
    }

    // El final del directorio va al final del archivo, tras un comentario de hasta 64 KB.
    const size_t tailSize = static_cast<size_t>(std::min<uint64_t>(size, kEndRecord + 0xFFFF));
    std::vector<uint8_t> tail(tailSize);
    if (!source.read(size - tailSize, tail.data(), tailSize)) {
        return fail("no se pudo leer el archivo");
    }
    size_t end = std::string::npos;
    for (size_t i = tailSize - kEndRecord + 1; i-- > 0;) {
        if (get32(&tail[i]) == kEndSignature && i + kEndRecord + get16(&tail[i + 20]) <= tailSize) {
            end = i;
            break;
        }
    }
    if (end == std::string::npos) {
        return fail("no es un archivo ZIP o está cortado");
    }
    const uint8_t* record = &tail[end];
    const uint64_t endOffset = size - tailSize + end;
    const uint16_t disk = get16(record + 4);
    const uint16_t directoryDisk = get16(record + 6);
    const uint16_t diskEntries = get16(record + 8);
    const uint16_t count = get16(record + 10);
    const uint32_t directorySize = get32(record + 12);
    const uint32_t directoryOffset = get32(record + 16);
    if (disk != 0 || directoryDisk != 0 || diskEntries != count) {
        return fail("es un ZIP en varias partes");
    }
    if (count == 0xFFFF || directorySize == 0xFFFFFFFFu || directoryOffset == 0xFFFFFFFFu) {
        return fail("es un ZIP64, que no se puede leer");
    }
    if (count > kMaxEntries) {
        return fail("tiene demasiados archivos dentro");
    }
    if (directorySize > kMaxDirectory || uint64_t{directoryOffset} + directorySize > endOffset) {
        return fail("el índice del ZIP está dañado");
    }

    std::vector<uint8_t> directory(directorySize);
    if (!source.read(directoryOffset, directory.data(), directory.size())) {
        return fail("no se pudo leer el índice del ZIP");
    }
    size_t at = 0;
    m_entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (directory.size() - at < kCentralHeader || get32(&directory[at]) != kCentralSignature) {
            return fail("el índice del ZIP está dañado");
        }
        const uint8_t* h = &directory[at];
        const uint16_t flags = get16(h + 8);
        const uint16_t method = get16(h + 10);
        const uint32_t crc = get32(h + 16);
        const uint32_t compressedSize = get32(h + 20);
        const uint32_t entrySize = get32(h + 24);
        const size_t nameLength = get16(h + 28);
        const size_t extraLength = get16(h + 30);
        const size_t commentLength = get16(h + 32);
        const uint32_t headerOffset = get32(h + 42);
        const size_t recordSize = kCentralHeader + nameLength + extraLength + commentLength;
        if (directory.size() - at < recordSize) {
            return fail("el índice del ZIP está dañado");
        }
        if (compressedSize == 0xFFFFFFFFu || entrySize == 0xFFFFFFFFu || headerOffset == 0xFFFFFFFFu) {
            return fail("es un ZIP64, que no se puede leer");
        }
        Entry entry;
        entry.name.assign(reinterpret_cast<const char*>(h + kCentralHeader), nameLength);
        if (entry.name.find('\0') != std::string::npos) {
            return fail("el índice del ZIP está dañado");
        }
        // Un método que no se sabe leer (o cifrado) queda como uno imposible: read() falla.
        entry.method = (flags & kEncryptedFlag) ? static_cast<Method>(0xFFFF) : static_cast<Method>(method);
        entry.crc = crc;
        entry.compressedSize = compressedSize;
        entry.size = entrySize;
        entry.headerOffset = headerOffset;
        if (entry.headerOffset + kLocalHeader > directoryOffset) {
            return fail("el índice del ZIP está dañado");
        }
        m_entries.push_back(std::move(entry));
        at += recordSize;
    }
    return true;
}

const Entry* Reader::find(std::string_view name) const {
    for (const Entry& entry : m_entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

bool Reader::read(const Entry& entry, std::vector<uint8_t>& out, size_t maxSize) {
    out.clear();
    if (!m_source) {
        return fail("no hay ningún ZIP abierto");
    }
    if (entry.method != Method::Stored && entry.method != Method::Deflate) {
        return fail(entry.name + " usa una compresión que no se puede leer");
    }
    if (entry.size > maxSize) {
        return fail(entry.name + " es demasiado grande");
    }
    // Deflate casi no hace crecer nada; más que esto no puede salir de un archivo válido.
    const uint64_t maxCompressed = entry.method == Method::Stored ? entry.size : entry.size + entry.size / 8 + 4096;
    if (entry.compressedSize > maxCompressed) {
        return fail(entry.name + " está dañado");
    }

    uint8_t local[kLocalHeader];
    if (!m_source->read(entry.headerOffset, local, sizeof(local)) || get32(local) != kLocalSignature) {
        return fail(entry.name + " está dañado");
    }
    const uint64_t dataOffset = entry.headerOffset + kLocalHeader + get16(local + 26) + get16(local + 28);
    if (dataOffset > m_source->size() || entry.compressedSize > m_source->size() - dataOffset) {
        return fail(entry.name + " está cortado");
    }

    try {
        if (entry.method == Method::Stored) {
            out.resize(static_cast<size_t>(entry.size));
            if (!m_source->read(dataOffset, out.data(), out.size())) {
                out.clear();
                return fail("no se pudo leer " + entry.name);
            }
        } else {
            std::vector<uint8_t> compressed(static_cast<size_t>(entry.compressedSize));
            if (!m_source->read(dataOffset, compressed.data(), compressed.size())) {
                return fail("no se pudo leer " + entry.name);
            }
            out.resize(static_cast<size_t>(entry.size));
            libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
            if (!decompressor) {
                out.clear();
                return fail("no hay memoria para descomprimir " + entry.name);
            }
            // Sin actual_out: tiene que salir exactamente del tamaño que dice el índice.
            const libdeflate_result result = libdeflate_deflate_decompress(
                decompressor, compressed.data(), compressed.size(), out.data(), out.size(), nullptr);
            libdeflate_free_decompressor(decompressor);
            if (result != LIBDEFLATE_SUCCESS) {
                out.clear();
                return fail(entry.name + " está dañado");
            }
        }
    } catch (const std::bad_alloc&) {
        out.clear();
        return fail("no hay memoria para leer " + entry.name);
    }
    if (libdeflate_crc32(0, out.data(), out.size()) != entry.crc) {
        out.clear();
        return fail(entry.name + " está dañado");
    }
    return true;
}

} // namespace zip
