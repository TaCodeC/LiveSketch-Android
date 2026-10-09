#include "IO/Library.h"

#include "IO/FileChooser.h"
#include "IO/ImageExport.h"
#include "IO/Png.h"
#include "IO/Project.h"
#include "IO/Zip.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace library {
namespace {

std::span<const uint8_t> bytesOf(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

bool isContentUri(const std::string& path) { return path.rfind("content://", 0) == 0; }

// Lo que puede ocupar como mucho una entrada de un proyecto de este tamaño (un PNG del lienzo
// entero sin comprimir, o la descripción).
size_t entryLimit(int width, int height) {
    project::LayerInfo whole;
    whole.rect = IRect::ofSize(std::max(width, 1), std::max(height, 1));
    return std::max(project::maxLayerFile(whole), json::kMaxText);
}

// Abre el ZIP de un proyecto y lee su descripción.
struct OpenProject {
    zip::FileSource source;
    zip::Reader reader;
    project::Document document;
    std::string documentText;
};

bool openProject(const std::string& path, OpenProject& project, std::string* error) {
    if (!project.source.open(SDL_IOFromFile(path.c_str(), "rb"))) {
        *error = "no se pudo leer el archivo";
        return false;
    }
    if (!project.reader.open(project.source)) {
        *error = "no es un proyecto de LiveSketch o está dañado";
        return false;
    }
    const zip::Entry* entry = project.reader.find(project::kDocumentEntry);
    std::vector<uint8_t> text;
    if (!entry || !project.reader.read(*entry, text, json::kMaxText)) {
        *error = entry ? "su descripción (document.json) está dañada" : "no es un proyecto de LiveSketch";
        return false;
    }
    project.documentText.assign(text.begin(), text.end());
    std::vector<std::string> warnings;
    return project::readDocument(project.documentText, 0, project.document, warnings, *error);
}

} // namespace

// -----------------------------------------------------------------------------
// Archivos
// -----------------------------------------------------------------------------

Summary summarize(const std::string& path) {
    Summary summary;
    summary.path = path;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path.c_str(), &info)) {
        summary.bytes = info.size;
        summary.fileTime = info.modify_time;
    }
    summary.modified = summary.fileTime;
    OpenProject project;
    if (!openProject(path, project, &summary.error)) {
        return summary;
    }
    const project::Document& document = project.document;
    summary.readable = true;
    summary.name = document.info.name;
    summary.width = document.width;
    summary.height = document.height;
    summary.profile = document.info.profile;
    summary.layers = static_cast<int>(document.layers.size());
    if (document.info.modified != 0) {
        summary.modified = document.info.modified;
    }

    // La miniatura, si está y se lee bien (si no, la tarjeta va sin ella).
    const zip::Entry* entry = project.reader.find(project::kThumbnailEntry);
    project::LayerInfo small;
    small.rect = IRect::ofSize(project::kThumbnailSide, project::kThumbnailSide);
    std::vector<uint8_t> file;
    png::Image image;
    if (entry && project.reader.read(*entry, file, project::maxLayerFile(small)) &&
        png::decode(file, image, static_cast<size_t>(project::kThumbnailSide) * project::kThumbnailSide) &&
        image.width > 0 && image.height > 0) {
        summary.thumbnail = std::move(image.rgba);
        summary.thumbnailWidth = image.width;
        summary.thumbnailHeight = image.height;
    }
    return summary;
}

