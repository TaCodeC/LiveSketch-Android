#include "Gfx/Pixels.h"

#include <algorithm>

namespace gfx {
namespace {

// Tabla para quitar el premultiplicado: values[a][c] = c * 255 / a, redondeado.
struct UnpremultiplyTable {
    uint8_t values[256][256];
    UnpremultiplyTable() {
        for (int a = 0; a < 256; ++a) {
            for (int c = 0; c < 256; ++c) {
                values[a][c] = a == 0 ? 0 : static_cast<uint8_t>(std::min(255, (c * 255 + a / 2) / a));
            }
        }
    }
};

const UnpremultiplyTable& unpremultiplyTable() {
    static const UnpremultiplyTable table;
    return table;
}

// Y para premultiplicar: values[a][c] = c * a / 255, redondeado.
struct PremultiplyTable {
    uint8_t values[256][256];
    PremultiplyTable() {
        for (int a = 0; a < 256; ++a) {
            for (int c = 0; c < 256; ++c) {
                values[a][c] = static_cast<uint8_t>((c * a + 127) / 255);
            }
        }
    }
};

const PremultiplyTable& premultiplyTable() {
    static const PremultiplyTable table;
    return table;
}

} // namespace

void unpremultiply(uint8_t* rgba, size_t pixelCount) {
    const UnpremultiplyTable& table = unpremultiplyTable();
    for (size_t i = 0; i < pixelCount; ++i) {
        uint8_t* p = rgba + i * 4;
        const uint8_t a = p[3];
        if (a == 255) {
            continue;
        }
        const uint8_t* row = table.values[a];
        p[0] = row[p[0]];
        p[1] = row[p[1]];
        p[2] = row[p[2]];
    }
}

void premultiply(uint8_t* rgba, size_t pixelCount) {
    const PremultiplyTable& table = premultiplyTable();
    for (size_t i = 0; i < pixelCount; ++i) {
        uint8_t* p = rgba + i * 4;
        const uint8_t a = p[3];
        if (a == 255) {
            continue;
        }
        const uint8_t* row = table.values[a];
        p[0] = row[p[0]];
        p[1] = row[p[1]];
        p[2] = row[p[2]];
    }
}

} // namespace gfx
