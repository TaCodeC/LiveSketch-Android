// Pruebas de los proyectos (.lvskt): el JSON, el ZIP y los PNG por separado, document.json, y
// guardar y abrir un lienzo de verdad comprobando que las capas vuelven idénticas.

#include "Test.h"

#include "App/CanvasProject.h"
#include "Canvas/Camera.h"
#include "Canvas/Canvas.h"
#include "Gfx/Pixels.h"
#include "IO/Json.h"
#include "IO/Png.h"
#include "IO/Project.h"
#include "IO/ProjectFile.h"
#include "IO/Tasks.h"
#include "IO/Zip.h"
#include "ThirdParty/stb_image.h"

#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_timer.h>
#include <libdeflate.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

// 2023-11-14T22:13:20Z en SDL_Time.
constexpr int64_t kSomeTime = 1700000000LL * 1000000000LL;

std::span<const uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

std::vector<uint8_t> bytesFrom(std::string_view text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string textOf(const std::vector<uint8_t>& data) { return std::string(data.begin(), data.end()); }

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// Los archivos de una carpeta, ordenados.
std::vector<std::string> filesIn(const std::string& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

uint16_t get16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get32le(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

void put16le(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void put32le(uint8_t* p, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void put32be(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

bool contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

bool anyContains(const std::vector<std::string>& texts, std::string_view part) {
    return std::any_of(texts.begin(), texts.end(), [part](const std::string& text) { return contains(text, part); });
}

// --- ZIP ---

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
    zip::Method method = zip::Method::Deflate;
};

std::vector<uint8_t> makeZip(const std::vector<ZipEntry>& entries) {
    std::vector<uint8_t> out;
    zip::Writer writer(
        [&out](const uint8_t* data, size_t size) {
            out.insert(out.end(), data, data + size);
            return true;
        },
        kSomeTime);
    for (const ZipEntry& entry : entries) {
        if (!writer.add(entry.name, entry.data, entry.method)) {
            return {};
        }
    }
    return writer.finish() ? out : std::vector<uint8_t>();
}

// Todas las entradas de un ZIP, en su orden (vacío si alguna no se puede leer).
std::vector<ZipEntry> unzip(const std::vector<uint8_t>& file) {
    zip::MemorySource source(file);
    zip::Reader reader;
    if (!reader.open(source)) {
        return {};
    }
    std::vector<ZipEntry> entries;
    for (const zip::Entry& entry : reader.entries()) {
        ZipEntry out;
        out.name = entry.name;
        out.method = entry.method;
        if (!reader.read(entry, out.data, size_t{1} << 30)) {
            return {};
        }
        entries.push_back(std::move(out));
    }
    return entries;
}

const ZipEntry* findEntry(const std::vector<ZipEntry>& entries, std::string_view name) {
    for (const ZipEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

// El mismo ZIP con las entradas que cambia `edit` (si devuelve false, la entrada se quita).
std::vector<uint8_t> rewriteZip(const std::vector<uint8_t>& file, const std::function<bool(ZipEntry&)>& edit) {
    std::vector<ZipEntry> kept;
    for (ZipEntry& entry : unzip(file)) {
        if (edit(entry)) {
            kept.push_back(std::move(entry));
        }
    }
    return makeZip(kept);
}

// --- PNG ---

uint8_t paethPredictor(int left, int up, int upLeft) {
    const int p = left + up - upLeft;
    const int pa = std::abs(p - left);
    const int pb = std::abs(p - up);
    const int pc = std::abs(p - upLeft);
    if (pa <= pb && pa <= pc) {
        return static_cast<uint8_t>(left);
    }
    return static_cast<uint8_t>(pb <= pc ? up : upLeft);
}

void pngChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32be(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32be(out, libdeflate_crc32(0, &out[start], data.size() + 4));
}

std::vector<uint8_t> zlib(const std::vector<uint8_t>& data) {
    libdeflate_compressor* compressor = libdeflate_alloc_compressor(6);
    std::vector<uint8_t> out(libdeflate_zlib_compress_bound(compressor, data.size()));
    out.resize(libdeflate_zlib_compress(compressor, data.data(), data.size(), out.data(), out.size()));
    libdeflate_free_compressor(compressor);
    return out;
}

// PNG RGBA de 8 bits hecho a mano: cada fila con el filtro `filters[y % n]` (como los de
// otros programas) y los datos repartidos en `parts` chunks IDAT. Las filas pueden llevar un
// filtro que no existe (para probar que se rechaza).
std::vector<uint8_t> craftPng(const std::vector<uint8_t>& rgba, int width, int height,
                              const std::vector<uint8_t>& filters, size_t parts = 1) {
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> raw;
    for (int y = 0; y < height; ++y) {
        const uint8_t type = filters[static_cast<size_t>(y) % filters.size()];
        raw.push_back(type);
        const uint8_t* row = &rgba[static_cast<size_t>(y) * rowBytes];
        const uint8_t* up = y > 0 ? row - rowBytes : nullptr;
        for (size_t i = 0; i < rowBytes; ++i) {
            const int left = i >= 4 ? row[i - 4] : 0;
            const int above = up ? up[i] : 0;
            const int upLeft = up && i >= 4 ? up[i - 4] : 0;
            int predicted = 0;
            switch (type) {
            case 1: predicted = left; break;
            case 2: predicted = above; break;
            case 3: predicted = (left + above) / 2; break;
            case 4: predicted = paethPredictor(left, above, upLeft); break;
            default: break;
            }
            raw.push_back(static_cast<uint8_t>(row[i] - predicted));
        }
    }
    const std::vector<uint8_t> compressed = zlib(raw);

    std::vector<uint8_t> file = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    std::vector<uint8_t> header;
    put32be(header, static_cast<uint32_t>(width));
    put32be(header, static_cast<uint32_t>(height));
    header.insert(header.end(), {8, 6, 0, 0, 0});
    pngChunk(file, "IHDR", header);
    const size_t part = (compressed.size() + parts - 1) / parts;
    for (size_t at = 0; at < compressed.size(); at += part) {
        const size_t end = std::min(compressed.size(), at + part);
        pngChunk(file, "IDAT", std::vector<uint8_t>(compressed.begin() + static_cast<std::ptrdiff_t>(at),
                                                    compressed.begin() + static_cast<std::ptrdiff_t>(end)));
    }
    pngChunk(file, "IEND", {});
    return file;
}

std::vector<uint8_t> encodePng(const std::vector<uint8_t>& rgba, int width, int height, const png::Info& info = {}) {
    std::vector<uint8_t> file;
    png::encode(rgba.data(), width, height, static_cast<size_t>(width) * 4, info,
                [&file](const uint8_t* data, size_t size) {
                    file.insert(file.end(), data, data + size);
                    return true;
                });
    return file;
}

std::vector<uint8_t> randomRgba(size_t pixels, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> out(pixels * 4);
    for (uint8_t& value : out) {
        value = static_cast<uint8_t>(rng());
    }
    return out;
}

// Píxeles premultiplicados al azar (cada canal, como mucho el alfa), con algunos transparentes.
std::vector<uint8_t> randomPremultiplied(size_t pixels, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> out(pixels * 4);
    for (size_t i = 0; i < out.size(); i += 4) {
        const uint32_t alpha = rng() % 5 == 0 ? 0 : rng() % 256;
        for (size_t c = 0; c < 3; ++c) {
            out[i + c] = static_cast<uint8_t>(rng() % (alpha + 1));
        }
        out[i + 3] = static_cast<uint8_t>(alpha);
    }
    return out;
}

// --- Proyectos ---

// Un documento con todo distinto de lo normal (sin capas).
project::Document sampleDocument() {
    project::Document document;
    document.width = 300;
    document.height = 200;
    document.info.name = "Boceto \"uno\" ñ 😀";
    document.info.ppi = 300.0f;
    document.info.unit = LengthUnit::Centimeters;
    document.info.profile = ColorProfile::DisplayP3;
    document.info.created = kSomeTime;
    document.info.modified = kSomeTime + 3600LL * 1000000000LL;
    document.info.drawingSeconds = 1234.56789;
    document.info.strokes = 42;
    document.background.color[0] = 0.1f;
    document.background.color[1] = 0.2f;
    document.background.color[2] = 0.3f;
    document.background.visible = false;
    document.activeLayer = 0;
    document.guide.enabled = true;
    document.guide.kind = GuideKind::Symmetry;
    document.guide.center = {150.5f, 99.25f};
    document.guide.angle = 0.3f;
    document.guide.opacity = 0.45f;
    document.guide.gridSize = 64.0f;
    document.guide.symmetry = SymmetryKind::Radial;
    document.guide.rotational = true;
    document.view.fitted = false;
    document.view.zoom = 2.5f;
    document.view.center = {10.0f, 20.0f};
    document.view.angle = -0.7f;
    document.view.flipped = true;
    document.hasColor = true;
    document.color[0] = 0.25f;
    document.color[1] = 0.5f;
    document.color[2] = 1.0f;
    return document;
}

project::LayerInfo layerInfo(std::string name, const IRect& rect, int index) {
    project::LayerInfo layer;
    layer.name = std::move(name);
    layer.rect = rect;
    layer.file = project::layerEntry(index);
    return layer;
}

// Guarda un documento hecho a mano (sin la GPU): `pixels[i]`, los de la capa i
// (premultiplicados, del tamaño de su caja).
project::Saver::Result saveDocument(const project::Document& document, const std::vector<std::vector<uint8_t>>& pixels,
                                    const project::Target& target) {
    project::Saver saver;
    if (!saver.begin(document, target, "LiveSketch pruebas")) {
        return saver.takeResult().value_or(project::Saver::Result{});
    }
    for (size_t i = 0; i < document.layers.size(); ++i) {
        saver.addLayer(static_cast<int>(i), i < pixels.size() ? pixels[i] : std::vector<uint8_t>());
    }
    std::vector<uint8_t> composite(static_cast<size_t>(document.width) * static_cast<size_t>(document.height) * 4, 255);
    saver.finish(std::move(composite), {});
    saver.wait();
    return saver.takeResult().value_or(project::Saver::Result{});
}

// Un proyecto pequeño de tres capas (la de en medio, vacía), guardado en `folder`.
struct SmallProject {
    project::Document document;
    std::vector<std::vector<uint8_t>> pixels;
    std::string path;
};

SmallProject smallProject(const std::string& folder) {
    SmallProject project;
    project.document = sampleDocument();
    project.document.width = 64;
    project.document.height = 48;
    project.document.layers = {layerInfo("Boceto", {4, 6, 40, 30}, 0), layerInfo("Vacía", {}, 1),
                               layerInfo("Tinta", {0, 0, 64, 48}, 2)};
    project.pixels = {randomPremultiplied(36 * 24, 1), {}, randomPremultiplied(64 * 48, 2)};
    project::Target target;
    target.path = folder + "pequeño.lvskt";
    const project::Saver::Result result = saveDocument(project.document, project.pixels, target);
    project.path = result.ok ? result.path : std::string();
    return project;
}

// Lo que va dando un Loader ya abierto: los píxeles de cada capa.
struct Loaded {
    bool ok = false;
    std::vector<std::vector<uint8_t>> pixels;
    std::vector<std::string> warnings;
};

Loaded loadLayers(project::Loader& loader) {
    Loaded loaded;
    loaded.pixels.resize(loader.document().layers.size());
    loader.start();
    const uint64_t start = SDL_GetTicks();
    int index = 0;
    std::vector<uint8_t> pixels;
    while (!loader.finished()) {
        if (loader.next(&index, pixels)) {
            loaded.pixels[static_cast<size_t>(index)] = std::move(pixels);
            pixels = {};
            continue;
        }
        loader.pump(20);
        if (SDL_GetTicks() - start > 60000) {
            return loaded;
        }
#ifndef SDL_PLATFORM_EMSCRIPTEN
        SDL_Delay(1);
#endif
    }
    loaded.warnings = loader.warnings();
    loaded.ok = true;
    return loaded;
}

// --- Lienzos ---

void setBrush(Canvas& canvas, float r, float g, float b, float opacity, float radius) {
    BrushSettings& brush = canvas.brushSettings();
    brush.color[0] = r;
    brush.color[1] = g;
    brush.color[2] = b;
    brush.opacity = opacity;
    brush.radius = radius;
    brush.eraser = false;
}

void drawLine(Canvas& canvas, glm::vec2 from, glm::vec2 to) {
    canvas.beginStroke(from.x, from.y, 1.0f);
    canvas.strokeTo((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f + 7.0f, 0.6f);
    canvas.strokeTo(to.x, to.y, 0.9f);
    canvas.endStroke();
    canvas.update();
}

// Guarda el lienzo como la app (canvasproject::save): las capas que cambiaron desde
// `previous` (todas, si no hay), recortadas a lo pintado y leídas de la GPU, las demás
// copiadas del archivo anterior, y el dibujo entero. `meanwhile` se llama al entregar el
// último dato, mientras se comprime en segundo plano. Deja en `pending` cómo se guardó y en
// `record` lo que tiene el archivo nuevo.
project::Saver::Result saveCanvas(Canvas& canvas, const project::Target& target, const project::ViewInfo& view,
                                  const std::function<void()>& meanwhile = {},
                                  const canvasproject::Record& previous = {},
                                  canvasproject::Pending* pending = nullptr, canvasproject::Record* record = nullptr) {
    canvas.settle();
    project::Document document = canvasproject::document(canvas);
    document.view = view;
    project::Saver saver;
    std::atomic<int> finished{0};
    canvasproject::Pending started;
    const bool ok = canvasproject::save(canvas, std::move(document), target, "LiveSketch pruebas", previous, saver,
                                        [&finished] { ++finished; }, &started);
    if (ok && meanwhile) {
        meanwhile();
    }
    saver.wait();
    const project::Saver::Result result = saver.takeResult().value_or(project::Saver::Result{});
    if (ok) {
        CHECK_EQ(finished.load(), 1);
    }
    if (pending) {
        *pending = started;
    }
    if (record) {
        *record = canvasproject::saved(started, result);
    }
    return result;
}

// Abre en `canvas` lo que leyó `loader` (ya abierto), como la app (App::startProjectOpen y
// App::stepProjectOpen), y deja en `record` lo que tiene el archivo. Devuelve los avisos, o
// nada si no se pudo.
std::optional<std::vector<std::string>> openCanvas(project::Loader& loader, Canvas& canvas,
                                                   canvasproject::Record* record = nullptr,
                                                   const std::string& path = {}) {
    const project::Document& document = loader.document();
    CanvasSpec spec;
    spec.width = document.width;
    spec.height = document.height;
    spec.ppi = document.info.ppi;
    spec.unit = document.info.unit;
    spec.profile = document.info.profile;
    spec.background = document.background;
    spec.name = document.info.name;
    std::vector<Canvas::OpenLayer> layers(document.layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        const project::LayerInfo& layer = document.layers[i];
        layers[i].properties.name = layer.name;
        layers[i].properties.visible = layer.visible;
        layers[i].properties.opacity = layer.opacity;
        layers[i].properties.blend = layer.blend;
        layers[i].properties.alphaLock = layer.alphaLock;
        layers[i].properties.clipping = layer.clipping;
        layers[i].reference = layer.reference;
    }
    if (!canvas.open(spec, document.info, layers, document.activeLayer, document.guide)) {
        return std::nullopt;
    }
    const uint64_t version = canvas.documentVersion();
    std::vector<canvasproject::SavedLayer> saved;
    loader.start();
    const uint64_t start = SDL_GetTicks();
    int index = 0;
    std::vector<uint8_t> pixels;
    while (!loader.finished()) {
        if (loader.next(&index, pixels)) {
            const project::LayerInfo& layer = document.layers[static_cast<size_t>(index)];
            if (!pixels.empty() && !canvas.setLayerPixels(index, layer.rect, pixels.data())) {
                return std::nullopt;
            }
            const bool loaded = layer.rect.empty() || !pixels.empty();
            if (std::optional<canvasproject::SavedLayer> layerRecord =
                    canvasproject::openedLayer(canvas, loader, index, loaded)) {
                saved.push_back(*layerRecord);
            }
            pixels = {};
            continue;
        }
        loader.pump(20);
        if (SDL_GetTicks() - start > 60000) {
            return std::nullopt;
        }
#ifndef SDL_PLATFORM_EMSCRIPTEN
        SDL_Delay(1);
#endif
    }
    canvas.update();
    if (record) {
        const bool complete = saved.size() == document.layers.size() && canvas.documentVersion() == version;
        *record = canvasproject::opened(canvas, loader, path, std::move(saved), complete);
    }
    return loader.warnings();
}

} // namespace

// -----------------------------------------------------------------------------
// Píxeles
// -----------------------------------------------------------------------------

TEST_CASE(pixels_premultiply_round_trip_is_exact) {
    // Todo color premultiplicado posible (cada canal, como mucho el alfa): quitar el
    // premultiplicado (como al guardar el PNG) y volver a ponerlo (al abrir) lo deja igual.
    std::vector<uint8_t> pixels;
    for (int alpha = 0; alpha < 256; ++alpha) {
        for (int c = 0; c <= alpha; ++c) {
            pixels.insert(pixels.end(), {static_cast<uint8_t>(c), static_cast<uint8_t>((c * 7) % (alpha + 1)),
                                         static_cast<uint8_t>(alpha - c), static_cast<uint8_t>(alpha)});
        }
    }
    std::vector<uint8_t> roundTrip = pixels;
    gfx::unpremultiply(roundTrip.data(), roundTrip.size() / 4);
    gfx::premultiply(roundTrip.data(), roundTrip.size() / 4);
    CHECK_EQ(test::maxDifference(roundTrip, pixels), 0);

    // Los transparentes quedan en negro.
    uint8_t clear[4] = {0, 0, 0, 0};
    gfx::premultiply(clear, 1);
    CHECK_EQ(int{clear[0]} + clear[1] + clear[2] + clear[3], 0);
    uint8_t half[4] = {255, 128, 0, 128};
    gfx::premultiply(half, 1);
    CHECK_EQ(int{half[0]}, 128);
    CHECK_EQ(int{half[1]}, 64);
    CHECK_EQ(int{half[2]}, 0);
    CHECK_EQ(int{half[3]}, 128);
}

// -----------------------------------------------------------------------------
// JSON
// -----------------------------------------------------------------------------

TEST_CASE(json_writes_and_reads_back) {
    const std::string tricky = "comillas \" barra \\ / salto\nfin\ttab \x01 \x7f ñ € 😀";
    json::Writer w;
    w.beginObject();
    w.key("texto").value(tricky);
    w.key("vacío").value("");
    w.key("sí").value(true);
    w.key("no").value(false);
    w.key("entero").value(-42);
    w.key("grande").value(int64_t{9007199254740992});
    w.key("sin signo").value(uint64_t{1234567890123});
    w.key("ppp").value(300.0f);
    w.key("décimo").value(0.1f);
    w.key("tercio").value(1.0 / 3.0);
    w.key("nan").value(std::nanf(""));
    w.key("infinito").value(std::numeric_limits<double>::infinity());
    w.key("nulo").null();
    w.key("color").beginArray(true).value(0.25f).value(0.5f).value(1.0f).endArray();
    w.key("lista").beginArray().beginObject().key("a").value(1).endObject().beginArray().endArray().endArray();
    w.key("objeto vacío").beginObject().endObject();
    w.endObject();
    const std::string& text = w.text();

    // Con sangría, una lista de números en una línea, los enteros sin exponente.
    CHECK(contains(text, "\n  \"sí\": true,\n"));
    CHECK(contains(text, "\"color\": [0.25, 0.5, 1]"));
    CHECK(contains(text, "\"ppp\": 300,"));
    CHECK(contains(text, "\"décimo\": 0.1,"));
    CHECK(contains(text, "\"nan\": null,"));
    CHECK(contains(text, "\"infinito\": null,"));
    CHECK(contains(text, "\\u0001"));
    CHECK(contains(text, "\\u007f"));
    CHECK(text.back() == '\n');

    json::Value root;
    std::string error;
    REQUIRE(json::parse(text, root, &error));
    CHECK(error.empty());
    REQUIRE(root.isObject());
    CHECK_EQ(root["texto"].string(), tricky);
    CHECK(root["vacío"].isString());
    CHECK(root["vacío"].string().empty());
    CHECK(root["sí"].boolean(false));
    CHECK(!root["no"].boolean(true));
    CHECK_EQ(root["entero"].number(), -42.0);
    CHECK_EQ(root["grande"].number(), 9007199254740992.0);
    CHECK_EQ(root["sin signo"].number(), 1234567890123.0);
    CHECK_EQ(static_cast<float>(root["ppp"].number()), 300.0f);
    CHECK_EQ(static_cast<float>(root["décimo"].number()), 0.1f);
    CHECK_EQ(root["tercio"].number(), 1.0 / 3.0);
    CHECK(root["nan"].isNull());
    CHECK(root["nulo"].isNull());
    CHECK(root["no existe"].isNull());
    CHECK_EQ(root["color"].items().size(), size_t{3});
    CHECK_EQ(static_cast<float>(root["color"][2].number()), 1.0f);
    CHECK(root["color"][3].isNull());
    CHECK_EQ(root["lista"][size_t{0}]["a"].number(), 1.0);
    CHECK(root["lista"][1].isArray());
    CHECK(root["objeto vacío"].isObject());
    CHECK(root["objeto vacío"].members().empty());
    // Los miembros, en el orden del texto.
    CHECK_EQ(root.members().front().first, std::string("texto"));
    CHECK_EQ(root.members().back().first, std::string("objeto vacío"));
    // Si es de otro tipo, lo que se pida por defecto.
    CHECK_EQ(root["texto"].number(7.0), 7.0);
    CHECK(root["entero"].boolean(true));
    CHECK(root["entero"].string().empty());
}

TEST_CASE(json_numbers_round_trip_exactly) {
    // Cualquier float o double finito vuelve idéntico (también los subnormales y -0).
    std::mt19937 rng(1234);
    int failures = 0;
    for (int i = 0; i < 20000; ++i) {
        const uint32_t bits = static_cast<uint32_t>(rng());
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) {
            continue;
        }
        json::Value parsed;
        const std::string text = json::number(value);
        const float back = json::parse(text, parsed) ? static_cast<float>(parsed.number()) : std::nanf("");
        if (std::memcmp(&value, &back, sizeof(float)) != 0) {
            if (++failures <= 3) {
                test::fail(__FILE__, __LINE__, "float " + text + " no vuelve igual");
            }
        }
    }
    for (int i = 0; i < 20000; ++i) {
        const uint64_t bits = (static_cast<uint64_t>(rng()) << 32) | rng();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) {
            continue;
        }
        json::Value parsed;
        const std::string text = json::number(value);
        const double back = json::parse(text, parsed) ? parsed.number() : std::nan("");
        if (std::memcmp(&value, &back, sizeof(double)) != 0) {
            if (++failures <= 6) {
                test::fail(__FILE__, __LINE__, "double " + text + " no vuelve igual");
            }
        }
    }
    CHECK_EQ(failures, 0);
    CHECK_EQ(json::number(0.0f), std::string("0"));
    CHECK_EQ(json::number(-0.0f), std::string("-0"));
    CHECK_EQ(json::number(72.0f), std::string("72"));
    CHECK_EQ(json::number(0.6f), std::string("0.6"));
    CHECK_EQ(json::number(FLT_MAX), std::string("3.4028235e+38"));
    CHECK_EQ(json::number(1e-7f), std::string("1e-07"));
    CHECK_EQ(json::number(std::nanf("")), std::string("null"));
}

TEST_CASE(json_rejects_malformed_text) {
    const char* const bad[] = {
        "",           " ",          "{",          "}",          "[1,]",       "[1 2]",      "{\"a\" 1}",
        "{\"a\":1,}", "{a:1}",      "01",         "-",          "1.",         ".5",         "1e",
        "+1",         "NaN",        "Infinity",   "tru",        "nul",        "\"abc",      "\"a\x01z\"",
        "\"\\x\"",    "\"\\u12\"",  "\"\\u12G4\"", "[1] 2",     "1e400",      "'a'",        "[\"a\" \"b\"]",
        "{\"a\":}",   "[,]",        "\"\\",       "-01",        "1.e5",       "{\"a\":1 \"b\":2}",
    };
    for (const char* text : bad) {
        json::Value value;
        std::string error;
        if (json::parse(text, value, &error)) {
            test::fail(__FILE__, __LINE__, std::string("se aceptó: ") + text);
        } else {
            CHECK(contains(error, "línea "));
            CHECK(value.isNull());
        }
    }
    // Un número demasiado largo.
    json::Value value;
    CHECK(!json::parse(std::string(65, '1'), value));
    CHECK(json::parse(std::string(64, '1'), value));

    // Dónde falla.
    std::string error;
    CHECK(!json::parse("{\n  \"a\": tru\n}", value, &error));
    CHECK_EQ(error, std::string("línea 2, columna 8: valor no válido"));

    // Anidado: 65 niveles se leen, 66 no.
    CHECK(json::parse(std::string(65, '[') + std::string(65, ']'), value));
    CHECK(!json::parse(std::string(66, '[') + std::string(66, ']'), value, &error));
    CHECK(contains(error, "demasiados niveles"));
    CHECK(!json::parse(std::string(100000, '['), value));

    // Demasiado largo.
    CHECK(!json::parse("\"" + std::string(json::kMaxText, 'a') + "\"", value, &error));
    CHECK(contains(error, "demasiado largo"));
}

TEST_CASE(json_reads_unicode_escapes) {
    json::Value value;
    REQUIRE(json::parse("\"\\u00f1\\u20AC\\ud83d\\ude00\"", value));
    CHECK_EQ(value.string(), std::string("ñ€😀"));
    // Mitades de par sueltas: el carácter de sustitución (sin perder lo que sigue).
    REQUIRE(json::parse("\"\\ud800\"", value));
    CHECK_EQ(value.string(), std::string("\xEF\xBF\xBD"));
    REQUIRE(json::parse("\"\\udc00x\"", value));
    CHECK_EQ(value.string(), std::string("\xEF\xBF\xBDx"));
    REQUIRE(json::parse("\"\\ud800\\u0041\"", value));
    CHECK_EQ(value.string(), std::string("\xEF\xBF\xBD" "A"));
    // La marca BOM del principio no cuenta; los espacios de alrededor, tampoco.
    REQUIRE(json::parse("\xEF\xBB\xBF \t\r\n{\"a\": [true, false, null]} \n", value));
    CHECK(value["a"][size_t{0}].boolean());
    // Miembros repetidos: vale el primero.
    REQUIRE(json::parse("{\"a\": 1, \"a\": 2}", value));
    CHECK_EQ(value["a"].number(), 1.0);
    CHECK_EQ(value.members().size(), size_t{2});
}

// -----------------------------------------------------------------------------
// ZIP
// -----------------------------------------------------------------------------

TEST_CASE(zip_round_trip_and_open_raster_header) {
    std::vector<uint8_t> text(5000);
    for (size_t i = 0; i < text.size(); ++i) {
        text[i] = static_cast<uint8_t>('a' + i % 7);
    }
    const std::vector<ZipEntry> entries = {
        {"mimetype", bytesFrom("image/openraster"), zip::Method::Stored},
        {"texto.txt", text, zip::Method::Deflate},
        {"azar.bin", randomRgba(1000, 3), zip::Method::Deflate},   // no se comprime: va tal cual
        {"vacío", {}, zip::Method::Deflate},
        {"carpeta/ñandú.txt", {'h', 'o', 'l', 'a'}, zip::Method::Deflate},
    };
    const std::vector<uint8_t> file = makeZip(entries);
    REQUIRE(!file.empty());

    // OpenRaster: «mimetype» el primero, sin comprimir y sin campo extra, así que su texto
    // está en el byte 38.
    CHECK_EQ(get32le(&file[0]), 0x04034b50u);
    CHECK_EQ(get16le(&file[8]), 0);
    CHECK_EQ(get16le(&file[26]), 8);
    CHECK_EQ(get16le(&file[28]), 0);
    CHECK_EQ(std::string(file.begin() + 30, file.begin() + 38), std::string("mimetype"));
    CHECK_EQ(std::string(file.begin() + 38, file.begin() + 54), std::string("image/openraster"));

    zip::MemorySource source(file);
    zip::Reader reader;
    REQUIRE(reader.open(source));
    REQUIRE(reader.entries().size() == entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const zip::Entry& entry = reader.entries()[i];
        CHECK_EQ(entry.name, entries[i].name);
        std::vector<uint8_t> data;
        CHECK(reader.read(entry, data, 1 << 20));
        CHECK(data == entries[i].data);
    }
    CHECK(reader.find("texto.txt")->method == zip::Method::Deflate);
    CHECK(reader.find("texto.txt")->compressedSize < 200);
    CHECK(reader.find("azar.bin")->method == zip::Method::Stored);
    CHECK(reader.find("no está") == nullptr);
    // Los nombres que no son ASCII llevan la marca de UTF-8 (bit 11).
    const zip::Entry* unicode = reader.find("carpeta/ñandú.txt");
    REQUIRE(unicode != nullptr);
    CHECK((get16le(&file[unicode->headerOffset + 6]) & (1 << 11)) != 0);
    CHECK((get16le(&file[reader.find("texto.txt")->headerOffset + 6]) & (1 << 11)) == 0);

    // Más grande de lo que se deja leer.
    std::vector<uint8_t> data;
    CHECK(!reader.read(*reader.find("texto.txt"), data, 4999));
    CHECK(contains(reader.error(), "demasiado grande"));
    CHECK(data.empty());

    // Con un comentario al final (lo permite ZIP) también se lee.
    std::vector<uint8_t> commented = file;
    put16le(&commented[commented.size() - 2], 11);
    const std::string comment = "comentario!";
    commented.insert(commented.end(), comment.begin(), comment.end());
    zip::MemorySource commentedSource(commented);
    zip::Reader commentedReader;
    CHECK(commentedReader.open(commentedSource));
    CHECK(commentedReader.read(*commentedReader.find("texto.txt"), data, 1 << 20));
    CHECK(data == text);

    // Desde un archivo.
    const std::string path = test::tempFolder("zip_round_trip") + "prueba.zip";
    writeFile(path, file);
    zip::FileSource fileSource;
    REQUIRE(fileSource.open(SDL_IOFromFile(path.c_str(), "rb")));
    CHECK_EQ(fileSource.size(), uint64_t{file.size()});
    zip::Reader fileReader;
    REQUIRE(fileReader.open(fileSource));
    CHECK(fileReader.read(*fileReader.find("carpeta/ñandú.txt"), data, 100));
    CHECK_EQ(textOf(data), std::string("hola"));
}

TEST_CASE(zip_detects_damage) {
    const std::vector<uint8_t> text(4000, 'x');
    const std::vector<uint8_t> stored = randomRgba(500, 4);
    const std::vector<uint8_t> file =
        makeZip({{"comprimido", text, zip::Method::Deflate}, {"tal cual", stored, zip::Method::Stored}});
    REQUIRE(!file.empty());

    auto opens = [](const std::vector<uint8_t>& bytes, std::string* error = nullptr) {
        zip::MemorySource source(bytes);
        zip::Reader reader;
        const bool ok = reader.open(source);
        if (error) {
            *error = reader.error();
        }
        return ok;
    };
    // Lee una entrada de una copia dañada (el índice, intacto).
    auto readsEntry = [](const std::vector<uint8_t>& bytes, const char* name, std::string* error) {
        zip::MemorySource source(bytes);
        zip::Reader reader;
        if (!reader.open(source)) {
            *error = reader.error();
            return false;
        }
        std::vector<uint8_t> data;
        const bool ok = reader.read(*reader.find(name), data, 1 << 20);
        *error = reader.error();
        return ok;
    };

    zip::MemorySource source(file);
    zip::Reader reader;
    REQUIRE(reader.open(source));
    const zip::Entry compressed = *reader.find("comprimido");
    const zip::Entry raw = *reader.find("tal cual");
    const size_t compressedData = compressed.headerOffset + 30 + compressed.name.size();
    const size_t rawData = raw.headerOffset + 30 + raw.name.size();

    std::string error;
    // Un byte cambiado en lo comprimido o en lo guardado tal cual: el CRC no cuadra (o el
    // deflate no se puede deshacer).
    std::vector<uint8_t> damaged = file;
    damaged[compressedData + compressed.compressedSize / 2] ^= 0x10;
    CHECK(!readsEntry(damaged, "comprimido", &error));
    CHECK(contains(error, "comprimido está dañado"));
    damaged = file;
    damaged[rawData + 100] ^= 0x01;
    CHECK(!readsEntry(damaged, "tal cual", &error));
    CHECK(contains(error, "tal cual está dañado"));
    CHECK(readsEntry(damaged, "comprimido", &error));
    // La cabecera local estropeada.
    damaged = file;
    damaged[raw.headerOffset] = 'X';
    CHECK(!readsEntry(damaged, "tal cual", &error));

    // Cortado (sin el final del directorio), vacío o basura: no se abre.
    CHECK(!opens(std::vector<uint8_t>(file.begin(), file.end() - 1), &error));
    CHECK(contains(error, "cortado"));
    CHECK(!opens(std::vector<uint8_t>(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() / 2))));
    CHECK(!opens({}, &error));
    CHECK_EQ(error, std::string("no es un archivo ZIP"));
    CHECK(!opens(randomRgba(4096, 5)));

    // El final del directorio con datos imposibles.
    const size_t end = file.size() - 22;
    REQUIRE(get32le(&file[end]) == 0x06054b50u);
    damaged = file;
    put16le(&damaged[end + 8], 5000);
    put16le(&damaged[end + 10], 5000);
    CHECK(!opens(damaged, &error));
    CHECK(contains(error, "demasiados archivos"));
    damaged = file;
    put32le(&damaged[end + 16], static_cast<uint32_t>(file.size()));   // el directorio, fuera
    CHECK(!opens(damaged, &error));
    CHECK(contains(error, "índice"));
    damaged = file;
    put16le(&damaged[end + 4], 1);   // en varias partes
    CHECK(!opens(damaged, &error));
    damaged = file;
    put16le(&damaged[end + 10], 0xFFFF);   // ZIP64
    put16le(&damaged[end + 8], 0xFFFF);
    CHECK(!opens(damaged, &error));
    CHECK(contains(error, "ZIP64"));

    // Una entrada del directorio que apunta fuera.
    const size_t directory = get32le(&file[end + 16]);
    damaged = file;
    put32le(&damaged[directory + 42], static_cast<uint32_t>(directory));
    CHECK(!opens(damaged, &error));
    // Un tamaño comprimido imposible para lo que dice ocupar.
    damaged = file;
    put32le(&damaged[directory + 20], 100000);
    CHECK(!readsEntry(damaged, "comprimido", &error));
    // Una compresión que no se sabe leer.
    damaged = file;
    put16le(&damaged[directory + 10], 12);   // bzip2
    CHECK(!readsEntry(damaged, "comprimido", &error));
    CHECK(contains(error, "compresión"));
    // Cifrado.
    damaged = file;
    put16le(&damaged[directory + 8], 1);
    CHECK(!readsEntry(damaged, "comprimido", &error));
}

// -----------------------------------------------------------------------------
// PNG
// -----------------------------------------------------------------------------

TEST_CASE(png_decodes_every_filter_type) {
    // Como los de otros programas: cada fila con su filtro (también la primera, con la de
    // arriba en ceros) y los datos en varios chunks IDAT.
    const int width = 7;
    const int height = 6;
    const std::vector<uint8_t> rgba = randomRgba(width * height, 6);
    for (uint8_t first = 0; first < 5; ++first) {
        std::vector<uint8_t> filters;
        for (int y = 0; y < height; ++y) {
            filters.push_back(static_cast<uint8_t>((first + y) % 5));
        }
        const std::vector<uint8_t> file = craftPng(rgba, width, height, filters, 3);
        png::Image image;
        std::string error;
        REQUIRE(png::decode(file, image, 1000, &error));
        CHECK_EQ(image.width, width);
        CHECK_EQ(image.height, height);
        CHECK_EQ(test::maxDifference(image.rgba, rgba), 0);
        // stb_image lee lo mismo.
        int w = 0;
        int h = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &channels, 4);
        REQUIRE(pixels != nullptr);
        CHECK_EQ(test::maxDifference(std::vector<uint8_t>(pixels, pixels + rgba.size()), rgba), 0);
        stbi_image_free(pixels);
    }
    // Lo que escribe LiveSketch (Up en todas las filas) y su versión en el mismo buffer.
    const std::vector<uint8_t> big = randomRgba(61 * 37, 7);
    png::Image image;
    REQUIRE(png::decode(encodePng(big, 61, 37), image, 61 * 37));
    CHECK_EQ(test::maxDifference(image.rgba, big), 0);

    std::vector<uint8_t> inPlace = big;
    const std::vector<uint8_t> filtered = png::filterRows(big.data(), 61, 37, 61 * 4);
    REQUIRE(png::filterRowsInPlace(inPlace, 61, 37));
    CHECK(inPlace == filtered);
    // Con el byte de más por fila ya reservado no se copia: el buffer sigue siendo el mismo.
    std::vector<uint8_t> reserved;
    reserved.reserve(filtered.size());
    reserved = big;
    const uint8_t* before = reserved.data();
    REQUIRE(png::filterRowsInPlace(reserved, 61, 37));
    CHECK(reserved.data() == before);
    CHECK(reserved == filtered);
    std::vector<uint8_t> wrong(10);
    CHECK(!png::filterRowsInPlace(wrong, 61, 37));
    CHECK_EQ(wrong.size(), size_t{10});
}

TEST_CASE(png_decode_rejects_damage) {
    const std::vector<uint8_t> rgba = randomRgba(20 * 10, 8);
    const std::vector<uint8_t> file = encodePng(rgba, 20, 10);
    REQUIRE(!file.empty());
    png::Image image;
    std::string error;
    REQUIRE(png::decode(file, image, 200, &error));

    // Más píxeles de los que se dejan leer.
    CHECK(!png::decode(file, image, 199, &error));
    CHECK_EQ(error, std::string("la imagen es demasiado grande"));
    CHECK(image.rgba.empty());
    // No es un PNG.
    CHECK(!png::decode(bytesOf("GIF89a, no un PNG"), image, 200, &error));
    CHECK_EQ(error, std::string("no es un PNG"));
    // Cortado.
    CHECK(!png::decode(std::span<const uint8_t>(file.data(), file.size() - 5), image, 200, &error));
    CHECK_EQ(error, std::string("el PNG está cortado"));
    CHECK(!png::decode(std::span<const uint8_t>(file.data(), 8), image, 200, &error));
    // Un byte cambiado: el CRC de su chunk no cuadra.
    std::vector<uint8_t> damaged = file;
    damaged[file.size() - 30] ^= 0x40;
    CHECK(!png::decode(damaged, image, 200, &error));
    CHECK_EQ(error, std::string("el PNG está dañado"));

    // Con el CRC bien pero los datos mal: el deflate no sale, o sale corto.
    const std::vector<uint8_t> garbage = craftPng(rgba, 20, 10, {2});
    auto withIdat = [&](const std::vector<uint8_t>& idat) {
        std::vector<uint8_t> out(garbage.begin(), garbage.begin() + 33);   // firma e IHDR
        pngChunk(out, "IDAT", idat);
        pngChunk(out, "IEND", {});
        return out;
    };
    CHECK(!png::decode(withIdat(randomRgba(50, 9)), image, 200, &error));
    CHECK_EQ(error, std::string("el PNG está dañado"));
    std::vector<uint8_t> shortRows(10 * (20 * 4 + 1) - 1, 0);
    CHECK(!png::decode(withIdat(zlib(shortRows)), image, 200, &error));
    // Un filtro que no existe.
    CHECK(!png::decode(craftPng(rgba, 20, 10, {2, 2, 7}), image, 200, &error));
    CHECK_EQ(error, std::string("el PNG está dañado"));
    // Sin datos de imagen.
    std::vector<uint8_t> empty(garbage.begin(), garbage.begin() + 33);
    pngChunk(empty, "IEND", {});
    CHECK(!png::decode(empty, image, 200, &error));
    // Una cabecera imposible (ancho 0).
    std::vector<uint8_t> header = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    pngChunk(header, "IHDR", {0, 0, 0, 0, 0, 0, 0, 1, 8, 6, 0, 0, 0});
    pngChunk(header, "IEND", {});
    CHECK(!png::decode(header, image, 200, &error));
}

TEST_CASE(png_solid_background_image) {
    // El fondo para los demás programas: con paleta, ocupa casi nada a cualquier tamaño.
    const uint8_t color[3] = {12, 200, 99};
    for (const ColorProfile profile : {ColorProfile::Srgb, ColorProfile::DisplayP3}) {
        png::Info info;
        info.profile = profile;
        std::vector<uint8_t> file;
        REQUIRE(png::writeSolid(37, 5, color, info, [&file](const uint8_t* data, size_t size) {
            file.insert(file.end(), data, data + size);
            return true;
        }));
        png::Image image;
        REQUIRE(png::decode(file, image, 37 * 5));
        CHECK_EQ(image.width, 37);
        CHECK_EQ(image.height, 5);
        bool solid = true;
        for (size_t i = 0; i < image.rgba.size(); i += 4) {
            solid = solid && image.rgba[i] == 12 && image.rgba[i + 1] == 200 && image.rgba[i + 2] == 99 &&
                    image.rgba[i + 3] == 255;
        }
        CHECK(solid);
    }
    size_t bytes = 0;
    CHECK(png::writeSolid(4000, 3000, color, {}, [&bytes](const uint8_t*, size_t size) {
        bytes += size;
        return true;
    }));
    CHECK(bytes < 4096);
}

// -----------------------------------------------------------------------------
// document.json, stack.xml e imágenes
// -----------------------------------------------------------------------------

TEST_CASE(project_document_round_trip) {
    project::Document document = sampleDocument();
    project::LayerInfo ink = layerInfo("Tinta «negra»", {10, 20, 110, 70}, 0);
    ink.opacity = 0.37f;
    ink.blend = BlendMode::Multiply;
    ink.alphaLock = true;
    project::LayerInfo shade = layerInfo("Sombras", {}, 1);
    shade.visible = false;
    shade.blend = BlendMode::HardMix;
    shade.clipping = true;
    project::LayerInfo photo = layerInfo("Foto", {0, 0, 300, 200}, 2);
    photo.reference = true;
    photo.blend = BlendMode::Luminosity;
    document.layers = {ink, shade, photo};
    document.activeLayer = 1;

    const std::string text = project::documentJson(document, "LiveSketch 9.9");
    CHECK(contains(text, "\"format\": \"LiveSketch\""));
    CHECK(contains(text, "\"version\": 1,"));
    CHECK(contains(text, "\"minVersion\": 1,"));
    CHECK(contains(text, "\"app\": \"LiveSketch 9.9\""));
    CHECK(contains(text, "\"created\": \"2023-11-14T22:13:20Z\""));
    CHECK(contains(text, "\"modified\": \"2023-11-14T23:13:20Z\""));
    CHECK(contains(text, "\"blend\": \"hard-mix\""));
    CHECK(contains(text, "\"profile\": \"display-p3\""));

    project::Document read;
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(project::readDocument(text, 0, read, warnings, error));
    CHECK(warnings.empty());
    CHECK(error.empty());
    CHECK_EQ(read.width, 300);
    CHECK_EQ(read.height, 200);
    CHECK_EQ(read.info.name, document.info.name);
    CHECK_EQ(read.info.ppi, 300.0f);
    CHECK(read.info.unit == LengthUnit::Centimeters);
    CHECK(read.info.profile == ColorProfile::DisplayP3);
    CHECK_EQ(read.info.created, document.info.created);
    CHECK_EQ(read.info.modified, document.info.modified);
    CHECK_EQ(read.info.drawingSeconds, document.info.drawingSeconds);
    CHECK_EQ(read.info.strokes, uint64_t{42});
    CHECK(read.background == document.background);
    REQUIRE(read.layers.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        const project::LayerInfo& a = read.layers[i];
        const project::LayerInfo& b = document.layers[i];
        CHECK_EQ(a.name, b.name);
        CHECK_EQ(a.visible, b.visible);
        CHECK_EQ(a.opacity, b.opacity);
        CHECK(a.blend == b.blend);
        CHECK_EQ(a.alphaLock, b.alphaLock);
        CHECK_EQ(a.clipping, b.clipping);
        CHECK_EQ(a.reference, b.reference);
        CHECK(a.rect == b.rect);
        CHECK_EQ(a.file, b.file);
    }
    CHECK_EQ(read.activeLayer, 1);
    CHECK(read.guide == document.guide);
    CHECK(!read.view.fitted);
    CHECK_EQ(read.view.zoom, 2.5f);
    CHECK(read.view.center == document.view.center);
    CHECK_EQ(read.view.angle, -0.7f);
    CHECK(read.view.flipped);
    CHECK(read.hasColor);
    CHECK(std::equal(read.color, read.color + 3, document.color));

    // Todos los modos de fusión vuelven como eran.
    project::Document blends = sampleDocument();
    for (int i = 0; i < kBlendModeCount; ++i) {
        project::LayerInfo layer = layerInfo("Capa " + std::to_string(i), {}, i);
        layer.blend = static_cast<BlendMode>(i);
        blends.layers.push_back(layer);
    }
    blends.width = 64;   // en uno pequeño caben 26 capas
    blends.height = 64;
    REQUIRE(project::readDocument(project::documentJson(blends, "x"), 0, read, warnings, error));
    REQUIRE(read.layers.size() == size_t{kBlendModeCount});
    for (int i = 0; i < kBlendModeCount; ++i) {
        CHECK(read.layers[static_cast<size_t>(i)].blend == static_cast<BlendMode>(i));
    }
    CHECK(warnings.empty());

    // Ajustada a la ventana: no se guarda el zoom.
    document.view = {};
    document.hasColor = false;
    const std::string fitted = project::documentJson(document, "x");
    CHECK(!contains(fitted, "\"zoom\""));
    CHECK(!contains(fitted, "\n  \"color\": ["));
    REQUIRE(project::readDocument(fitted, 0, read, warnings, error));
    CHECK(read.view.fitted);
    CHECK(!read.view.flipped);
    CHECK(!read.hasColor);
}

TEST_CASE(project_document_checks_what_it_reads) {
    struct Result {
        bool ok = false;
        project::Document document;
        std::vector<std::string> warnings;
        std::string error;
    };
    auto read = [](const std::string& text, int maxSide = 0) {
        Result result;
        result.ok = project::readDocument(text, maxSide, result.document, result.warnings, result.error);
        return result;
    };
    // Un documento mínimo con lo que se le añada.
    auto doc = [](const std::string& canvas, const std::string& layers, const std::string& more = "") {
        return "{\"format\": \"LiveSketch\", \"version\": 1, \"canvas\": {" + canvas + "}, \"layers\": [" + layers +
               "]" + more + "}";
    };
    const std::string size = "\"width\": 100, \"height\": 80";

    // Lo mínimo se abre, con todo lo demás por defecto.
    Result r = read(doc(size, ""));
    REQUIRE(r.ok);
    CHECK(r.warnings.empty());
    CHECK_EQ(r.document.layers.size(), size_t{1});   // sin capas: una vacía
    CHECK(r.document.layers[0].rect.empty());
    CHECK_EQ(r.document.activeLayer, 0);
    CHECK_EQ(r.document.info.ppi, 72.0f);
    CHECK(r.document.info.profile == ColorProfile::Srgb);
    CHECK(r.document.info.unit == LengthUnit::Pixels);
    CHECK(r.document.background == CanvasBackground{});
    CHECK(r.document.guide == guide::defaults({100.0f, 80.0f}));
    CHECK(r.document.view.fitted);
    CHECK(!r.document.hasColor);
    CHECK_EQ(r.document.info.created, int64_t{0});

    // No es JSON, no es de LiveSketch o le falta lo imprescindible.
    r = read("{\"format\": \"LiveSketch\",");
    CHECK(!r.ok);
    CHECK(contains(r.error, "su descripción (document.json) está dañada: línea 1"));
    CHECK_EQ(read("{\"format\": \"Krita\", \"version\": 1}").error, std::string("no es un proyecto de LiveSketch"));
    CHECK_EQ(read("[1, 2]").error, std::string("no es un proyecto de LiveSketch"));
    CHECK(contains(read("{\"format\": \"LiveSketch\"}").error, "falta la versión del formato"));
    CHECK(contains(read("{\"format\": \"LiveSketch\", \"version\": 1.5}").error, "falta la versión"));
    CHECK(contains(read(doc(size, "", ", \"minVersion\": \"dos\"")).error, "la versión mínima no es válida"));
    CHECK(contains(read(doc("\"width\": 100", "")).error, "el tamaño del lienzo no es válido"));
    CHECK(contains(read(doc("\"width\": 100.5, \"height\": 80", "")).error, "el tamaño del lienzo no es válido"));
    CHECK(contains(read(doc("\"width\": -100, \"height\": 80", "")).error, "el tamaño del lienzo no es válido"));
    CHECK(contains(read("{\"format\": \"LiveSketch\", \"version\": 1, \"canvas\": {" + size + "}}").error,
                   "faltan las capas"));

    // Versiones: una más nueva que esta puede leer se abre con un aviso; si pide más, no.
    r = read("{\"format\": \"LiveSketch\", \"version\": 2, \"minVersion\": 2, \"canvas\": {" + size +
             "}, \"layers\": []}");
    CHECK(!r.ok);
    CHECK_EQ(r.error,
             std::string("se guardó con una versión más nueva de LiveSketch: actualiza la app para abrirlo"));
    r = read("{\"format\": \"LiveSketch\", \"version\": 3, \"minVersion\": 1, \"canvas\": {" + size +
             "}, \"layers\": [], \"algo nuevo\": {\"x\": 1}}");
    CHECK(r.ok);
    REQUIRE(r.warnings.size() == 1);
    CHECK_EQ(r.warnings[0],
             std::string("Se guardó con una versión más nueva de LiveSketch: lo que esta no conoce no se abre."));
    // Sin minVersion, la mínima es la suya.
    CHECK(!read("{\"format\": \"LiveSketch\", \"version\": 2, \"canvas\": {" + size + "}, \"layers\": []}").ok);

    // Tamaños que no se pueden abrir aquí, explicados con números.
    CHECK_EQ(read(doc("\"width\": 4, \"height\": 80", "")).error,
             std::string("el lienzo mide 4 × 80 px, menos del mínimo de 8 px"));
    CHECK_EQ(read(doc("\"width\": 5000, \"height\": 100", ""), 4096).error,
             std::string("el lienzo mide 5000 × 100 px y este dispositivo admite como mucho 4096 px de lado"));
    CHECK(read(doc("\"width\": 5000, \"height\": 100", ""), 8192).ok);
    CHECK(contains(read(doc("\"width\": 20000, \"height\": 20000", "")).error,
                   "el lienzo mide 20000 × 20000 px, más de lo que cabe en memoria"));
    const int limit = canvasspec::layerLimit(6000, 6000);
    std::string tooMany;
    for (int i = 0; i <= limit; ++i) {
        tooMany += std::string(i > 0 ? ", " : "") + "{}";
    }
    CHECK_EQ(read(doc("\"width\": 6000, \"height\": 6000", tooMany)).error,
             "tiene " + std::to_string(limit + 1) + " capas y en un lienzo de 6000 × 6000 px caben como mucho " +
                 std::to_string(limit));

    // Lo que se puede arreglar se arregla y se avisa.
    r = read(doc(size + ", \"profile\": \"rec2020\", \"ppi\": 0, \"unit\": \"pies\"",
                 "{\"name\": \"Tinta\", \"blend\": \"brillo\", \"opacity\": 1.5, \"clipping\": true, "
                 "\"reference\": true, \"x\": 0, \"y\": 0, \"width\": 0, \"height\": 0},"
                 "{\"name\": \"Mala\", \"x\": 90, \"y\": 0, \"width\": 20, \"height\": 10},"
                 "{\"reference\": true, \"opacity\": \"mucha\", \"x\": 10, \"y\": 20, \"width\": 30, \"height\": 40, "
                 "\"blend\": 3}",
                 ", \"activeLayer\": 99"));
    REQUIRE(r.ok);
    CHECK(r.document.info.profile == ColorProfile::Srgb);
    CHECK_EQ(r.document.info.ppi, canvasspec::kMinPpi);
    CHECK(r.document.info.unit == LengthUnit::Pixels);
    REQUIRE(r.document.layers.size() == 3);
    CHECK(r.document.layers[0].blend == BlendMode::Normal);
    CHECK_EQ(r.document.layers[0].opacity, 1.0f);
    CHECK(!r.document.layers[0].clipping);   // la de abajo no recorta con nada
    CHECK(r.document.layers[0].reference);
    CHECK(r.document.layers[1].rect.empty());   // se sale del lienzo
    CHECK(!r.document.layers[2].reference);     // solo una de referencia
    CHECK_EQ(r.document.layers[2].opacity, 1.0f);
    CHECK(r.document.layers[2].blend == BlendMode::Normal);
    CHECK(r.document.layers[2].rect == (IRect{10, 20, 40, 60}));
    CHECK_EQ(r.document.activeLayer, 2);   // fuera de rango: la de arriba
    CHECK(anyContains(r.warnings, "Su perfil de color no existe en esta versión: se abre como sRGB."));
    CHECK(anyContains(r.warnings,
                      "La capa «Tinta» usa un modo de fusión que esta versión no tiene: queda en Normal."));
    CHECK(anyContains(r.warnings, "La capa «Mala» está dañada: se abre vacía."));
    CHECK(anyContains(r.warnings, "La capa sin nombre usa un modo de fusión"));
    CHECK_EQ(r.warnings.size(), size_t{4});

    // Lo que no tiene sentido vuelve a lo normal (sin aviso).
    r = read(doc(size + ", \"background\": {\"color\": [2, -1, \"x\"]}, \"created\": \"ayer\", "
                        "\"modified\": \"2023-11-14T22:13:20Z\", \"drawingSeconds\": -5, \"strokes\": 1.5",
                 "{\"name\": \"a\\u0000b\\nc\", \"x\": 0, \"y\": 0, \"width\": 0, \"height\": 0}",
                 ", \"guide\": {\"enabled\": true, \"kind\": \"symmetry\", \"center\": [1e300, 2], \"gridSize\": 1,"
                 " \"opacity\": 9, \"symmetry\": \"spiral\"},"
                 " \"view\": {\"fitted\": false, \"zoom\": 0, \"center\": [1, 2, 3]},"
                 " \"color\": [0.5, 0.5]"));
    REQUIRE(r.ok);
    CHECK(r.warnings.empty());
    CHECK(r.document.background == CanvasBackground{});
    CHECK_EQ(r.document.info.created, kSomeTime);   // sin fecha de creación: la del último cambio
    CHECK_EQ(r.document.info.modified, kSomeTime);
    CHECK_EQ(r.document.info.drawingSeconds, 0.0);
    CHECK_EQ(r.document.info.strokes, uint64_t{0});
    CHECK_EQ(r.document.layers[0].name, std::string("abc"));
    CHECK(r.document.guide.enabled);
    CHECK(r.document.guide.kind == GuideKind::Symmetry);
    CHECK(r.document.guide.center == guide::defaults({100.0f, 80.0f}).center);
    CHECK_EQ(r.document.guide.gridSize, guide::kMinGridSize);
    CHECK_EQ(r.document.guide.opacity, 1.0f);
    CHECK(r.document.guide.symmetry == SymmetryKind::Vertical);
    CHECK(r.document.view.fitted);   // sin centro válido, ajustada
    CHECK(!r.document.hasColor);
}

TEST_CASE(project_names_and_dates) {
    CHECK_EQ(project::cleanName("Capa\n1\t"), std::string("Capa1"));
    CHECK_EQ(project::cleanName("a\xFF" "b\xC0\xAF" "c"), std::string("abc"));   // bytes sueltos y formas largas
    CHECK_EQ(project::cleanName("x\xED\xA0\x80y"), std::string("xy"));            // mitad de par UTF-16
    CHECK_EQ(project::cleanName("\xC2\x85z\x7F"), std::string("z"));             // controles C1 y DEL
    CHECK_EQ(project::cleanName("ñ € 😀"), std::string("ñ € 😀"));
    CHECK_EQ(project::cleanName("\xF0\x9F\x98"), std::string());                 // cortado
    CHECK_EQ(project::cleanName(std::string(300, 'a')).size(), project::kMaxNameBytes);
    std::string enes;
    for (int i = 0; i < 300; ++i) {
        enes += "ñ";
    }
    const std::string cut = project::cleanName(enes);
    CHECK_EQ(cut.size(), size_t{254});   // sin partir una «ñ»
    CHECK_EQ(project::cleanName(cut), cut);

    CHECK_EQ(project::isoTime(0), std::string("1970-01-01T00:00:00Z"));
    CHECK_EQ(project::isoTime(kSomeTime), std::string("2023-11-14T22:13:20Z"));
    CHECK_EQ(project::isoTime(kSomeTime + 999999999), std::string("2023-11-14T22:13:20Z"));
    int64_t time = 0;
    CHECK(project::parseIsoTime("2023-11-14T22:13:20Z", &time));
    CHECK_EQ(time, kSomeTime);
    CHECK(project::parseIsoTime("2023-11-15T00:13:20+02:00", &time));
    CHECK_EQ(time, kSomeTime);
    CHECK(project::parseIsoTime("2023-11-14T17:13:20.5-05:00", &time));
    CHECK_EQ(time, kSomeTime + 500000000);
    CHECK(project::parseIsoTime("2024-02-29t12:00:00z", &time));
    const char* const bad[] = {"", "ayer", "2023-11-14", "2023-11-14T22:13:20", "2023-11-14T22:13:20+0200",
                               "2023-13-14T22:13:20Z", "2023-02-30T22:13:20Z", "2023-11-14T24:00:00Z",
                               "2023-11-14T22:60:00Z", "2023-11-14T22:13:60Z", "2023-11-14T22:13:20.Z",
                               "2023-11-14 22:13:20Z", "2023-11-14T22:13:20Zx", "2023-11-14T22:13:20+25:00",
                               "2023-00-10T22:13:20Z", "2023-11-00T22:13:20Z"};
    for (const char* text : bad) {
        if (project::parseIsoTime(text, &time)) {
            test::fail(__FILE__, __LINE__, std::string("se aceptó la fecha ") + text);
        }
    }
}

TEST_CASE(project_stack_xml_for_other_apps) {
    project::Document document = sampleDocument();
    project::LayerInfo sketch = layerInfo("Boceto <1> & \"más\" 'aquí'", {5, 7, 25, 17}, 0);
    sketch.visible = false;
    sketch.opacity = 0.5f;
    project::LayerInfo burn = layerInfo("Quemar", {}, 1);
    burn.blend = BlendMode::LinearBurn;
    burn.alphaLock = true;
    project::LayerInfo top = layerInfo("Arriba", {0, 0, 300, 200}, 2);
    top.blend = BlendMode::Add;
    document.layers = {sketch, burn, top};
    document.activeLayer = 1;
    document.background.visible = true;
    const std::string xml = project::stackXml(document);

    CHECK(contains(xml, "<image version=\"0.0.5\" w=\"300\" h=\"200\" xres=\"300\" yres=\"300\">"));
    // De arriba abajo, y el fondo como la capa de abajo.
    const size_t a = xml.find("name=\"Arriba\"");
    const size_t b = xml.find("name=\"Quemar\"");
    const size_t c = xml.find("name=\"Boceto &lt;1&gt; &amp; &quot;más&quot; &apos;aquí&apos;\"");
    const size_t d = xml.find("name=\"Fondo\" src=\"data/background.png\"");
    CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos && d != std::string::npos);
    CHECK(a < b && b < c && c < d);
    CHECK(contains(xml, "src=\"data/layer0.png\" x=\"5\" y=\"7\" opacity=\"0.5\" visibility=\"hidden\" "
                        "composite-op=\"svg:src-over\""));
    CHECK(contains(xml, "src=\"data/layer1.png\" x=\"0\" y=\"0\" opacity=\"1\" visibility=\"visible\" "
                        "composite-op=\"krita:linear_burn\" alpha-preserve=\"true\" selected=\"true\""));
    CHECK(contains(xml, "composite-op=\"svg:plus\""));
    CHECK(contains(xml, "name=\"Fondo\" src=\"data/background.png\" x=\"0\" y=\"0\" opacity=\"1\" "
                        "visibility=\"visible\""));
    // Sin fondo visible, el fondo va oculto.
    document.background.visible = false;
    CHECK(contains(project::stackXml(document), "name=\"Fondo\" src=\"data/background.png\" x=\"0\" y=\"0\" "
                                                "opacity=\"1\" visibility=\"hidden\""));
    // Cada modo, con su nombre en los dos sitios.
    for (int i = 0; i < kBlendModeCount; ++i) {
        const BlendMode blend = static_cast<BlendMode>(i);
        CHECK(std::strlen(project::blendName(blend)) > 0);
        CHECK(std::strncmp(project::compositeOp(blend), "svg:", 4) == 0 ||
              std::strncmp(project::compositeOp(blend), "krita:", 6) == 0);
    }
}

TEST_CASE(project_layer_images_round_trip) {
    // Los píxeles de una capa vuelven idénticos al pasar por su PNG.
    const IRect rect{3, 4, 103, 54};
    const std::vector<uint8_t> pixels = randomPremultiplied(100 * 50, 10);
    for (const ColorProfile profile : {ColorProfile::Srgb, ColorProfile::DisplayP3}) {
        png::Info info;
        info.profile = profile;
        std::vector<uint8_t> work = pixels;
        std::string error;
        const std::vector<uint8_t> file = project::encodeImage(work, 100, 50, info, &error);
        REQUIRE(!file.empty());
        project::LayerInfo layer = layerInfo("x", rect, 0);
        CHECK(file.size() <= project::maxLayerFile(layer));
        std::vector<uint8_t> out;
        REQUIRE(project::decodeLayer(file, layer, out, &error));
        CHECK_EQ(test::maxDifference(out, pixels), 0);

        // Si no mide lo que dice el documento, está dañada.
        layer.rect = {3, 4, 103, 55};
        CHECK(!project::decodeLayer(file, layer, out, &error));
        CHECK_EQ(error, std::string("la imagen no mide lo que dice el proyecto"));
        CHECK(out.empty());
        // Una capa vacía no necesita imagen.
        layer.rect = {};
        CHECK(project::decodeLayer({}, layer, out, &error));
        CHECK(out.empty());
    }
    // Las peores capas (ruido) caben en lo que se deja ocupar a su archivo.
    std::vector<uint8_t> noise = randomRgba(640 * 480, 11);
    for (size_t i = 3; i < noise.size(); i += 4) {
        noise[i] = 255;
    }
    png::Info p3;
    p3.profile = ColorProfile::DisplayP3;
    std::string error;
    const std::vector<uint8_t> noisy = project::encodeImage(noise, 640, 480, p3, &error);
    REQUIRE(!noisy.empty());
    CHECK(noisy.size() <= project::maxLayerFile(layerInfo("ruido", {0, 0, 640, 480}, 0)));
    std::vector<uint8_t> wrong(10);
    CHECK(project::encodeImage(wrong, 640, 480, p3, &error).empty());
    CHECK(!error.empty());

    // El fondo, del tamaño del lienzo y de su color.
    const float color[3] = {1.0f, 0.5f, 0.0f};
    const std::vector<uint8_t> background = project::encodeBackground(30, 20, color, ColorProfile::Srgb, &error);
    png::Image image;
    REQUIRE(png::decode(background, image, 600));
    CHECK_EQ(image.width, 30);
    CHECK_EQ(image.height, 20);
    CHECK(test::pixelAt(image.rgba, 30, 29, 19) == (test::Pixel{255, 128, 0, 255}));
}

TEST_CASE(project_thumbnail_averages_down) {
    // 600 × 300 de un color: 256 × 128 del mismo color.
    std::vector<uint8_t> solid(600 * 300 * 4);
    for (size_t i = 0; i < solid.size(); i += 4) {
        solid[i] = 100;   // premultiplicado: (200, 100, 50) con alfa 128
        solid[i + 1] = 50;
        solid[i + 2] = 25;
        solid[i + 3] = 128;
    }
    int width = 0;
    int height = 0;
    std::vector<uint8_t> small = project::thumbnail(solid.data(), 600, 300, 256, &width, &height);
    CHECK_EQ(width, 256);
    CHECK_EQ(height, 128);
    REQUIRE(small.size() == size_t{256 * 128 * 4});
    CHECK(test::pixelAt(small, 256, 0, 0) == (test::Pixel{199, 100, 50, 128}));
    CHECK(test::pixelAt(small, 256, 255, 127) == (test::Pixel{199, 100, 50, 128}));

    // La media se hace premultiplicada: lo transparente no ennegrece.
    const uint8_t quad[16] = {255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0};
    small = project::thumbnail(quad, 2, 2, 1, &width, &height);
    CHECK_EQ(width, 1);
    CHECK_EQ(height, 1);
    CHECK(test::pixelAt(small, 1, 0, 0) == (test::Pixel{128, 0, 128, 128}));
    // Más pequeño que el máximo: del mismo tamaño. Muy alargado: al menos 1 px.
    small = project::thumbnail(solid.data(), 60, 30, 256, &width, &height);
    CHECK_EQ(width, 60);
    CHECK_EQ(height, 30);
    small = project::thumbnail(solid.data(), 2000, 4, 256, &width, &height);
    CHECK_EQ(width, 256);
    CHECK_EQ(height, 1);
    CHECK(project::thumbnail(nullptr, 10, 10, 256, &width, &height).empty());
    CHECK_EQ(width, 0);
}

// -----------------------------------------------------------------------------
// Guardar y abrir
// -----------------------------------------------------------------------------

TEST_CASE(task_pool_runs_every_task) {
    std::atomic<int> done{0};
    {
        io::TaskPool pool(3);
        for (int i = 0; i < 200; ++i) {
            pool.submit([&done] { ++done; });
        }
        pool.submit({});   // vacío: se ignora
        pool.waitIdle();
        CHECK_EQ(done.load(), 200);
        for (int i = 0; i < 50; ++i) {
            pool.submit([&done] { ++done; });
        }
        if (!pool.threaded()) {
            CHECK(pool.runPending(0));   // al menos uno
            CHECK(done.load() > 200);
        }
    }   // al destruirse, termina lo que falta
    CHECK_EQ(done.load(), 250);
}

TEST_CASE(project_saver_writes_layers_in_order) {
    const std::string folder = test::tempFolder("project_saver_order");
    const SmallProject project = smallProject(folder);
    REQUIRE(!project.path.empty());
    CHECK(filesIn(folder) == std::vector<std::string>{"pequeño.lvskt"});

    const std::vector<uint8_t> file = readFile(project.path);
    const std::vector<ZipEntry> entries = unzip(file);
    std::vector<std::string> names;
    for (const ZipEntry& entry : entries) {
        names.push_back(entry.name);
    }
    // Primero «mimetype» (OpenRaster), luego las capas de abajo arriba, y la descripción la
    // última: un archivo cortado antes no se abre a medias.
    CHECK(names == (std::vector<std::string>{"mimetype", "data/layer0.png", "data/layer1.png", "data/layer2.png",
                                             "data/background.png", "Thumbnails/thumbnail.png", "mergedimage.png",
                                             "stack.xml", "livesketch/document.json"}));
    REQUIRE(entries.size() == 9);
    CHECK(entries[0].method == zip::Method::Stored);
    CHECK_EQ(textOf(entries[0].data), std::string("image/openraster"));
    CHECK_EQ(std::string(file.begin() + 38, file.begin() + 54), std::string("image/openraster"));
    // Los PNG van tal cual (ya están comprimidos); el texto, comprimido.
    CHECK(entries[1].method == zip::Method::Stored);
    CHECK(entries[8].method == zip::Method::Deflate);
    // La capa vacía lleva una imagen de 1 × 1 transparente (OpenRaster quiere una por capa).
    png::Image image;
    REQUIRE(png::decode(entries[2].data, image, 1));
    CHECK(image.rgba == (std::vector<uint8_t>{0, 0, 0, 0}));
    // La miniatura y el dibujo entero, del tamaño que toca.
    REQUIRE(png::decode(entries[5].data, image, 64 * 48));
    CHECK_EQ(image.width, 64);
    CHECK_EQ(image.height, 48);
    REQUIRE(png::decode(entries[6].data, image, 64 * 48));
    CHECK_EQ(image.width, 64);
    // El fondo, de su color (0.1, 0.2, 0.3).
    REQUIRE(png::decode(entries[4].data, image, 64 * 48));
    CHECK(test::pixelAt(image.rgba, 64, 63, 47) == (test::Pixel{26, 51, 77, 255}));
    // Las capas guardan sus nombres de archivo en la descripción.
    project::Document document;
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(project::readDocument(textOf(entries[8].data), 0, document, warnings, error));
    CHECK_EQ(document.layers[2].file, std::string("data/layer2.png"));
    CHECK(contains(textOf(entries[8].data), "\"app\": \"LiveSketch pruebas\""));

    // Python, Krita o cualquier otro lo leen: el directorio central tiene los mismos datos que
    // las cabeceras locales.
    zip::MemorySource source(file);
    zip::Reader reader;
    REQUIRE(reader.open(source));
    for (const zip::Entry& entry : reader.entries()) {
        const uint8_t* local = &file[entry.headerOffset];
        CHECK_EQ(get32le(local + 14), entry.crc);
        CHECK_EQ(uint64_t{get32le(local + 18)}, entry.compressedSize);
        CHECK_EQ(uint64_t{get32le(local + 22)}, entry.size);
    }
}

TEST_CASE(project_loader_gives_layers_back) {
    const std::string folder = test::tempFolder("project_loader");
    const SmallProject project = smallProject(folder);
    REQUIRE(!project.path.empty());

    // Desde el archivo y desde memoria (como en Android, si no se puede ir de un sitio a otro).
    for (int fromMemory = 0; fromMemory < 2; ++fromMemory) {
        project::Loader loader;
        REQUIRE(fromMemory ? loader.open(readFile(project.path), 0) : loader.open(project.path, 0));
        CHECK(loader.error().empty());
        CHECK(loader.warnings().empty());
        const project::Document& document = loader.document();
        CHECK_EQ(document.width, 64);
        CHECK_EQ(document.layers.size(), size_t{3});
        CHECK_EQ(document.layers[2].name, std::string("Tinta"));
        CHECK_EQ(loader.progress(), 0.0f);
        const Loaded loaded = loadLayers(loader);
        REQUIRE(loaded.ok);
        CHECK(loaded.warnings.empty());
        CHECK_EQ(test::maxDifference(loaded.pixels[0], project.pixels[0]), 0);
        CHECK(loaded.pixels[1].empty());
        CHECK_EQ(test::maxDifference(loaded.pixels[2], project.pixels[2]), 0);
        CHECK_EQ(loader.progress(), 1.0f);
        int index = 0;
        std::vector<uint8_t> pixels;
        CHECK(!loader.next(&index, pixels));
    }

    // Un Loader que se destruye a medias no deja nada colgado.
    {
        project::Loader loader;
        REQUIRE(loader.open(project.path, 0));
        loader.start();
    }

    // No cabe en esta GPU.
    project::Loader small;
    CHECK(!small.open(project.path, 32));
    CHECK_EQ(small.error(), std::string("el lienzo mide 64 × 48 px y este dispositivo admite como mucho 32 px de lado"));
}

TEST_CASE(project_loader_survives_damage) {
    const std::string folder = test::tempFolder("project_damage");
    const SmallProject project = smallProject(folder);
    REQUIRE(!project.path.empty());
    const std::vector<uint8_t> file = readFile(project.path);

    auto openBytes = [](const std::vector<uint8_t>& bytes, std::string* error) {
        project::Loader loader;
        const bool ok = loader.open(bytes, 0);
        *error = loader.error();
        return ok;
    };
    std::string error;

    // Lo que no es un proyecto.
    CHECK(!openBytes({}, &error));
    CHECK_EQ(error, std::string("no es un proyecto de LiveSketch o está dañado"));
    CHECK(!openBytes(std::vector<uint8_t>(file.begin(), file.end() - 10), &error));
    CHECK_EQ(error, std::string("no es un proyecto de LiveSketch o está dañado"));
    CHECK(!openBytes(randomRgba(1000, 12), &error));
    CHECK(!openBytes(encodePng(randomRgba(4, 13), 2, 2), &error));
    CHECK_EQ(error, std::string("no es un proyecto de LiveSketch o está dañado"));
    CHECK(!openBytes(makeZip({{"hola.txt", {'h', 'o', 'l', 'a'}}}), &error));
    CHECK_EQ(error, std::string("no es un proyecto de LiveSketch"));
    project::Loader missing;
    CHECK(!missing.open(folder + "no existe.lvskt", 0));
    CHECK_EQ(missing.error(), std::string("no se pudo leer el archivo"));

    // Un OpenRaster de otro programa (sin la descripción de LiveSketch).
    CHECK(!openBytes(rewriteZip(file, [](ZipEntry& entry) { return entry.name != project::kDocumentEntry; }), &error));
    CHECK_EQ(error, std::string("es un archivo OpenRaster de otro programa: por ahora solo se abren proyectos de "
                                "LiveSketch"));
    // La descripción dañada.
    CHECK(!openBytes(rewriteZip(file,
                                [](ZipEntry& entry) {
                                    if (entry.name == project::kDocumentEntry) {
                                        entry.data.resize(entry.data.size() / 2);
                                    }
                                    return true;
                                }),
                     &error));
    CHECK(contains(error, "su descripción (document.json) está dañada: "));
    std::vector<uint8_t> damaged = file;
    zip::MemorySource source(file);
    zip::Reader reader;
    REQUIRE(reader.open(source));
    const zip::Entry* document = reader.find(project::kDocumentEntry);
    REQUIRE(document != nullptr);
    damaged[document->headerOffset + 30 + document->name.size() + 5] ^= 0x22;
    CHECK(!openBytes(damaged, &error));
    CHECK_EQ(error, std::string("su descripción (document.json) está dañada"));

    // Una capa dañada: se abren las demás, y esa vacía con un aviso.
    auto loadWith = [](const std::vector<uint8_t>& bytes) {
        project::Loader loader;
        Loaded loaded;
        if (loader.open(bytes, 0)) {
            loaded = loadLayers(loader);
        }
        return loaded;
    };
    const zip::Entry* ink = reader.find("data/layer2.png");
    REQUIRE(ink != nullptr);
    damaged = file;
    damaged[ink->headerOffset + 30 + ink->name.size() + ink->compressedSize / 2] ^= 0x08;
    Loaded loaded = loadWith(damaged);
    REQUIRE(loaded.ok);
    CHECK_EQ(test::maxDifference(loaded.pixels[0], project.pixels[0]), 0);
    CHECK(loaded.pixels[2].empty());
    CHECK(loaded.warnings == std::vector<std::string>{"La capa «Tinta» está dañada: se abre vacía."});

    // Sin la imagen de una capa, o con una que no mide lo que su caja.
    loaded = loadWith(rewriteZip(file, [](ZipEntry& entry) { return entry.name != "data/layer0.png"; }));
    REQUIRE(loaded.ok);
    CHECK(loaded.pixels[0].empty());
    CHECK_EQ(test::maxDifference(loaded.pixels[2], project.pixels[2]), 0);
    CHECK(loaded.warnings == std::vector<std::string>{"La capa «Boceto» está dañada: se abre vacía."});
    loaded = loadWith(rewriteZip(file, [](ZipEntry& entry) {
        if (entry.name == "data/layer0.png") {
            entry.data = encodePng(randomRgba(4, 14), 2, 2);
        }
        return true;
    }));
    REQUIRE(loaded.ok);
    CHECK(loaded.pixels[0].empty());
    CHECK(anyContains(loaded.warnings, "La capa «Boceto» está dañada"));
    // Una imagen que dice ocupar mucho más de lo que puede ocupar la capa.
    loaded = loadWith(rewriteZip(file, [](ZipEntry& entry) {
        if (entry.name == "data/layer0.png") {
            entry.data.resize(4 << 20, 0);
            entry.method = zip::Method::Deflate;
        }
        return true;
    }));
    REQUIRE(loaded.ok);
    CHECK(loaded.pixels[0].empty());
    CHECK_EQ(loaded.warnings.size(), size_t{1});
}

TEST_CASE(project_loader_survives_random_damage) {
    // Archivos estropeados al azar (bytes cambiados o cortados en cualquier sitio): o no se
    // abren o se abren con lo que se pueda, pero nunca se cuelga ni lee fuera de su memoria.
    const std::string folder = test::tempFolder("project_random_damage");
    const SmallProject project = smallProject(folder);
    REQUIRE(!project.path.empty());
    const std::vector<uint8_t> file = readFile(project.path);
    std::mt19937 rng(99);
    int opened = 0;
    for (int round = 0; round < 300; ++round) {
        std::vector<uint8_t> damaged = file;
        if (round % 4 == 0) {
            damaged.resize(rng() % file.size());
        } else {
            const int changes = 1 + static_cast<int>(rng() % 8);
            for (int i = 0; i < changes; ++i) {
                damaged[rng() % damaged.size()] = static_cast<uint8_t>(rng());
            }
        }
        project::Loader loader;
        if (!loader.open(damaged, 0)) {
            CHECK(!loader.error().empty());
            continue;
        }
        ++opened;
        const Loaded loaded = loadLayers(loader);
        CHECK(loaded.ok);
        for (size_t i = 0; i < loaded.pixels.size(); ++i) {
            const IRect& rect = loader.document().layers[i].rect;
            CHECK(loaded.pixels[i].empty() ||
                  loaded.pixels[i].size() == static_cast<size_t>(rect.width()) * rect.height() * 4);
        }
    }
    CHECK(opened > 0);
}

TEST_CASE(project_readers_survive_random_input) {
    // El JSON de la descripción y los PNG, estropeados al azar después de las comprobaciones
    // de integridad (con el CRC recalculado): se rechazan o se leen, sin más.
    project::Document document = sampleDocument();
    document.layers = {layerInfo("Tinta", {1, 2, 30, 40}, 0), layerInfo("Fondo", {}, 1)};
    const std::string text = project::documentJson(document, "x");
    std::mt19937 rng(7);
    for (int round = 0; round < 2000; ++round) {
        std::string damaged = text;
        const int changes = 1 + static_cast<int>(rng() % 4);
        for (int i = 0; i < changes; ++i) {
            const size_t at = rng() % damaged.size();
            switch (rng() % 3) {
            case 0: damaged[at] = static_cast<char>(rng()); break;
            case 1: damaged.erase(at, 1 + rng() % 8); break;
            default: damaged.insert(at, 1, "{}[]\",:0e-.\\u"[rng() % 14]); break;
            }
        }
        project::Document read;
        std::vector<std::string> warnings;
        std::string error;
        if (project::readDocument(damaged, 4096, read, warnings, error)) {
            CHECK(!read.layers.empty());
            CHECK(read.activeLayer >= 0 && read.activeLayer < static_cast<int>(read.layers.size()));
            for (const project::LayerInfo& layer : read.layers) {
                CHECK(layer.rect.empty() || (layer.rect.x0 >= 0 && layer.rect.y0 >= 0 &&
                                             layer.rect.x1 <= read.width && layer.rect.y1 <= read.height));
                CHECK(layer.opacity >= 0.0f && layer.opacity <= 1.0f);
            }
        } else {
            CHECK(!error.empty());
        }
    }

    // Un PNG de cada tipo que puede llegar (el de LiveSketch, uno con paleta y uno con filtros
    // de otro programa), con los datos de un chunk cambiados y su CRC rehecho.
    const std::vector<uint8_t> rgba = randomRgba(24 * 16, 17);
    std::vector<std::vector<uint8_t>> files = {encodePng(rgba, 24, 16), craftPng(rgba, 24, 16, {0, 1, 2, 3, 4}, 2)};
    const uint8_t color[3] = {1, 2, 3};
    files.emplace_back();
    png::writeSolid(24, 16, color, {}, [&files](const uint8_t* data, size_t size) {
        files.back().insert(files.back().end(), data, data + size);
        return true;
    });
    for (const std::vector<uint8_t>& original : files) {
        // Dónde empieza cada chunk.
        std::vector<size_t> chunks;
        for (size_t at = 8; at + 12 <= original.size();) {
            chunks.push_back(at);
            at += 12 + ((size_t{original[at]} << 24) | (size_t{original[at + 1]} << 16) |
                        (size_t{original[at + 2]} << 8) | original[at + 3]);
        }
        for (int round = 0; round < 400; ++round) {
            std::vector<uint8_t> damaged = original;
            const size_t chunk = chunks[rng() % chunks.size()];
            const size_t length = (size_t{damaged[chunk]} << 24) | (size_t{damaged[chunk + 1]} << 16) |
                                  (size_t{damaged[chunk + 2]} << 8) | damaged[chunk + 3];
            if (length == 0) {
                continue;
            }
            const int changes = 1 + static_cast<int>(rng() % 3);
            for (int i = 0; i < changes; ++i) {
                damaged[chunk + 8 + rng() % length] = static_cast<uint8_t>(rng());
            }
            const uint32_t crc = libdeflate_crc32(0, &damaged[chunk + 4], length + 4);
            for (int i = 0; i < 4; ++i) {
                damaged[chunk + 8 + length + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (24 - 8 * i));
            }
            png::Image image;
            std::string error;
            if (png::decode(damaged, image, 24 * 16, &error)) {
                CHECK(image.width > 0 && image.height > 0);
                CHECK(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) <= size_t{24 * 16});
                CHECK_EQ(image.rgba.size(), static_cast<size_t>(image.width) * image.height * 4);
            } else {
                CHECK(!error.empty());
            }
        }
    }
}

TEST_CASE(project_saver_replaces_atomically_and_cleans_up) {
    const std::string folder = test::tempFolder("project_saver_files");
    project::Document document = sampleDocument();
    document.width = 16;
    document.height = 16;
    document.layers = {layerInfo("Única", {0, 0, 16, 16}, 0)};
    const std::vector<std::vector<uint8_t>> pixels = {randomPremultiplied(256, 15)};

    // En una carpeta: un archivo nuevo cada vez, sin pisar ninguno.
    project::Target target;
    target.folder = folder;
    target.stem = "Mi dibujo";
    project::Saver::Result first = saveDocument(document, pixels, target);
    project::Saver::Result second = saveDocument(document, pixels, target);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    CHECK_EQ(first.path, folder + "Mi dibujo.lvskt");
    CHECK_EQ(second.path, folder + "Mi dibujo (2).lvskt");
    CHECK_EQ(first.bytes, uint64_t{readFile(first.path).size()});

    // Sustituir uno: se escribe al lado y se cambia de nombre al terminar.
    project::Target replace;
    replace.path = folder + "Mi dibujo.lvskt";
    writeFile(replace.path, {'v', 'i', 'e', 'j', 'o'});
    project::Saver::Result replaced = saveDocument(document, pixels, replace);
    REQUIRE(replaced.ok);
    CHECK_EQ(replaced.path, replace.path);
    CHECK(readFile(replace.path).size() > 100);
    CHECK(filesIn(folder) == (std::vector<std::string>{"Mi dibujo (2).lvskt", "Mi dibujo.lvskt"}));
    project::Loader loader;
    CHECK(loader.open(replace.path, 0));

    // En una carpeta que no existe: falla sin dejar nada.
    project::Target nowhere;
    nowhere.path = folder + "no existe/dibujo.lvskt";
    project::Saver::Result failed = saveDocument(document, pixels, nowhere);
    CHECK(!failed.ok);
    CHECK(!failed.error.empty());
    nowhere = {};
    nowhere.folder = folder + "tampoco/";
    nowhere.stem = "dibujo";
    CHECK(!saveDocument(document, pixels, nowhere).ok);

    // Si falla a mitad (no se pudo leer la GPU), no queda nada: tampoco se toca el que había.
    const std::vector<uint8_t> before = readFile(replace.path);
    {
        project::Saver saver;
        REQUIRE(saver.begin(document, replace, "x"));
        CHECK(saver.busy());
        CHECK(!saver.begin(document, replace, "x"));   // uno cada vez
        std::atomic<int> finished{0};
        saver.abort("no se pudo leer el dibujo de la memoria de gráficos", [&finished] { ++finished; });
        saver.wait();
        CHECK(!saver.busy());
        CHECK_EQ(finished.load(), 1);
        const std::optional<project::Saver::Result> result = saver.takeResult();
        REQUIRE(result.has_value());
        CHECK(!result->ok);
        CHECK_EQ(result->error, std::string("no se pudo leer el dibujo de la memoria de gráficos"));
        CHECK(!saver.takeResult().has_value());   // se entrega una vez
    }
    CHECK(readFile(replace.path) == before);
    CHECK(filesIn(folder) == (std::vector<std::string>{"Mi dibujo (2).lvskt", "Mi dibujo.lvskt"}));

    // Si se cierra la app a mitad de guardar, tampoco.
    {
        project::Target third;
        third.folder = folder;
        third.stem = "A medias";
        project::Saver saver;
        REQUIRE(saver.begin(document, third, "x"));
        saver.addLayer(0, pixels[0]);
    }
    CHECK(filesIn(folder) == (std::vector<std::string>{"Mi dibujo (2).lvskt", "Mi dibujo.lvskt"}));
}

TEST_CASE(project_round_trip_keeps_layers_exact) {
    for (const ColorProfile profile : {ColorProfile::Srgb, ColorProfile::DisplayP3}) {
        CanvasSpec spec;
        spec.width = 120;
        spec.height = 80;
        spec.ppi = 300.0f;
        spec.unit = LengthUnit::Centimeters;
        spec.profile = profile;
        spec.background.color[0] = 0.9f;
        spec.background.color[1] = 0.85f;
        spec.background.color[2] = 0.7f;
        spec.name = "Retrato & <prueba> \"1\" ñ";
        Canvas canvas;
        REQUIRE(canvas.init(spec));

        // Capa 1: un trazo (con bordes suaves: muchos alfas distintos).
        setBrush(canvas, 0.8f, 0.2f, 0.1f, 0.7f, 9.0f);
        drawLine(canvas, {10.0f, 12.0f}, {100.0f, 60.0f});
        // Capa 2: píxeles al azar en una parte.
        REQUIRE(canvas.addLayer());
        const IRect randomRect{20, 10, 70, 50};
        const std::vector<uint8_t> random = randomPremultiplied(50 * 40, 16);
        REQUIRE(canvas.setLayerPixels(1, randomRect, random.data()));
        // Capa 3: un rectángulo opaco.
        REQUIRE(canvas.addLayer());
        const IRect boxRect{5, 40, 45, 75};
        test::fillRect(canvas.layers().at(2).target, boxRect, 0.0f, 0.5f, 0.25f, 1.0f);
        canvas.layers().markDirty(boxRect);
        // Capa 4: vacía.
        REQUIRE(canvas.addLayer());
        canvas.renameLayer(1, "Tinta & <sombras> \"1\"");
        canvas.setLayerOpacity(1, 0.37f);
        canvas.setLayerBlend(1, BlendMode::Multiply);
        canvas.setLayerAlphaLock(2, true);
        canvas.setLayerClipping(2, true);
        canvas.setLayerVisible(3, false);
        canvas.setReferenceLayer(3);
        canvas.selectLayer(1);
        DrawingGuide guide = canvas.guide();
        guide.enabled = true;
        guide.kind = GuideKind::Symmetry;
        guide.center = {33.5f, 40.25f};
        guide.angle = 0.4f;
        guide.symmetry = SymmetryKind::Radial;
        guide.rotational = true;
        guide.gridSize = 64.0f;
        guide.opacity = 0.35f;
        canvas.setGuide(guide);
        canvas.update();

        std::vector<std::vector<uint8_t>> layersBefore;
        for (int i = 0; i < canvas.layers().count(); ++i) {
            layersBefore.push_back(test::readTarget(canvas.layers().at(i).target));
        }
        const std::vector<uint8_t> compositeBefore = test::readTarget(canvas.composite());
        const CanvasInfo infoBefore = canvas.info();
        const CanvasBackground backgroundBefore = canvas.background();
        CHECK(canvas.layerContent(1) == randomRect);
        CHECK(canvas.layerContent(2) == boxRect);
        CHECK(canvas.layerContent(3).empty());

        const std::string folder = test::tempFolder("project_round_trip");
        project::Target target;
        target.folder = folder;
        target.stem = "dibujo";
        project::ViewInfo view;
        view.fitted = false;
        view.zoom = 2.5f;
        view.center = {30.0f, 20.0f};
        view.angle = -0.5f;
        view.flipped = true;
        const project::Saver::Result saved = saveCanvas(canvas, target, view, [&canvas] {
            // Se sigue dibujando mientras se guarda: el archivo es lo de antes.
            setBrush(canvas, 0.0f, 0.0f, 1.0f, 1.0f, 12.0f);
            drawLine(canvas, {5.0f, 70.0f}, {110.0f, 5.0f});
        });
        REQUIRE(saved.ok);
        CHECK_EQ(saved.path, folder + "dibujo.lvskt");
        CHECK_EQ(saved.bytes, uint64_t{readFile(saved.path).size()});
        CHECK(test::maxDifference(test::readTarget(canvas.layers().at(1).target), layersBefore[1]) > 0);

        project::Loader loader;
        REQUIRE(loader.open(saved.path, 0));
        const project::Document& document = loader.document();
        CHECK(!document.view.fitted);
        CHECK_EQ(document.view.zoom, 2.5f);
        CHECK(document.view.center == view.center);
        CHECK_EQ(document.view.angle, -0.5f);
        CHECK(document.view.flipped);
        Canvas opened;
        const std::optional<std::vector<std::string>> warnings = openCanvas(loader, opened);
        REQUIRE(warnings.has_value());
        CHECK(warnings->empty());

        // Las capas, idénticas píxel a píxel, con sus propiedades.
        REQUIRE(opened.layers().count() == 4);
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(test::maxDifference(test::readTarget(opened.layers().at(i).target),
                                         layersBefore[static_cast<size_t>(i)]),
                     0);
        }
        const LayerStack& layers = opened.layers();
        CHECK_EQ(layers.at(0).name, std::string("Capa 1"));
        CHECK_EQ(layers.at(1).name, std::string("Tinta & <sombras> \"1\""));
        CHECK_EQ(layers.at(1).opacity, 0.37f);
        CHECK(layers.at(1).blend == BlendMode::Multiply);
        CHECK(layers.at(2).alphaLock);
        CHECK(layers.at(2).clipping);
        CHECK(!layers.at(3).visible);
        CHECK_EQ(layers.referenceIndex(), 3);
        CHECK_EQ(layers.activeIndex(), 1);
        CHECK(opened.guide() == guide);
        CHECK(opened.background() == backgroundBefore);
        // Y el dibujo se ve igual.
        CHECK_EQ(test::maxDifference(test::readTarget(opened.composite()), compositeBefore), 0);

        // Los datos del lienzo (las fechas, al segundo).
        const CanvasInfo& info = opened.info();
        CHECK_EQ(info.name, spec.name);
        CHECK_EQ(info.ppi, 300.0f);
        CHECK(info.unit == LengthUnit::Centimeters);
        CHECK(info.profile == profile);
        CHECK_EQ(info.created / 1000000000, infoBefore.created / 1000000000);
        CHECK_EQ(info.modified / 1000000000, infoBefore.modified / 1000000000);
        CHECK_EQ(info.drawingSeconds, infoBefore.drawingSeconds);
        CHECK_EQ(info.strokes, infoBefore.strokes);
        CHECK_EQ(info.strokes, uint64_t{1});

        // El dibujo entero que ven los demás programas es el compuesto, sin premultiplicar.
        const std::vector<ZipEntry> entries = unzip(readFile(saved.path));
        const ZipEntry* merged = findEntry(entries, project::kMergedEntry);
        REQUIRE(merged != nullptr);
        png::Image image;
        REQUIRE(png::decode(merged->data, image, 120 * 80));
        std::vector<uint8_t> expected = compositeBefore;
        gfx::unpremultiply(expected.data(), expected.size() / 4);
        CHECK_EQ(test::maxDifference(image.rgba, expected), 0);
    }
}

