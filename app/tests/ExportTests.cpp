#include "Test.h"

#include "Gfx/Pixels.h"
#include "IO/ImageExport.h"
#include "IO/Png.h"
#include "ThirdParty/stb_image.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <libdeflate.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>

namespace {

// Lee un PNG como RGBA8. Vacío si no se puede.
std::vector<uint8_t> loadPng(const std::string& path, int* width, int* height) {
    int channels = 0;
    stbi_uc* data = stbi_load(path.c_str(), width, height, &channels, STBI_rgb_alpha);
    if (!data) {
        return {};
    }
    std::vector<uint8_t> pixels(data, data + static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4);
    stbi_image_free(data);
    return pixels;
}

std::vector<uint8_t> decodePng(const std::vector<uint8_t>& file, int* width, int* height) {
    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), width, height, &channels,
                                          STBI_rgb_alpha);
    if (!data) {
        return {};
    }
    std::vector<uint8_t> pixels(data, data + static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4);
    stbi_image_free(data);
    return pixels;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Un PNG en memoria.
std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height, size_t stride, const png::Info& info) {
    std::vector<uint8_t> file;
    const bool ok = png::encode(rgba, width, height, stride, info, [&file](const uint8_t* data, size_t size) {
        file.insert(file.end(), data, data + size);
        return true;
    });
    return ok ? file : std::vector<uint8_t>();
}

uint32_t get32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

struct Chunk {
    std::string type;
    std::vector<uint8_t> data;
};

