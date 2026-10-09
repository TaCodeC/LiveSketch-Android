#pragma once

#include <SDL3/SDL_iostream.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ZIP sencillo para los archivos de proyecto: escribir entradas tal cual o comprimidas con
// deflate (libdeflate) y leerlas comprobando su tamaño y su CRC. Sin ZIP64 (cada entrada y el
// ZIP entero, menos de 4 GB), sin cifrado y en una sola parte.
namespace zip {

// Recibe el ZIP por partes, en orden. Devuelve false si no pudo (y se para).
using Sink = std::function<bool(const uint8_t* data, size_t size)>;

enum class Method : uint16_t { Stored = 0, Deflate = 8 };

class Writer {
public:
    // `time`: fecha de las entradas (SDL_Time; se guarda en hora local, como hace todo ZIP).
    Writer(Sink sink, int64_t time);

    // Añade una entrada. Con Deflate se comprime al nivel `level`; si no gana nada, va tal
    // cual. Los nombres, con '/' entre carpetas. Si falla, deja el motivo en SDL_GetError().
    bool add(std::string_view name, std::span<const uint8_t> data, Method method, int level = 6);
    // Escribe el directorio central: el ZIP queda completo.
    bool finish();
    // Bytes escritos hasta ahora.
    uint64_t size() const { return m_offset; }

private:
    struct Entry {
        std::string name;
        uint16_t method = 0;
        uint16_t flags = 0;
        uint32_t crc = 0;
        uint32_t compressedSize = 0;
        uint32_t size = 0;
        uint32_t offset = 0;
    };
    bool write(const void* data, size_t size);

    Sink m_sink;
    uint16_t m_dosTime = 0;
    uint16_t m_dosDate = 0;
    uint64_t m_offset = 0;
    std::vector<Entry> m_entries;
    bool m_failed = false;
};

// Lo que se lee: un archivo o un bloque de memoria, con acceso aleatorio.
class Source {
public:
    virtual ~Source() = default;
    virtual uint64_t size() const = 0;
    // Lee `size` bytes desde `offset`. False si no hay tantos o falla la lectura.
    virtual bool read(uint64_t offset, void* data, size_t size) = 0;
};

class MemorySource : public Source {
public:
    explicit MemorySource(std::span<const uint8_t> data) : m_data(data) {}
    uint64_t size() const override { return m_data.size(); }
    bool read(uint64_t offset, void* data, size_t size) override;

private:
    std::span<const uint8_t> m_data;
};

// Un archivo abierto con SDL (en Android también un content://). Necesita poder moverse por
// él: si no se puede (una tubería), open() devuelve false y hay que leerlo entero a memoria.
class FileSource : public Source {
public:
    FileSource() = default;
    ~FileSource() override;
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    // Se queda con `io` (lo cierra al destruirse), también si devuelve false.
    bool open(SDL_IOStream* io);
    uint64_t size() const override { return m_size; }
    bool read(uint64_t offset, void* data, size_t size) override;

private:
    SDL_IOStream* m_io = nullptr;
    uint64_t m_size = 0;
};

struct Entry {
    std::string name;
    Method method = Method::Stored;
    uint32_t crc = 0;
    uint64_t compressedSize = 0;
    uint64_t size = 0;
    uint64_t headerOffset = 0;   // de su cabecera local
};

class Reader {
public:
    // Lee el directorio central de `source`, que tiene que durar lo que dure el Reader. False
    // si no es un ZIP que se pueda leer (motivo en error()).
    bool open(Source& source);
    const std::vector<Entry>& entries() const { return m_entries; }
    // La entrada con ese nombre (la primera, si hay varias), o null.
    const Entry* find(std::string_view name) const;
    // Lee la entrada y la descomprime comprobando su tamaño y su CRC. False si está dañada,
    // usa algo que no se sabe leer o descomprimida ocupa más de `maxSize` (motivo en error()).
    bool read(const Entry& entry, std::vector<uint8_t>& out, size_t maxSize);
    const std::string& error() const { return m_error; }

private:
    bool fail(std::string message);

    Source* m_source = nullptr;
    std::vector<Entry> m_entries;
    std::string m_error;
};

// Entradas como mucho en un ZIP que se lee (un proyecto tiene unas pocas por capa).
inline constexpr size_t kMaxEntries = 4096;

} // namespace zip
