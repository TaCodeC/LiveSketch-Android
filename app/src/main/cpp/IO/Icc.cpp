#include "IO/Icc.h"

#include <cstring>

namespace icc {
namespace {

// Números s15Fixed16 (el valor por 65536, con signo), como van en el perfil.
struct Xyz {
    int32_t x;
    int32_t y;
    int32_t z;
};

// Blanco del espacio de conexión (D50): 0,9642, 1 y 0,8249.
constexpr Xyz kD50 = {0xF6D6, 0x10000, 0xD32D};

// Display P3. Los colores de los primarios (0,680 0,320; 0,265 0,690; 0,150 0,060) con el
// blanco D65 (0,3127 0,3290), pasados a D50 con la matriz de Bradford como pide ICC v4.
constexpr Xyz kP3Red = {33759, 15807, -69};       // 0,51512  0,24119  -0,00105
constexpr Xyz kP3Green = {19135, 45367, 2745};    // 0,29198  0,69224   0,04188
constexpr Xyz kP3Blue = {10296, 4363, 51385};     // 0,15710  0,06657   0,78407
// La adaptación de D65 a D50 (Bradford), por filas.
constexpr int32_t kD65ToD50[9] = {
    68674, 1502, -3291,    // 1,04789  0,02292  -0,05022
    1939,  64912, -1119,   // 0,02958  0,99048  -0,01708
    -606,  988,   49262,   // -0,00925 0,01507   0,75168
};
// La curva de sRGB como función paramétrica de tipo 3: Y = (aX + b)^g desde X = d, y
// Y = cX por debajo. g = 2,4; a = 1/1,055; b = 0,055/1,055; c = 1/12,92; d = 0,04045.
constexpr int32_t kSrgbCurve[5] = {157286, 62119, 3417, 5072, 2651};

class Writer {
public:
    std::vector<uint8_t> bytes;

    void u16(uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value));
    }
    void u32(uint32_t value) {
        u16(value >> 16);
        u16(value & 0xFFFF);
    }
    void s32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void signature(const char* text) { bytes.insert(bytes.end(), text, text + 4); }
    void zeros(size_t count) { bytes.insert(bytes.end(), count, 0); }
    void xyz(const Xyz& value) {
        s32(value.x);
        s32(value.y);
        s32(value.z);
    }
};

// Texto en inglés de EE. UU. (multiLocalizedUnicodeType, UTF-16BE). `ascii` sin acentos.
std::vector<uint8_t> textTag(const char* ascii) {
    Writer w;
    const size_t length = std::strlen(ascii);
    w.signature("mluc");
    w.zeros(4);
    w.u32(1);    // un idioma
    w.u32(12);   // lo que ocupa cada registro
    w.signature("enUS");
    w.u32(static_cast<uint32_t>(length * 2));
    w.u32(28);   // el texto va detrás del registro
    for (size_t i = 0; i < length; ++i) {
        w.u16(static_cast<uint8_t>(ascii[i]));
    }
    return w.bytes;
}

std::vector<uint8_t> xyzTag(const Xyz& value) {
    Writer w;
    w.signature("XYZ ");
    w.zeros(4);
    w.xyz(value);
    return w.bytes;
}

std::vector<uint8_t> matrixTag(const int32_t (&matrix)[9]) {
    Writer w;
    w.signature("sf32");
    w.zeros(4);
    for (const int32_t value : matrix) {
        w.s32(value);
    }
    return w.bytes;
}

std::vector<uint8_t> curveTag(const int32_t (&parameters)[5]) {
    Writer w;
    w.signature("para");
    w.zeros(4);
    w.u16(3);   // Y = (aX + b)^g y Y = cX
    w.zeros(2);
    for (const int32_t value : parameters) {
        w.s32(value);
    }
    return w.bytes;
}

std::vector<uint8_t> buildDisplayP3() {
    struct Tag {
        const char* signature;
        int data;   // índice en `data` (las tres curvas comparten la suya)
    };
    const std::vector<uint8_t> data[] = {
        textTag("Display P3"),
        textTag("No copyright, use freely"),
        xyzTag(kD50),   // en ICC v4 el blanco de una pantalla es el de D50
        matrixTag(kD65ToD50),
        xyzTag(kP3Red),
        xyzTag(kP3Green),
        xyzTag(kP3Blue),
        curveTag(kSrgbCurve),
    };
    const Tag tags[] = {
        {"desc", 0}, {"cprt", 1}, {"wtpt", 2}, {"chad", 3}, {"rXYZ", 4},
        {"gXYZ", 5}, {"bXYZ", 6}, {"rTRC", 7}, {"gTRC", 7}, {"bTRC", 7},
    };
    constexpr size_t kHeader = 128;
    constexpr size_t kTagCount = sizeof(tags) / sizeof(tags[0]);

    // Dónde va cada dato: detrás de la tabla de etiquetas, cada uno en múltiplo de 4.
    size_t offsets[sizeof(data) / sizeof(data[0])];
    size_t at = kHeader + 4 + kTagCount * 12;
    for (size_t i = 0; i < sizeof(data) / sizeof(data[0]); ++i) {
        offsets[i] = at;
        at += (data[i].size() + 3) & ~size_t{3};
    }
    const size_t size = at;

    Writer w;
    w.bytes.reserve(size);
    w.u32(static_cast<uint32_t>(size));
    w.zeros(4);              // módulo de color preferido: ninguno
    w.u32(0x04300000);       // versión 4.3
    w.signature("mntr");     // pantalla
    w.signature("RGB ");
    w.signature("XYZ ");
    // Fecha de creación: 8 de octubre de 2026.
    for (const uint32_t part : {2026u, 10u, 8u, 0u, 0u, 0u}) {
        w.u16(part);
    }
    w.signature("acsp");
    w.zeros(4 + 4 + 4 + 4 + 8);   // plataforma, opciones, fabricante, modelo y atributos
    w.u32(0);                     // intención perceptual
    w.xyz(kD50);
    w.zeros(4 + 16 + 28);         // creador, identificador (sin calcular) y reservado

    w.u32(static_cast<uint32_t>(kTagCount));
    for (const Tag& tag : tags) {
        w.signature(tag.signature);
        w.u32(static_cast<uint32_t>(offsets[tag.data]));
        w.u32(static_cast<uint32_t>(data[tag.data].size()));
    }
    for (const std::vector<uint8_t>& block : data) {
        w.bytes.insert(w.bytes.end(), block.begin(), block.end());
        w.zeros((4 - block.size() % 4) % 4);
    }
    return w.bytes;
}

} // namespace

const std::vector<uint8_t>& profile(ColorProfile profile) {
    static const std::vector<uint8_t> none;
    static const std::vector<uint8_t> displayP3 = buildDisplayP3();
    return profile == ColorProfile::DisplayP3 ? displayP3 : none;
}

} // namespace icc
