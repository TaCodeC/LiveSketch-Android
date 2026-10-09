// Proyectos (.lvskt): la biblioteca (una carpeta de la app con un archivo por proyecto), el
// guardado automático del lienzo en su archivo, dejar el lienzo, abrir proyectos (de la
// biblioteca o de fuera) y exportarlos. El archivo lo escriben y lo leen project::Saver y
// project::Loader (IO/ProjectFile.h), con la compresión en otros hilos; aquí, en el hilo de
// GL, se leen las capas de la GPU al guardar y se suben al abrir.
#include "App/App.h"

#include "App/AppEvents.h"
#include "IO/FileChooser.h"
#include "IO/ImageExport.h"
#include "IO/Storage.h"

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
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// Guardado automático: un momento después del último cambio (sin estar dibujando).
constexpr uint64_t kAutosaveDelayMs = 2500;
// Desde que se pide guardar hasta que se leen las capas: el aviso de «Guardando…» ya se ve
// mientras tanto (en un lienzo grande, leerlas para un momento la interfaz).
constexpr uint64_t kSaveDelayMs = 120;
// Al abrir, tiempo de cada frame para subir capas a la GPU (y, en la web, descomprimirlas).
constexpr uint64_t kOpenBudgetMs = 12;
// En la web, tiempo de cada frame para la biblioteca (mirar la carpeta, renombrar...).
constexpr uint64_t kLibraryBudgetMs = 6;
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

