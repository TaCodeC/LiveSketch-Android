#include "IO/Png.h"

#include "IO/Icc.h"

#include <SDL3/SDL_error.h>
#include <libdeflate.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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

    if (!sink(kSignature, sizeof(kSignature))) {
        return false;
    }
    uint8_t header[13];
    put32(header, static_cast<uint32_t>(width));
    put32(header + 4, static_cast<uint32_t>(height));
    header[8] = 8;    // bits por canal
    header[9] = 6;    // RGBA
    header[10] = 0;   // deflate
    header[11] = 0;   // filtros por fila
    header[12] = 0;   // sin entrelazar
    if (!chunk(sink, "IHDR", header, sizeof(header))) {
        return false;
    }

    // El perfil de color. sRGB: el chunk sRGB (intención perceptual). Display P3: cICP
    // (primarios P3, curva de sRGB, RGB y rango completo), que leen los programas recientes,
    // y el perfil ICC (iCCP) para los demás; con iCCP no puede ir el chunk sRGB. Además, gAMA
    // y cHRM con la curva y los primarios para quien no lea nada de eso (lo que recomienda
    // la especificación).
    if (displayP3) {
        const uint8_t cicp[4] = {12, 13, 0, 1};
        if (!chunk(sink, "cICP", cicp, sizeof(cicp)) || !chunk(sink, "iCCP", profile)) {
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

    for (size_t at = 0; at < compressed.size(); at += kIdatBytes) {
        if (!chunk(sink, "IDAT", compressed.data() + at, std::min(kIdatBytes, compressed.size() - at))) {
            return false;
        }
    }
    return chunk(sink, "IEND", nullptr, 0);
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
