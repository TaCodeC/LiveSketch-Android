#include "IO/ImageExport.h"

#include "Gfx/Pixels.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>

#if defined(SDL_PLATFORM_ANDROID)
#include <SDL3/SDL_system.h>
#include <jni.h>
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
#include <emscripten/em_js.h>
#endif

#include <cstdio>
#include <cstring>
#include <system_error>

#ifdef SDL_PLATFORM_EMSCRIPTEN
// Descarga en el navegador un archivo del sistema de archivos en memoria de Emscripten. La
// página puede encargarse ella misma definiendo Module.saveFile(blob, nombre), que devuelve
// true si lo hizo (por ejemplo, en un visor que no deja descargar con un enlace).
EM_JS(void, liveSketchDownload, (const char* path, const char* name, const char* mimeType), {
    const blob = new Blob([FS.readFile(UTF8ToString(path))], {type: UTF8ToString(mimeType)});
    const fileName = UTF8ToString(name);
    if (Module['saveFile'] && Module['saveFile'](blob, fileName)) {
        return;
    }
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = fileName;
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
});
#endif

namespace io {
namespace {

[[maybe_unused]] std::string withTrailingSlash(std::string folder) {
    if (!folder.empty() && folder.back() != '/') {
        folder += '/';
    }
    return folder;
}

#ifdef SDL_PLATFORM_ANDROID

// Descarta la excepción de Java pendiente. Devuelve true si había una.
bool jniFailed(JNIEnv* env) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

// Ejecuta `function(env)` dentro de un marco de referencias locales de JNI.
template <typename Function>
void withJni(Function&& function) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env || env->PushLocalFrame(16) != JNI_OK) {
        return;
    }
    function(env);
    env->PopLocalFrame(nullptr);
}

// Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS).getAbsolutePath()
std::string androidDownloadsFolder(JNIEnv* env) {
    jclass environment = env->FindClass("android/os/Environment");
    if (jniFailed(env)) {
        return {};
    }
    jfieldID downloads = env->GetStaticFieldID(environment, "DIRECTORY_DOWNLOADS", "Ljava/lang/String;");
    if (jniFailed(env)) {
        return {};
    }
    jobject type = env->GetStaticObjectField(environment, downloads);
    jmethodID publicDirectory =
        env->GetStaticMethodID(environment, "getExternalStoragePublicDirectory", "(Ljava/lang/String;)Ljava/io/File;");
    if (jniFailed(env)) {
        return {};
    }
    jobject directory = env->CallStaticObjectMethod(environment, publicDirectory, type);
    if (jniFailed(env) || !directory) {
        return {};
    }
    jclass file = env->GetObjectClass(directory);
    jmethodID absolutePath = env->GetMethodID(file, "getAbsolutePath", "()Ljava/lang/String;");
    if (jniFailed(env)) {
        return {};
    }
    auto path = static_cast<jstring>(env->CallObjectMethod(directory, absolutePath));
    if (jniFailed(env) || !path) {
        return {};
    }
    std::string result;
    if (const char* chars = env->GetStringUTFChars(path, nullptr)) {
        result = chars;
        env->ReleaseStringUTFChars(path, chars);
    }
    return result;
}

// MediaScannerConnection.scanFile(activity, {path}, {mimeType}, null)
void androidScanFile(JNIEnv* env, const std::string& path, const char* mimeType) {
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (jniFailed(env) || !activity) {
        return;
    }
    jclass scanner = env->FindClass("android/media/MediaScannerConnection");
    if (jniFailed(env)) {
        return;
    }
    jmethodID scanFile = env->GetStaticMethodID(scanner, "scanFile",
                                                "(Landroid/content/Context;[Ljava/lang/String;[Ljava/lang/String;"
                                                "Landroid/media/MediaScannerConnection$OnScanCompletedListener;)V");
    if (jniFailed(env)) {
        return;
    }
    jclass stringClass = env->FindClass("java/lang/String");
    if (jniFailed(env)) {
        return;
    }
    jstring jpath = env->NewStringUTF(path.c_str());
    if (jniFailed(env) || !jpath) {
        return;
    }
    jstring jtype = env->NewStringUTF(mimeType);
    if (jniFailed(env) || !jtype) {
        return;
    }
    jobjectArray paths = env->NewObjectArray(1, stringClass, jpath);
    if (jniFailed(env) || !paths) {
        return;
    }
    jobjectArray types = env->NewObjectArray(1, stringClass, jtype);
    if (jniFailed(env) || !types) {
        return;
    }
    env->CallStaticVoidMethod(scanner, scanFile, activity, paths, types, nullptr);
    jniFailed(env);
}