uint64_t mixHash(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64_t floatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
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
// La biblioteca
// -----------------------------------------------------------------------------

void App::openLibrary() {
    char* pref = SDL_GetPrefPath("TaCodec", "LiveSketch");
    const std::string folder = pref ? std::string(pref) + "proyectos/" : std::string();
    SDL_free(pref);
    m_library.setOnChange([] { appevents::push(appevents::wake, 1); });
    const bool opened = !folder.empty() && m_library.open(folder);
    if (opened) {
        m_library.refresh();
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Sin carpeta de proyectos: los lienzos no se guardarán");
        m_ui.notify("No se pueden guardar proyectos en este dispositivo", Notice::Error, 8000);
    }
    // Android: se abrió la app con un proyecto desde otra («Abrir con»). Si no, se vuelve al
    // proyecto que estaba abierto.
    const std::string launch = io::takeLaunchFile();
    if (!launch.empty()) {
        SDL_Log("Abierto con LiveSketch: %s", launch.c_str());
        openProjectFile(launch, false);
        return;
    }
    if (!opened) {
        return;
    }
    const std::string last = m_ui.lastProject();
    if (last.empty()) {
        return;
    }
    const std::string path = m_library.folder() + last;
    if (!m_library.contains(path) || !SDL_GetPathInfo(path.c_str(), nullptr)) {
        setLastProject({});
        return;
    }
    openProjectFile(path, false);
}

void App::setLastProject(const std::string& path) {
    std::string name;
    if (!path.empty() && m_library.contains(path)) {
        name = path.substr(m_library.folder().size());
    }
    if (name != m_ui.lastProject()) {
        m_ui.setLastProject(std::move(name));
    }
}

void App::applyProjectAction(const UiRequests::ProjectAction& action) {
    using Kind = UiRequests::ProjectAction::Kind;
    if (action.kind == Kind::None || action.path.empty()) {
        return;
    }
    switch (action.kind) {
    case Kind::Open:
        openProjectFile(action.path, false);
        break;
    case Kind::Rename:
        m_library.rename(action.path, action.name);
        break;
    case Kind::Duplicate:
        m_library.duplicate(action.path, action.name);
        break;
    case Kind::Remove:
        if (m_library.contains(action.path) && action.path.substr(m_library.folder().size()) == m_ui.lastProject()) {
            setLastProject({});
        }
        m_library.remove(action.path);
        break;
    case Kind::Export:
        exportProject(action.path, action.name, ExportTo::Downloads);
        break;
    case Kind::ExportTo:
        exportProject(action.path, action.name, ExportTo::Choose);
        break;
    case Kind::None:
        break;
    }
}

void App::onLibraryDone(const library::Library::Done& done) {
    using Kind = library::Library::Done::Kind;
    if (done.ok) {
        io::persist();
    }
    const library::Summary* item = m_library.find(done.path);
    const std::string title = quoted(item ? item->name : std::string(), "el proyecto");
    switch (done.kind) {
    case Kind::Rename:
        if (!done.ok) {
            m_ui.showAlert("No se pudo renombrar " + title, sentence(done.error), Notice::Error);
        }
        break;
    case Kind::Duplicate:
        if (!done.ok) {
            m_ui.showAlert("No se pudo duplicar " + title, sentence(done.error), Notice::Error);
        }
        break;
    case Kind::Remove:
        if (done.ok) {
            m_ui.notify("Se eliminó " + title, Notice::Info, 3000);
        } else {
            m_ui.showAlert("No se pudo eliminar " + title, sentence(done.error), Notice::Error);
        }
        break;
    case Kind::Copy: {
        m_ui.dismissNotice(Notice::Progress);
        if (!done.ok) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo exportar %s a %s: %s", done.path.c_str(),
                         done.result.c_str(), done.error.c_str());
            m_ui.showAlert("No se pudo exportar " + title, sentence(done.error), Notice::Error);
            break;
        }
        SDL_Log("Proyecto exportado a %s", done.result.c_str());
        // Un documento elegido en Android (content://...) ya lo conoce el sistema.
        if (done.result.rfind("content://", 0) == 0) {
            const std::string name = fileName(done.result);
            m_ui.notify(name.empty() ? std::string("Proyecto exportado") : "Proyecto exportado: " + name, Notice::Success,
                        4000);
            break;
        }
        io::announceFile(done.result, project::kMimeType);
        const std::string name = done.result.substr(done.result.find_last_of("/\\") + 1);
#ifdef SDL_PLATFORM_EMSCRIPTEN
        m_ui.notify("Proyecto descargado: " + name, Notice::Success, 5000);
#elif defined(SDL_PLATFORM_ANDROID)
        m_ui.notify("Exportado a Descargas: " + name, Notice::Success, 4000);
#else
        m_ui.notify("Proyecto exportado a " + done.result, Notice::Success, 5000);
#endif
        break;
    }
    }
}

// -----------------------------------------------------------------------------
// Guardar
// -----------------------------------------------------------------------------

