// Pruebas de la biblioteca de proyectos: mirar la carpeta (lo más reciente primero, también
// los archivos dañados), y renombrar, duplicar, copiar y borrar sin estropear nada.

#include "Test.h"

#include "IO/Json.h"
#include "IO/Library.h"
#include "IO/Project.h"
#include "IO/ProjectFile.h"
#include "IO/Zip.h"

#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

// 2023-11-14T22:13:20Z en SDL_Time.
constexpr int64_t kSomeTime = 1700000000LL * 1000000000LL;
constexpr int64_t kHour = 3600LL * 1000000000LL;

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<std::string> filesIn(const std::string& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<uint8_t> randomPremultiplied(size_t pixels, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> out(pixels * 4);
    for (size_t i = 0; i < out.size(); i += 4) {
        const uint32_t alpha = rng() % 256;
        for (size_t c = 0; c < 3; ++c) {
            out[i + c] = static_cast<uint8_t>(rng() % (alpha + 1));
        }
        out[i + 3] = static_cast<uint8_t>(alpha);
    }
    return out;
}

// Un proyecto de 64 × 48 con dos capas (la segunda, vacía) guardado en `path`.
struct Saved {
    project::Document document;
    std::vector<uint8_t> pixels;   // los de la primera capa
    std::string path;
};

Saved saveProject(const std::string& path, const std::string& name, int64_t modified, uint32_t seed) {
    Saved saved;
    project::Document& document = saved.document;
    document.width = 64;
    document.height = 48;
    document.info.name = name;
    document.info.profile = seed % 2 == 0 ? ColorProfile::Srgb : ColorProfile::DisplayP3;
    document.info.created = kSomeTime;
    document.info.modified = modified;
    project::LayerInfo first;
    first.name = "Tinta";
    first.rect = {8, 4, 40, 36};
    project::LayerInfo second;
    second.name = "Vacía";
    document.layers = {first, second};
    saved.pixels = randomPremultiplied(32 * 32, seed);

    project::Saver saver;
    project::Target target;
    target.path = path;
    if (!saver.begin(document, target, "LiveSketch pruebas")) {
        return saved;
    }
    saver.addLayer(0, saved.pixels);
    saver.addLayer(1, {});
    saver.finish(randomPremultiplied(64 * 48, seed + 1), {});
    saver.wait();
    const std::optional<project::Saver::Result> result = saver.takeResult();
    if (result && result->ok) {
        saved.path = result->path;
    }
    return saved;
}

// Espera a que la biblioteca termine lo que tenga en marcha (en la web, lo hace aquí).
bool settle(library::Library& library) {
    const uint64_t start = SDL_GetTicks();
    while (true) {
        library.update(50);
        if (!library.working()) {
            library.update(0);
            return true;
        }
        if (SDL_GetTicks() - start > 30000) {
            return false;
        }
#ifndef SDL_PLATFORM_EMSCRIPTEN
        SDL_Delay(1);
#endif
    }
}

std::vector<std::string> names(const library::Library& library) {
    std::vector<std::string> out;
    for (const library::Summary& item : library.items()) {
        out.push_back(item.readable ? item.name : "(dañado)");
    }
    return out;
}

// Las capas de un proyecto, leídas como al abrirlo.
std::vector<std::vector<uint8_t>> loadLayers(const std::string& path) {
    project::Loader loader;
    if (!loader.open(path, 0)) {
        return {};
    }
    std::vector<std::vector<uint8_t>> layers(loader.document().layers.size());
    loader.start();
    const uint64_t start = SDL_GetTicks();
    int index = 0;
    std::vector<uint8_t> pixels;
    while (!loader.finished() && SDL_GetTicks() - start < 30000) {
        if (loader.next(&index, pixels)) {
            layers[static_cast<size_t>(index)] = std::move(pixels);
            pixels = {};
        } else {
            loader.pump(20);
        }
    }
    return layers;
}

std::string documentText(const std::string& path) {
    const std::vector<uint8_t> file = readFile(path);
    zip::MemorySource source(file);
    zip::Reader reader;
    std::vector<uint8_t> text;
    if (!reader.open(source) || !reader.find(project::kDocumentEntry) ||
        !reader.read(*reader.find(project::kDocumentEntry), text, json::kMaxText)) {
        return {};
    }
    return std::string(text.begin(), text.end());
}

} // namespace