#endif // SDL_PLATFORM_ANDROID

// Nombres de archivo de como mucho tantos bytes (sin la extensión ni el « (2)»).
constexpr size_t kMaxStem = 96;
// Nombres que se prueban antes de rendirse: «stem», «stem (2)»... «stem (999)».
constexpr int kMaxCopies = 999;

// Quita los espacios y puntos de los extremos (un nombre que empieza por punto queda oculto
// en Linux y Android, y Windows no admite que acabe en punto o espacio).
std::string trimStem(const std::string& text) {
    const size_t first = text.find_first_not_of(" .");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" .") - first + 1);
}

// Escribe con `produce` (que recibe adónde) en `io` y lo cierra. Si algo falla, borra el
// archivo a medias y deja el motivo en SDL_GetError().
template <typename Produce>
bool fillFile(SDL_IOStream* io, const std::string& path, Produce produce) {
    bool written = true;
    const png::Sink sink = [io, &written](const uint8_t* data, size_t size) {
        written = written && SDL_WriteIO(io, data, size) == size;
        return written;
    };
    const bool produced = produce(sink);
    std::string error = produced ? std::string() : std::string(SDL_GetError());
    if (!SDL_CloseIO(io) && error.empty()) {
        error = SDL_GetError();
    }
    if (error.empty()) {
        return true;
    }
    SDL_RemovePath(path.c_str());
    SDL_SetError("%s", error.c_str());
    return false;
}

} // namespace

std::string downloadsFolder() {
#if defined(SDL_PLATFORM_ANDROID)
    std::string folder;
    withJni([&folder](JNIEnv* env) { folder = androidDownloadsFolder(env); });
    if (folder.empty()) {
        folder = "/storage/emulated/0/Download";   // la ruta que usaba la versión anterior
    }
    SDL_CreateDirectory(folder.c_str());
    return withTrailingSlash(folder);
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    return "/tmp/";
#else
    if (const char* downloads = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS)) {
        if (SDL_CreateDirectory(downloads)) {
            return withTrailingSlash(downloads);
        }
    }
    std::string folder;
    if (char* current = SDL_GetCurrentDirectory()) {
        folder = current;
        SDL_free(current);
    }
    return withTrailingSlash(folder);
#endif
}

std::string fileStem(const std::string& canvasName) {
    std::string stem;
    for (const char c : canvasName) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F || std::strchr("*?\"<>|", c)) {
            continue;
        }
        stem += (c == '/' || c == '\\' || c == ':') ? '-' : c;
    }
    stem = trimStem(stem);
    if (stem.size() > kMaxStem) {
        // Sin partir un carácter UTF-8 por la mitad.
        size_t cut = kMaxStem;
        while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        stem = trimStem(stem.substr(0, cut));
    }
    if (!stem.empty()) {
        return stem;
    }

    SDL_Time now = 0;
    SDL_DateTime date;
    SDL_zero(date);
    if (SDL_GetCurrentTime(&now)) {
        SDL_TimeToDateTime(now, &date, true);
    }
    char stamp[48];
    std::snprintf(stamp, sizeof(stamp), "LiveSketch_%04d%02d%02d_%02d%02d%02d", date.year, date.month, date.day,
                  date.hour, date.minute, date.second);
    return stamp;
}

