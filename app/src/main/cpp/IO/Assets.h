#pragma once

#include <cstdint>
#include <vector>

namespace io {

// Lee un archivo de app/src/main/assets. En Android SDL lo busca en los assets del APK;
// en escritorio se lee de la carpeta del proyecto. Devuelve un vector vacío si falla.
std::vector<uint8_t> loadAsset(const char* name);

} // namespace io
