// Guardar y abrir proyectos (.lvskt): lo que hace la app con el lienzo, la vista y el pincel.
// El archivo lo escriben y lo leen project::Saver y project::Loader (IO/ProjectFile.h), con
// la compresión en otros hilos; aquí, en el hilo de GL, se leen las capas de la GPU al
// guardar y se suben al abrir.
#include "App/App.h"

#include "App/AppEvents.h"
#include "IO/ImageExport.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#ifdef SDL_PLATFORM_EMSCRIPTEN
#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

// Desde que se pide guardar hasta que se leen las capas: el aviso de «Guardando proyecto…»
// ya se ve mientras tanto (en un lienzo grande, leerlas para un momento la interfaz).
constexpr uint64_t kSaveDelayMs = 120;
// Al abrir, tiempo de cada frame para subir capas a la GPU (y, en la web, descomprimirlas).
constexpr uint64_t kOpenBudgetMs = 12;
// Avisos de un proyecto abierto que se enseñan (el resto se cuenta).
constexpr size_t kShownWarnings = 4;

// El programa y su versión, para document.json («LiveSketch 0.3.0-alpha»).
std::string appName() {
    const char* name = SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING);
    const char* version = SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING);
    std::string text = name && *name ? name : "LiveSketch";
    if (version && *version) {
        text += ' ';
        text += version;
    }
    return text;
}

// «Retrato», o lo que se dice si no hay nombre.
std::string quoted(const std::string& name, const char* fallback) {
    return name.empty() ? std::string(fallback) : "«" + name + "»";
}

// Un motivo («no hay memoria…») como frase: con mayúscula y punto.
std::string sentence(std::string text) {
    if (text.empty()) {
        return text;
    }
    if (text[0] >= 'a' && text[0] <= 'z') {
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    }
    if (text.back() != '.' && text.back() != '!' && text.back() != '?') {
        text += '.';
    }
    return text;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// El nombre de un archivo, para mostrarlo: el de la ruta o, en Android, el que lleve la URI
// del documento (content://…/primary%3ADownload%2FRetrato.lvskt). Vacío si no se sabe.
std::string fileName(const std::string& path) {
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    if (path.rfind("content://", 0) == 0) {
        std::string decoded;
        for (size_t i = 0; i < name.size(); ++i) {
            const int high = name[i] == '%' && i + 2 < name.size() ? hexDigit(name[i + 1]) : -1;
            const int low = high >= 0 ? hexDigit(name[i + 2]) : -1;
            if (low >= 0) {
                decoded += static_cast<char>(high * 16 + low);
                i += 2;
            } else {
                decoded += name[i];
            }
        }
        name = decoded.substr(decoded.find_last_of(":/") + 1);
        // Sin extensión es el número del documento, no su nombre.
        if (name.find('.') == std::string::npos) {
            return {};
        }
    }
    return project::cleanName(name);
}

bool hasExtension(const std::string& name, std::initializer_list<const char*> extensions) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = name.substr(dot + 1);
    for (char& c : extension) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return std::any_of(extensions.begin(), extensions.end(),
                       [&extension](const char* e) { return extension == e; });
}

// El nombre sin la extensión (ni el .zip que añaden algunos sitios al descargar un proyecto).
std::string withoutExtension(std::string name) {
    if (hasExtension(name, {"zip"}) && hasExtension(name.substr(0, name.size() - 4), {"lvskt"})) {
        name.resize(name.size() - 4);
    }
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// En la web, lo que se elige o se suelta llega como copia en la memoria de la página, cada
// una en su carpeta: se borran al terminar.
bool temporaryPath([[maybe_unused]] const std::string& path) {
#ifdef SDL_PLATFORM_EMSCRIPTEN
    return path.rfind("/tmp/open/", 0) == 0 || path.rfind("/tmp/filedrop/", 0) == 0;
#else
    return false;
#endif
}

void removeTemporary(const std::string& path) {
    SDL_RemovePath(path.c_str());
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        SDL_RemovePath(path.substr(0, slash).c_str());   // la carpeta, ya vacía
    }
}