SDL_IOStream* createFile(const std::string& folder, const std::string& stem, const char* extension,
                         std::string* path) {
    if (!SDL_GetPathInfo(folder.empty() ? "." : folder.c_str(), nullptr)) {
        return nullptr;   // sin la carpeta no sirve probar otros nombres
    }
    for (int n = 1; n <= kMaxCopies; ++n) {
        std::string candidate = folder + stem;
        if (n > 1) {
            candidate += " (" + std::to_string(n) + ")";
        }
        candidate += extension;
        // "x": solo si no existe, así no se pisa ninguno (ni uno que acabe de crear otro).
        // No se mira antes si existe: en Android los archivos que dejó otra app (o una
        // instalación anterior de esta) pueden no verse y aun así ocupar el nombre.
        if (SDL_IOStream* io = SDL_IOFromFile(candidate.c_str(), "wbx")) {
            *path = std::move(candidate);
            return io;
        }
    }
    *path = folder + stem + extension;
    return nullptr;
}

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height, const png::Info& info) {
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "wb");
    if (!io) {
        return false;
    }
    return fillFile(io, path, [&](const png::Sink& sink) {
        return png::encode(rgba, width, height, static_cast<size_t>(width) * 4, info, sink);
    });
}

void announceFile([[maybe_unused]] const std::string& path, [[maybe_unused]] const char* mimeType) {
#if defined(SDL_PLATFORM_ANDROID)
    withJni([&](JNIEnv* env) { androidScanFile(env, path, mimeType); });
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    const std::string name = path.substr(path.find_last_of('/') + 1);
    liveSketchDownload(path.c_str(), name.c_str(), mimeType);
    SDL_RemovePath(path.c_str());
#endif
}

// -----------------------------------------------------------------------------
// PngExporter
// -----------------------------------------------------------------------------

PngExporter::~PngExporter() {
    wait();
}

bool PngExporter::start(std::vector<uint8_t> premultiplied, int width, int height, std::string folder,
                        std::string stem, png::Info info, std::function<void()> onFinished) {
    const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (width <= 0 || height <= 0 || premultiplied.size() != expectedBytes) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_busy) {
            return false;
        }
        m_busy = true;
    }
    if (m_thread.joinable()) {
        m_thread.join();   // el guardado anterior ya terminó
    }

    auto save = [this, pixels = std::move(premultiplied), width, height, folder = std::move(folder),
                 stem = std::move(stem), info = std::move(info), onFinished = std::move(onFinished)]() mutable {
        Result result;
        gfx::unpremultiply(pixels.data(), pixels.size() / 4);
        std::vector<uint8_t> filtered = png::filterRows(pixels.data(), width, height, static_cast<size_t>(width) * 4);
        pixels = {};   // mientras se comprime, una imagen menos en memoria
        if (filtered.empty()) {
            result.error = "no hay memoria para comprimir la imagen";
        } else if (SDL_IOStream* io = createFile(folder, stem, ".png", &result.path)) {
            result.ok = fillFile(io, result.path,
                                 [&](const png::Sink& sink) { return png::write(filtered, width, height, info, sink); });
        }
        if (!result.ok && result.error.empty()) {
            result.error = SDL_GetError();
        }
        if (result.path.empty()) {
            result.path = folder + stem + ".png";
        }
        filtered = {};

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result = std::move(result);
            m_busy = false;
        }
        if (onFinished) {
            onFinished();
        }
    };
#ifdef SDL_PLATFORM_EMSCRIPTEN
    save();
#else
    try {
        m_thread = std::thread(std::move(save));
    } catch (const std::system_error&) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busy = false;
        return false;
    }
#endif
    return true;
}

bool PngExporter::busy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_busy;
}

std::optional<PngExporter::Result> PngExporter::takeResult() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::optional<Result> result = std::move(m_result);
    m_result.reset();
    return result;
}

void PngExporter::wait() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

} // namespace io
