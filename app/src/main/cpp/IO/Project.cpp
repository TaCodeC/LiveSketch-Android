#include "IO/Project.h"

#include "Gfx/Pixels.h"
#include "IO/Json.h"
#include "IO/Png.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <new>

namespace project {
namespace {

// Los modos de fusión, en el orden de BlendMode: su nombre en document.json y su operación en
// stack.xml. Los que SVG no tiene van con el nombre de Krita (OpenRaster pide que quien no
// conozca un modo use Normal). Las fórmulas son las de W3C (las de SVG y Krita); Mezcla dura
// es la de Photoshop.
struct BlendNames {
    BlendMode mode;
    const char* name;
    const char* op;
};
constexpr BlendNames kBlendNames[] = {
    {BlendMode::Normal, "normal", "svg:src-over"},
    {BlendMode::Multiply, "multiply", "svg:multiply"},
    {BlendMode::Darken, "darken", "svg:darken"},
    {BlendMode::ColorBurn, "color-burn", "svg:color-burn"},
    {BlendMode::LinearBurn, "linear-burn", "krita:linear_burn"},
    {BlendMode::DarkerColor, "darker-color", "krita:darker color"},
    {BlendMode::Lighten, "lighten", "svg:lighten"},
    {BlendMode::Screen, "screen", "svg:screen"},
    {BlendMode::ColorDodge, "color-dodge", "svg:color-dodge"},
    {BlendMode::Add, "add", "svg:plus"},
    {BlendMode::LighterColor, "lighter-color", "krita:lighter color"},
    {BlendMode::Overlay, "overlay", "svg:overlay"},
    {BlendMode::SoftLight, "soft-light", "svg:soft-light"},
    {BlendMode::HardLight, "hard-light", "svg:hard-light"},
    {BlendMode::VividLight, "vivid-light", "krita:vivid_light"},
    {BlendMode::LinearLight, "linear-light", "krita:linear light"},
    {BlendMode::PinLight, "pin-light", "krita:pin_light"},
    {BlendMode::HardMix, "hard-mix", "krita:hard_mix_photoshop"},
    {BlendMode::Difference, "difference", "svg:difference"},
    {BlendMode::Exclusion, "exclusion", "krita:exclusion"},
    {BlendMode::Subtract, "subtract", "krita:subtract"},
    {BlendMode::Divide, "divide", "krita:divide"},
    {BlendMode::Hue, "hue", "svg:hue"},
    {BlendMode::Saturation, "saturation", "svg:saturation"},
    {BlendMode::Color, "color", "svg:color"},
    {BlendMode::Luminosity, "luminosity", "svg:luminosity"},
};
static_assert(std::size(kBlendNames) == kBlendModeCount);

constexpr const char* kUnitNames[kLengthUnitCount] = {"px", "mm", "cm", "in"};
constexpr const char* kProfileNames[kColorProfileCount] = {"srgb", "display-p3"};
constexpr const char* kGuideNames[2] = {"grid", "symmetry"};
constexpr const char* kSymmetryNames[4] = {"vertical", "horizontal", "quadrant", "radial"};

// Lo más grande que se acepta en un número entero o en una cuenta (el double lo guarda exacto).
constexpr double kMaxExactInteger = 9007199254740992.0;   // 2^53

// Número entero entre `low` y `high`. False si falta o no lo es.
bool integer(const json::Value& value, double low, double high, int64_t* out) {
    if (!value.isNumber()) {
        return false;
    }
    const double number = value.number();
    if (number != std::floor(number) || number < low || number > high) {
        return false;
    }
    *out = static_cast<int64_t>(number);
    return true;
}

int intOr(const json::Value& value, int low, int high, int fallback) {
    int64_t number = 0;
    return integer(value, low, high, &number) ? static_cast<int>(number) : fallback;
}

float floatOr(const json::Value& value, float low, float high, float fallback) {
    const double number = value.number(std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number)) {
        return fallback;
    }
    return static_cast<float>(std::clamp(number, static_cast<double>(low), static_cast<double>(high)));
}

// Un nombre de la lista `names` (de `count`): su posición, o -1.
int nameIndex(const json::Value& value, const char* const* names, int count) {
    if (!value.isString()) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        if (value.string() == names[i]) {
            return i;
        }
    }
    return -1;
}