// Los chunks de un PNG, comprobando la firma y el CRC de cada uno. Vacío si algo falla.
std::vector<Chunk> readChunks(const std::vector<uint8_t>& file) {
    static constexpr uint8_t kSignature[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    if (file.size() < 8 || std::memcmp(file.data(), kSignature, 8) != 0) {
        return {};
    }
    std::vector<Chunk> chunks;
    size_t at = 8;
    while (at + 12 <= file.size()) {
        const uint32_t size = get32(file.data() + at);
        if (at + 12 + size > file.size()) {
            return {};
        }
        const uint8_t* type = file.data() + at + 4;
        const uint32_t crc = libdeflate_crc32(libdeflate_crc32(0, type, 4), type + 4, size);
        if (crc != get32(type + 4 + size)) {
            return {};
        }
        chunks.push_back({std::string(reinterpret_cast<const char*>(type), 4), {type + 4, type + 4 + size}});
        at += 12 + size;
    }
    return at == file.size() ? chunks : std::vector<Chunk>();
}

const Chunk* findChunk(const std::vector<Chunk>& chunks, const char* type) {
    for (const Chunk& chunk : chunks) {
        if (chunk.type == type) {
            return &chunk;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE(unpremultiply_values) {
    uint8_t pixels[] = {
        64, 32, 0, 128,       // mitad de transparente
        10, 10, 10, 0,        // transparente: queda en negro
        200, 100, 50, 255,    // opaco: no cambia
        1, 0, 0, 1,
    };
    gfx::unpremultiply(pixels, 4);
    const uint8_t expected[] = {128, 64, 0, 128, 0, 0, 0, 0, 200, 100, 50, 255, 255, 0, 0, 1};
    CHECK(std::memcmp(pixels, expected, sizeof(expected)) == 0);
}

TEST_CASE(png_roundtrip_orientation_and_alpha) {
    // 3 filas: roja opaca arriba, verde a medias, transparente abajo.
    const int width = 4;
    const int height = 3;
    std::vector<uint8_t> rgba;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t row[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 0, 0}};
            rgba.insert(rgba.end(), row[y], row[y] + 4);
        }
    }
    const std::string path = test::tempFolder("png_roundtrip") + "image.png";
    REQUIRE(io::writePng(path, rgba.data(), width, height));

    int w = 0;
    int h = 0;
    const std::vector<uint8_t> loaded = loadPng(path, &w, &h);
    CHECK_EQ(w, width);
    CHECK_EQ(h, height);
    CHECK(loaded == rgba);
}

TEST_CASE(png_write_failure_cleans_up) {
    const std::string path = test::tempFolder("png_failure") + "no/existe/image.png";
    const uint8_t pixel[4] = {1, 2, 3, 4};
    SDL_ClearError();
    CHECK(!io::writePng(path, pixel, 1, 1));
    CHECK(std::strlen(SDL_GetError()) > 0);
    CHECK(!std::filesystem::exists(path));
}

TEST_CASE(png_carries_resolution_profile_and_name) {
    const uint8_t pixels[8] = {255, 0, 0, 255, 0, 0, 255, 128};
    png::Info info;
    info.ppi = 300.0f;
    info.title = "Retrato de Ñandú";
    const std::vector<uint8_t> file = encodePng(pixels, 2, 1, 8, info);
    const std::vector<Chunk> chunks = readChunks(file);
    REQUIRE(!chunks.empty());
    CHECK_EQ(chunks.front().type, std::string("IHDR"));
    CHECK_EQ(chunks.back().type, std::string("IEND"));

    const Chunk* header = findChunk(chunks, "IHDR");
    REQUIRE(header && header->data.size() == 13);
    CHECK_EQ(get32(header->data.data()), 2u);
    CHECK_EQ(get32(header->data.data() + 4), 1u);
    CHECK_EQ(int{header->data[8]}, 8);   // 8 bits
    CHECK_EQ(int{header->data[9]}, 6);   // RGBA

    // 300 ppp = 11811 píxeles por metro, en metros (unidad 1).
    const Chunk* physical = findChunk(chunks, "pHYs");
    REQUIRE(physical && physical->data.size() == 9);
    CHECK_EQ(get32(physical->data.data()), 11811u);
    CHECK_EQ(get32(physical->data.data() + 4), 11811u);
    CHECK_EQ(int{physical->data[8]}, 1);

    const Chunk* srgb = findChunk(chunks, "sRGB");
    REQUIRE(srgb && srgb->data.size() == 1);
    CHECK_EQ(int{srgb->data[0]}, 0);
    const Chunk* gamma = findChunk(chunks, "gAMA");
    REQUIRE(gamma && gamma->data.size() == 4);
    CHECK_EQ(get32(gamma->data.data()), 45455u);
    CHECK(findChunk(chunks, "cHRM") != nullptr);

    const Chunk* software = findChunk(chunks, "tEXt");
    REQUIRE(software);
    CHECK_EQ(std::string(software->data.begin(), software->data.end()), std::string("Software\0LiveSketch", 19));
    const Chunk* title = findChunk(chunks, "iTXt");
    REQUIRE(title);
    const std::string expected = std::string("Title\0\0\0\0\0", 10) + info.title;
    CHECK_EQ(std::string(title->data.begin(), title->data.end()), expected);

    // Los chunks del color y de la resolución van antes de los píxeles.
    size_t firstData = chunks.size();
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i].type == "IDAT" && firstData == chunks.size()) {
            firstData = i;
        }
        if (chunks[i].type == "sRGB" || chunks[i].type == "pHYs" || chunks[i].type == "cHRM" ||
            chunks[i].type == "gAMA") {
            CHECK(i < firstData);
        }
    }

    int w = 0;
    int h = 0;
    CHECK(decodePng(file, &w, &h) == std::vector<uint8_t>(pixels, pixels + 8));

    // Sin ppp ni nombre, no van.
    const std::vector<Chunk> plain = readChunks(encodePng(pixels, 2, 1, 8, {}));
    CHECK(findChunk(plain, "pHYs") == nullptr);
    CHECK(findChunk(plain, "iTXt") == nullptr);
    CHECK(findChunk(plain, "sRGB") != nullptr);
}

TEST_CASE(png_pixels_per_meter) {
    CHECK_EQ(png::pixelsPerMeter(300.0f), 11811u);
    CHECK_EQ(png::pixelsPerMeter(72.0f), 2835u);
    CHECK_EQ(png::pixelsPerMeter(96.5f), 3799u);
    CHECK_EQ(png::pixelsPerMeter(0.0f), 0u);
    CHECK_EQ(png::pixelsPerMeter(-5.0f), 0u);
    CHECK_EQ(png::pixelsPerMeter(std::nanf("")), 0u);
}

