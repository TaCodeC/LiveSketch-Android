#pragma once

#include "IO/Png.h"

#include <SDL3/SDL_iostream.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace io {

// Carpeta donde se guardan las imágenes, terminada en '/'. En Android es la carpeta
// pública Download; en escritorio, la de descargas del usuario o, si no hay, la actual;
// en la web, una carpeta en la memoria de la página (ver announceFile).
std::string downloadsFolder();

// Nombre de archivo (sin extensión) para un lienzo: su nombre sin lo que no admiten los
// sistemas de archivos (/ \ : pasan a guiones; * ? " < > | y los caracteres de control se
// quitan, y también los espacios y puntos de los extremos) o, si no queda nada, la fecha y
// hora local: LiveSketch_20260928_153012.
std::string fileStem(const std::string& canvasName);

// Crea en `folder` un archivo nuevo, `stem` + `extension`, sin pisar ninguno: si ya existe,
// «stem (2)», «stem (3)»... Deja la ruta en `path`. Null si no se pudo (motivo en
// SDL_GetError()).
SDL_IOStream* createFile(const std::string& folder, const std::string& stem, const char* extension,
                         std::string* path);

// Escribe un PNG RGBA8 sin premultiplicar, con la fila 0 arriba (si ya existe, lo
// sustituye). Si falla, borra el archivo a medias y deja el motivo en SDL_GetError().
bool writePng(const std::string& path, const uint8_t* rgba, int width, int height, const png::Info& info = {});

// Avisa al sistema de un archivo nuevo. En Android lo registra para que aparezca enseguida
// en Descargas y en la galería; en la web se lo pasa al navegador como descarga y lo borra
// de la memoria de la página. En escritorio no hace nada.
void announceFile(const std::string& path, const char* mimeType);

// Guarda el compuesto como PNG en un hilo aparte: quitar el premultiplicado y comprimir
// un lienzo grande tarda demasiado para el hilo de GL. En la web no hay hilos (harían
// falta cabeceras COOP/COEP en el servidor), así que se guarda dentro de start().
class PngExporter {
public:
    struct Result {
        bool ok = false;
        std::string path;    // el archivo que se creó (o el que se intentó crear)
        std::string error;   // motivo si falló
    };

    PngExporter() = default;
    ~PngExporter();
    PngExporter(const PngExporter&) = delete;
    PngExporter& operator=(const PngExporter&) = delete;

    // `premultiplied`: RGBA8 premultiplicado con la fila 0 arriba (Canvas::readComposite).
    // Se guarda en un archivo nuevo de `folder` llamado `stem` (ver createFile).
    // `onFinished` se llama desde el hilo de guardado al terminar. Devuelve false si ya
    // hay un guardado en curso.
    bool start(std::vector<uint8_t> premultiplied, int width, int height, std::string folder, std::string stem,
               png::Info info, std::function<void()> onFinished = {});
    bool busy() const;
    // Resultado del último guardado; se entrega una sola vez.
    std::optional<Result> takeResult();
    // Espera a que termine el guardado en curso.
    void wait();

private:
    std::thread m_thread;
    mutable std::mutex m_mutex;
    bool m_busy = false;
    std::optional<Result> m_result;
};

} // namespace io
