#include "IO/Assets.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

#include <string>

namespace io {

std::vector<uint8_t> loadAsset(const char* name) {
#ifdef LIVESKETCH_ASSETS_DIR
    const std::string path = std::string(LIVESKETCH_ASSETS_DIR) + "/" + name;
#else
    // Ruta relativa: SDL la resuelve contra los assets del APK.
    const std::string path = name;
#endif
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No se pudo abrir %s: %s", path.c_str(), SDL_GetError());
        return {};
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> result(bytes, bytes + size);
    SDL_free(data);
    return result;
}

} // namespace io