// Un punto [x, y] con números finitos.
bool point(const json::Value& value, glm::vec2* out) {
    const double x = value[size_t{0}].number(std::numeric_limits<double>::quiet_NaN());
    const double y = value[size_t{1}].number(std::numeric_limits<double>::quiet_NaN());
    if (!value.isArray() || value.items().size() != 2 || !std::isfinite(x) || !std::isfinite(y) ||
        std::fabs(x) > 1e9 || std::fabs(y) > 1e9) {
        return false;
    }
    *out = glm::vec2(static_cast<float>(x), static_cast<float>(y));
    return true;
}

// Un color [r, g, b] (0..1).
bool color(const json::Value& value, float out[3]) {
    if (!value.isArray() || value.items().size() != 3) {
        return false;
    }
    float rgb[3];
    for (size_t i = 0; i < 3; ++i) {
        const double c = value[i].number(std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(c)) {
            return false;
        }
        rgb[i] = static_cast<float>(std::clamp(c, 0.0, 1.0));
    }
    std::copy(rgb, rgb + 3, out);
    return true;
}

void writeColor(json::Writer& w, const float rgb[3]) {
    w.beginArray(true);
    for (int i = 0; i < 3; ++i) {
        w.value(rgb[i]);
    }
    w.endArray();
}

void writePoint(json::Writer& w, glm::vec2 p) {
    w.beginArray(true).value(p.x).value(p.y).endArray();
}

std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
    return out;
}

// Una capa de stack.xml.
void stackLayer(std::string& xml, const std::string& name, const std::string& file, int x, int y, float opacity,
                bool visible, const char* op, bool alphaLock, bool selected) {
    xml += "  <layer name=\"" + xmlEscape(name) + "\" src=\"" + xmlEscape(file) + "\" x=\"" + std::to_string(x) +
           "\" y=\"" + std::to_string(y) + "\" opacity=\"" + json::number(opacity) + "\" visibility=\"" +
           (visible ? "visible" : "hidden") + "\" composite-op=\"" + op + "\"";
    if (alphaLock) {
        xml += " alpha-preserve=\"true\"";
    }
    if (selected) {
        xml += " selected=\"true\"";
    }
    xml += " />\n";
}

bool noMemory(std::string* error) {
    if (error) {
        *error = "no hay memoria para guardarlo";
    }
    return false;
}

// Agrega a un vector (para recibir un PNG en memoria). False si no hay memoria.
png::Sink appendTo(std::vector<uint8_t>& out) {
    return [&out](const uint8_t* data, size_t size) {
        try {
            out.insert(out.end(), data, data + size);
        } catch (const std::bad_alloc&) {
            SDL_SetError("no hay memoria para guardarlo");
            return false;
        }
        return true;
    };
}

// Dígitos de `text` desde `at` (exactamente `count`). -1 si no lo son.
int digits(std::string_view text, size_t at, size_t count) {
    if (at + count > text.size()) {
        return -1;
    }
    int value = 0;
    for (size_t i = at; i < at + count; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return -1;
        }
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

} // namespace

// -----------------------------------------------------------------------------
// Nombres y fechas
// -----------------------------------------------------------------------------

const char* blendName(BlendMode blend) {
    const int index = static_cast<int>(blend);
    return index >= 0 && index < kBlendModeCount ? kBlendNames[index].name : "normal";
}

const char* compositeOp(BlendMode blend) {
    const int index = static_cast<int>(blend);
    return index >= 0 && index < kBlendModeCount ? kBlendNames[index].op : "svg:src-over";
}

