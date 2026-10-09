#pragma once

#include <functional>
#include <string>

// Los datos de la app (proyectos, preferencias, pinceles y tamaños) en la carpeta de datos
// de SDL (SDL_GetPrefPath). En Android y en escritorio esa carpeta está en el disco; en la
// web vive en la memoria de la página y se copia al almacenamiento del navegador
// (IndexedDB), así que sobrevive a recargar la página (ver src/web/storage.js).
namespace io {

// Después de escribir en la carpeta de datos: en la web, la copia al navegador se hace un
// momento después (varias llamadas seguidas hacen una sola copia). En el resto no hace nada.
void persist();

// En la web, por qué el navegador no guarda los datos de la app (vacío si los guarda). Sin
// eso la app funciona igual, pero lo que guarda se pierde al cerrar la página. En el resto,
// siempre vacío.
std::string storageProblem();

// En la web, `listener` se llama cuando aparece un problema al copiar al navegador. Se llama
// desde fuera del bucle de la app: solo debe despertarlo. En el resto no se llama nunca.
void setStorageListener(std::function<void()> listener);

} // namespace io