TEST_CASE(project_round_trip_without_background) {
    // Con el fondo oculto, lo que no está pintado queda transparente también al abrirlo.
    Canvas canvas;
    REQUIRE(canvas.init(64, 64));
    test::hideBackground(canvas);
    setBrush(canvas, 0.2f, 0.6f, 0.9f, 0.5f, 6.0f);
    drawLine(canvas, {8.0f, 8.0f}, {56.0f, 50.0f});
    const std::vector<uint8_t> before = test::readTarget(canvas.composite());

    project::Target target;
    target.path = test::tempFolder("project_no_background") + "transparente.lvskt";
    const project::Saver::Result saved = saveCanvas(canvas, target, {});
    REQUIRE(saved.ok);
    project::Loader loader;
    REQUIRE(loader.open(saved.path, 0));
    CHECK(loader.document().view.fitted);
    Canvas opened;
    REQUIRE(openCanvas(loader, opened).has_value());
    CHECK(!opened.background().visible);
    CHECK_EQ(test::maxDifference(test::readTarget(opened.composite()), before), 0);
    CHECK(test::pixelAt(test::readTarget(opened.composite()), 64, 60, 2) == (test::Pixel{0, 0, 0, 0}));
    // Abrir es otro documento: su versión es otra (así no se da por guardado lo que no lo está).
    const uint64_t version = canvas.documentVersion();
    project::Loader again;
    REQUIRE(again.open(saved.path, 0));
    REQUIRE(openCanvas(again, canvas).has_value());
    CHECK(canvas.documentVersion() > version);
}