std::string layerEntry(int index) { return "data/layer" + std::to_string(index) + ".png"; }

std::string damagedLayer(const LayerInfo& layer) {
    const std::string name = layer.name.empty() ? std::string("sin nombre") : "«" + layer.name + "»";
    return "La capa " + name + " está dañada: se abre vacía.";
}

size_t maxLayerFile(const LayerInfo& layer) {
    // Las filas con su byte de filtro, y un poco más: deflate sin comprimir y los chunks.
    const size_t raw = (static_cast<size_t>(layer.rect.width()) * 4 + 1) * static_cast<size_t>(layer.rect.height());
    return raw + raw / 64 + 65536;
}

std::string cleanName(std::string_view name) {
    std::string out;
    size_t i = 0;
    while (i < name.size()) {
        const auto lead = static_cast<unsigned char>(name[i]);
        size_t length = 1;
        uint32_t code = lead;
        if (lead >= 0x80) {
            if (lead >= 0xC2 && lead <= 0xDF) {
                length = 2;
                code = lead & 0x1F;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                length = 3;
                code = lead & 0x0F;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                length = 4;
                code = lead & 0x07;
            } else {
                ++i;   // byte suelto: no es UTF-8
                continue;
            }
            bool valid = i + length <= name.size();
            for (size_t k = 1; valid && k < length; ++k) {
                const auto next = static_cast<unsigned char>(name[i + k]);
                valid = (next & 0xC0) == 0x80;
                code = (code << 6) | (next & 0x3F);
            }
            // Sin formas largas, mitades de par (UTF-16) ni más allá de U+10FFFF.
            const uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
            if (!valid || code < minimum || (code >= 0xD800 && code < 0xE000) || code > 0x10FFFF) {
                ++i;
                continue;
            }
        }
        const bool control = code < 0x20 || code == 0x7F || (code >= 0x80 && code < 0xA0);
        if (!control) {
            if (out.size() + length > kMaxNameBytes) {
                break;
            }
            out.append(name.substr(i, length));
        }
        i += length;
    }
    return out;
}

std::string isoTime(int64_t time) {
    SDL_DateTime date;
    SDL_zero(date);
    if (!SDL_TimeToDateTime(time, &date, false)) {
        return {};
    }
    char text[40];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", date.year, date.month, date.day, date.hour,
                  date.minute, date.second);
    return text;
}

bool parseIsoTime(std::string_view text, int64_t* time) {
    // AAAA-MM-DDTHH:MM:SS, con fracción de segundo opcional y la zona: Z, +HH:MM o -HH:MM.
    SDL_DateTime date;
    SDL_zero(date);
    date.year = digits(text, 0, 4);
    date.month = digits(text, 5, 2);
    date.day = digits(text, 8, 2);
    date.hour = digits(text, 11, 2);
    date.minute = digits(text, 14, 2);
    date.second = digits(text, 17, 2);
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't') ||
        text[13] != ':' || text[16] != ':' || date.year < 0 || date.month < 0 || date.day < 0 || date.hour < 0 ||
        date.minute < 0 || date.second < 0) {
        return false;
    }
    size_t at = 19;
    if (text[at] == '.') {
        ++at;
        int scale = 100000000;
        const size_t first = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            date.nanosecond += (text[at] - '0') * scale;
            scale /= 10;
            ++at;
        }
        if (at == first) {
            return false;
        }
    }
    if (at < text.size() && (text[at] == 'Z' || text[at] == 'z') && at + 1 == text.size()) {
        date.utc_offset = 0;
    } else if (at + 6 == text.size() && (text[at] == '+' || text[at] == '-') && text[at + 3] == ':') {
        const int hours = digits(text, at + 1, 2);
        const int minutes = digits(text, at + 4, 2);
        if (hours < 0 || minutes < 0 || hours > 23 || minutes > 59) {
            return false;
        }
        date.utc_offset = (text[at] == '-' ? -1 : 1) * (hours * 3600 + minutes * 60);
    } else {
        return false;
    }
    // Se comprueba aquí y no solo en SDL, que puede compilarse sin comprobar parámetros.
    const int days = date.month >= 1 && date.month <= 12 ? SDL_GetDaysInMonth(date.year, date.month) : -1;
    SDL_Time result = 0;
    if (date.day < 1 || date.day > days || date.hour > 23 || date.minute > 59 || date.second > 59 ||
        !SDL_DateTimeToTime(&date, &result)) {
        return false;
    }
    *time = result;
    return true;
}

