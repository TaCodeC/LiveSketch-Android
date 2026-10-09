#include "IO/FileChooser.h"

#include <SDL3/SDL_platform_defines.h>

#if defined(SDL_PLATFORM_ANDROID)
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
#include <emscripten/em_asm.h>
#else
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#endif

#include <utility>

namespace io {
namespace {

#if defined(SDL_PLATFORM_ANDROID)

// La respuesta del selector llega desde Java (MainActivity.nativeSaveFileChosen).
std::mutex g_saveMutex;
SaveFileChosen g_saveDone;

// A quién avisar de otro «Abrir con» (MainActivity.nativeOpenRequested).
std::mutex g_launchMutex;
std::function<void()> g_launchListener;

bool isContentUri(const std::string& path) { return path.rfind("content://", 0) == 0; }

// Descarta la excepción de Java pendiente. Devuelve true si había una.
bool jniFailed(JNIEnv* env) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

// Ejecuta `function(env, activityClass)` dentro de un marco de referencias locales de JNI.
// `activityClass` es MainActivity (la clase de la actividad de SDL). Devuelve lo que devuelva
// `function`, o false si no se pudo llamar.
template <typename Function>
bool withActivityClass(Function&& function) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env || env->PushLocalFrame(16) != JNI_OK) {
        return false;
    }
    bool result = false;
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!jniFailed(env) && activity) {
        jclass activityClass = env->GetObjectClass(activity);
        if (!jniFailed(env) && activityClass) {
            result = function(env, activityClass);
        }
    }
    env->PopLocalFrame(nullptr);
    return result;
}

// El texto de un String de Java (vacío si es null). JNI lo da en su UTF-8 modificado, que
// para URIs (ASCII) y textos sin emojis es el mismo.
std::string javaText(JNIEnv* env, jstring string) {
    std::string text;
    if (string) {
        if (const char* chars = env->GetStringUTFChars(string, nullptr)) {
            text = chars;
            env->ReleaseStringUTFChars(string, chars);
        }
    }
    return text;
}

// Un byte[] de Java como texto (en Java, UTF-8 de verdad; vacío si es null). Para lo que puede
// llevar emojis: JNI no los lee bien en un String.
std::string javaBytes(JNIEnv* env, jbyteArray bytes) {
    std::string text;
    if (bytes) {
        const jsize length = env->GetArrayLength(bytes);
        text.resize(static_cast<size_t>(length));
        env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(text.data()));
        if (jniFailed(env)) {
            text.clear();
        }
    }
    return text;
}

// Un texto como byte[] de Java (null si no se pudo).
jbyteArray newBytes(JNIEnv* env, const std::string& text) {
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(text.size()));
    if (jniFailed(env) || !bytes) {
        return nullptr;
    }
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte*>(text.data()));
    return jniFailed(env) ? nullptr : bytes;
}

// MainActivity.closeDocument(fd, failed).
bool closeJavaDocument(int fd, bool failed) {
    return withActivityClass([fd, failed](JNIEnv* env, jclass activityClass) {
        jmethodID close = env->GetStaticMethodID(activityClass, "closeDocument", "(IZ)Z");
        if (jniFailed(env) || !close) {
            return false;
        }
        const jboolean ok = env->CallStaticBooleanMethod(activityClass, close, static_cast<jint>(fd),
                                                         failed ? JNI_TRUE : JNI_FALSE);
        return !jniFailed(env) && ok == JNI_TRUE;
    });
}

// Un documento de otra app abierto con MainActivity.openDocument. Se lee y escribe con una
// copia del descriptor; al cerrarlo, el de Java se cierra allí, que es lo que avisa a esa app.
struct Document {
    FILE* file = nullptr;
    int javaFd = -1;
    bool failed = false;   // no se pudo escribir algo
};

Sint64 SDLCALL documentSeek(void* userdata, Sint64 offset, SDL_IOWhence whence) {
    auto* document = static_cast<Document*>(userdata);
    int origin = SEEK_SET;
    if (whence == SDL_IO_SEEK_CUR) {
        origin = SEEK_CUR;
    } else if (whence == SDL_IO_SEEK_END) {
        origin = SEEK_END;
    }
    // SDL_TellIO no mueve nada.
    if (!(whence == SDL_IO_SEEK_CUR && offset == 0) &&
        fseeko(document->file, static_cast<off_t>(offset), origin) != 0) {
        SDL_SetError("no se pudo ir a otra parte del documento: %s", std::strerror(errno));
        return -1;
    }
    const off_t position = ftello(document->file);
    if (position < 0) {
        SDL_SetError("no se pudo saber por dónde va el documento: %s", std::strerror(errno));
        return -1;
    }
    return position;
}

size_t SDLCALL documentRead(void* userdata, void* ptr, size_t size, SDL_IOStatus* status) {
    auto* document = static_cast<Document*>(userdata);
    const size_t got = std::fread(ptr, 1, size, document->file);
    if (got < size) {
        if (std::ferror(document->file)) {
            *status = SDL_IO_STATUS_ERROR;
            SDL_SetError("no se pudo leer el documento: %s", std::strerror(errno));
        } else {
            *status = SDL_IO_STATUS_EOF;
        }
    }
    return got;
}