bool rewrite(const std::string& from, const std::string& to, const std::function<bool(json::Value&)>& edit,
             std::string* error) {
    const std::string temp = to + ".tmp";
    bool ok = false;
    {
        OpenProject project;
        if (!openProject(from, project, error)) {
            return false;
        }
        json::Value root;
        if (!json::parse(project.documentText, root, error) || !edit(root)) {
            if (error->empty()) {
                *error = "su descripción (document.json) no se puede cambiar";
            }
            return false;
        }
        const std::string document = json::write(root);
        // Lo cambiado tiene que seguir siendo una descripción que se abre.
        project::Document check;
        std::vector<std::string> warnings;
        if (!project::readDocument(document, 0, check, warnings, *error)) {
            return false;
        }

        SDL_IOStream* out = SDL_IOFromFile(temp.c_str(), "wb");
        if (!out) {
            *error = SDL_GetError();
            return false;
        }
        SDL_Time now = 0;
        SDL_GetCurrentTime(&now);
        zip::Writer writer([out](const uint8_t* data, size_t size) { return SDL_WriteIO(out, data, size) == size; },
                           now);
        const size_t limit = entryLimit(check.width, check.height);
        std::vector<uint8_t> data;
        ok = true;
        // Las entradas en su orden, cada una como estaba (la descripción, la última).
        for (const zip::Entry& entry : project.reader.entries()) {
            if (entry.name == project::kDocumentEntry) {
                ok = writer.add(entry.name, bytesOf(document), zip::Method::Deflate);
            } else if (!project.reader.read(entry, data, limit)) {
                *error = project.reader.error();
                ok = false;
                break;
            } else {
                ok = writer.add(entry.name, data, entry.method);
            }
            if (!ok) {
                if (error->empty()) {
                    *error = SDL_GetError();
                }
                break;
            }
        }
        if (ok && !writer.finish()) {
            *error = SDL_GetError();
            ok = false;
        }
        if (!SDL_CloseIO(out) && ok) {
            *error = SDL_GetError();
            ok = false;
        }
    }
    // El original ya está cerrado: se puede sustituir (aunque sea el mismo).
    if (ok && !SDL_RenamePath(temp.c_str(), to.c_str())) {
        *error = SDL_GetError();
        ok = false;
    }
    if (!ok) {
        SDL_RemovePath(temp.c_str());
    }
    return ok;
}

bool copyFile(const std::string& from, const project::Target& to, std::string* written, std::string* error) {
    SDL_IOStream* in = io::openFile(from, "rb");
    if (!in) {
        *error = "no se pudo leer el proyecto";
        return false;
    }
    std::string temp;
    SDL_IOStream* out = nullptr;
    if (!to.path.empty()) {
        *written = to.path;
        // Una URI de Android no se puede escribir al lado y cambiar de nombre.
        temp = isContentUri(to.path) ? std::string() : to.path + ".tmp";
        out = io::openFile(temp.empty() ? to.path : temp, "wb");
    } else {
        out = io::createFile(to.folder, to.stem, project::kExtension, written);
    }
    if (!out) {
        *error = SDL_GetError();
        SDL_CloseIO(in);
        // El documento lo creó el selector (vacío): no se queda.
        io::removeDocument(to.path);
        return false;
    }
    std::vector<uint8_t> buffer;
    bool ok = true;
    try {
        buffer.resize(size_t{1} << 20);
    } catch (const std::bad_alloc&) {
        *error = "no hay memoria para copiarlo";
        ok = false;
    }
    while (ok) {
        const size_t got = SDL_ReadIO(in, buffer.data(), buffer.size());
        if (got == 0) {
            if (SDL_GetIOStatus(in) != SDL_IO_STATUS_EOF) {
                *error = "no se pudo leer el proyecto";
                ok = false;
            }
            break;
        }
        if (SDL_WriteIO(out, buffer.data(), got) != got) {
            *error = SDL_GetError();
            ok = false;
        }
    }
    SDL_CloseIO(in);
    if (!SDL_CloseIO(out) && ok) {
        *error = SDL_GetError();
        ok = false;
    }
    if (ok && !temp.empty() && !SDL_RenamePath(temp.c_str(), to.path.c_str())) {
        *error = SDL_GetError();
        ok = false;
    }
    if (!ok) {
        if (!temp.empty()) {
            SDL_RemovePath(temp.c_str());
        } else if (to.path.empty()) {
            SDL_RemovePath(written->c_str());
        } else {
            io::removeDocument(to.path);   // a medias: no se queda
        }
    }
    return ok;
}

// -----------------------------------------------------------------------------
// La carpeta
// -----------------------------------------------------------------------------

// Una vuelta por la carpeta: lo que había (para no volver a leer lo que no cambió) y lo que
// se encontró.
struct Library::Scan {
    struct Known {
        std::string path;
        uint64_t bytes = 0;
        int64_t fileTime = 0;
    };
    struct File {
        std::string path;
        uint64_t bytes = 0;
        int64_t fileTime = 0;
        bool keep = false;   // no cambió: vale lo que ya se tenía
        Summary fresh;
    };
    std::vector<Known> known;
    std::vector<File> files;
    int pending = 0;         // archivos por leer
};

Library::~Library() { wait(); }

