#pragma once

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>

#include <functional>
#include <string>

// Lo que se elige o llega de fuera de la app: dónde exportar un archivo y, en Android, el
// documento con el que se abrió la app desde otra («Abrir con»), y cómo abrir esos documentos.
namespace io {

// Respuesta de chooseSaveFile, desde cualquier hilo: el archivo elegido (una ruta o, en
// Android, la URI de un documento); vacío si se canceló o si falló (el motivo, en `error`).
using SaveFileChosen = std::function<void(std::string path, std::string error)>;

// Elegir dónde guardar un archivo llamado `name` (con su extensión). En escritorio, el diálogo
// de guardar del sistema, que empieza en `folder`; en Android, el selector de documentos (que
// ya crea el archivo, vacío). En la web no hay: devuelve false y `done` no se llama.
bool chooseSaveFile(SDL_Window* window, const std::string& folder, const std::string& name, SaveFileChosen done);

// En Android, el documento que se pidió abrir desde otra app (su URI o, si era un archivo, su
// ruta), una sola vez; vacío si no hay. En el resto, siempre vacío.
std::string takeLaunchFile();

// En Android, `listener` se llama (desde el hilo de la interfaz de Android) cuando, con la app
// ya abierta, se pide abrir otro documento desde otra app: se recoge con takeLaunchFile.
void setLaunchFileListener(std::function<void()> listener);

// Como SDL_IOFromFile con "rb" o "wb". En Android, un documento de otra app (una URI
// content://) se abre a través de MainActivity: así, si ya no hay permiso, solo falla (con
// SDL no queda una excepción de Java pendiente), y al cerrarlo esa app se entera de que se
// terminó de escribir (por ejemplo, para subirlo a la nube).
SDL_IOStream* openFile(const std::string& path, const char* mode);

// Android: borra un documento de otra app (una URI content://), el que creó el selector para
// una exportación que falló. False si no se pudo (o si no es un documento).
bool removeDocument(const std::string& uri);

} // namespace io