project::Document App::projectDocument() const {
    project::Document document = canvasproject::document(m_canvas);
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

uint64_t App::projectMeta() const {
    if (!m_canvas.ready()) {
        return 0;
    }
    uint64_t hash = mixHash(0, static_cast<uint64_t>(m_canvas.layers().activeIndex()));
    const bool fitted = !m_camera.userMoved() || m_fitAnimation.active;
    hash = mixHash(hash, fitted ? 1 : 0);
    hash = mixHash(hash, m_camera.flipped() ? 1 : 0);
    if (!fitted) {
        const Camera::Placement placement = m_camera.placement();
        hash = mixHash(hash, floatBits(placement.zoom));
        hash = mixHash(hash, floatBits(placement.center.x));
        hash = mixHash(hash, floatBits(placement.center.y));
        hash = mixHash(hash, floatBits(placement.angle));
    }
    float color[3];
    m_ui.brushColor(m_canvas, color);
    for (const float channel : color) {
        hash = mixHash(hash, floatBits(channel));
    }
    return hash;
}

bool App::projectUnsaved(bool meta) const {
    if (!m_canvas.ready() || m_open.loading) {
        return false;
    }
    if (m_project.import || m_canvas.documentVersion() != m_project.savedVersion) {
        return true;
    }
    return meta && m_project.stored && projectMeta() != m_project.savedMeta;
}

void App::startSave(SaveReason reason, bool full) {
    if (m_saving.active || m_projectSaver.busy() || !m_canvas.ready() || m_open.loading) {
        return;
    }
    // Un resultado sin recoger (de un guardado que ya no es de nadie) no se confunde con este.
    if (std::optional<project::Saver::Result> stale = m_projectSaver.takeResult()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Guardado sin recoger: %s", stale->path.c_str());
    }
    m_saving = {};
    m_saving.active = true;
    m_saving.reason = reason;
    m_saving.full = full;
    m_saving.version = m_canvas.documentVersion();
    m_saving.meta = projectMeta();
    m_saving.generation = m_canvasGeneration;

    project::Saver::Result failure;
    if (!m_library.ready()) {
        failure.error = "no hay dónde guardar proyectos en este dispositivo";
    } else if (m_project.path.empty()) {
        m_project.path = m_library.newPath();
        if (m_project.path.empty()) {
            failure.error = "no se pudo crear el archivo del proyecto";
        }
    }
    if (!failure.error.empty()) {
        onProjectSaved(failure);
        return;
    }
    project::Target target;
    target.path = m_project.path;
    const canvasproject::Record previous = full ? canvasproject::Record() : m_project.record;
    if (canvasproject::save(m_canvas, projectDocument(), target, appName(), previous, m_projectSaver,
                            [] { appevents::push(appevents::projectSaved); }, &m_saving.pending)) {
        return;
    }
    // Ya terminó (no se pudo crear el archivo) o está terminando (no se pudo leer la GPU: el
    // resultado llega como el de cualquier guardado).
    if (m_projectSaver.busy()) {
        return;
    }
    std::optional<project::Saver::Result> result = m_projectSaver.takeResult();
    if (!result) {
        result = project::Saver::Result();
        result->path = m_project.path;
        result->error = "no se pudo leer el lienzo";
    }
    onProjectSaved(*result);
}

void App::onProjectSaved(const project::Saver::Result& result) {
    if (!m_saving.active) {
        return;
    }
    const SaveState saving = std::move(m_saving);
    m_saving = {};
    const bool current = saving.generation == m_canvasGeneration && m_canvas.ready();
    if (!result.ok) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo guardar %s: %s", result.path.c_str(),
                     result.error.c_str());
        // Se iba a copiar algo del archivo anterior: guardándolo entero puede salir.
        if (result.reused && !saving.full && current && !m_open.loading) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Se vuelve a guardar sin copiar nada del archivo anterior");
            startSave(saving.reason, true);
            return;
        }
        const std::string error = result.error.empty() ? std::string("no se pudo escribir el archivo") : result.error;
        // Solo se avisa del primer fallo seguido: después lo dice el botón Guardar.
        const bool firstFailure = !current || m_project.error.empty();
        if (current) {
            m_project.failedVersion = saving.version;
            m_project.error = error;
        }
        switch (saving.reason) {
        case SaveReason::Auto:
            if (firstFailure) {
                m_ui.notify("No se pudo guardar el proyecto: " + error, Notice::Error, 6000);
            }
            break;
        case SaveReason::User:
        case SaveReason::Export:
            m_ui.dismissNotice(Notice::Progress);
            m_ui.showAlert("No se pudo guardar el proyecto", sentence(error), Notice::Error);
            break;
        case SaveReason::Leave:
            if (m_leave.action != Leave::None) {
                const std::string name = m_canvas.ready() ? m_canvas.info().name : std::string();
                const char* confirm = "Continuar";
                if (m_leave.action == Leave::Quit) {
                    confirm = "Salir";
                } else if (m_leave.action == Leave::Open) {
                    confirm = "Abrir";
                }
                m_leave.asking = true;
                m_ui.askLeave("No se pudo guardar " + quoted(name, "el dibujo"),
                              sentence(error) + " Si continúas, se pierde lo que cambió desde la última vez que se "
                                                "guardó.",
                              confirm);
            }
            break;
        case SaveReason::Background:
            break;   // al volver, el botón Guardar lo dice
        }
        return;
    }
    if (current) {
        m_project.record = canvasproject::saved(saving.pending, result);
        m_project.savedVersion = saving.version;
        m_project.savedMeta = saving.meta;
        m_project.stored = true;
        m_project.failedVersion = kNoVersion;
        m_project.error.clear();
        if (m_project.import) {
            // Ya está en la biblioteca: el archivo de fuera ya no hace falta.
            m_project.import = false;
            if (m_project.temporary) {
                removeTemporary(m_project.source);
            }
            m_project.source.clear();
            m_project.temporary = false;
        }
        setLastProject(m_project.path);
    }
    SDL_Log("Proyecto guardado en %s (%llu bytes; capas comprimidas %d, copiadas %d)", result.path.c_str(),
            static_cast<unsigned long long>(result.bytes), saving.pending.encodedLayers,
            saving.pending.copiedLayers);
    m_library.refresh();
    io::persist();
    if (saving.reason == SaveReason::User) {
        m_ui.notify("Guardado", Notice::Success, 2000);
    } else if (saving.reason == SaveReason::Export && current) {
        exportProject(m_project.path, m_canvas.info().name, m_exportAfterSave);
    }
}

