#include "App/DisplayGamut.h"

#include <SDL3/SDL_properties.h>

#if defined(SDL_PLATFORM_ANDROID)
#include <SDL3/SDL_system.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
#include <emscripten/em_js.h>
#endif

#ifdef SDL_PLATFORM_EMSCRIPTEN
// La pantalla muestra P3 y el navegador deja elegir el espacio de color de WebGL (Chrome
// 104 y Safari 16.4 en adelante).
EM_JS(int, liveSketchP3Available, (), {
    try {
        const gl = GL.currentContext && GL.currentContext.GLctx;
        if (!gl || !('drawingBufferColorSpace' in gl) || !window.matchMedia) {
            return 0;
        }
        Module['liveSketchP3Query'] = Module['liveSketchP3Query'] || window.matchMedia('(color-gamut: p3)');
        return Module['liveSketchP3Query'].matches ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

// Pone el lienzo de WebGL en Display P3 (1) o en sRGB (0). Devuelve 1 si queda en P3.
EM_JS(int, liveSketchSetP3, (int on), {
    try {
        const gl = GL.currentContext && GL.currentContext.GLctx;
        if (!gl || !('drawingBufferColorSpace' in gl)) {
            return 0;
        }
        const space = on ? 'display-p3' : 'srgb';
        if (gl.drawingBufferColorSpace !== space) {
            gl.drawingBufferColorSpace = space;
        }
        return gl.drawingBufferColorSpace === 'display-p3' ? 1 : 0;
    } catch (e) {
        return 0;
    }
});
#endif

namespace {

#ifdef SDL_PLATFORM_ANDROID

// ADataSpace (android/data_space.h): desconocido (lo que pone EGL; se ve como sRGB) y
// Display P3 (primarios DCI-P3, blanco D65 y la curva de sRGB).
constexpr int32_t kDataSpaceUnknown = 0;
constexpr int32_t kDataSpaceDisplayP3 = 143261696;

// Mensaje para MainActivity.onUnhandledMessage (SDLActivity.COMMAND_USER + 1): ventana de
// gama amplia (1) o normal (0).
constexpr Uint32 kCommandColorMode = 0x8000 + 1;

// ANativeWindow_setBuffersDataSpace y ANativeWindow_getBuffersDataSpace son de Android 9 y
// la app se instala desde Android 8: se buscan al usarlas.
struct DataSpaceApi {
    int32_t (*set)(ANativeWindow*, int32_t) = nullptr;
    int32_t (*get)(ANativeWindow*) = nullptr;
};

const DataSpaceApi& dataSpaceApi() {
    static const DataSpaceApi api = [] {
        DataSpaceApi result;
        if (void* library = dlopen("libandroid.so", RTLD_NOW)) {
            result.set = reinterpret_cast<int32_t (*)(ANativeWindow*, int32_t)>(
                dlsym(library, "ANativeWindow_setBuffersDataSpace"));
            result.get = reinterpret_cast<int32_t (*)(ANativeWindow*)>(
                dlsym(library, "ANativeWindow_getBuffersDataSpace"));
        }
        if (!result.set || !result.get) {
            result = {};
        }
        return result;
    }();
    return api;
}

// Descarta la excepción de Java pendiente. Devuelve true si había una.
bool jniFailed(JNIEnv* env) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

// getResources().getConfiguration().isScreenWideColorGamut() de la actividad.
bool queryWideColorGamut(JNIEnv* env) {
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (jniFailed(env) || !activity) {
        return false;
    }
    jmethodID getResources =
        env->GetMethodID(env->GetObjectClass(activity), "getResources", "()Landroid/content/res/Resources;");
    if (jniFailed(env)) {
        return false;
    }
    jobject resources = env->CallObjectMethod(activity, getResources);
    if (jniFailed(env) || !resources) {
        return false;
    }
    jmethodID getConfiguration = env->GetMethodID(env->GetObjectClass(resources), "getConfiguration",
                                                  "()Landroid/content/res/Configuration;");
    if (jniFailed(env)) {
        return false;
    }
    jobject configuration = env->CallObjectMethod(resources, getConfiguration);
    if (jniFailed(env) || !configuration) {
        return false;
    }
    jmethodID isWide = env->GetMethodID(env->GetObjectClass(configuration), "isScreenWideColorGamut", "()Z");
    if (jniFailed(env)) {
        return false;
    }
    const jboolean wide = env->CallBooleanMethod(configuration, isWide);
    return !jniFailed(env) && wide == JNI_TRUE;
}

bool screenWideColorGamut() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env || env->PushLocalFrame(16) != JNI_OK) {
        return false;
    }
    const bool wide = queryWideColorGamut(env);
    env->PopLocalFrame(nullptr);
    return wide;
}

#endif // SDL_PLATFORM_ANDROID

} // namespace

ColorProfile DisplayGamut::sync(SDL_Window* window, ColorProfile wanted) {
#if defined(SDL_PLATFORM_ANDROID)
    const DataSpaceApi& api = dataSpaceApi();
    if (!m_checked) {
        m_available = api.set && screenWideColorGamut();
        m_checked = true;
        m_windowWide = -1;   // se vuelve a mandar
    }
    auto* nativeWindow = static_cast<ANativeWindow*>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    const bool wide = wanted == ColorProfile::DisplayP3 && m_available && nativeWindow;
    if (m_windowWide != (wide ? 1 : 0)) {
        // La actividad en gama amplia: el sistema pone la pantalla en P3.
        SDL_SendAndroidMessage(kCommandColorMode, wide ? 1 : 0);
        m_windowWide = wide ? 1 : 0;
    }
    m_profile = ColorProfile::Srgb;
    if (nativeWindow && api.get) {
        // EGL vuelve a poner el espacio desconocido cada vez que se crea la superficie (al
        // volver a primer plano): se comprueba en cada frame.
        const int32_t current = api.get(nativeWindow);
        if (wide && current != kDataSpaceDisplayP3) {
            api.set(nativeWindow, kDataSpaceDisplayP3);
        } else if (!wide && current == kDataSpaceDisplayP3) {
            api.set(nativeWindow, kDataSpaceUnknown);
        }
        if (api.get(nativeWindow) == kDataSpaceDisplayP3) {
            m_profile = ColorProfile::DisplayP3;
        }
    }
#elif defined(SDL_PLATFORM_EMSCRIPTEN)
    (void)window;
    // La ventana puede pasar a otra pantalla: con un lienzo P3 se mira en cada frame.
    if (!m_checked || wanted == ColorProfile::DisplayP3) {
        m_available = liveSketchP3Available() != 0;
        m_checked = true;
    }
    const bool wide = wanted == ColorProfile::DisplayP3 && m_available;
    if (wide != (m_profile == ColorProfile::DisplayP3)) {
        m_profile = liveSketchSetP3(wide ? 1 : 0) != 0 ? ColorProfile::DisplayP3 : ColorProfile::Srgb;
    }
#else
    // Escritorio: la ventana es sRGB.
    (void)window;
    (void)wanted;
    m_checked = true;
    m_available = false;
    m_profile = ColorProfile::Srgb;
#endif
    return m_profile;
}