// -----------------------------------------------------------------------------
// Escribir
// -----------------------------------------------------------------------------

std::string documentJson(const Document& document, std::string_view app) {
    const CanvasInfo& info = document.info;
    json::Writer w;
    w.beginObject();
    w.key("format").value("LiveSketch");
    w.key("version").value(kFormatVersion);
    w.key("minVersion").value(1);
    w.key("app").value(app);

    w.key("canvas").beginObject();
    w.key("name").value(info.name);
    w.key("width").value(document.width);
    w.key("height").value(document.height);
    w.key("ppi").value(info.ppi);
    w.key("unit").value(kUnitNames[std::clamp(static_cast<int>(info.unit), 0, kLengthUnitCount - 1)]);
    w.key("profile").value(kProfileNames[std::clamp(static_cast<int>(info.profile), 0, kColorProfileCount - 1)]);
    w.key("background").beginObject();
    w.key("color");
    writeColor(w, document.background.color);
    w.key("visible").value(document.background.visible);
    w.endObject();
    if (const std::string created = isoTime(info.created); info.created != 0 && !created.empty()) {
        w.key("created").value(created);
    }
    if (const std::string modified = isoTime(info.modified); info.modified != 0 && !modified.empty()) {
        w.key("modified").value(modified);
    }
    w.key("drawingSeconds").value(info.drawingSeconds);
    w.key("strokes").value(info.strokes);
    w.endObject();

    w.key("layers").beginArray();
    for (const LayerInfo& layer : document.layers) {
        const bool empty = layer.rect.empty();
        w.beginObject();
        w.key("name").value(layer.name);
        w.key("file").value(layer.file);
        w.key("x").value(empty ? 0 : layer.rect.x0);
        w.key("y").value(empty ? 0 : layer.rect.y0);
        w.key("width").value(empty ? 0 : layer.rect.width());
        w.key("height").value(empty ? 0 : layer.rect.height());
        w.key("visible").value(layer.visible);
        w.key("opacity").value(layer.opacity);
        w.key("blend").value(blendName(layer.blend));
        w.key("alphaLock").value(layer.alphaLock);
        w.key("clipping").value(layer.clipping);
        w.key("reference").value(layer.reference);
        w.endObject();
    }
    w.endArray();
    w.key("activeLayer").value(document.activeLayer);

    const DrawingGuide& guide = document.guide;
    w.key("guide").beginObject();
    w.key("enabled").value(guide.enabled);
    w.key("kind").value(kGuideNames[guide.kind == GuideKind::Symmetry ? 1 : 0]);
    w.key("center");
    writePoint(w, guide.center);
    w.key("angle").value(guide.angle);
    w.key("opacity").value(guide.opacity);
    w.key("gridSize").value(guide.gridSize);
    w.key("symmetry").value(kSymmetryNames[std::clamp(static_cast<int>(guide.symmetry), 0, 3)]);
    w.key("rotational").value(guide.rotational);
    w.endObject();

    const ViewInfo& view = document.view;
    w.key("view").beginObject();
    w.key("fitted").value(view.fitted);
    if (!view.fitted) {
        w.key("zoom").value(view.zoom);
        w.key("center");
        writePoint(w, view.center);
        w.key("angle").value(view.angle);
    }
    w.key("flipped").value(view.flipped);
    w.endObject();

    if (document.hasColor) {
        w.key("color");
        writeColor(w, document.color);
    }
    w.endObject();
    return w.text();
}