#ifndef SDL_PLATFORM_EMSCRIPTEN
// Respuesta del selector de archivos del sistema (puede llegar desde otro hilo).
void SDLCALL onProjectChosen(void*, const char* const* files, int) {
    if (!files) {
        appevents::push(appevents::fileChosen, -1, SDL_strdup(SDL_GetError()));
    } else if (!files[0]) {
        appevents::push(appevents::fileChosen, 1);
    } else {
        appevents::push(appevents::fileChosen, 0, SDL_strdup(files[0]));
    }
}
#endif

} // namespace

#ifdef SDL_PLATFORM_EMSCRIPTEN
// Elegir un archivo en el navegador: un <input type="file"> oculto. El archivo elegido se
// copia a la memoria de la página (/tmp/open/N/nombre) y llega como el del selector del
// sistema. El navegador solo lo deja abrir justo después de un toque o una tecla. La página
// puede cambiar los tipos que se ofrecen con Module.openAccept ("" para cualquiera, si sus
// descargas cambian la extensión). (En EM_JS, nada de barras invertidas ni de '': no llegan
// bien a JavaScript.)
EM_JS_DEPS(livesketch_open, "$stringToNewUTF8");
EM_JS(void, liveSketchChooseFile, (const char* accept), {
    let input = Module['liveSketchInput'];
    if (!input) {
        input = document.createElement('input');
        input.type = 'file';
        input.style.display = 'none';
        document.body.appendChild(input);
        Module['liveSketchInput'] = input;
        let count = 0;
        input.addEventListener('change', () => {
            const file = input.files && input.files[0];
            input.value = "";
            if (!file) {
                return;
            }
            file.arrayBuffer().then((buffer) => {
                count += 1;
                const folder = '/tmp/open/' + count;
                FS.mkdirTree(folder);
                const backslash = String.fromCharCode(92);
                const name = (file.name || "proyecto.lvskt").split("/").join("_").split(backslash).join("_");
                const path = folder + "/" + name;
                FS.writeFile(path, new Uint8Array(buffer));
                _liveSketchFileChosen(stringToNewUTF8(path), 0);
            }).catch((error) => {
                _liveSketchFileChosen(stringToNewUTF8(String(error)), -1);
            });
        });
    }
    const override = Module['openAccept'];
    input.accept = typeof override === "string" ? override : UTF8ToString(accept);
    input.click();
});

// `text` (de malloc, desde JavaScript): la ruta o, con `code` -1, el motivo del fallo.
extern "C" EMSCRIPTEN_KEEPALIVE void liveSketchFileChosen(char* text, int code) {
    appevents::push(appevents::fileChosen, code, text ? SDL_strdup(text) : nullptr);
    std::free(text);
}
#endif

// -----------------------------------------------------------------------------
// Guardar
// -----------------------------------------------------------------------------

project::Document App::projectDocument() const {
    project::Document document;
    document.width = m_canvas.width();
    document.height = m_canvas.height();
    document.info = m_canvas.info();
    document.background = m_canvas.background();
    const LayerStack& layers = m_canvas.layers();
    document.layers.resize(static_cast<size_t>(layers.count()));
    for (int i = 0; i < layers.count(); ++i) {
        const Layer& layer = layers.at(i);
        project::LayerInfo& out = document.layers[static_cast<size_t>(i)];
        out.name = layer.name;
        out.visible = layer.visible;
        out.opacity = layer.opacity;
        out.blend = layer.blend;
        out.alphaLock = layer.alphaLock;
        out.clipping = layer.clipping;
        out.reference = layer.reference;
    }
    document.activeLayer = layers.activeIndex();
    document.guide = m_canvas.guide();
    // La vista: ajustada (también si se está centrando) o como la dejó el usuario.
    document.view.flipped = m_camera.flipped();
    document.view.fitted = !m_camera.userMoved() || m_fitAnimation.active;
    if (!document.view.fitted) {
        const Camera::Placement placement = m_camera.placement();
        document.view.zoom = placement.zoom;
        document.view.center = placement.center;
        document.view.angle = placement.angle;
    }
    document.hasColor = true;
    m_ui.brushColor(m_canvas, document.color);
    return document;
}