TEST_CASE(json_write_round_trips_a_tree) {
    const std::string text = R"({
  "a": 1,
  "b": [0.25, -3, 1e+20],
  "c": {"d": "texto \"con\" comillas\n y ñ", "e": [true, false, null, {"f": []}]},
  "g": 0.1
})";
    json::Value value;
    REQUIRE(json::parse(text, value));
    const std::string written = json::write(value);
    json::Value again;
    REQUIRE(json::parse(written, again));
    CHECK_EQ(json::write(again), written);
    CHECK_EQ(again["a"].number(), 1.0);
    CHECK_EQ(again["b"][0].number(), 0.25);
    CHECK_EQ(again["b"][2].number(), 1e20);
    CHECK_EQ(again["c"]["d"].string(), std::string("texto \"con\" comillas\n y ñ"));
    CHECK(again["c"]["e"][2].isNull());
    CHECK(again["c"]["e"][3]["f"].isArray());
    CHECK_EQ(again["g"].number(), 0.1);
    // Las listas de números, en una línea (como los colores de document.json).
    CHECK(written.find("[0.25, -3, 1e+20]") != std::string::npos || written.find("[0.25, -3, 100000000000000000000]") !=
                                                                           std::string::npos);
    // Cambiar un miembro y añadir otro.
    json::Value* c = again.member("c");
    REQUIRE(c != nullptr);
    *c->member("d") = json::Value::text("otro");
    *again.member("nuevo") = json::Value::text("sí");
    json::Value changed;
    REQUIRE(json::parse(json::write(again), changed));
    CHECK_EQ(changed["c"]["d"].string(), std::string("otro"));
    CHECK_EQ(changed["nuevo"].string(), std::string("sí"));
    CHECK(changed["a"].number() == 1.0);
    // Un valor que no es un objeto no tiene miembros que cambiar.
    json::Value list;
    REQUIRE(json::parse("[1, 2]", list));
    CHECK(list.member("x") == nullptr);
}