// Las entradas de un archivo que no son document.json ni stack.xml, por nombre.
std::vector<uint8_t> entryData(const std::vector<ZipEntry>& entries, std::string_view name) {
    const ZipEntry* entry = findEntry(entries, name);
    return entry ? entry->data : std::vector<uint8_t>();
}

// Las capas del lienzo, enteras.
std::vector<std::vector<uint8_t>> layerPixels(const Canvas& canvas) {
    std::vector<std::vector<uint8_t>> pixels;
    for (int i = 0; i < canvas.layers().count(); ++i) {
        pixels.push_back(test::readTarget(canvas.layers().at(i).target));
    }
    return pixels;
}

// Abre `path` en un lienzo nuevo y comprueba que sus capas y su dibujo son los de `canvas`.
void checkSameAsFile(const Canvas& canvas, const std::string& path) {
    project::Loader loader;
    REQUIRE(loader.open(path, 0));
    Canvas opened;
    const std::optional<std::vector<std::string>> warnings = openCanvas(loader, opened);
    REQUIRE(warnings.has_value());
    CHECK(warnings->empty());
    REQUIRE(opened.layers().count() == canvas.layers().count());
    for (int i = 0; i < canvas.layers().count(); ++i) {
        CHECK_EQ(test::maxDifference(test::readTarget(opened.layers().at(i).target),
                                     test::readTarget(canvas.layers().at(i).target)),
                 0);
        CHECK_EQ(opened.layers().at(i).name, canvas.layers().at(i).name);
        CHECK_EQ(opened.layers().at(i).opacity, canvas.layers().at(i).opacity);
    }
    CHECK_EQ(test::maxDifference(test::readTarget(opened.composite()), test::readTarget(canvas.composite())), 0);
    // El dibujo entero que ven los demás programas también es el de ahora.
    const std::vector<ZipEntry> entries = unzip(readFile(path));
    png::Image image;
    REQUIRE(png::decode(entryData(entries, project::kMergedEntry), image,
                        static_cast<size_t>(canvas.width()) * static_cast<size_t>(canvas.height())));
    std::vector<uint8_t> expected = test::readTarget(canvas.composite());
    gfx::unpremultiply(expected.data(), expected.size() / 4);
    CHECK_EQ(test::maxDifference(image.rgba, expected), 0);
}