void App::requestProjectSave() {
    if (!m_canvas.ready() || m_open.loading || m_leave.action != Leave::None || m_userSaveAtMs != 0) {
        return;
    }
    m_userSaveAtMs = SDL_GetTicks() + kSaveDelayMs;
    m_ui.notify("Guardando…", Notice::Progress, 600000);
}

void App::updateProjects() {
    // La biblioteca: lo que se lee de la carpeta (en la web, aquí) y lo que hicieron sus
    // operaciones.
    m_library.update(kLibraryBudgetMs);
    for (const library::Library::Done& done : m_library.takeDone()) {
        onLibraryDone(done);
    }
    // El guardado en marcha (en la web, se comprime aquí) y su resultado.
    if (m_projectSaver.busy()) {
        m_projectSaver.pump(8);
    }
    if (std::optional<project::Saver::Result> saved = m_projectSaver.takeResult()) {
        onProjectSaved(*saved);
    }
    if (m_leave.action != Leave::None) {
        stepLeave();
        return;
    }
    if (!m_canvas.ready() || m_open.loading) {
        return;
    }
    const uint64_t now = SDL_GetTicks();
    const uint64_t version = m_canvas.documentVersion();
    if (version != m_project.seenVersion) {
        m_project.seenVersion = version;
        m_project.changedMs = now;
    }
    if (m_saving.active || m_projectSaver.busy() || canvasInteractionActive() || m_canvas.stroking()) {
        return;
    }
    // Exportar esperaba a que terminara el guardado que estaba en marcha.
    if (m_exportCanvas != ExportTo::None) {
        const ExportTo to = m_exportCanvas;
        m_exportCanvas = ExportTo::None;
        exportCanvasProject(to);
        return;
    }
    // Guardar: lo que se ve (con lo que esté a medias aplicado). Sin nada nuevo, ya está.
    if (m_userSaveAtMs != 0) {
        if (now >= m_userSaveAtMs) {
            m_userSaveAtMs = 0;
            m_canvas.settle();
            if (projectUnsaved(true) || !m_project.stored) {
                startSave(SaveReason::User);
            } else {
                m_ui.notify("Guardado", Notice::Success, 2000);
            }
        }
        return;
    }
    // Solo: un momento después del último cambio (uno de fuera, enseguida). Lo que esté a
    // medias (una transformación, un ajuste) no se aplica: las capas aún no lo tienen.
    const bool due = m_project.import || now - m_project.changedMs >= kAutosaveDelayMs;
    if (due && version != m_project.failedVersion && projectUnsaved(false)) {
        startSave(SaveReason::Auto);
    }
}