TEST_CASE(library_lists_projects_newest_first) {
    const std::string folder = test::tempFolder("library_list");
    library::Library library;
    REQUIRE(library.open(folder));
    const Saved old = saveProject(library.newPath(), "Antiguo", kSomeTime, 1);
    const Saved recent = saveProject(library.newPath(), "Reciente", kSomeTime + 2 * kHour, 2);
    const Saved middle = saveProject(library.newPath(), "", kSomeTime + kHour, 3);
    REQUIRE(!old.path.empty() && !recent.path.empty() && !middle.path.empty());
    CHECK(library.contains(old.path));
    CHECK(!library.contains(folder + "otra/carpeta.lvskt"));
    CHECK(!library.contains("/tmp/x.lvskt"));
    // Uno que no es un proyecto, otro archivo cualquiera y lo que dejó un guardado cortado.
    writeFile(folder + "roto.lvskt", randomPremultiplied(100, 7));
    writeFile(folder + "nota.txt", {'h', 'o', 'l', 'a'});
    writeFile(folder + "20260101-000000-abcdef.lvskt.tmp", {1, 2, 3});

    library::Library again;
    REQUIRE(again.open(folder));
    CHECK(!std::filesystem::exists(folder + "20260101-000000-abcdef.lvskt.tmp"));
    CHECK(again.loading());
    again.refresh();
    REQUIRE(settle(again));
    CHECK(!again.loading());
    REQUIRE(again.items().size() == size_t{4});
    // El dañado no dice su fecha: va por la del archivo, que es la de ahora.
    CHECK(names(again) == (std::vector<std::string>{"(dañado)", "Reciente", "", "Antiguo"}));
    const library::Summary& broken = again.items()[0];
    CHECK(!broken.readable);
    CHECK(!broken.error.empty());
    const library::Summary& item = again.items()[1];
    CHECK_EQ(item.path, recent.path);
    CHECK_EQ(item.width, 64);
    CHECK_EQ(item.height, 48);
    CHECK_EQ(item.layers, 2);
    CHECK(item.profile == ColorProfile::Srgb);
    CHECK(again.items()[3].profile == ColorProfile::DisplayP3);
    CHECK_EQ(item.modified, kSomeTime + 2 * kHour);
    CHECK_EQ(item.bytes, uint64_t{readFile(item.path).size()});
    CHECK_EQ(item.thumbnailWidth, 64);
    CHECK_EQ(item.thumbnailHeight, 48);
    CHECK_EQ(item.thumbnail.size(), size_t{64 * 48 * 4});
    CHECK(again.find(old.path) != nullptr);
    CHECK(again.find(folder + "nota.txt") == nullptr);

    // Lo que cambia se vuelve a leer; lo que se borra desaparece.
    saveProject(old.path, "Antiguo cambiado", kSomeTime + 3 * kHour, 4);
    std::filesystem::remove(folder + "roto.lvskt");
    again.refresh();
    REQUIRE(settle(again));
    CHECK(names(again) == (std::vector<std::string>{"Antiguo cambiado", "Reciente", ""}));
}

TEST_CASE(library_renames_duplicates_copies_and_removes) {
    const std::string folder = test::tempFolder("library_jobs");
    library::Library library;
    REQUIRE(library.open(folder));
    const Saved first = saveProject(library.newPath(), "Retrato", kSomeTime, 11);
    const Saved second = saveProject(library.newPath(), "Paisaje", kSomeTime + kHour, 12);
    REQUIRE(!first.path.empty() && !second.path.empty());
    library.refresh();
    REQUIRE(settle(library));
    CHECK(names(library) == (std::vector<std::string>{"Paisaje", "Retrato"}));

    // Renombrar no mueve el archivo ni cambia la fecha del dibujo, y sus capas siguen igual.
    library.rename(first.path, "Retrato \"final\" ñ");
    CHECK(library.busy(first.path));
    REQUIRE(settle(library));
    std::vector<library::Library::Done> done = library.takeDone();
    REQUIRE(done.size() == size_t{1});
    CHECK(done[0].ok);
    CHECK(done[0].kind == library::Library::Done::Kind::Rename);
    CHECK(!library.busy(first.path));
    CHECK(names(library) == (std::vector<std::string>{"Paisaje", "Retrato \"final\" ñ"}));
    CHECK_EQ(library.find(first.path)->modified, kSomeTime);
    std::vector<std::vector<uint8_t>> layers = loadLayers(first.path);
    REQUIRE(layers.size() == size_t{2});
    CHECK(layers[0] == first.pixels);
    CHECK(layers[1].empty());

    // Duplicar crea otro archivo con otro nombre, que pasa a ser el más reciente.
    library.duplicate(second.path, "Paisaje (copia)");
    REQUIRE(settle(library));
    done = library.takeDone();
    REQUIRE(done.size() == size_t{1});
    CHECK(done[0].ok);
    const std::string copy = done[0].result;
    CHECK(library.contains(copy));
    CHECK(copy != second.path);
    CHECK(names(library) == (std::vector<std::string>{"Paisaje (copia)", "Paisaje", "Retrato \"final\" ñ"}));
    layers = loadLayers(copy);
    REQUIRE(layers.size() == size_t{2});
    CHECK(layers[0] == second.pixels);

    // Copiar fuera: un archivo nuevo con el nombre que se pida, idéntico.
    const std::string outside = test::tempFolder("library_jobs_out");
    project::Target target;
    target.folder = outside;
    target.stem = "Paisaje";
    library.copy(second.path, target);
    library.copy(second.path, target);
    REQUIRE(settle(library));
    done = library.takeDone();
    REQUIRE(done.size() == size_t{2});
    CHECK(done[0].ok && done[1].ok);
    CHECK_EQ(done[0].result, outside + "Paisaje.lvskt");
    CHECK_EQ(done[1].result, outside + "Paisaje (2).lvskt");
    CHECK(readFile(done[0].result) == readFile(second.path));
    // O a una ruta concreta, que se sustituye.
    target = {};
    target.path = outside + "elegido.lvskt";
    writeFile(target.path, {1, 2, 3});
    library.copy(first.path, target);
    REQUIRE(settle(library));
    done = library.takeDone();
    REQUIRE(done.size() == size_t{1});
    CHECK(done[0].ok);
    CHECK(readFile(target.path) == readFile(first.path));
    CHECK(filesIn(outside) == (std::vector<std::string>{"Paisaje (2).lvskt", "Paisaje.lvskt", "elegido.lvskt"}));

    // Borrar.
    library.remove(second.path);
    REQUIRE(settle(library));
    done = library.takeDone();
    REQUIRE(done.size() == size_t{1});
    CHECK(done[0].ok);
    CHECK(!std::filesystem::exists(second.path));
    CHECK(names(library) == (std::vector<std::string>{"Paisaje (copia)", "Retrato \"final\" ñ"}));

    // Lo que falla se dice y no deja nada a medias (borrar lo que ya no está no es un fallo).
    library.rename(folder + "no-existe.lvskt", "x");
    library.remove(folder + "no-existe.lvskt");
    REQUIRE(settle(library));
    done = library.takeDone();
    REQUIRE(done.size() == size_t{2});
    CHECK(!done[0].ok && !done[0].error.empty());
    CHECK(done[1].ok);
    CHECK_EQ(filesIn(folder).size(), size_t{2});
}

