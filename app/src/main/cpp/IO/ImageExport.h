#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace io {

// Carpeta donde se guardan las imágenes, terminada en '/'. En Android es la carpeta
// pública Download; en escritorio, la de descargas del usuario o, si no hay, la actual.
std::string downloadsFolder();

// Ruta libre en `folder` con la fecha y hora local: LiveSketch_20260928_153012.png o, si
// ya existe, LiveSketch_20260928_153012_2.png, _3...
std::string timestampedPath(const std::string& folder, const char* prefix, const char* extension);

// Escribe un PNG RGBA8 sin premultiplicar, con la fila 0 arriba. Si falla, borra el
// archivo a medias y deja el motivo en SDL_GetError().
bool writePng(const std::string& path, const uint8_t* rgba, int width, int height);

// Avisa al sistema de un archivo nuevo para que aparezca enseguida en Descargas y en la
// galería. Solo hace algo en Android.
void announceFile(const std::string& path, const char* mimeType);

// Guarda el compuesto como PNG en un hilo aparte: quitar el premultiplicado y comprimir
// un lienzo grande tarda demasiado para el hilo de GL.
class PngExporter {
public:
    struct Result {
        bool ok = false;
        std::string path;
        std::string error;   // motivo si falló
    };

    PngExporter() = default;
    ~PngExporter();
    PngExporter(const PngExporter&) = delete;
    PngExporter& operator=(const PngExporter&) = delete;

    // `premultiplied`: RGBA8 premultiplicado con la fila 0 arriba (Canvas::readComposite).
    // `onFinished` se llama desde el hilo de guardado al terminar. Devuelve false si ya
    // hay un guardado en curso.
    bool start(std::vector<uint8_t> premultiplied, int width, int height, std::string path,
               std::function<void()> onFinished = {});
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