size_t SDLCALL documentWrite(void* userdata, const void* ptr, size_t size, SDL_IOStatus* status) {
    auto* document = static_cast<Document*>(userdata);
    const size_t put = std::fwrite(ptr, 1, size, document->file);
    if (put < size) {
        document->failed = true;
        *status = SDL_IO_STATUS_ERROR;
        SDL_SetError("no se pudo escribir el documento: %s", std::strerror(errno));
    }
    return put;
}

bool SDLCALL documentFlush(void* userdata, SDL_IOStatus* status) {
    auto* document = static_cast<Document*>(userdata);
    if (std::fflush(document->file) != 0) {
        document->failed = true;
        *status = SDL_IO_STATUS_ERROR;
        return SDL_SetError("no se pudo escribir el documento: %s", std::strerror(errno));
    }
    return true;
}

bool SDLCALL documentClose(void* userdata) {
    auto* document = static_cast<Document*>(userdata);
    bool ok = true;
    if (std::fclose(document->file) != 0) {
        document->failed = true;
        ok = SDL_SetError("no se pudo terminar de escribir el documento: %s", std::strerror(errno));
    }
    if (!closeJavaDocument(document->javaFd, document->failed) && ok) {
        ok = SDL_SetError("la app del documento no lo pudo guardar");
    }
    delete document;
    return ok;
}

SDL_IOStream* openDocument(const std::string& uri, const char* mode) {
    const bool write = std::strchr(mode, 'w') != nullptr;
    int javaFd = -1;
    // MainActivity.openDocument(byte[] uriUtf8, String modo)
    withActivityClass([&](JNIEnv* env, jclass activityClass) {
        jmethodID open = env->GetStaticMethodID(activityClass, "openDocument", "([BLjava/lang/String;)I");
        if (jniFailed(env) || !open) {
            return false;
        }
        jbyteArray bytes = newBytes(env, uri);
        jstring javaMode = bytes ? env->NewStringUTF(write ? "wt" : "r") : nullptr;
        if (jniFailed(env) || !javaMode) {
            return false;
        }
        const jint fd = env->CallStaticIntMethod(activityClass, open, bytes, javaMode);
        if (jniFailed(env)) {
            return false;
        }
        javaFd = fd;
        return true;
    });
    if (javaFd < 0) {
        SDL_SetError(write ? "no se pudo escribir en el documento" : "no se pudo leer el documento (puede que ya no "
                                                                     "haya permiso para abrirlo)");
        return nullptr;
    }
    const int fd = dup(javaFd);
    FILE* file = fd >= 0 ? fdopen(fd, write ? "wb" : "rb") : nullptr;
    if (!file) {
        const int error = errno;
        if (fd >= 0) {
            close(fd);
        }
        closeJavaDocument(javaFd, true);
        SDL_SetError("no se pudo abrir el documento: %s", std::strerror(error));
        return nullptr;
    }
    auto* document = new Document;
    document->file = file;
    document->javaFd = javaFd;
    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.seek = documentSeek;
    iface.read = documentRead;
    iface.write = documentWrite;
    iface.flush = documentFlush;
    iface.close = documentClose;
    SDL_IOStream* io = SDL_OpenIO(&iface, document);
    if (!io) {
        document->failed = true;
        documentClose(document);
    }
    return io;
}

#elif !defined(SDL_PLATFORM_EMSCRIPTEN)

// Respuesta del diálogo de guardar (puede llegar desde otro hilo).
void SDLCALL onSaveFileChosen(void* userdata, const char* const* files, int) {
    SaveFileChosen* done = static_cast<SaveFileChosen*>(userdata);
    if (!files) {
        const char* error = SDL_GetError();
        (*done)({}, error && *error ? error : "no se pudo mostrar el diálogo");
    } else {
        (*done)(files[0] ? std::string(files[0]) : std::string(), {});
    }
    delete done;
}

#endif

} // namespace

bool chooseSaveFile([[maybe_unused]] SDL_Window* window, [[maybe_unused]] const std::string& folder,
                    [[maybe_unused]] const std::string& name, [[maybe_unused]] SaveFileChosen done) {
#if defined(SDL_PLATFORM_ANDROID)
    {
        std::lock_guard<std::mutex> lock(g_saveMutex);
        g_saveDone = std::move(done);
    }
    // MainActivity.chooseSaveFile(byte[] nombreUtf8)
    const bool shown = withActivityClass([&name](JNIEnv* env, jclass activityClass) {
        jmethodID choose = env->GetStaticMethodID(activityClass, "chooseSaveFile", "([B)Z");
        if (jniFailed(env) || !choose) {
            return false;
        }
        jbyteArray bytes = newBytes(env, name);
        if (!bytes) {
            return false;
        }
        const jboolean ok = env->CallStaticBooleanMethod(activityClass, choose, bytes);
        return !jniFailed(env) && ok == JNI_TRUE;
    });
    if (!shown) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo mostrar el selector de documentos");
        std::lock_guard<std::mutex> lock(g_saveMutex);
        g_saveDone = nullptr;
    }
    return shown;
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    return false;
#else
    static const SDL_DialogFileFilter kFilters[] = {{"Proyectos de LiveSketch (.lvskt)", "lvskt"}};
    // La carpeta con el nombre propuesto: el diálogo empieza ahí y ya lo trae escrito.
    const std::string location = folder + name;
    SDL_ShowSaveFileDialog(onSaveFileChosen, new SaveFileChosen(std::move(done)), window, kFilters, 1,
                           location.c_str());
    return true;
