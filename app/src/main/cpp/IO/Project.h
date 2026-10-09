#pragma once

#include "Canvas/CanvasSpec.h"
#include "Canvas/DrawingGuide.h"
#include "Canvas/Layer.h"
#include "Canvas/Rect.h"
#include "IO/Png.h"

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Proyectos de LiveSketch (.lvskt): un ZIP con la estructura de OpenRaster, así que con la
// extensión .ora lo abren los programas que leen OpenRaster (Krita, con sus capas, opacidades
// y modos de fusión).
//
//   mimetype                   "image/openraster", el primero y sin comprimir
//   stack.xml                  las capas para OpenRaster (de arriba abajo) y el fondo como capa
//   data/layerN.png            cada capa, recortada a lo que tiene pintado (N: 0 la de abajo)
//   data/background.png        el color de fondo, del tamaño del lienzo (para los demás)
//   mergedimage.png            el dibujo entero, como se ve
//   Thumbnails/thumbnail.png   el dibujo en pequeño (256 px como mucho)
//   livesketch/document.json   todo lo de LiveSketch: lo que manda al abrir
//
// Los PNG llevan los colores sin premultiplicar y el perfil del lienzo; al abrir, los píxeles
// vuelven idénticos (ver gfx::premultiply). Aquí está lo que no necesita la GPU: escribir y
// leer el documento y pasar las capas a PNG y de PNG.
namespace project {

inline constexpr const char* kExtension = ".lvskt";
inline constexpr const char* kMimeType = "application/x-livesketch";
// Versión del formato que escribe esta app: la más nueva que sabe leer entera.
inline constexpr int kFormatVersion = 1;

inline constexpr const char* kDocumentEntry = "livesketch/document.json";
inline constexpr const char* kStackEntry = "stack.xml";
inline constexpr const char* kMergedEntry = "mergedimage.png";
inline constexpr const char* kThumbnailEntry = "Thumbnails/thumbnail.png";
inline constexpr const char* kBackgroundEntry = "data/background.png";
// Lado máximo de la miniatura (lo que pide OpenRaster).
inline constexpr int kThumbnailSide = 256;
// Bytes como mucho de un nombre (del lienzo o de una capa).
inline constexpr size_t kMaxNameBytes = 255;

// Una capa: sus propiedades y dónde van sus píxeles.
struct LayerInfo {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blend = BlendMode::Normal;
    bool alphaLock = false;
    bool clipping = false;
    bool reference = false;
    IRect rect;            // lo pintado, en el lienzo (vacío: la capa es transparente)
    std::string file;      // su PNG dentro del archivo
};

// La vista al guardar, para volver a ella al abrir.
struct ViewInfo {
    bool fitted = true;        // el lienzo estaba ajustado a la ventana
    float zoom = 1.0f;         // si no: el zoom dividido por el del ajuste,
    glm::vec2 center{0.0f};    // el punto del lienzo en el centro de la zona libre
    float angle = 0.0f;        // y el giro (radianes)
    bool flipped = false;      // volteada en horizontal (también ajustada)
};

struct Document {
    int width = 0;
    int height = 0;
    CanvasInfo info;
    CanvasBackground background;
    std::vector<LayerInfo> layers;   // de abajo arriba
    int activeLayer = 0;
    DrawingGuide guide;
    ViewInfo view;
    bool hasColor = false;           // el color del pincel, en el perfil del lienzo
    float color[3] = {0.0f, 0.0f, 0.0f};
};

// --- Escribir ---
// Nombre de la entrada del PNG de la capa `index` (0: la de abajo).
std::string layerEntry(int index);
// livesketch/document.json. `app`: el programa y su versión ("LiveSketch 0.3.0").
std::string documentJson(const Document& document, std::string_view app);
// stack.xml de OpenRaster: las capas de arriba abajo con sus nombres, posiciones, opacidades,
// visibilidad y modos de fusión, y el fondo como capa de abajo («Fondo»).
std::string stackXml(const Document& document);
// Nombre del modo de fusión en document.json ("multiply", "color-burn"...) y en stack.xml
// ("svg:multiply", "krita:linear_burn"...).
const char* blendName(BlendMode blend);
const char* compositeOp(BlendMode blend);

// PNG de una capa (o del dibujo entero) a partir de sus píxeles premultiplicados (RGBA8, filas
// de arriba abajo), que se quedan sin premultiplicar y filtrados (se reutiliza su memoria: si
// se reservó un byte más por fila, no hace falta más). Vacío si falla (motivo en `error`).
std::vector<uint8_t> encodeImage(std::vector<uint8_t>& premultiplied, int width, int height, const png::Info& info,
                                 std::string* error);
// PNG del color de fondo, del tamaño del lienzo.
std::vector<uint8_t> encodeBackground(int width, int height, const float rgb[3], ColorProfile profile,
                                      std::string* error);
// Miniatura del dibujo (premultiplicado): reducida promediando hasta que su lado mayor sea
// `maxSide` como mucho, y ya sin premultiplicar. Deja su tamaño en `outWidth` y `outHeight`.
std::vector<uint8_t> thumbnail(const uint8_t* premultiplied, int width, int height, int maxSide, int* outWidth,
                               int* outHeight);

// --- Leer ---
// Lee document.json. `maxSide`: el lado máximo de textura de esta GPU (0: no se comprueba).
// Devuelve false si el proyecto no se puede abrir, con el motivo en `error` ("es de una
// versión más nueva de LiveSketch"...). Lo que se puede arreglar se arregla y se cuenta en
// `warnings` (un modo de fusión que no existe pasa a Normal...).
bool readDocument(std::string_view text, int maxSide, Document& document, std::vector<std::string>& warnings,
                  std::string& error);

// Lee el PNG de una capa: tiene que medir lo que su caja. Deja en `out` los píxeles
// premultiplicados. False si está dañado (motivo en `error`).
bool decodeLayer(std::span<const uint8_t> file, const LayerInfo& layer, std::vector<uint8_t>& out,
                 std::string* error);

// Un nombre como lo guarda el proyecto: UTF-8 válido, sin caracteres de control y de
// kMaxNameBytes como mucho (sin partir un carácter).
std::string cleanName(std::string_view name);
// El aviso de una capa que no se pudo leer: «La capa «Tinta» está dañada: se abre vacía.»
std::string damagedLayer(const LayerInfo& layer);
// Lo que puede ocupar como mucho el PNG de una capa (más es que está dañado).
size_t maxLayerFile(const LayerInfo& layer);

// Fecha y hora UTC en ISO 8601 ("2026-10-08T22:14:45Z") desde un SDL_Time, y al revés
// (acepta también fracciones de segundo y una zona "+02:00"). False si no es una fecha.
std::string isoTime(int64_t time);
bool parseIsoTime(std::string_view text, int64_t* time);

} // namespace project