uint64_t App::autosaveDeadline() const {
    if (!m_canvas.ready() || m_open.loading || m_leave.action != Leave::None || m_saving.active ||
        m_canvas.documentVersion() == m_project.failedVersion || !projectUnsaved(false)) {
        return 0;
    }
    return m_project.import ? SDL_GetTicks() : m_project.changedMs + kAutosaveDelayMs;
}

void App::saveBeforeSuspend() {
    // Lo que se estaba guardando termina (y, si hay que repetirlo entero, también).
    auto finish = [this] {
        while (m_saving.active || m_projectSaver.busy()) {
            m_projectSaver.wait();
            std::optional<project::Saver::Result> result = m_projectSaver.takeResult();
            if (!result) {
                break;
            }
            onProjectSaved(*result);
        }
    };
    finish();
    // Sin aplicar lo que esté a medias: al volver sigue como estaba.
    if (projectUnsaved(true)) {
        startSave(SaveReason::Background);
        finish();
    }
}

void App::checkStorage() {
    const std::string problem = io::storageProblem();
    if (problem.empty() || m_storageWarned) {
        return;
    }
    m_storageWarned = true;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Almacenamiento del navegador: %s", problem.c_str());
    m_ui.showAlert("Los proyectos no se guardan en este navegador",
                   sentence(problem) + " Mientras tanto puedes seguir dibujando y descargar el proyecto (Acciones → "
                                       "Compartir) para no perderlo.",
                   Notice::Warning);
}

// -----------------------------------------------------------------------------
// Dejar el lienzo
// -----------------------------------------------------------------------------

void App::requestLeave(Leave action, const CanvasSpec* spec) {
    if (m_leave.action != Leave::None || action == Leave::None) {
        return;
    }
    m_leave = {};
    m_leave.action = action;
    if (spec) {
        m_leave.spec = *spec;
    }
    m_leave.sinceMs = SDL_GetTicks();
    m_userSaveAtMs = 0;
    if (m_canvas.ready() && !m_open.loading) {
        // Se guarda lo que se ve.
        endGestures();
        m_canvas.settle();
    }
    stepLeave();
}

void App::stepLeave() {
    if (m_leave.action == Leave::None || m_leave.asking) {
        return;
    }
    // Primero termina el guardado que estuviera en marcha.
    if (m_saving.active || m_projectSaver.busy()) {
        return;
    }
    if (!m_leave.saved && projectUnsaved(true)) {
        m_leave.saved = true;
        startSave(SaveReason::Leave);
        return;
    }
    performLeave();
}

void App::performLeave() {
    const LeaveState leave = std::move(m_leave);
    m_leave = {};
    switch (leave.action) {
    case Leave::Projects:
        closeCanvas();
        break;
    case Leave::NewCanvas:
        createCanvas(leave.spec);
        break;
    case Leave::Open:
        startProjectOpen();
        break;
    case Leave::Quit:
        m_quit = true;
        break;
    case Leave::None:
        break;
    }
    m_redrawFrames = std::max(m_redrawFrames, 2);
}

void App::closeCanvas() {
    closeProjectFile();
    m_ndiResume = m_ndiResume || m_ndi.running();
    m_ndi.stop();
    endGestures();
    m_fitAnimation.active = false;
    m_canvas.destroy();
    resetProject();
    setLastProject({});
    m_library.refresh();
}

void App::resetProject() {
    if (m_project.temporary && !m_project.source.empty()) {
        removeTemporary(m_project.source);
    }
    m_project = {};
    ++m_canvasGeneration;
    m_userSaveAtMs = 0;
}

// -----------------------------------------------------------------------------
// Exportar
// -----------------------------------------------------------------------------