void App::requestProjectSave() {
    if (!m_canvas.ready() || m_open.loader) {
        return;
    }
    // Ya se está guardando (el aviso lo dice).
    if (m_saveAtMs != 0 || m_projectSaver.busy() || m_awaitingPermission == StorageTask::Project) {
        return;
    }
    if (storageReady(StorageTask::Project)) {
        scheduleProjectSave();
    }
}

void App::scheduleProjectSave() {
    if (!m_canvas.ready()) {
        return;
    }
    m_saveAtMs = SDL_GetTicks() + kSaveDelayMs;
    m_ui.notify("Guardando proyecto…", Notice::Progress, 600000);
}

void App::saveProject() {
    if (!m_canvas.ready() || m_projectSaver.busy()) {
        return;
    }
    // Se guarda lo que se ve: lo que esté a medias (una transformación, un ajuste...) se aplica.
    m_canvas.settle();
    project::Document document = projectDocument();
    const int count = static_cast<int>(document.layers.size());
    std::vector<IRect> rects(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        rects[static_cast<size_t>(i)] = m_canvas.layerContent(i);
        document.layers[static_cast<size_t>(i)].rect = rects[static_cast<size_t>(i)];
    }
    project::Target target;
    target.folder = io::downloadsFolder();
    target.stem = io::fileStem(document.info.name);
    const uint64_t version = m_canvas.documentVersion();
    if (!m_projectSaver.begin(std::move(document), target, appName())) {
        if (std::optional<project::Saver::Result> result = m_projectSaver.takeResult()) {
            onProjectSaved(*result);
        }
        return;
    }
    m_savingVersion = version;

    // Cada capa, recortada a lo pintado, en cuanto quepa en la memoria de lo que espera a
    // comprimirse (con un byte más por fila: el PNG se filtra ahí mismo).
    std::string error;
    auto read = [this, &error](int index, const IRect& rect, std::vector<uint8_t>& pixels) {
        const size_t row = static_cast<size_t>(rect.width()) * 4;
        const size_t rows = static_cast<size_t>(rect.height());
        m_projectSaver.reserve((row + 1) * rows);
        try {
            pixels.reserve((row + 1) * rows);
            pixels.resize(row * rows);
        } catch (const std::bad_alloc&) {
            error = "no hay memoria para leer el dibujo";
            return false;
        }
        if (!m_canvas.readRegion(index, rect, pixels.data())) {
            error = "no se pudo leer el dibujo de la memoria de gráficos";
            return false;
        }
        return true;
    };
    const auto done = [] { appevents::push(appevents::projectSaved); };
    for (int i = 0; i < count; ++i) {
        std::vector<uint8_t> pixels;
        const IRect& rect = rects[static_cast<size_t>(i)];
        if (!rect.empty() && !read(i, rect, pixels)) {
            m_projectSaver.abort(error, done);
            return;
        }
        m_projectSaver.addLayer(i, std::move(pixels));
    }
    std::vector<uint8_t> composite;
    if (!read(-1, IRect::ofSize(m_canvas.width(), m_canvas.height()), composite)) {
        m_projectSaver.abort(error, done);
        return;
    }
    m_projectSaver.finish(std::move(composite), done);
}

void App::onProjectSaved(const project::Saver::Result& result) {
    if (!result.ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo guardar %s: %s", result.path.c_str(),
                     result.error.c_str());
        m_ui.dismissNotice(Notice::Progress);
        m_ui.showAlert("No se pudo guardar el proyecto", sentence(result.error), Notice::Error);
        return;
    }
    io::announceFile(result.path, project::kMimeType);
    // Las versiones solo suben (también de un lienzo a otro): un guardado del lienzo anterior
    // que termina después de abrir otro no lo da por guardado.
    m_savedVersion = std::max(m_savedVersion, m_savingVersion);
    SDL_Log("Proyecto guardado en %s (%llu bytes)", result.path.c_str(),
            static_cast<unsigned long long>(result.bytes));
    const std::string name = result.path.substr(result.path.find_last_of('/') + 1);