TEST_CASE(png_large_image_and_stride) {
    // Ruido: casi no se comprime, así que los datos van en varios chunks IDAT.
    const int width = 700;
    const int height = 600;
    const size_t stride = static_cast<size_t>(width + 13) * 4;   // filas con relleno al final
    std::vector<uint8_t> source(stride * height);
    std::mt19937 random(7);
    for (uint8_t& value : source) {
        value = static_cast<uint8_t>(random());
    }
    const std::vector<uint8_t> file = encodePng(source.data(), width, height, stride, {});
    const std::vector<Chunk> chunks = readChunks(file);
    REQUIRE(!chunks.empty());
    int idat = 0;
    for (const Chunk& chunk : chunks) {
        if (chunk.type == "IDAT") {
            ++idat;
            CHECK(chunk.data.size() <= (size_t{1} << 20));
        }
    }
    CHECK(idat >= 2);

    int w = 0;
    int h = 0;
    const std::vector<uint8_t> decoded = decodePng(file, &w, &h);
    REQUIRE(w == width && h == height);
    bool same = true;
    for (int y = 0; y < height && same; ++y) {
        same = std::memcmp(decoded.data() + static_cast<size_t>(y) * width * 4, source.data() + y * stride,
                           static_cast<size_t>(width) * 4) == 0;
    }
    CHECK(same);

    // Un trozo de la imagen (el principio de una fila más adentro y el mismo stride).
    const uint8_t* corner = source.data() + 10 * stride + 20 * 4;
    const std::vector<uint8_t> part = decodePng(encodePng(corner, 30, 40, stride, {}), &w, &h);
    REQUIRE(w == 30 && h == 40 && part.size() == 30u * 40u * 4u);
    CHECK(std::memcmp(part.data() + 5 * 30 * 4, corner + 5 * stride, 30 * 4) == 0);
}

TEST_CASE(png_rejects_bad_sizes) {
    const uint8_t pixel[4] = {0, 0, 0, 0};
    bool wrote = false;
    const png::Sink sink = [&wrote](const uint8_t*, size_t) {
        wrote = true;
        return true;
    };
    CHECK(!png::encode(pixel, 0, 1, 4, {}, sink));
    CHECK(!png::encode(pixel, 1, -1, 4, {}, sink));
    CHECK(!png::write(std::vector<uint8_t>(3), 1, 1, {}, sink));   // faltan bytes
    CHECK(!wrote);

    // Si quien recibe el PNG falla, se para.
    int calls = 0;
    CHECK(!png::encode(pixel, 1, 1, 4, {}, [&calls](const uint8_t*, size_t) { return ++calls < 2; }));
    CHECK_EQ(calls, 2);
}

TEST_CASE(file_stem_from_canvas_name) {
    CHECK_EQ(io::fileStem("Retrato"), std::string("Retrato"));
    CHECK_EQ(io::fileStem("Boceto 1/2: fondo\\luz"), std::string("Boceto 1-2- fondo-luz"));
    CHECK_EQ(io::fileStem("¿Qué? <*> \"sí\" |"), std::string("¿Qué  sí"));
    CHECK_EQ(io::fileStem("  ..oculto.. "), std::string("oculto"));
    CHECK_EQ(io::fileStem("tab\tnueva\nlínea"), std::string("tabnuevalínea"));

    // Nada que valga: la fecha y la hora.
    const std::regex stamp(R"(LiveSketch_\d{8}_\d{6})");
    CHECK(std::regex_match(io::fileStem(""), stamp));
    CHECK(std::regex_match(io::fileStem(" . ?* "), stamp));

    // Los nombres largos se cortan sin partir un carácter.
    std::string longName;
    for (int i = 0; i < 70; ++i) {
        longName += "ñ";   // 2 bytes
    }
    const std::string cut = io::fileStem(longName);
    CHECK_EQ(cut.size(), 96u);
    CHECK_EQ(cut.substr(0, 4), std::string("ññ"));
    longName = "a" + longName;   // 141 bytes: el corte caería en medio de una ñ
    CHECK_EQ(io::fileStem(longName).size(), 95u);
}