bool Library::open(const std::string& folder) {
    wait();
    m_folder.clear();
    m_items.clear();
    m_listed = false;
    if (folder.empty() || !SDL_CreateDirectory(folder.c_str())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo crear la carpeta de proyectos %s: %s", folder.c_str(),
                     SDL_GetError());
        return false;
    }
    m_folder = folder.back() == '/' ? folder : folder + '/';
    // Lo que dejó un guardado que no llegó a terminar (la app se cerró a medias).
    int count = 0;
    if (char** names = SDL_GlobDirectory(m_folder.c_str(), "*.tmp", 0, &count)) {
        for (int i = 0; i < count; ++i) {
            SDL_RemovePath((m_folder + names[i]).c_str());
        }
        SDL_free(names);
    }
    if (!m_pool) {
        // Uno solo: las operaciones van en orden.
        m_pool = std::make_unique<io::TaskPool>(1);
    }
    return true;
}

std::string Library::newPath() const {
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    SDL_DateTime date;
    SDL_zero(date);
    SDL_TimeToDateTime(now, &date, true);
    for (int attempt = 0; attempt < 1000; ++attempt) {
        char name[64];
        SDL_snprintf(name, sizeof(name), "%04d%02d%02d-%02d%02d%02d-%06x", date.year, date.month, date.day, date.hour,
                     date.minute, date.second, static_cast<unsigned>(SDL_rand_bits() & 0xFFFFFF));
        const std::string path = m_folder + name + project::kExtension;
        if (!SDL_GetPathInfo(path.c_str(), nullptr) && !busy(path)) {
            return path;
        }
    }
    return {};
}

bool Library::contains(const std::string& path) const {
    if (m_folder.empty() || path.size() <= m_folder.size() || path.compare(0, m_folder.size(), m_folder) != 0) {
        return false;
    }
    const std::string_view name = std::string_view(path).substr(m_folder.size());
    const std::string_view extension = project::kExtension;
    return name.find('/') == std::string_view::npos && name.size() > extension.size() &&
           name.substr(name.size() - extension.size()) == extension;
}

const Summary* Library::find(const std::string& path) const {
    for (const Summary& item : m_items) {
        if (item.path == path) {
            return &item;
        }
    }
    return nullptr;
}

void Library::refresh() {
    if (!ready()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scan) {
            m_rescan = true;
            return;
        }
    }
    startScan();
}

void Library::startScan() {
    auto scan = std::make_shared<Scan>();
    scan->known.reserve(m_items.size());
    for (const Summary& item : m_items) {
        scan->known.push_back({item.path, item.bytes, item.fileTime});
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_scan = scan;
        m_rescan = false;
    }
    m_pool->submit([this, scan] {
        int count = 0;
        char** names = SDL_GlobDirectory(m_folder.c_str(), (std::string("*") + project::kExtension).c_str(), 0, &count);
        for (int i = 0; names && i < count; ++i) {
            Scan::File file;
            file.path = m_folder + names[i];
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(file.path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) {
                continue;
            }
            file.bytes = info.size;
            file.fileTime = info.modify_time;
            file.keep = std::any_of(scan->known.begin(), scan->known.end(), [&file](const Scan::Known& known) {
                return known.path == file.path && known.bytes == file.bytes && known.fileTime == file.fileTime;
            });
            scan->files.push_back(std::move(file));
        }
        SDL_free(names);
        // Cada archivo nuevo o cambiado se lee aparte (en la web, uno por frame como mucho).
        std::vector<size_t> toRead;
        for (size_t i = 0; i < scan->files.size(); ++i) {
            if (!scan->files[i].keep) {
                toRead.push_back(i);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            scan->pending = static_cast<int>(toRead.size());
            if (toRead.empty()) {
                m_scanned = true;
            }
        }
        if (toRead.empty()) {
            changed();
            return;
        }
        for (const size_t i : toRead) {
            m_pool->submit([this, scan, i] {
                scan->files[i].fresh = summarize(scan->files[i].path);
                bool last = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    last = --scan->pending == 0;
                    if (last) {
                        m_scanned = true;
                    }
                }
                if (last) {
                    changed();
                }
            });
        }
    });
}

