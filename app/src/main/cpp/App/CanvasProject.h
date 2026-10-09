#pragma once

#include "Canvas/Canvas.h"
#include "Canvas/Rect.h"
#include "IO/ProjectFile.h"
#include "IO/Zip.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Un lienzo y su archivo de proyecto. Se recuerda qué tiene el archivo de cada capa: al
// guardar otra vez, solo se leen de la GPU y se comprimen las capas que cambiaron desde
// entonces (y el dibujo entero, si cambió lo que se ve); lo demás se copia del archivo tal
// cual. Aquí está guardar como lo hace la app, que también usan las pruebas.
namespace canvasproject {

// Una capa tal como está en el archivo.
struct SavedLayer {
    uint32_t id = 0;          // Layer::id
    uint64_t revision = 0;    // Layer::revision de lo que tiene el archivo
    IRect rect;               // lo pintado (vacía: es transparente, no hay nada que copiar)
    zip::Entry entry;         // su PNG, como lo describe el índice del archivo
};

// Lo que tiene el archivo de un lienzo.
struct Record {
    std::string path;                 // el archivo (vacío: aún no tiene)
    std::vector<SavedLayer> layers;
    uint64_t composite = 0;           // compositeKey() de su dibujo entero y su miniatura (0: no valen)
    zip::Entry thumbnail;
    zip::Entry merged;

    const SavedLayer* find(uint32_t id) const;
};

// Lo que cambia el dibujo entero y su PNG: el tamaño, el perfil, el fondo, los ppp y, de cada
// capa, sus píxeles (su revisión), su visibilidad, su opacidad, su modo de fusión y su
// recorte. Nunca es 0.
uint64_t compositeKey(const Canvas& canvas);

// El documento del lienzo: sus datos, el fondo, las capas con sus propiedades (aún sin cajas),
// la capa activa y la guía. La vista y el color del pincel los pone la app.
project::Document document(const Canvas& canvas);

// Un guardado en marcha: con su resultado, el Record del archivo nuevo.
struct Pending {
    Record record;                 // aún sin el archivo ni sus entradas
    int encodedLayers = 0;         // capas leídas de la GPU para comprimirlas
    int copiedLayers = 0;          // capas copiadas del archivo anterior
    bool copiedComposite = false;  // el dibujo entero y la miniatura, también
};

// Empieza a guardar el lienzo con `saver` (libre) y `document` (el de document(), con la vista
// y el color): lee de la GPU las capas que cambiaron desde `previous`, copia las demás de
// previous.path (si se puede leer) y lee el dibujo entero solo si cambió. Para guardar lo que
// se ve con lo que esté a medias aplicado, antes Canvas::settle(). Devuelve false si ya
// terminó (no se pudo crear el archivo o leer la GPU): el motivo queda en
// saver.takeResult(), y `onFinished` se llama si llegó a empezar.
bool save(Canvas& canvas, project::Document document, const project::Target& target, const std::string& app,
          const Record& previous, project::Saver& saver, std::function<void()> onFinished, Pending* pending);

// El Record del archivo que acaba de escribir el guardado `pending`.
Record saved(const Pending& pending, const project::Saver::Result& result);

// Al abrir: lo que tiene el archivo de la capa `index`, justo después de subirla al lienzo, si
// se puede copiar tal cual al guardar. `loaded`: la capa quedó como en el archivo (no estaba
// dañada y cupo en la GPU).
std::optional<SavedLayer> openedLayer(const Canvas& canvas, const project::Loader& loader, int index, bool loaded);
// El Record del archivo `path` recién abierto, con lo que se pudo copiar de sus capas. Si
// están todas (`complete`, y nada ha cambiado desde que se abrió), también el dibujo entero.
Record opened(const Canvas& canvas, const project::Loader& loader, const std::string& path,
              std::vector<SavedLayer> layers, bool complete);

} // namespace canvasproject