// Un lienzo de 160 × 120 con cuatro capas: un trazo, píxeles al azar, un rectángulo y una
// vacía.
bool fourLayers(Canvas& canvas) {
    if (!canvas.init(160, 120)) {
        return false;
    }
    setBrush(canvas, 0.8f, 0.2f, 0.1f, 0.7f, 9.0f);
    drawLine(canvas, {10.0f, 12.0f}, {140.0f, 90.0f});
    if (!canvas.addLayer()) {
        return false;
    }
    const std::vector<uint8_t> random = randomPremultiplied(60 * 50, 31);
    if (!canvas.setLayerPixels(1, {30, 20, 90, 70}, random.data())) {
        return false;
    }
    if (!canvas.addLayer()) {
        return false;
    }
    test::fillRect(canvas.layers().at(2).target, {5, 60, 65, 115}, 0.0f, 0.5f, 0.25f, 1.0f);
    ++canvas.layers().at(2).revision;
    canvas.layers().markDirty({5, 60, 65, 115});
    if (!canvas.addLayer()) {
        return false;
    }
    canvas.update();
    return true;
}

TEST_CASE(project_save_copies_what_did_not_change) {
    Canvas canvas;
    REQUIRE(fourLayers(canvas));
    project::Target target;
    target.path = test::tempFolder("project_incremental") + "dibujo.lvskt";

    // La primera vez se comprime todo (la capa vacía no hace falta leerla).
    canvasproject::Pending pending;
    canvasproject::Record record;
    project::Saver::Result saved = saveCanvas(canvas, target, {}, {}, {}, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.encodedLayers, 3);
    CHECK_EQ(pending.copiedLayers, 0);
    CHECK(!pending.copiedComposite);
    CHECK(!saved.reused);
    CHECK_EQ(record.path, target.path);
    CHECK_EQ(record.layers.size(), size_t{4});
    CHECK(record.composite != 0);
    CHECK_EQ(record.merged.name, std::string(project::kMergedEntry));
    const std::vector<ZipEntry> first = unzip(readFile(saved.path));
    REQUIRE(!first.empty());

    // Sin cambios, todo se copia tal cual.
    saved = saveCanvas(canvas, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.encodedLayers, 0);
    CHECK_EQ(pending.copiedLayers, 3);
    CHECK(pending.copiedComposite);
    CHECK(saved.reused);
    const std::vector<ZipEntry> second = unzip(readFile(saved.path));
    for (int i = 0; i < 4; ++i) {
        CHECK(entryData(second, project::layerEntry(i)) == entryData(first, project::layerEntry(i)));
    }
    CHECK(entryData(second, project::kMergedEntry) == entryData(first, project::kMergedEntry));
    CHECK(entryData(second, project::kThumbnailEntry) == entryData(first, project::kThumbnailEntry));
    checkSameAsFile(canvas, saved.path);

    // Con un trazo en la capa 2, solo se comprime esa (y el dibujo entero).
    canvas.selectLayer(1);
    setBrush(canvas, 0.1f, 0.3f, 0.9f, 1.0f, 6.0f);
    drawLine(canvas, {20.0f, 100.0f}, {150.0f, 10.0f});
    saved = saveCanvas(canvas, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.encodedLayers, 1);
    CHECK_EQ(pending.copiedLayers, 2);
    CHECK(!pending.copiedComposite);
    const std::vector<ZipEntry> third = unzip(readFile(saved.path));
    CHECK(entryData(third, project::layerEntry(0)) == entryData(first, project::layerEntry(0)));
    CHECK(entryData(third, project::layerEntry(1)) != entryData(first, project::layerEntry(1)));
    CHECK(entryData(third, project::layerEntry(2)) == entryData(first, project::layerEntry(2)));
    checkSameAsFile(canvas, saved.path);

    // Mover, borrar, añadir y cambiar propiedades no vuelve a comprimir ninguna capa: las
    // copias van con su nombre nuevo.
    REQUIRE(canvas.moveLayer(0, 3));
    REQUIRE(canvas.removeLayer(1));
    REQUIRE(canvas.addLayer());
    canvas.setLayerOpacity(2, 0.5f);
    canvas.renameLayer(0, "Fondo de color");
    canvas.update();
    saved = saveCanvas(canvas, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.encodedLayers, 0);
    CHECK_EQ(pending.copiedLayers, 2);
    CHECK(!pending.copiedComposite);
    checkSameAsFile(canvas, saved.path);

    // El nombre del lienzo solo va en document.json; los ppp, también en el PNG del dibujo
    // entero (las capas no cambian).
    canvas.setName("Otro nombre");
    saved = saveCanvas(canvas, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.encodedLayers, 0);
    CHECK(pending.copiedComposite);
    CHECK(contains(textOf(entryData(unzip(readFile(saved.path)), project::kDocumentEntry)), "Otro nombre"));
    canvas.setPpi(150.0f);
    saved = saveCanvas(canvas, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK(!pending.copiedComposite);
    checkSameAsFile(canvas, saved.path);

    // Deshacer también es un cambio de píxeles: se vuelve a comprimir esa capa.
    REQUIRE(canvas.undo());
    REQUIRE(canvas.undo());
    canvas.update();
    saved = saveCanvas(canvas, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    checkSameAsFile(canvas, saved.path);
}

TEST_CASE(project_save_after_open_copies_layers) {
    Canvas canvas;
    REQUIRE(fourLayers(canvas));
    project::Target target;
    target.path = test::tempFolder("project_incremental_open") + "abierto.lvskt";
    REQUIRE(saveCanvas(canvas, target, {}).ok);

    // Recién abierto, el archivo ya tiene todo lo del lienzo.
    project::Loader loader;
    REQUIRE(loader.open(target.path, 0));
    Canvas opened;
    canvasproject::Record record;
    REQUIRE(openCanvas(loader, opened, &record, target.path).has_value());
    CHECK_EQ(record.path, target.path);
    CHECK_EQ(record.layers.size(), size_t{4});
    CHECK(record.composite == canvasproject::compositeKey(opened));
    canvasproject::Pending pending;
    project::Saver::Result saved = saveCanvas(opened, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.encodedLayers, 0);
    CHECK_EQ(pending.copiedLayers, 3);
    CHECK(pending.copiedComposite);
    checkSameAsFile(canvas, saved.path);

    // Al pintar en una, solo se comprime esa.
    opened.selectLayer(3);
    setBrush(opened, 0.9f, 0.9f, 0.1f, 1.0f, 5.0f);
    drawLine(opened, {100.0f, 100.0f}, {20.0f, 30.0f});
    saved = saveCanvas(opened, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.encodedLayers, 1);
    CHECK_EQ(pending.copiedLayers, 3);
    checkSameAsFile(opened, saved.path);
}

TEST_CASE(project_open_records_only_layers_that_loaded) {
    // Una capa dañada se abre vacía: no se copia su PNG roto, se guarda como está (vacía), y
    // el dibujo entero del archivo ya no vale.
    Canvas canvas;
    REQUIRE(fourLayers(canvas));
    const std::string folder = test::tempFolder("project_incremental_damaged");
    project::Target target;
    target.path = folder + "dañado.lvskt";
    REQUIRE(saveCanvas(canvas, target, {}).ok);
    const std::vector<uint8_t> broken = rewriteZip(readFile(target.path), [](ZipEntry& entry) {
        if (entry.name == project::layerEntry(1)) {
            entry.data.resize(entry.data.size() / 2);
        }
        if (entry.name != "mimetype") {
            entry.method = zip::Method::Stored;
        }
        return true;
    });
    REQUIRE(!broken.empty());
    writeFile(target.path, broken);

    project::Loader loader;
    REQUIRE(loader.open(target.path, 0));
    Canvas opened;
    canvasproject::Record record;
    const std::optional<std::vector<std::string>> warnings = openCanvas(loader, opened, &record, target.path);
    REQUIRE(warnings.has_value());
    CHECK_EQ(warnings->size(), size_t{1});
    CHECK_EQ(record.layers.size(), size_t{3});
    CHECK(record.find(opened.layers().at(1).id) == nullptr);
    CHECK_EQ(record.composite, uint64_t{0});

    canvasproject::Pending pending;
    const project::Saver::Result saved = saveCanvas(opened, target, {}, {}, record, &pending, &record);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.copiedLayers, 2);
    CHECK(!pending.copiedComposite);
    checkSameAsFile(opened, saved.path);
    project::Loader again;
    REQUIRE(again.open(saved.path, 0));
    CHECK(again.document().layers[1].rect.empty());
}

TEST_CASE(project_save_refuses_a_changed_previous_file) {
    Canvas canvas;
    REQUIRE(fourLayers(canvas));
    const std::string folder = test::tempFolder("project_incremental_changed");
    project::Target target;
    target.path = folder + "dibujo.lvskt";
    canvasproject::Record record;
    REQUIRE(saveCanvas(canvas, target, {}, {}, {}, nullptr, &record).ok);

    // Otro proyecto ocupa su sitio: lo que dice el registro ya no está, no se copia nada de
    // él y el archivo se queda como estaba.
    Canvas other;
    REQUIRE(other.init(160, 120));
    setBrush(other, 0.2f, 0.9f, 0.2f, 1.0f, 8.0f);
    drawLine(other, {0.0f, 0.0f}, {160.0f, 120.0f});
    REQUIRE(other.addLayer());
    REQUIRE(other.addLayer());
    test::fillRect(other.layers().at(2).target, {0, 0, 160, 120}, 0.5f, 0.0f, 0.0f, 0.5f);
    ++other.layers().at(2).revision;
    other.layers().markDirty({0, 0, 160, 120});
    other.update();
    REQUIRE(saveCanvas(other, target, {}).ok);
    const std::vector<uint8_t> otherFile = readFile(target.path);
    project::Saver::Result saved = saveCanvas(canvas, target, {}, {}, record);
    CHECK(!saved.ok);
    CHECK(saved.reused);
    CHECK(contains(saved.error, "cambió"));
    CHECK(readFile(target.path) == otherFile);
    CHECK(filesIn(folder) == std::vector<std::string>{"dibujo.lvskt"});

    // Guardándolo todo, sale bien.
    saved = saveCanvas(canvas, target, {}, {}, {}, nullptr, &record);
    REQUIRE(saved.ok);
    checkSameAsFile(canvas, saved.path);

    // Una capa que se estropeó en el disco (su CRC ya no cuadra) tampoco se copia.
    std::vector<uint8_t> file = readFile(target.path);
    {
        zip::MemorySource source(file);
        zip::Reader reader;
        REQUIRE(reader.open(source));
        const zip::Entry* entry = reader.find(project::layerEntry(2));
        REQUIRE(entry != nullptr);
        const size_t at = static_cast<size_t>(entry->headerOffset) + 30 +
                          get16le(&file[static_cast<size_t>(entry->headerOffset) + 26]) + 40;
        file[at] ^= 0x5a;
    }
    writeFile(target.path, file);
    saved = saveCanvas(canvas, target, {}, {}, record);
    CHECK(!saved.ok);
    CHECK(saved.reused);
    CHECK(readFile(target.path) == file);
    CHECK(filesIn(folder) == std::vector<std::string>{"dibujo.lvskt"});

    // Si no se puede leer el anterior, se guarda entero sin más.
    std::filesystem::remove(target.path);
    canvasproject::Pending pending;
    saved = saveCanvas(canvas, target, {}, {}, record, &pending);
    REQUIRE(saved.ok);
    CHECK_EQ(pending.copiedLayers, 0);
    CHECK_EQ(pending.encodedLayers, 3);
    checkSameAsFile(canvas, saved.path);
}

TEST_CASE(project_saver_copies_entries_without_gpu) {
    // El Saver solo: copia entradas de otro archivo a uno nuevo, con su nombre nuevo.
    const std::string folder = test::tempFolder("project_saver_copies");
    const SmallProject project = smallProject(folder);
    REQUIRE(!project.path.empty());
    std::vector<uint8_t> original = readFile(project.path);
    zip::MemorySource source(original);
    zip::Reader reader;
    REQUIRE(reader.open(source));
    const zip::Entry boceto = *reader.find(project::layerEntry(0));
    const zip::Entry tinta = *reader.find(project::layerEntry(2));
    const zip::Entry thumbnail = *reader.find(project::kThumbnailEntry);
    const zip::Entry merged = *reader.find(project::kMergedEntry);

    // Las capas al revés: la de arriba pasa abajo.
    project::Document document = project.document;
    std::swap(document.layers[0], document.layers[2]);
    project::Saver saver;
    project::Target target;
    target.folder = folder;
    target.stem = "copia";
    REQUIRE(saver.begin(document, target, "x", project.path));
    CHECK(saver.reusing());
    saver.reuseLayer(0, tinta);
    saver.addLayer(1, {});
    saver.reuseLayer(2, boceto);
    std::atomic<int> finished{0};
    saver.finishReusing(thumbnail, merged, [&finished] { ++finished; });
    saver.wait();
    CHECK_EQ(finished.load(), 1);
    std::optional<project::Saver::Result> result = saver.takeResult();
    REQUIRE(result && result->ok);
    CHECK(result->reused);
    CHECK_EQ(result->path, folder + "copia.lvskt");
    REQUIRE(result->entries.size() == size_t{9});
    CHECK_EQ(result->entries.front().name, std::string("mimetype"));
    CHECK_EQ(result->entries.back().name, std::string(project::kDocumentEntry));
    const std::vector<ZipEntry> before = unzip(original);
    const std::vector<ZipEntry> after = unzip(readFile(result->path));
    CHECK(entryData(after, project::layerEntry(0)) == entryData(before, project::layerEntry(2)));
    CHECK(entryData(after, project::layerEntry(2)) == entryData(before, project::layerEntry(0)));
    CHECK(entryData(after, project::kMergedEntry) == entryData(before, project::kMergedEntry));
    for (const zip::Entry& entry : result->entries) {
        const ZipEntry* written = findEntry(after, entry.name);
        REQUIRE(written != nullptr);
        CHECK_EQ(entry.crc, libdeflate_crc32(0, written->data.data(), written->data.size()));
        CHECK_EQ(entry.size, uint64_t{written->data.size()});
    }
    project::Loader loader;
    REQUIRE(loader.open(result->path, 0));
    const Loaded loaded = loadLayers(loader);
    REQUIRE(loaded.ok);
    CHECK(loaded.warnings.empty());
    CHECK(loaded.pixels[0] == project.pixels[2]);
    CHECK(loaded.pixels[2] == project.pixels[0]);

    // Una entrada que el archivo ya no tiene así: falla y no deja nada.
    zip::Entry wrong = boceto;
    wrong.crc ^= 1;
    target.stem = "mala";
    REQUIRE(saver.begin(document, target, "x", project.path));
    saver.reuseLayer(0, tinta);
    saver.addLayer(1, {});
    saver.reuseLayer(2, wrong);
    saver.finishReusing(thumbnail, merged, {});
    saver.wait();
    result = saver.takeResult();
    REQUIRE(result.has_value());
    CHECK(!result->ok);
    CHECK(result->reused);
    CHECK(result->entries.empty());
    CHECK(!std::filesystem::exists(folder + "mala.lvskt"));

    // Sin archivo anterior que leer, no se puede copiar.
    target.stem = "sin";
    REQUIRE(saver.begin(document, target, "x", folder + "no-existe.lvskt"));
    CHECK(!saver.reusing());
    saver.abort("prueba", {});
    saver.wait();
    CHECK(!std::filesystem::exists(folder + "sin.lvskt"));

    // Una capa que no se dio: el guardado termina (con error) en vez de esperar para siempre.
    target.stem = "incompleto";
    REQUIRE(saver.begin(document, target, "x"));
    saver.addLayer(0, project.pixels[2]);
    saver.finish(std::vector<uint8_t>(static_cast<size_t>(64 * 48 * 4), 255), {});
    saver.wait();
    result = saver.takeResult();
    REQUIRE(result.has_value());
    CHECK(!result->ok);
    CHECK(!std::filesystem::exists(folder + "incompleto.lvskt"));
}

TEST_CASE(camera_placement_survives_another_window) {
    // La vista que se guarda (zoom relativo al ajuste, el punto del centro y el giro) vuelve
    // igual en una ventana de otro tamaño.
    Camera camera;
    camera.setViewport({1280.0f, 800.0f});
    camera.setCanvasSize({1920.0f, 1080.0f});
    camera.zoomAt({400.0f, 300.0f}, 2.0f);
    camera.pan({-120.0f, 45.0f});
    camera.rotateAt({640.0f, 400.0f}, 0.6f);
    const Camera::Placement placement = camera.placement();
    CHECK(placement.zoom > 1.5f);
    CHECK_NEAR(placement.angle, 0.6f, 1e-5f);

    Camera other;
    other.setViewport({800.0f, 1280.0f});
    other.setInsets(60.0f, 0.0f, 90.0f, 0.0f);
    other.setCanvasSize({1920.0f, 1080.0f});
    CHECK(!other.userMoved());
    other.setPlacement(placement);
    CHECK(other.userMoved());
    const Camera::Placement back = other.placement();
    CHECK_NEAR(back.zoom, placement.zoom, 1e-4f);
    CHECK_NEAR(back.center.x, placement.center.x, 0.05f);
    CHECK_NEAR(back.center.y, placement.center.y, 0.05f);
    CHECK_NEAR(back.angle, placement.angle, 1e-5f);
    // El punto guardado queda en el centro de la zona libre (entre las barras).
    const glm::vec2 center = other.canvasToScreen(placement.center);
    CHECK_NEAR(center.x, 400.0f, 0.05f);
    CHECK_NEAR(center.y, 60.0f + (1280.0f - 150.0f) * 0.5f, 0.05f);

    // Lo que no tiene sentido no cambia nada.
    const glm::vec2 offset = other.offset();
    other.setPlacement({std::nanf(""), {0.0f, 0.0f}, 0.0f});
    other.setPlacement({0.0f, {0.0f, 0.0f}, 0.0f});
    other.setPlacement({1.0f, {std::numeric_limits<float>::infinity(), 0.0f}, 0.0f});
    CHECK(other.offset() == offset);
    // Un zoom imposible se queda en el límite.
    other.setPlacement({1e6f, {960.0f, 540.0f}, 0.0f});
    CHECK_NEAR(other.zoom(), other.maxZoom(), 1e-3f);
}