bool Library::update(uint64_t budgetMs) {
    if (!ready()) {
        return false;
    }
    m_pool->runPending(budgetMs);
    std::shared_ptr<Scan> finished;
    bool again = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_scanned) {
            m_scanned = false;
            finished = std::move(m_scan);
            m_scan.reset();
        }
        again = m_rescan && !m_scan;
    }
    bool changed = false;
    if (finished) {
        std::vector<Summary> items;
        items.reserve(finished->files.size());
        for (Scan::File& file : finished->files) {
            if (!file.keep) {
                items.push_back(std::move(file.fresh));
                continue;
            }
            auto old = std::find_if(m_items.begin(), m_items.end(),
                                    [&file](const Summary& item) { return item.path == file.path; });
            items.push_back(old != m_items.end() ? std::move(*old) : summarize(file.path));
        }
        // El último que cambió, primero.
        std::sort(items.begin(), items.end(), [](const Summary& a, const Summary& b) {
            if (a.modified != b.modified) {
                return a.modified > b.modified;
            }
            if (a.fileTime != b.fileTime) {
                return a.fileTime > b.fileTime;
            }
            return a.path < b.path;
        });
        m_items = std::move(items);
        m_listed = true;
        changed = true;
    }
    if (again) {
        startScan();
    }
    return changed;
}

bool Library::busy(const std::string& path) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::find(m_busy.begin(), m_busy.end(), path) != m_busy.end();
}

bool Library::working() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_scan != nullptr || m_rescan || m_jobs > 0;
}

void Library::run(Done done, std::function<bool(Done&)> work) {
    if (!ready()) {
        done.error = "no hay carpeta de proyectos";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_done.push_back(std::move(done));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busy.push_back(done.path);
        if (!done.result.empty()) {
            m_busy.push_back(done.result);
        }
        ++m_jobs;
    }
    m_pool->submit([this, done = std::move(done), work = std::move(work)]() mutable {
        done.ok = work(done);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const std::string* path : {&done.path, &done.result}) {
                auto it = std::find(m_busy.begin(), m_busy.end(), *path);
                if (!path->empty() && it != m_busy.end()) {
                    m_busy.erase(it);
                }
            }
            --m_jobs;
            m_done.push_back(std::move(done));
            m_rescan = true;   // la lista cambió
        }
        changed();
    });
}

void Library::rename(const std::string& path, std::string name) {
    Done done;
    done.kind = Done::Kind::Rename;
    done.path = path;
    run(std::move(done), [path, name = project::cleanName(name)](Done& result) {
        return rewrite(path, path,
                       [&name](json::Value& root) {
                           json::Value* canvas = root.member("canvas");
                           json::Value* field = canvas ? canvas->member("name") : nullptr;
                           if (!field) {
                               return false;
                           }
                           *field = json::Value::text(name);
                           return true;
                       },
                       &result.error);
    });
}

void Library::duplicate(const std::string& path, std::string name) {
    Done done;
    done.kind = Done::Kind::Duplicate;
    done.path = path;
    done.result = newPath();
    if (done.result.empty()) {
        done.error = "no se pudo crear otro archivo";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_done.push_back(std::move(done));
        return;
    }
    run(std::move(done), [path, name = project::cleanName(name)](Done& result) {
        SDL_Time now = 0;
        SDL_GetCurrentTime(&now);
        return rewrite(path, result.result,
                       [&name, now](json::Value& root) {
                           json::Value* canvas = root.member("canvas");
                           json::Value* field = canvas ? canvas->member("name") : nullptr;
                           json::Value* modified = canvas ? canvas->member("modified") : nullptr;
                           if (!field || !modified) {
                               return false;
                           }
                           *field = json::Value::text(name);
                           *modified = json::Value::text(project::isoTime(now));
                           return true;
                       },
                       &result.error);
    });
}

void Library::remove(const std::string& path) {
    Done done;
    done.kind = Done::Kind::Remove;
    done.path = path;
    run(std::move(done), [path](Done& result) {
        if (!SDL_RemovePath(path.c_str())) {
            result.error = SDL_GetError();
            return false;
        }
        return true;
    });
}

void Library::copy(const std::string& path, const project::Target& to) {
    Done done;
    done.kind = Done::Kind::Copy;
    done.path = path;
    run(std::move(done), [path, to](Done& result) { return copyFile(path, to, &result.result, &result.error); });
}

std::vector<Library::Done> Library::takeDone() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Done> done = std::move(m_done);
    m_done.clear();
    return done;
}

void Library::wait() {
    if (m_pool) {
        m_pool->waitIdle();
    }
}

} // namespace library