void App::exportProject(const std::string& path, const std::string& name, ExportTo to) {
    if (path.empty() || to == ExportTo::None) {
        return;
    }
    if (to == ExportTo::Choose) {
        // Donde se elija: el selector del sistema y, al volver, la copia (ver exportChosen).
        if (m_exportChoosing) {
            return;   // ya está abierto
        }
        m_ui.dismissNotice(Notice::Progress);
        m_exportChoosing = path;
#ifdef SDL_PLATFORM_ANDROID
        const std::string folder;   // Android empieza donde se quedó la última vez
#else
        const std::string folder = io::downloadsFolder();
#endif
        const bool shown = io::chooseSaveFile(m_window, folder, io::fileStem(name) + project::kExtension,
                                              [](std::string chosen, std::string error) {
                                                  if (!chosen.empty()) {
                                                      appevents::push(appevents::saveFileChosen, 0,
                                                                      SDL_strdup(chosen.c_str()));
                                                  } else if (error.empty()) {
                                                      appevents::push(appevents::saveFileChosen, 1);
                                                  } else {
                                                      appevents::push(appevents::saveFileChosen, -1,
                                                                      SDL_strdup(error.c_str()));
                                                  }
                                              });
        if (!shown) {
            m_exportChoosing.reset();
            m_ui.notify("No se pudo elegir dónde exportar el proyecto", Notice::Error, 5000);
        }
        return;
    }
    if (!storageReady(StorageTask::Export)) {
        m_pendingExport = {path, name};   // al llegar el permiso
        return;
    }
    project::Target target;
    target.folder = io::downloadsFolder();
    target.stem = io::fileStem(name);
    m_library.copy(path, target);
    m_ui.notify("Exportando…", Notice::Progress, 600000);
}

void App::exportChosen(const std::string& source, const std::string& chosen) {
    project::Target target;
    if (chosen.rfind("content://", 0) != 0 && !hasExtension(chosen, {"lvskt"})) {
        // El diálogo no le puso la extensión: se le pone, sin pisar ningún archivo que ya la
        // tenga (eso no se confirmó).
        const size_t slash = chosen.find_last_of("/\\");
        target.folder = slash == std::string::npos ? std::string() : chosen.substr(0, slash + 1);
        target.stem = chosen.substr(slash == std::string::npos ? 0 : slash + 1);
    } else {
        target.path = chosen;   // el diálogo ya preguntó si se sustituye
    }
    m_library.copy(source, target);
    m_ui.notify("Exportando…", Notice::Progress, 600000);
}

void App::exportCanvasProject(ExportTo to) {
    if (!m_canvas.ready() || m_open.loading || m_leave.action != Leave::None || to == ExportTo::None) {
        return;
    }
    // Se exporta lo que se ve: primero se guarda (si hay algo nuevo, o si aún no tiene archivo).
    m_canvas.settle();
    if (m_saving.active || m_projectSaver.busy()) {
        m_exportCanvas = to;   // al terminar el guardado en marcha
        return;
    }
    if (projectUnsaved(true) || !m_project.stored) {
        m_exportAfterSave = to;
        m_ui.notify("Guardando…", Notice::Progress, 600000);
        startSave(SaveReason::Export);
        return;
    }
    exportProject(m_project.path, m_canvas.info().name, to);
}

// -----------------------------------------------------------------------------
// Abrir
// -----------------------------------------------------------------------------

void App::chooseProject() {
    if (m_choosingFile || m_open.loader || m_leave.action != Leave::None) {
        return;
    }
#ifdef SDL_PLATFORM_EMSCRIPTEN
    liveSketchChooseFile(".lvskt");
#else
    static const SDL_DialogFileFilter kFilters[] = {{"Proyectos de LiveSketch (.lvskt)", "lvskt"}};
#ifdef SDL_PLATFORM_ANDROID
    const std::string folder;   // Android abre donde se quedó la última vez
#else
    const std::string folder = io::downloadsFolder();   // donde se exportan
#endif
    m_choosingFile = true;
    SDL_ShowOpenFileDialog(onProjectChosen, nullptr, m_window, kFilters, 1, folder.empty() ? nullptr : folder.c_str(),
                           false);
#endif
}