TEST_CASE(create_file_never_overwrites) {
    const std::string folder = test::tempFolder("create_file");
    std::string paths[3];
    for (int i = 0; i < 3; ++i) {
        SDL_IOStream* io = io::createFile(folder, "Dibujo", ".png", &paths[i]);
        REQUIRE(io);
        const char mark = static_cast<char>('a' + i);
        CHECK(SDL_WriteIO(io, &mark, 1) == 1);
        CHECK(SDL_CloseIO(io));
    }
    CHECK_EQ(paths[0], folder + "Dibujo.png");
    CHECK_EQ(paths[1], folder + "Dibujo (2).png");
    CHECK_EQ(paths[2], folder + "Dibujo (3).png");
    CHECK(readFile(paths[0]) == std::vector<uint8_t>{'a'});   // el primero sigue igual
    CHECK(readFile(paths[2]) == std::vector<uint8_t>{'c'});

    std::string path;
    CHECK(io::createFile(folder + "no/existe/", "Dibujo", ".png", &path) == nullptr);
}

TEST_CASE(downloads_folder_exists) {
    const std::string folder = io::downloadsFolder();
    CHECK(!folder.empty());
    CHECK(folder.back() == '/');
    CHECK(std::filesystem::is_directory(folder));
}

TEST_CASE(png_exporter_unpremultiplies_in_background) {
    const std::string folder = test::tempFolder("exporter");
    const std::string path = folder + "Mi dibujo.png";
    // 2×1: naranja premultiplicado al 50 % y blanco opaco.
    std::vector<uint8_t> premultiplied = {128, 64, 0, 128, 255, 255, 255, 255};
    std::atomic<int> finished{0};

    io::PngExporter exporter;
    png::Info info;
    info.ppi = 150.0f;
    REQUIRE(exporter.start(premultiplied, 2, 1, folder, "Mi dibujo", info, [&finished] { ++finished; }));
    exporter.wait();
    CHECK(!exporter.busy());
    CHECK_EQ(finished.load(), 1);

    const std::optional<io::PngExporter::Result> result = exporter.takeResult();
    REQUIRE(result.has_value());
    CHECK(result->ok);
    CHECK_EQ(result->path, path);
    CHECK(!exporter.takeResult().has_value());   // se entrega una sola vez

    int w = 0;
    int h = 0;
    const std::vector<uint8_t> loaded = loadPng(path, &w, &h);
    const std::vector<uint8_t> expected = {255, 128, 0, 128, 255, 255, 255, 255};
    CHECK(loaded == expected);
    const std::vector<Chunk> chunks = readChunks(readFile(path));
    const Chunk* physical = findChunk(chunks, "pHYs");
    REQUIRE(physical);
    CHECK_EQ(get32(physical->data.data()), png::pixelsPerMeter(150.0f));

    // Otra vez con el mismo nombre: no pisa el primero.
    REQUIRE(exporter.start(premultiplied, 2, 1, folder, "Mi dibujo", info));
    exporter.wait();
    const std::optional<io::PngExporter::Result> second = exporter.takeResult();
    REQUIRE(second.has_value() && second->ok);
    CHECK_EQ(second->path, folder + "Mi dibujo (2).png");

    // Tamaño que no cuadra con el buffer: no se empieza.
    CHECK(!exporter.start(premultiplied, 3, 1, folder, "x", {}));
}

TEST_CASE(png_exporter_reports_errors) {
    const std::string folder = test::tempFolder("exporter_error") + "no/existe/";
    io::PngExporter exporter;
    REQUIRE(exporter.start({0, 0, 0, 0}, 1, 1, folder, "x", {}));
    exporter.wait();
    const std::optional<io::PngExporter::Result> result = exporter.takeResult();
    REQUIRE(result.has_value());
    CHECK(!result->ok);
    CHECK(!result->error.empty());
}
