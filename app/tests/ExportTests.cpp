#include "Test.h"

#include "Gfx/Pixels.h"
#include "IO/ImageExport.h"
#include "ThirdParty/stb_image.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>

#include <atomic>
#include <cstring>
#include <filesystem>
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

TEST_CASE(timestamped_path_is_unique) {
    const std::string folder = test::tempFolder("timestamped");
    const std::regex pattern(R"(LiveSketch_\d{8}_\d{6}(_\d+)?\.png)");

    // Si cambia el segundo entre las dos llamadas, se repite.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::string first = io::timestampedPath(folder, "LiveSketch", ".png");
        CHECK(first.rfind(folder, 0) == 0);
        const std::string name = first.substr(folder.size());
        CHECK(std::regex_match(name, pattern));
        const uint8_t pixel[4] = {0, 0, 0, 0};
        REQUIRE(io::writePng(first, pixel, 1, 1));

        const std::string second = io::timestampedPath(folder, "LiveSketch", ".png");
        CHECK(second != first);
        if (second.substr(0, first.size() - 4) == first.substr(0, first.size() - 4)) {
            CHECK_EQ(second, first.substr(0, first.size() - 4) + "_2.png");
            return;
        }
    }
    test::fail(__FILE__, __LINE__, "no se pudo probar el sufijo _2");
}

TEST_CASE(downloads_folder_exists) {
    const std::string folder = io::downloadsFolder();
    CHECK(!folder.empty());
    CHECK(folder.back() == '/');
    CHECK(std::filesystem::is_directory(folder));
}

TEST_CASE(png_exporter_unpremultiplies_in_background) {
    const std::string path = test::tempFolder("exporter") + "export.png";
    // 2×1: naranja premultiplicado al 50 % y blanco opaco.
    std::vector<uint8_t> premultiplied = {128, 64, 0, 128, 255, 255, 255, 255};
    std::atomic<int> finished{0};

    io::PngExporter exporter;
    REQUIRE(exporter.start(premultiplied, 2, 1, path, [&finished] { ++finished; }));
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

    // Tamaño que no cuadra con el buffer: no se empieza.
    CHECK(!exporter.start(premultiplied, 3, 1, path));
}

TEST_CASE(png_exporter_reports_errors) {
    const std::string path = test::tempFolder("exporter_error") + "no/existe/x.png";
    io::PngExporter exporter;
    REQUIRE(exporter.start({0, 0, 0, 0}, 1, 1, path));
    exporter.wait();
    const std::optional<io::PngExporter::Result> result = exporter.takeResult();
    REQUIRE(result.has_value());
    CHECK(!result->ok);
    CHECK(!result->error.empty());
}