void App::fileChosen(std::string path) {
    const bool temporary = temporaryPath(path);
    // Ya se está abriendo otro (o se soltaron varios a la vez): este no.
    if (m_chosenFile || m_open.loader || m_leave.action != Leave::None) {
        if (temporary) {
            removeTemporary(path);
        }
        m_ui.notify("Ya se está abriendo un proyecto", Notice::Info, 2500);
        return;
    }
    m_chosenFile = ChosenFile{std::move(path), temporary};
}

void App::openProjectFile(const std::string& path, bool temporary) {
    if (m_open.loader || m_leave.action != Leave::None) {
        if (temporary) {
            removeTemporary(path);
        }
        return;
    }
    const bool library = m_library.contains(path);
    // Ya es el lienzo.
    if (library && m_canvas.ready() && path == m_project.path) {
        return;
    }
    const std::string name = fileName(path);
    auto loader = std::make_unique<project::Loader>();
    if (!loader->open(path, m_maxCanvasSize)) {
        const std::string error = loader->error();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No se pudo abrir %s: %s", path.c_str(), error.c_str());
        loader.reset();   // suelta el archivo antes de borrarlo
        if (temporary) {
            removeTemporary(path);
        }
        // Al arrancar no se vuelve a intentar.
        if (library && path.substr(m_library.folder().size()) == m_ui.lastProject()) {
            setLastProject({});
        }
        const library::Summary* item = library ? m_library.find(path) : nullptr;
        const std::string title = item && !item->name.empty() ? item->name : withoutExtension(name);
        const std::string reason = hasExtension(name, {"png", "jpg", "jpeg", "webp", "gif", "bmp"})
                                       ? "Es una imagen: por ahora solo se abren proyectos de LiveSketch (.lvskt)."
                                       : sentence(error);
        m_ui.showAlert("No se pudo abrir " + quoted(library ? title : name, "el proyecto"), reason, Notice::Error);
        return;
    }
    m_open.loader = std::move(loader);
    m_open.path = path;
    m_open.temporary = temporary;
    m_open.library = library;
    const std::string& canvasName = m_open.loader->document().info.name;
    m_open.title = !canvasName.empty() ? canvasName : (library ? std::string() : withoutExtension(name));
    // Primero se deja el lienzo de ahora (se guarda).
    if (m_canvas.ready()) {
        requestLeave(Leave::Open);
        return;
    }
    startProjectOpen();
}