#endif
}

std::string takeLaunchFile() {
#if defined(SDL_PLATFORM_ANDROID)
    std::string file;
    // MainActivity.takeOpenUri(): byte[] en UTF-8, o null
    withActivityClass([&file](JNIEnv* env, jclass activityClass) {
        jmethodID take = env->GetStaticMethodID(activityClass, "takeOpenUri", "()[B");
        if (jniFailed(env) || !take) {
            return false;
        }
        auto bytes = static_cast<jbyteArray>(env->CallStaticObjectMethod(activityClass, take));
        if (jniFailed(env)) {
            return false;
        }
        file = javaBytes(env, bytes);
        return true;
    });
    return file;
#else
    return {};
#endif
}

void setLaunchFileListener([[maybe_unused]] std::function<void()> listener) {
#if defined(SDL_PLATFORM_ANDROID)
    std::lock_guard<std::mutex> lock(g_launchMutex);
    g_launchListener = std::move(listener);
#endif
}

bool takeRestoredLaunch() {
#if defined(SDL_PLATFORM_ANDROID)
    bool restored = false;
    // MainActivity.takeRestored()
    withActivityClass([&restored](JNIEnv* env, jclass activityClass) {
        jmethodID take = env->GetStaticMethodID(activityClass, "takeRestored", "()Z");
        if (jniFailed(env) || !take) {
            return false;
        }
        const jboolean value = env->CallStaticBooleanMethod(activityClass, take);
        if (jniFailed(env)) {
            return false;
        }
        restored = value == JNI_TRUE;
        return true;
    });
    return restored;
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    static bool taken = false;
    if (taken) {
        return false;
    }
    taken = true;
    return EM_ASM_INT({ return document.wasDiscarded ? 1 : 0; }) != 0;
#else
    return false;
#endif
}

SDL_IOStream* openFile(const std::string& path, const char* mode) {
#if defined(SDL_PLATFORM_ANDROID)
    if (isContentUri(path)) {
        return openDocument(path, mode);
    }
#endif
    return SDL_IOFromFile(path.c_str(), mode);
}

bool removeDocument([[maybe_unused]] const std::string& uri) {
#if defined(SDL_PLATFORM_ANDROID)
    if (!isContentUri(uri)) {
        return false;
    }
    // MainActivity.removeDocument(byte[] uriUtf8)
    return withActivityClass([&uri](JNIEnv* env, jclass activityClass) {
        jmethodID remove = env->GetStaticMethodID(activityClass, "removeDocument", "([B)Z");
        if (jniFailed(env) || !remove) {
            return false;
        }
        jbyteArray bytes = newBytes(env, uri);
        if (!bytes) {
            return false;
        }
        const jboolean ok = env->CallStaticBooleanMethod(activityClass, remove, bytes);
        return !jniFailed(env) && ok == JNI_TRUE;
    });
#else
    return false;
#endif
}

} // namespace io

#if defined(SDL_PLATFORM_ANDROID)
// Respuesta de MainActivity.chooseSaveFile (desde el hilo de la interfaz de Android): la URI
// del documento que se creó, o null si se canceló (o si falló, con el motivo en `error`).
// Devuelve false si la app no la esperaba (entonces MainActivity borra el documento).
extern "C" JNIEXPORT jboolean JNICALL Java_com_tacodec_livesketch_MainActivity_nativeSaveFileChosen(JNIEnv* env,
                                                                                                  jclass,
                                                                                                  jstring uri,
                                                                                                  jstring error) {
    io::SaveFileChosen done;
    {
        std::lock_guard<std::mutex> lock(io::g_saveMutex);
        done = std::move(io::g_saveDone);
        io::g_saveDone = nullptr;
    }
    if (!done) {
        return JNI_FALSE;
    }
    done(io::javaText(env, uri), io::javaText(env, error));
    return JNI_TRUE;
}

// Con la app ya abierta, se pidió abrir otro documento desde otra app (MainActivity.onNewIntent).
extern "C" JNIEXPORT void JNICALL Java_com_tacodec_livesketch_MainActivity_nativeOpenRequested(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(io::g_launchMutex);
    if (io::g_launchListener) {
        io::g_launchListener();
    }
}
#endif
