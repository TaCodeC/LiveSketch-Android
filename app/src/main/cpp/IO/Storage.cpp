#include "IO/Storage.h"

#include <SDL3/SDL_platform_defines.h>

#include <utility>

#ifdef SDL_PLATFORM_EMSCRIPTEN
#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>

#include <cstdlib>

// La copia la hace la página (src/web/storage.js): aquí solo se le avisa. (En EM_JS, nada de
// barras invertidas ni de comillas simples: no llegan bien a JavaScript.)
EM_JS(void, liveSketchPersist, (), {
    if (Module['liveSketchPersist']) {
        Module['liveSketchPersist']();
    }
});

EM_JS_DEPS(livesketch_storage, "$stringToNewUTF8");
EM_JS(char*, liveSketchStorageProblemText, (), {
    const storage = Module['liveSketchStorage'];
    const text = storage && storage.problem ? String(storage.problem) : "";
    return text ? stringToNewUTF8(text) : 0;
});

namespace {
std::function<void()> g_storageListener;
}

// La página no pudo copiar los datos al navegador (src/web/storage.js).
extern "C" EMSCRIPTEN_KEEPALIVE void liveSketchStorageProblem() {
    if (g_storageListener) {
        g_storageListener();
    }
}
#endif

namespace io {

void persist() {
#ifdef SDL_PLATFORM_EMSCRIPTEN
    liveSketchPersist();
#endif
}

std::string storageProblem() {
#ifdef SDL_PLATFORM_EMSCRIPTEN
    std::string problem;
    if (char* text = liveSketchStorageProblemText()) {
        problem = text;
        std::free(text);
    }
    return problem;
#else
    return {};
#endif
}

void setStorageListener([[maybe_unused]] std::function<void()> listener) {
#ifdef SDL_PLATFORM_EMSCRIPTEN
    g_storageListener = std::move(listener);
#endif
}

} // namespace io