std::string stackXml(const Document& document) {
    // xres e yres son ppp enteros (Krita los toma como la resolución del documento).
    const long ppi = std::lround(std::clamp(static_cast<double>(document.info.ppi), 1.0, 1.0e6));
    std::string xml = "<?xml version='1.0' encoding='UTF-8'?>\n";
    xml += "<image version=\"0.0.5\" w=\"" + std::to_string(document.width) + "\" h=\"" +
           std::to_string(document.height) + "\" xres=\"" + std::to_string(ppi) + "\" yres=\"" +
           std::to_string(ppi) + "\">\n";
    xml += " <stack name=\"root\" composite-op=\"svg:src-over\" opacity=\"1\" visibility=\"visible\" "
           "isolation=\"isolate\">\n";
    // OpenRaster va de arriba abajo. Una capa vacía lleva un PNG de 1 × 1 transparente.
    for (size_t i = document.layers.size(); i-- > 0;) {
        const LayerInfo& layer = document.layers[i];
        const bool empty = layer.rect.empty();
        stackLayer(xml, layer.name, layer.file, empty ? 0 : layer.rect.x0, empty ? 0 : layer.rect.y0, layer.opacity,
                   layer.visible, compositeOp(layer.blend), layer.alphaLock,
                   static_cast<int>(i) == document.activeLayer);
    }
    stackLayer(xml, "Fondo", kBackgroundEntry, 0, 0, 1.0f, document.background.visible, "svg:src-over", false, false);
    xml += " </stack>\n</image>\n";
    return xml;
}

std::vector<uint8_t> encodeImage(std::vector<uint8_t>& premultiplied, int width, int height, const png::Info& info,
                                 std::string* error) {
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (width <= 0 || height <= 0 || premultiplied.size() != pixels * 4) {
        if (error) {
            *error = "tamaño de imagen no válido";
        }
        return {};
    }
    gfx::unpremultiply(premultiplied.data(), pixels);
    if (!png::filterRowsInPlace(premultiplied, width, height)) {
        noMemory(error);
        return {};
    }
    std::vector<uint8_t> file;
    if (!png::write(premultiplied, width, height, info, appendTo(file))) {
        if (error) {
            *error = SDL_GetError();
        }
        return {};
    }
    return file;
}

std::vector<uint8_t> encodeBackground(int width, int height, const float rgb[3], ColorProfile profile,
                                      std::string* error) {
    uint8_t color[3];
    for (int i = 0; i < 3; ++i) {
        color[i] = static_cast<uint8_t>(std::lround(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f));
    }
    png::Info info;
    info.profile = profile;
    std::vector<uint8_t> file;
    if (!png::writeSolid(width, height, color, info, appendTo(file))) {
        if (error) {
            *error = SDL_GetError();
        }
        return {};
    }
    return file;
}