#ifdef SDL_PLATFORM_EMSCRIPTEN
    m_ui.notify("Proyecto descargado: " + name, Notice::Success, 5000);
#elif defined(SDL_PLATFORM_ANDROID)
    m_ui.notify("Guardado en Descargas: " + name, Notice::Success, 4000);
#else
    m_ui.notify("Proyecto guardado en " + result.path, Notice::Success, 5000);
#endif
}

// -----------------------------------------------------------------------------
// Abrir
// -----------------------------------------------------------------------------

void App::chooseProject() {
    if (m_choosingFile || m_open.loader) {
        return;
    }
#ifdef SDL_PLATFORM_EMSCRIPTEN
    liveSketchChooseFile(".lvskt");
#else
    static const SDL_DialogFileFilter kFilters[] = {{"Proyectos de LiveSketch (.lvskt)", "lvskt"}};
#ifdef SDL_PLATFORM_ANDROID
    const std::string folder;   // Android abre donde se quedó la última vez
#else
    const std::string folder = io::downloadsFolder();   // donde se guardan
#endif
    m_choosingFile = true;
    SDL_ShowOpenFileDialog(onProjectChosen, nullptr, m_window, kFilters, 1, folder.empty() ? nullptr : folder.c_str(),
                           false);
#endif
}

void App::fileChosen(std::string path) {
    const bool temporary = temporaryPath(path);
    // Mientras se pregunta si abrir otro, el que llega después lo sustituye.
    if (m_open.confirming && !m_chosenFile) {
        closeProjectFile();
        m_ui.cancelOpenProject();
    }
    // Ya se está abriendo otro (o se soltaron varios a la vez): este no.
    if (m_chosenFile || m_open.loader) {
        if (temporary) {
            removeTemporary(path);
        }
        m_ui.notify("Ya se está abriendo un proyecto", Notice::Info, 2500);
        return;
    }
    m_chosenFile = ChosenFile{std::move(path), temporary};
}

void App::openProjectFile(const std::string& path, bool temporary) {
    const std::string name = fileName(path);
    auto loader = std::make_unique<project::Loader>();
    if (!loader->open(path, m_maxCanvasSize)) {
        const std::string error = loader->error();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No se pudo abrir %s: %s", path.c_str(), error.c_str());
        loader.reset();   // suelta el archivo antes de borrarlo
        if (temporary) {
            removeTemporary(path);
        }
        const std::string reason = hasExtension(name, {"png", "jpg", "jpeg", "webp", "gif", "bmp"})
                                       ? "Es una imagen: por ahora solo se abren proyectos de LiveSketch (.lvskt)."
                                       : sentence(error);
        m_ui.showAlert("No se pudo abrir " + quoted(name, "el archivo"), reason, Notice::Error);
        return;
    }
    m_open.loader = std::move(loader);
    m_open.path = path;
    m_open.temporary = temporary;
    const std::string& canvasName = m_open.loader->document().info.name;
    m_open.title = canvasName.empty() ? withoutExtension(name) : canvasName;
    if (canvasDirty()) {
        m_open.confirming = true;
        m_ui.askOpenProject(m_open.title);
        return;
    }
    startProjectOpen();
}