void App::startProjectOpen() {
    if (!m_open.loader) {
        return;
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
    m_ndiResume = m_ndiResume || m_ndi.running();
    m_ndi.stop();
    endGestures();
    m_fitAnimation.active = false;
    const bool opened = m_canvas.open(spec, document.info, layers, document.activeLayer, document.guide);
    // Es otro lienzo (o ninguno, si no se pudo).
    resetProject();
    if (!opened) {
        const std::string message = "No hay memoria de gráficos para un lienzo de " + std::to_string(spec.width) +
                                    " × " + std::to_string(spec.height) + " px con " +
                                    std::to_string(layers.size()) + (layers.size() == 1 ? " capa." : " capas.");
        const std::string title = "No se pudo abrir " + quoted(m_open.title, "el proyecto");
        closeProjectFile();
        setLastProject({});
        m_ui.canvasCreated();
        m_ui.showAlert(title, message, Notice::Error);
        return;
    }
    // El proyecto del lienzo: el mismo archivo o, si es de fuera, uno nuevo de la biblioteca
    // que se guarda al terminar de abrirlo (copiando lo que se pueda del de fuera, que hasta
    // entonces es suyo: en la web, se borra después).
    if (m_open.library) {
        m_project.path = m_open.path;
        m_project.stored = true;
        setLastProject(m_open.path);
    } else {
        m_project.import = true;
        m_project.source = m_open.path;
        m_project.temporary = m_open.temporary;
        m_open.temporary = false;
        setLastProject({});
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
    m_project.savedVersion = m_canvas.documentVersion();
    m_open.version = m_canvas.documentVersion();
    m_open.loading = true;
    m_open.failedUploads = 0;
    m_open.layers.clear();
    m_open.layers.reserve(document.layers.size());
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
        // Vacía con su caja: estaba dañada (se abre vacía).
        bool loaded = layer.rect.empty() || !pixels.empty();
        if (!pixels.empty() && !m_canvas.setLayerPixels(index, layer.rect, pixels.data())) {
            ++m_open.failedUploads;
            loaded = false;
        }
        // Lo que tiene el archivo de la capa: al guardar se copia si no cambió.
        if (std::optional<canvasproject::SavedLayer> saved = canvasproject::openedLayer(m_canvas, loader, index, loaded)) {
            m_open.layers.push_back(std::move(*saved));
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
    project::Loader& loader = *m_open.loader;
    std::vector<std::string> warnings = loader.warnings();
    if (m_open.failedUploads > 0) {
        warnings.push_back(std::to_string(m_open.failedUploads) +
                           (m_open.failedUploads == 1 ? " capa no cabía" : " capas no cabían") +
                           " en la memoria de gráficos: se abren vacías.");
    }
    m_canvas.update();
    const bool complete = m_open.layers.size() == loader.document().layers.size() &&
                          m_canvas.documentVersion() == m_open.version;
    m_project.record = canvasproject::opened(m_canvas, loader, m_open.path, std::move(m_open.layers), complete);
    // Recién abierto tiene lo que el archivo (lo que no se pudo abrir sigue en el archivo hasta
    // que se cambie algo).
    m_project.savedVersion = m_canvas.documentVersion();
    m_project.savedMeta = projectMeta();
    m_project.seenVersion = m_project.savedVersion;
    m_project.changedMs = SDL_GetTicks();
    const std::string title = quoted(m_open.title, "el proyecto");
    const bool import = m_project.import;
    SDL_Log("Proyecto abierto: %s", m_open.path.c_str());
    closeProjectFile();
    if (m_ndiResume) {
        m_ndiResume = false;
        setNdiEnabled(true);
    }
    if (warnings.empty()) {
        // Uno de la biblioteca no necesita aviso: ya se ve.
        if (import) {
            m_ui.notify("Se abrió " + title + ": queda guardado en Proyectos", Notice::Success, 3500);
        }
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

bool App::reloadProject() {
    // Lo que se estaba abriendo vuelve a empezar; si no, el archivo del lienzo (lo que no se
    // guardó se perdió con el contexto: el archivo es lo más reciente que queda).
    std::string path;
    bool temporary = false;
    if (m_open.loading) {
        path = m_open.path;
        temporary = !m_open.library && m_project.temporary;
    } else if (m_project.import) {
        path = m_project.source;
        temporary = m_project.temporary;
    } else if (m_project.stored) {
        path = m_project.path;
    }
    if (path.empty()) {
        return false;
    }
    // El archivo de fuera pasa otra vez a lo que se abre (si no, se borraría con el proyecto).
    m_project.temporary = false;
    m_open.temporary = false;
    closeProjectFile();
    auto loader = std::make_unique<project::Loader>();
    if (!loader->open(path, m_maxCanvasSize)) {
        if (temporary) {
            removeTemporary(path);
        }
        return false;
    }
    m_open.loader = std::move(loader);
    m_open.path = path;
    m_open.temporary = temporary;
    m_open.library = m_library.contains(path);
    m_open.title = m_open.loader->document().info.name;
    startProjectOpen();
    return m_open.loading;
}