std::vector<uint8_t> thumbnail(const uint8_t* premultiplied, int width, int height, int maxSide, int* outWidth,
                               int* outHeight) {
    *outWidth = 0;
    *outHeight = 0;
    if (!premultiplied || width <= 0 || height <= 0 || maxSide <= 0) {
        return {};
    }
    const double scale = std::min(1.0, static_cast<double>(maxSide) / std::max(width, height));
    const int tw = std::clamp(static_cast<int>(std::lround(width * scale)), 1, width);
    const int th = std::clamp(static_cast<int>(std::lround(height * scale)), 1, height);
    std::vector<uint8_t> out;
    std::vector<uint64_t> sums;
    std::vector<int> columns;   // dónde empieza cada columna de la miniatura en el original
    try {
        out.resize(static_cast<size_t>(tw) * static_cast<size_t>(th) * 4);
        sums.resize(static_cast<size_t>(tw) * 4);
        columns.resize(static_cast<size_t>(tw) + 1);
    } catch (const std::bad_alloc&) {
        return {};
    }
    for (int x = 0; x <= tw; ++x) {
        columns[static_cast<size_t>(x)] = static_cast<int>(static_cast<int64_t>(x) * width / tw);
    }
    // Cada píxel es la media (premultiplicada) de su caja en el original.
    for (int ty = 0; ty < th; ++ty) {
        const int y0 = static_cast<int>(static_cast<int64_t>(ty) * height / th);
        const int y1 = static_cast<int>(static_cast<int64_t>(ty + 1) * height / th);
        std::fill(sums.begin(), sums.end(), 0);
        for (int y = y0; y < y1; ++y) {
            const uint8_t* row = premultiplied + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
            for (int tx = 0; tx < tw; ++tx) {
                uint64_t* sum = &sums[static_cast<size_t>(tx) * 4];
                for (int x = columns[static_cast<size_t>(tx)]; x < columns[static_cast<size_t>(tx) + 1]; ++x) {
                    const uint8_t* p = row + static_cast<size_t>(x) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                }
            }
        }
        for (int tx = 0; tx < tw; ++tx) {
            const uint64_t count = static_cast<uint64_t>(columns[static_cast<size_t>(tx) + 1] -
                                                         columns[static_cast<size_t>(tx)]) *
                                   static_cast<uint64_t>(y1 - y0);
            uint8_t* to = &out[(static_cast<size_t>(ty) * static_cast<size_t>(tw) + static_cast<size_t>(tx)) * 4];
            for (int c = 0; c < 4; ++c) {
                to[c] = static_cast<uint8_t>((sums[static_cast<size_t>(tx) * 4 + static_cast<size_t>(c)] + count / 2) /
                                             std::max<uint64_t>(count, 1));
            }
        }
    }
    gfx::unpremultiply(out.data(), out.size() / 4);
    *outWidth = tw;
    *outHeight = th;
    return out;
}

// -----------------------------------------------------------------------------
// Leer
// -----------------------------------------------------------------------------

