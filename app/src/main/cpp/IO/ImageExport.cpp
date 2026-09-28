#include "IO/ImageExport.h"

#include "Gfx/Pixels.h"
#include "ThirdParty/stb_image_write.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>

#ifdef SDL_PLATFORM_ANDROID
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

#include <cstdio>
#include <system_error>

namespace io {
namespace {

std::string withTrailingSlash(std::string folder) {
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

struct PngStream {
    SDL_IOStream* io = nullptr;
    bool ok = true;
};

void writeToStream(void* context, void* data, int size) {
    auto* stream = static_cast<PngStream*>(context);
    const auto bytes = static_cast<size_t>(size);
    if (stream->ok && SDL_WriteIO(stream->io, data, bytes) != bytes) {
        stream->ok = false;
    }
}

} // namespace

std::string downloadsFolder() {
#ifdef SDL_PLATFORM_ANDROID
    std::string folder;
    withJni([&folder](JNIEnv* env) { folder = androidDownloadsFolder(env); });
    if (folder.empty()) {
        folder = "/storage/emulated/0/Download";   // la ruta que usaba la versión anterior
    }
    SDL_CreateDirectory(folder.c_str());
    return withTrailingSlash(folder);
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

std::string timestampedPath(const std::string& folder, const char* prefix, const char* extension) {
    SDL_Time now = 0;
    SDL_DateTime date;
    SDL_zero(date);
    if (SDL_GetCurrentTime(&now)) {
        SDL_TimeToDateTime(now, &date, true);
    }
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", date.year, date.month, date.day, date.hour,
                  date.minute, date.second);

    const std::string base = folder + prefix + "_" + stamp;
    std::string path = base + extension;
    for (int n = 2; SDL_GetPathInfo(path.c_str(), nullptr) && n < 1000; ++n) {
        path = base + "_" + std::to_string(n) + extension;
    }
    return path;
}

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height) {
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "wb");
    if (!io) {
        return false;
    }
    PngStream stream{io, true};
    // stb arma el PNG entero en memoria y lo entrega de una vez.
    const bool encoded = stbi_write_png_to_func(writeToStream, &stream, width, height, 4, rgba, width * 4) != 0;

    std::string error;
    if (!encoded) {
        error = "no hay memoria para comprimir la imagen";
    } else if (!stream.ok) {
        error = SDL_GetError();
    }
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

void announceFile([[maybe_unused]] const std::string& path, [[maybe_unused]] const char* mimeType) {
#ifdef SDL_PLATFORM_ANDROID
    withJni([&](JNIEnv* env) { androidScanFile(env, path, mimeType); });
#endif
}

// -----------------------------------------------------------------------------
// PngExporter
// -----------------------------------------------------------------------------

PngExporter::~PngExporter() {
    wait();
}

bool PngExporter::start(std::vector<uint8_t> premultiplied, int width, int height, std::string path,
                        std::function<void()> onFinished) {
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

    auto save = [this, pixels = std::move(premultiplied), width, height, path = std::move(path),
                 onFinished = std::move(onFinished)]() mutable {
        gfx::unpremultiply(pixels.data(), pixels.size() / 4);
        Result result;
        result.ok = writePng(path, pixels.data(), width, height);
        if (!result.ok) {
            result.error = SDL_GetError();
        }
        result.path = std::move(path);
        pixels = {};

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_result = std::move(result);
            m_busy = false;
        }
        if (onFinished) {
            onFinished();
        }
    };
    try {
        m_thread = std::thread(std::move(save));
    } catch (const std::system_error&) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busy = false;
        return false;
    }
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