void App::startProjectOpen() {
    if (!m_open.loader) {
        return;
    }
    m_open.confirming = false;
    // Un guardado que estaba a punto de empezar se hace ya, con el lienzo de ahora.
    if (m_saveAtMs != 0) {
        m_saveAtMs = 0;
        saveProject();
    }
    const project::Document& document = m_open.loader->document();
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
        LayerProperties& properties = layers[i].properties;
        properties.name = layer.name;
        properties.visible = layer.visible;
        properties.opacity = layer.opacity;
        properties.blend = layer.blend;
        properties.alphaLock = layer.alphaLock;
        properties.clipping = layer.clipping;
        layers[i].reference = layer.reference;
    }

    // NDI emite al tamaño del lienzo: vuelve a empezar al terminar de abrirlo.
    m_open.ndi = m_ndi.running();
    m_ndi.stop();
    endGestures();
    m_fitAnimation.active = false;
    if (!m_canvas.open(spec, document.info, layers, document.activeLayer, document.guide)) {
        const std::string message = "No hay memoria de gráficos para un lienzo de " + std::to_string(spec.width) +
                                    " × " + std::to_string(spec.height) + " px con " +
                                    std::to_string(layers.size()) + (layers.size() == 1 ? " capa." : " capas.");
        const std::string title = "No se pudo abrir " + quoted(m_open.title, "el proyecto");
        closeProjectFile();
        m_ui.canvasCreated();
        m_ui.showAlert(title, message, Notice::Error);
        return;
    }
    m_ui.canvasCreated();
    m_ui.projectOpened(m_canvas, document.hasColor ? document.color : nullptr);
    m_camera.setCanvasSize({static_cast<float>(spec.width), static_cast<float>(spec.height)});
    if (document.view.flipped) {
        m_camera.setFlipped(true);
    }
    if (!document.view.fitted) {
        m_camera.setPlacement({document.view.zoom, document.view.center, document.view.angle});
    }
    m_savedVersion = m_canvas.documentVersion();
    m_open.loading = true;
    m_open.loader->start();
    SDL_Log("Abriendo %s: %dx%d, %zu capas", m_open.path.c_str(), spec.width, spec.height, layers.size());
}

void App::stepProjectOpen() {
    project::Loader& loader = *m_open.loader;
    const uint64_t start = SDL_GetTicks();
    // Lo que ya está leído sube a la GPU (al menos una capa por frame) y deja sitio para leer
    // más; en la web, después se descomprime con lo que quede del tiempo.
    int index = 0;
    std::vector<uint8_t> pixels;
    while (loader.next(&index, pixels)) {
        const project::LayerInfo& layer = loader.document().layers[static_cast<size_t>(index)];
        if (!pixels.empty() && !m_canvas.setLayerPixels(index, layer.rect, pixels.data())) {
            ++m_open.failedUploads;
        }
        pixels = {};
        if (SDL_GetTicks() - start >= kOpenBudgetMs) {
            break;
        }
    }
    const uint64_t spent = SDL_GetTicks() - start;
    loader.pump(spent < kOpenBudgetMs ? kOpenBudgetMs - spent : 1);
    if (loader.finished()) {
        finishProjectOpen();
    }
}

void App::finishProjectOpen() {
    std::vector<std::string> warnings = m_open.loader->warnings();
    if (m_open.failedUploads > 0) {
        warnings.push_back(std::to_string(m_open.failedUploads) +
                           (m_open.failedUploads == 1 ? " capa no cabía" : " capas no cabían") +
                           " en la memoria de gráficos: se abren vacías.");
    }
    const std::string title = quoted(m_open.title, "el proyecto");
    const bool ndi = m_open.ndi;
    SDL_Log("Proyecto abierto: %s", m_open.path.c_str());
    closeProjectFile();
    // Recién abierto no tiene nada que perder.
    m_savedVersion = m_canvas.documentVersion();
    if (ndi) {
        setNdiEnabled(true);
    }
    if (warnings.empty()) {
        m_ui.notify("Se abrió " + title, Notice::Success, 3000);
        return;
    }
    std::string message;
    for (size_t i = 0; i < warnings.size(); ++i) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%s", warnings[i].c_str());
        if (i < kShownWarnings) {
            message += (i > 0 ? "\n" : "") + warnings[i];
        }
    }
    if (warnings.size() > kShownWarnings) {
        const size_t more = warnings.size() - kShownWarnings;
        message += "\nY " + std::to_string(more) + (more == 1 ? " aviso más." : " avisos más.");
    }
    m_ui.showAlert("Se abrió " + title + " con avisos", message, Notice::Warning);
}

void App::closeProjectFile() {
    const std::string path = std::move(m_open.path);
    const bool temporary = m_open.temporary;
    m_open = {};   // el lector deja de leer y cierra el archivo
    if (temporary && !path.empty()) {
        removeTemporary(path);
    }
}