bool readDocument(std::string_view text, int maxSide, Document& document, std::vector<std::string>& warnings,
                  std::string& error) {
    document = {};
    json::Value root;
    std::string problem;
    if (!json::parse(text, root, &problem)) {
        error = "su descripción (document.json) está dañada: " + problem;
        return false;
    }
    if (!root.isObject() || root["format"].string() != "LiveSketch") {
        error = "no es un proyecto de LiveSketch";
        return false;
    }
    int64_t version = 0;
    if (!integer(root["version"], 1, kMaxExactInteger, &version)) {
        error = "su descripción (document.json) está dañada: falta la versión del formato";
        return false;
    }
    int64_t minVersion = version;
    if (!root["minVersion"].isNull() && !integer(root["minVersion"], 1, kMaxExactInteger, &minVersion)) {
        error = "su descripción (document.json) está dañada: la versión mínima no es válida";
        return false;
    }
    // Un archivo nuevo que esta versión puede leer lo dice con minVersion; si no puede,
    // mejor no abrirlo que perder algo sin avisar.
    if (minVersion > kFormatVersion) {
        error = "se guardó con una versión más nueva de LiveSketch: actualiza la app para abrirlo";
        return false;
    }
    if (version > kFormatVersion) {
        warnings.push_back("Se guardó con una versión más nueva de LiveSketch: lo que esta no conoce no se abre.");
    }

    // El lienzo.
    const json::Value& canvas = root["canvas"];
    int64_t width = 0;
    int64_t height = 0;
    if (!canvas.isObject() || !integer(canvas["width"], 1, 1 << 30, &width) ||
        !integer(canvas["height"], 1, 1 << 30, &height)) {
        error = "su descripción (document.json) está dañada: el tamaño del lienzo no es válido";
        return false;
    }
    const std::string size = std::to_string(width) + " × " + std::to_string(height) + " px";
    switch (canvasspec::sizeProblem(static_cast<int>(width), static_cast<int>(height), maxSide)) {
    case canvasspec::SizeProblem::TooSmall:
        error = "el lienzo mide " + size + ", menos del mínimo de " + std::to_string(canvasspec::kMinSide) + " px";
        return false;
    case canvasspec::SizeProblem::TooWide:
        error = "el lienzo mide " + size + " y este dispositivo admite como mucho " + std::to_string(maxSide) +
                " px de lado";
        return false;
    case canvasspec::SizeProblem::TooManyPixels:
        error = "el lienzo mide " + size + ", más de lo que cabe en memoria (" +
                canvasspec::formatCount(canvasspec::kMaxPixels / 1000000) + " millones de píxeles como mucho)";
        return false;
    case canvasspec::SizeProblem::None:
        break;
    }
    document.width = static_cast<int>(width);
    document.height = static_cast<int>(height);
    const glm::vec2 canvasSize(static_cast<float>(width), static_cast<float>(height));

    CanvasInfo& info = document.info;
    info.name = cleanName(canvas["name"].string());
    info.ppi = floatOr(canvas["ppi"], canvasspec::kMinPpi, canvasspec::kMaxPpi, 72.0f);
    const int unit = nameIndex(canvas["unit"], kUnitNames, kLengthUnitCount);
    info.unit = unit >= 0 ? static_cast<LengthUnit>(unit) : LengthUnit::Pixels;
    const int profile = nameIndex(canvas["profile"], kProfileNames, kColorProfileCount);
    info.profile = profile >= 0 ? static_cast<ColorProfile>(profile) : ColorProfile::Srgb;
    if (profile < 0 && !canvas["profile"].isNull()) {
        warnings.push_back("Su perfil de color no existe en esta versión: se abre como sRGB.");
    }
    const json::Value& background = canvas["background"];
    if (!color(background["color"], document.background.color)) {
        std::fill(document.background.color, document.background.color + 3, 1.0f);
    }
    document.background.visible = background["visible"].boolean(true);
    int64_t created = 0;
    int64_t modified = 0;
    const bool hasCreated = parseIsoTime(canvas["created"].string(), &created);
    const bool hasModified = parseIsoTime(canvas["modified"].string(), &modified);
    info.created = hasCreated ? created : hasModified ? modified : 0;
    info.modified = hasModified ? modified : info.created;
    const double seconds = canvas["drawingSeconds"].number(0.0);
    info.drawingSeconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 1.0e12) : 0.0;
    int64_t strokes = 0;
    info.strokes = integer(canvas["strokes"], 0, kMaxExactInteger, &strokes) ? static_cast<uint64_t>(strokes) : 0;

    // Las capas.
    const json::Value& layers = root["layers"];
    if (!layers.isArray()) {
        error = "su descripción (document.json) está dañada: faltan las capas";
        return false;
    }
    const int limit = canvasspec::layerLimit(document.width, document.height);
    if (layers.items().size() > static_cast<size_t>(limit)) {
        error = "tiene " + std::to_string(layers.items().size()) + " capas y en un lienzo de " + size +
                " caben como mucho " + std::to_string(limit);
        return false;
    }
    bool hasReference = false;
    for (const json::Value& item : layers.items()) {
        LayerInfo layer;
        layer.name = cleanName(item["name"].string());
        layer.file = item["file"].string();
        layer.visible = item["visible"].boolean(true);
        layer.opacity = floatOr(item["opacity"], 0.0f, 1.0f, 1.0f);
        const json::Value& blend = item["blend"];
        bool knownBlend = blend.isNull();
        for (const BlendNames& names : kBlendNames) {
            if (blend.string() == names.name) {
                layer.blend = names.mode;
                knownBlend = true;
                break;
            }
        }
        if (!knownBlend) {
            const std::string shown = layer.name.empty() ? std::string("sin nombre") : "«" + layer.name + "»";
            warnings.push_back("La capa " + shown + " usa un modo de fusión que esta versión no tiene: queda en Normal.");
        }
        layer.alphaLock = item["alphaLock"].boolean(false);
        // La de abajo no puede recortar con nada.
        layer.clipping = item["clipping"].boolean(false) && !document.layers.empty();
        // Como mucho una capa de referencia: la primera.
        layer.reference = item["reference"].boolean(false) && !hasReference;
        hasReference = hasReference || layer.reference;

        // Dónde van sus píxeles: dentro del lienzo. Sin tamaño, la capa está vacía.
        int64_t x = 0;
        int64_t y = 0;
        int64_t w = 0;
        int64_t h = 0;
        const bool placed = integer(item["x"], 0, document.width, &x) && integer(item["y"], 0, document.height, &y) &&
                            integer(item["width"], 0, document.width, &w) &&
                            integer(item["height"], 0, document.height, &h) && x + w <= document.width &&
                            y + h <= document.height;
        if (!placed) {
            warnings.push_back(damagedLayer(layer));
        } else if (w > 0 && h > 0) {
            layer.rect = {static_cast<int>(x), static_cast<int>(y), static_cast<int>(x + w), static_cast<int>(y + h)};
        }
        document.layers.push_back(std::move(layer));
    }
    if (document.layers.empty()) {
        document.layers.emplace_back();   // un lienzo tiene al menos una capa
    }
    const int top = static_cast<int>(document.layers.size()) - 1;
    document.activeLayer = intOr(root["activeLayer"], 0, top, top);

    // La guía de dibujo.
    DrawingGuide& guide = document.guide;
    guide = guide::defaults(canvasSize);
    if (const json::Value& g = root["guide"]; g.isObject()) {
        guide.enabled = g["enabled"].boolean(false);
        guide.kind = nameIndex(g["kind"], kGuideNames, 2) == 1 ? GuideKind::Symmetry : GuideKind::Grid;
        point(g["center"], &guide.center);
        guide.angle = floatOr(g["angle"], -1000.0f, 1000.0f, 0.0f);
        guide.opacity = floatOr(g["opacity"], 0.0f, 1.0f, guide.opacity);
        guide.gridSize = floatOr(g["gridSize"], guide::kMinGridSize, guide::kMaxGridSize, guide.gridSize);
        const int symmetry = nameIndex(g["symmetry"], kSymmetryNames, 4);
        guide.symmetry = symmetry >= 0 ? static_cast<SymmetryKind>(symmetry) : SymmetryKind::Vertical;
        guide.rotational = g["rotational"].boolean(false);
    }

    // La vista.
    ViewInfo& view = document.view;
    if (const json::Value& v = root["view"]; v.isObject()) {
        view.flipped = v["flipped"].boolean(false);
        view.fitted = v["fitted"].boolean(true);
        if (!view.fitted) {
            view.zoom = floatOr(v["zoom"], 1e-3f, 1e3f, 1.0f);
            view.angle = floatOr(v["angle"], -1000.0f, 1000.0f, 0.0f);
            if (!point(v["center"], &view.center)) {
                view.fitted = true;
            }
        }
    }

    document.hasColor = color(root["color"], document.color);
    return true;
}

bool decodeLayer(std::span<const uint8_t> file, const LayerInfo& layer, std::vector<uint8_t>& out,
                 std::string* error) {
    out.clear();
    const size_t pixels = static_cast<size_t>(layer.rect.width()) * static_cast<size_t>(layer.rect.height());
    if (layer.rect.empty()) {
        return true;
    }
    png::Image image;
    if (!png::decode(file, image, pixels, error)) {
        return false;
    }
    if (image.width != layer.rect.width() || image.height != layer.rect.height()) {
        if (error) {
            *error = "la imagen no mide lo que dice el proyecto";
        }
        return false;
    }
    gfx::premultiply(image.rgba.data(), pixels);
    out = std::move(image.rgba);
    return true;
}

} // namespace project