TEST_CASE(library_rewrite_keeps_what_it_does_not_know) {
    // Un proyecto de una versión más nueva puede llevar cosas que esta no conoce: renombrarlo
    // no las pierde.
    const std::string folder = test::tempFolder("library_unknown");
    library::Library library;
    REQUIRE(library.open(folder));
    const Saved saved = saveProject(library.newPath(), "Original", kSomeTime, 21);
    REQUIRE(!saved.path.empty());
    std::string error;
    REQUIRE(library::rewrite(saved.path, saved.path,
                             [](json::Value& root) {
                                 *root.member("futuro") = json::Value::text("algo nuevo");
                                 *root.member("canvas")->member("textura") = json::Value::text("papel");
                                 return true;
                             },
                             &error));
    library.rename(saved.path, "Renombrado");
    REQUIRE(settle(library));
    const std::vector<library::Library::Done> done = library.takeDone();
    REQUIRE(done.size() == size_t{1} && done[0].ok);
    const std::string text = documentText(saved.path);
    CHECK(text.find("\"futuro\": \"algo nuevo\"") != std::string::npos);
    CHECK(text.find("\"textura\": \"papel\"") != std::string::npos);
    CHECK(text.find("\"name\": \"Renombrado\"") != std::string::npos);
    CHECK(loadLayers(saved.path).at(0) == saved.pixels);

    // Si el cambio no se puede hacer, el archivo queda como estaba y no deja restos.
    const std::vector<uint8_t> before = readFile(saved.path);
    CHECK(!library::rewrite(saved.path, saved.path, [](json::Value&) { return false; }, &error));
    CHECK(!library::rewrite(saved.path, saved.path,
                            [](json::Value& root) {
                                *root.member("canvas")->member("width") = json::Value::text("ancho");
                                return true;
                            },
                            &error));
    CHECK(readFile(saved.path) == before);
    CHECK_EQ(filesIn(folder).size(), size_t{1});
}
