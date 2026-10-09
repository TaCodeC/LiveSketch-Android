#include "IO/Png.h"

#include "IO/Icc.h"
#include "ThirdParty/stb_image.h"

#include <SDL3/SDL_error.h>
#include <libdeflate.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace png {
namespace {

constexpr uint8_t kSignature[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
// Nivel de deflate: el 3 comprime una capa 4K muy pintada en la sexta parte de lo que
// tardaba stb, y el 6 solo la deja un 3 % más pequeña a cambio de tardar el doble.
constexpr int kLevel = 3;
// Los datos comprimidos van en chunks IDAT de 1 MB como mucho.
constexpr size_t kIdatBytes = size_t{1} << 20;
constexpr uint8_t kFilterUp = 2;

void put32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

uint32_t get32(const uint8_t* in) {
    return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

uint8_t paeth(int left, int up, int upLeft) {
    const int p = left + up - upLeft;
    const int pa = std::abs(p - left);
    const int pb = std::abs(p - up);
    const int pc = std::abs(p - upLeft);
    if (pa <= pb && pa <= pc) {
        return static_cast<uint8_t>(left);
    }
    return static_cast<uint8_t>(pb <= pc ? up : upLeft);
}

// Deshace el filtro de una fila de RGBA8 (4 bytes por píxel). `prior`: la fila de arriba ya
// sin filtro, o null en la primera. False si el tipo de filtro no existe.
bool unfilterRow(uint8_t type, uint8_t* row, const uint8_t* prior, size_t bytes) {
    constexpr size_t kPixel = 4;
    switch (type) {
    case 0:
        return true;
    case 1:
        for (size_t i = kPixel; i < bytes; ++i) {
            row[i] = static_cast<uint8_t>(row[i] + row[i - kPixel]);
        }
        return true;
    case 2:
        if (prior) {
            for (size_t i = 0; i < bytes; ++i) {
                row[i] = static_cast<uint8_t>(row[i] + prior[i]);
            }
        }
        return true;
    case 3:
        for (size_t i = 0; i < bytes; ++i) {
            const int left = i >= kPixel ? row[i - kPixel] : 0;
            const int up = prior ? prior[i] : 0;
            row[i] = static_cast<uint8_t>(row[i] + ((left + up) >> 1));
        }
        return true;
    case 4:
        for (size_t i = 0; i < bytes; ++i) {
            const int left = i >= kPixel ? row[i - kPixel] : 0;
            const int up = prior ? prior[i] : 0;
            const int upLeft = prior && i >= kPixel ? prior[i - kPixel] : 0;
            row[i] = static_cast<uint8_t>(row[i] + paeth(left, up, upLeft));
        }
        return true;
    default:
        return false;
    }
}

// Un chunk: largo, tipo, datos y el CRC del tipo y los datos.
bool chunk(const Sink& sink, const char type[4], const uint8_t* data, size_t size) {
    uint8_t head[8];
    put32(head, static_cast<uint32_t>(size));
    std::memcpy(head + 4, type, 4);
    uint32_t crc = libdeflate_crc32(0, head + 4, 4);
    if (size > 0) {
        crc = libdeflate_crc32(crc, data, size);
    }
    uint8_t tail[4];
    put32(tail, crc);
    return sink(head, sizeof(head)) && (size == 0 || sink(data, size)) && sink(tail, sizeof(tail));
}

bool chunk(const Sink& sink, const char type[4], const std::vector<uint8_t>& data) {
    return chunk(sink, type, data.data(), data.size());
}

void append(std::vector<uint8_t>& out, const char* text) {
    out.insert(out.end(), text, text + std::strlen(text) + 1);   // con el 0 del final
}

bool noMemory() {
    SDL_SetError("no hay memoria para comprimir la imagen");
    return false;
}

// Comprime `data` con zlib. Vacío si no hay memoria.
std::vector<uint8_t> zlibCompress(const uint8_t* data, size_t size, int level) {
    std::vector<uint8_t> out;
    libdeflate_compressor* compressor = libdeflate_alloc_compressor(level);
    if (!compressor) {
        return out;
    }
    try {
        out.resize(libdeflate_zlib_compress_bound(compressor, size));
    } catch (const std::bad_alloc&) {
        libdeflate_free_compressor(compressor);
        return {};
    }
    const size_t compressed = libdeflate_zlib_compress(compressor, data, size, out.data(), out.size());
    libdeflate_free_compressor(compressor);
    out.resize(compressed);
    return out;
}

// El chunk iCCP de un perfil: su nombre, la compresión (0, deflate) y el perfil ICC
// comprimido. Vacío si no hay memoria.
std::vector<uint8_t> iccChunk(ColorProfile profile) {
    const std::vector<uint8_t>& icc = icc::profile(profile);
    const std::vector<uint8_t> compressed = zlibCompress(icc.data(), icc.size(), 9);
    if (compressed.empty()) {
        return {};
    }
    std::vector<uint8_t> out;
    append(out, colorspace::name(profile));
    out.push_back(0);
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

// cHRM: el blanco y los primarios rojo, verde y azul (x e y por 100000).
bool chromaticities(const Sink& sink, const uint32_t (&values)[8]) {
    uint8_t data[32];
    for (int i = 0; i < 8; ++i) {
        put32(data + i * 4, values[i]);
    }
    return chunk(sink, "cHRM", data, sizeof(data));
}

} // namespace

uint32_t pixelsPerMeter(float ppi) {
    if (!std::isfinite(ppi) || ppi <= 0.0f) {
        return 0;
    }
    return static_cast<uint32_t>(std::lround(std::min(static_cast<double>(ppi), 1.0e6) / 0.0254));
}

std::vector<uint8_t> filterRows(const uint8_t* rgba, int width, int height, size_t stride) {
    std::vector<uint8_t> out;
    if (width <= 0 || height <= 0) {
        return out;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    try {
        out.resize((rowBytes + 1) * static_cast<size_t>(height));
    } catch (const std::bad_alloc&) {
        return {};
    }
    uint8_t* to = out.data();
    const uint8_t* above = nullptr;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
        *to++ = kFilterUp;   // en la primera fila, la de arriba cuenta como ceros
        if (above) {
            for (size_t i = 0; i < rowBytes; ++i) {
                to[i] = static_cast<uint8_t>(row[i] - above[i]);
            }
        } else {
            std::memcpy(to, row, rowBytes);
        }
        to += rowBytes;
        above = row;
    }
    return out;
}

bool filterRowsInPlace(std::vector<uint8_t>& image, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    const size_t rows = static_cast<size_t>(height);
    if (image.size() != rowBytes * rows) {
        return false;
    }
    std::vector<uint8_t> row;
    try {
        row.resize(rowBytes);
        image.resize((rowBytes + 1) * rows);
    } catch (const std::bad_alloc&) {
        return false;
    }
    // De abajo arriba, cada fila filtrada va un byte por fila más adelante que la original:
    // así nunca pisa la de arriba, que hace falta para filtrar esta. La fila se copia antes
    // porque su sitio nuevo se solapa con el viejo.
    uint8_t* data = image.data();
    for (size_t y = rows; y-- > 0;) {
        std::memcpy(row.data(), data + y * rowBytes, rowBytes);
        uint8_t* to = data + y * (rowBytes + 1);
        if (y > 0) {
            const uint8_t* above = data + (y - 1) * rowBytes;
            for (size_t i = 0; i < rowBytes; ++i) {
                to[1 + i] = static_cast<uint8_t>(row[i] - above[i]);
            }
        } else {
            std::memcpy(to + 1, row.data(), rowBytes);
        }
        to[0] = kFilterUp;
    }
    return true;
}

namespace {

// Lo que va antes de los datos de la imagen: la cabecera, el perfil de color, la resolución
// y los textos. `profile`: el chunk iCCP de Display P3 (vacío con sRGB). `cicp`: también el
// chunk cICP (no se usa con paleta).
bool writeHead(const Sink& sink, int width, int height, uint8_t depth, uint8_t colorType, const Info& info,
               const std::vector<uint8_t>& profile, bool cicp) {
    if (!sink(kSignature, sizeof(kSignature))) {
        return false;
    }
    uint8_t header[13];
    put32(header, static_cast<uint32_t>(width));
    put32(header + 4, static_cast<uint32_t>(height));
    header[8] = depth;       // bits por canal (o por índice de la paleta)
    header[9] = colorType;   // 6: RGBA; 3: paleta
    header[10] = 0;          // deflate
    header[11] = 0;          // filtros por fila
    header[12] = 0;          // sin entrelazar
    if (!chunk(sink, "IHDR", header, sizeof(header))) {
        return false;
    }

    // El perfil de color. sRGB: el chunk sRGB (intención perceptual). Display P3: cICP
    // (primarios P3, curva de sRGB, RGB y rango completo), que leen los programas recientes,
    // y el perfil ICC (iCCP) para los demás; con iCCP no puede ir el chunk sRGB. Además, gAMA
    // y cHRM con la curva y los primarios para quien no lea nada de eso (lo que recomienda
    // la especificación).
    const bool displayP3 = info.profile == ColorProfile::DisplayP3;
    if (displayP3) {
        const uint8_t code[4] = {12, 13, 0, 1};
        if ((cicp && !chunk(sink, "cICP", code, sizeof(code))) || !chunk(sink, "iCCP", profile)) {
            return false;
        }
    } else {
        const uint8_t intent = 0;
        if (!chunk(sink, "sRGB", &intent, 1)) {
            return false;
        }
    }
    {
        uint8_t gamma[4];
        put32(gamma, 45455);
        if (!chunk(sink, "gAMA", gamma, sizeof(gamma))) {
            return false;
        }
        constexpr uint32_t kSrgb[8] = {31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000};
        constexpr uint32_t kDisplayP3[8] = {31270, 32900, 68000, 32000, 26500, 69000, 15000, 6000};
        if (!chromaticities(sink, displayP3 ? kDisplayP3 : kSrgb)) {
            return false;
        }
    }

    // Resolución: píxeles por metro, igual en los dos ejes.
    if (const uint32_t ppm = pixelsPerMeter(info.ppi); ppm > 0) {
        uint8_t physical[9];
        put32(physical, ppm);
        put32(physical + 4, ppm);
        physical[8] = 1;   // la unidad es el metro
        if (!chunk(sink, "pHYs", physical, sizeof(physical))) {
            return false;
        }
    }

    // Textos: el programa y, en UTF-8 (iTXt), el nombre del lienzo.
    {
        std::vector<uint8_t> software;
        append(software, "Software");
        const char* name = "LiveSketch";
        software.insert(software.end(), name, name + std::strlen(name));
        if (!chunk(sink, "tEXt", software)) {
            return false;
        }
    }
    if (!info.title.empty() && info.title.find('\0') == std::string::npos) {
        std::vector<uint8_t> title;
        append(title, "Title");
        title.push_back(0);   // sin comprimir
        title.push_back(0);
        append(title, "");    // idioma
        append(title, "");    // palabra clave traducida
        title.insert(title.end(), info.title.begin(), info.title.end());
        if (!chunk(sink, "iTXt", title)) {
            return false;
        }
    }
    return true;
}

// Los datos comprimidos en chunks IDAT y el final.
bool writeData(const Sink& sink, const std::vector<uint8_t>& compressed) {
    for (size_t at = 0; at < compressed.size(); at += kIdatBytes) {
        if (!chunk(sink, "IDAT", compressed.data() + at, std::min(kIdatBytes, compressed.size() - at))) {
            return false;
        }
    }
    return chunk(sink, "IEND", nullptr, 0);
}

} // namespace

bool write(const std::vector<uint8_t>& filtered, int width, int height, const Info& info, const Sink& sink) {
    if (width <= 0 || height <= 0 ||
        filtered.size() != (static_cast<size_t>(width) * 4 + 1) * static_cast<size_t>(height)) {
        SDL_SetError("tamaño de imagen no válido");
        return false;
    }

    // Comprimir primero: si no hay memoria, no se escribe nada.
    const std::vector<uint8_t> compressed = zlibCompress(filtered.data(), filtered.size(), kLevel);
    const bool displayP3 = info.profile == ColorProfile::DisplayP3;
    const std::vector<uint8_t> profile = displayP3 ? iccChunk(info.profile) : std::vector<uint8_t>();
    if (compressed.empty() || (displayP3 && profile.empty())) {
        return noMemory();
    }
    return writeHead(sink, width, height, 8, 6, info, profile, true) && writeData(sink, compressed);
}

bool writeSolid(int width, int height, const uint8_t rgb[3], const Info& info, const Sink& sink) {
    if (width <= 0 || height <= 0) {
        SDL_SetError("tamaño de imagen no válido");
        return false;
    }
    // Cada fila: el filtro (ninguno) y un bit por píxel, todos el color 0 de la paleta. Todo
    // ceros, que deflate deja en casi nada.
    const size_t rowBytes = 1 + (static_cast<size_t>(width) + 7) / 8;
    std::vector<uint8_t> rows;
    try {
        rows.assign(rowBytes * static_cast<size_t>(height), 0);
    } catch (const std::bad_alloc&) {
        return noMemory();
    }
    const std::vector<uint8_t> compressed = zlibCompress(rows.data(), rows.size(), 6);
    rows = {};
    const bool displayP3 = info.profile == ColorProfile::DisplayP3;
    const std::vector<uint8_t> profile = displayP3 ? iccChunk(info.profile) : std::vector<uint8_t>();
    if (compressed.empty() || (displayP3 && profile.empty())) {
        return noMemory();
    }
    return writeHead(sink, width, height, 1, 3, info, profile, false) && chunk(sink, "PLTE", rgb, 3) &&
           writeData(sink, compressed);
}

bool decode(std::span<const uint8_t> data, Image& out, size_t maxPixels, std::string* error) {
    out = {};
    auto fail = [error](const char* why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    if (data.size() < sizeof(kSignature) || std::memcmp(data.data(), kSignature, sizeof(kSignature)) != 0) {
        return fail("no es un PNG");
    }

    // Primero se recorren los chunks comprobando su CRC: la cabecera, cuánto ocupan los
    // datos de la imagen (IDAT) y que llegue hasta el final (IEND).
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t depth = 0;
    uint8_t colorType = 0;
    bool simple = false;     // RGBA de 8 bits sin entrelazar
    bool header = false;
    bool end = false;
    size_t idatBytes = 0;
    for (size_t at = sizeof(kSignature); at < data.size() && !end;) {
        if (data.size() - at < 12) {
            return fail("el PNG está cortado");
        }
        const uint32_t length = get32(&data[at]);
        if (length > 0x7FFFFFFFu || data.size() - at - 12 < length) {
            return fail("el PNG está cortado");
        }
        const uint8_t* type = &data[at + 4];
        const uint8_t* body = type + 4;
        if (libdeflate_crc32(0, type, length + size_t{4}) != get32(body + length)) {
            return fail("el PNG está dañado");
        }
        if (!header) {
            if (std::memcmp(type, "IHDR", 4) != 0 || length != 13) {
                return fail("el PNG está dañado");
            }
            width = get32(body);
            height = get32(body + 4);
            depth = body[8];
            colorType = body[9];
            if (width == 0 || height == 0 || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu || body[10] != 0 ||
                body[11] != 0 || body[12] > 1) {
                return fail("el PNG está dañado");
            }
            if (static_cast<uint64_t>(width) * height > maxPixels) {
                return fail("la imagen es demasiado grande");
            }
            simple = depth == 8 && colorType == 6 && body[12] == 0;
            header = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idatBytes += length;
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            end = true;
        }
        at += size_t{12} + length;
    }
    if (!header || !end || idatBytes == 0) {
        return fail("el PNG está cortado");
    }

    if (!simple) {
        // Otros tipos (gris, RGB, paleta, 16 bits, entrelazado): stb_image, pasado a RGBA8.
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return fail("la imagen es demasiado grande");
        }
        int w = 0;
        int h = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels, 4);
        if (!pixels) {
            return fail("el PNG está dañado");
        }
        if (static_cast<uint32_t>(w) != width || static_cast<uint32_t>(h) != height) {
            stbi_image_free(pixels);
            return fail("el PNG está dañado");
        }
        try {
            out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        } catch (const std::bad_alloc&) {
            stbi_image_free(pixels);
            return fail("no hay memoria para leer la imagen");
        }
        stbi_image_free(pixels);
        out.width = w;
        out.height = h;
        return true;
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> pixels;
    try {
        compressed.reserve(idatBytes);
        for (size_t at = sizeof(kSignature); at < data.size();) {
            const uint32_t length = get32(&data[at]);
            const uint8_t* type = &data[at + 4];
            if (std::memcmp(type, "IDAT", 4) == 0) {
                compressed.insert(compressed.end(), type + 4, type + 4 + length);
            } else if (std::memcmp(type, "IEND", 4) == 0) {
                break;
            }
            at += size_t{12} + length;
        }
        pixels.resize((rowBytes + 1) * height);
    } catch (const std::bad_alloc&) {
        return fail("no hay memoria para leer la imagen");
    }
    libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    if (!decompressor) {
        return fail("no hay memoria para leer la imagen");
    }
    // Sin actual_out: los datos tienen que dar exactamente las filas de la cabecera.
    const libdeflate_result result = libdeflate_zlib_decompress(decompressor, compressed.data(), compressed.size(),
                                                                pixels.data(), pixels.size(), nullptr);
    libdeflate_free_decompressor(decompressor);
    compressed = {};
    if (result != LIBDEFLATE_SUCCESS) {
        return fail("el PNG está dañado");
    }

    // Cada fila se deshace en su sitio (mirando la de arriba, ya en el suyo) y se junta con
    // las anteriores, sin el byte del filtro: todo en el mismo buffer.
    for (size_t y = 0; y < height; ++y) {
        uint8_t* row = pixels.data() + y * (rowBytes + 1);
        const uint8_t type = row[0];
        uint8_t* target = pixels.data() + y * rowBytes;
        const uint8_t* prior = y > 0 ? target - rowBytes : nullptr;
        if (!unfilterRow(type, row + 1, prior, rowBytes)) {
            return fail("el PNG está dañado");
        }
        std::memmove(target, row + 1, rowBytes);
    }
    pixels.resize(rowBytes * height);
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.rgba = std::move(pixels);
    return true;
}

bool encode(const uint8_t* rgba, int width, int height, size_t stride, const Info& info, const Sink& sink) {
    if (width <= 0 || height <= 0) {
        SDL_SetError("tamaño de imagen no válido");
        return false;
    }
    const std::vector<uint8_t> filtered = filterRows(rgba, width, height, stride);
    if (filtered.empty()) {
        return noMemory();
    }
    return write(filtered, width, height, info, sink);
}

} // namespace png
