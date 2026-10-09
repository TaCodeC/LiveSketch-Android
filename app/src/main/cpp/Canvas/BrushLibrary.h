#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Pinceles: los ajustes que definen cómo pinta cada uno y la biblioteca de fábrica. Sin
// GL: el motor (StrokePath y Brush) y la interfaz leen de aquí.

// Forma de cada sello ("dab"). La redonda se calcula en el shader, con su dureza; el
// resto son imágenes generadas por código (BrushTips) o las de los pinceles de antes.
enum class BrushTip : uint8_t {
    Round,
    Pencil,     // disco con grano
    Charcoal,   // mancha irregular con huecos
    Chalk,      // bloque con motas
    Bristle,    // cerdas repartidas en un círculo
    Flat,       // cerdas en fila (pincel plano)
    Nib,        // plumilla rectangular
    Spray,      // nube de gotitas
    Splatter,   // unas gotas grandes
    Wash,       // aguada: disco suave con el borde más cargado
    Classic0,   // brush0.png ... brush3.png
    Classic1,
    Classic2,
    Classic3,
};
inline constexpr int kBrushTipCount = 14;

// Textura del papel, fija al lienzo: se ve igual en todos los trazos que pasan por encima.
enum class BrushGrain : uint8_t { None, Paper, Canvas, Rough, Watercolor, Noise };
inline constexpr int kBrushGrainCount = 6;

// Cómo se suman los sellos de un mismo trazo.
enum class BrushBuildUp : uint8_t {
    Glaze,     // se acumulan: repasar oscurece (lápiz, aerógrafo)
    Uniform,   // el trazo no pasa del alfa de un sello (tinta, rotulador)
};

struct BrushParams {
    // Forma.
    BrushTip tip = BrushTip::Round;
    float hardness = 1.0f;         // punta redonda: 0 difusa, 1 dura
    float roundness = 1.0f;        // 0,1..1: aplana la punta a lo alto
    float angle = 0.0f;            // grados
    bool followStroke = false;     // el ángulo se suma a la dirección del trazo
    float rotationJitter = 0.0f;   // giro al azar de cada sello (1: hasta ±180°)

    // Trazo.
    float spacing = 0.08f;         // entre sellos, en fracción del diámetro...
    float minSpacing = 0.5f;       // ...y como poco estos píxeles
    float scatter = 0.0f;          // dispersión de cada sello, en fracción del diámetro
    float count = 1.0f;            // sellos en cada paso
    float streamline = 0.0f;       // estabilización: el trazo sigue al lápiz con retraso

    // Afinado de los extremos (0..1: la longitud crece con el tamaño del pincel).
    float taperStart = 0.0f;
    float taperEnd = 0.0f;
    float taperTip = 0.0f;         // tamaño en la punta, en fracción del normal

    // Grano.
    BrushGrain grain = BrushGrain::None;
    float grainScale = 1.0f;       // tamaño del grano (1: su textura a un texel por píxel)
    float grainDepth = 0.0f;       // cuánto se nota (0..1)

    // Pintura.
    float flow = 1.0f;             // alfa de cada sello con presión 1
    float eraseFlow = 0.0f;        // al borrar (0: el mismo flujo)
    BrushBuildUp buildUp = BrushBuildUp::Glaze;

    // Mezcla húmeda: con cualquiera de las tres, el pincel pinta directamente sobre la
    // capa mezclando su color con lo que ya hay (ver Brush::drawWet).
    float wetPull = 0.0f;          // arrastre: cuánto se lleva de lo que hay debajo
    float wetCharge = 1.0f;        // carga: cuánta pintura lleva (1: no se acaba)
    float wetDilution = 0.0f;      // dilución: agua en la pintura (más clara y más mezclada)

    // Dinámica.
    float pressureSize = 1.0f;     // cuánto adelgaza con poca presión (0..1)
    float pressureOpacity = 0.0f;  // cuánto se aclara con poca presión (0..1)
    float sizeJitter = 0.0f;       // variación al azar de cada sello (0..1)
    float opacityJitter = 0.0f;

    // Tamaño: el deslizador (0..1) va de minRadius a maxRadius (px del lienzo, radio).
    float minRadius = 1.0f;
    float maxRadius = 200.0f;
    float size = 0.3f;             // posición del deslizador al elegirlo por primera vez
    float opacity = 1.0f;          // opacidad al elegirlo por primera vez
    float previewSize = 0.12f;     // radio en la muestra, en fracción de su alto

    // Se comporta como los pinceles de antes: radio mínimo de 1 px e imagen invertida.
    bool classic = false;

    bool operator==(const BrushParams&) const = default;
};

enum class BrushCategory : uint8_t { Sketching, Inking, Painting, Airbrushing, Artistic, Calligraphy, Classic };
inline constexpr int kBrushCategoryCount = 7;

struct BrushPreset {
    const char* id;     // estable: es lo que se guarda en el archivo de ajustes
    const char* name;
    BrushCategory category;
    BrushParams params;
};

namespace brushes {

std::span<const BrushPreset> library();
// Índice en library() del pincel con ese id, o -1.
int indexOf(std::string_view id);
const char* categoryName(BrushCategory category);

// Pinceles por defecto de cada herramienta.
inline constexpr std::string_view kDefaultPaint = "pluma-estudio";
inline constexpr std::string_view kDefaultErase = "aerografo-duro";
inline constexpr std::string_view kDefaultSmudge = "aerografo-suave";
// El "Básico" de siempre: con el que pinta el lienzo si nadie elige otro.
const BrushParams& basic();

// Radio (px del lienzo) de una posición del deslizador de tamaño, y al revés.
float radiusFor(const BrushParams& params, float size);
float sizeFor(const BrushParams& params, float radius);

// El pincel mezcla con lo que hay en la capa (tiene arrastre, poca carga o dilución).
bool isWet(const BrushParams& params);
// Pintura (0..1) que deja un sello de un pincel húmedo a `distance` píxeles del principio
// del trazo: la dilución la rebaja y, sin carga completa, se va acabando. `radius`: el
// del pincel (uno más grande lleva más pintura).
float wetPaint(const BrushParams& params, float radius, float distance);

// Ajustes numéricos que se pueden cambiar en el panel del pincel.
enum class ParamFormat { Percent, Degrees, Count, Scale, Diameter };
enum class ParamCurve { Linear, Square, Log };
struct ParamInfo {
    const char* key;        // en el archivo de ajustes
    const char* label;      // en el panel
    float BrushParams::* member;
    float min;
    float max;
    ParamFormat format;
    ParamCurve curve;
};
std::span<const ParamInfo> params();
const ParamInfo* findParam(std::string_view key);
// Posición (0..1) de un deslizador para el valor y al revés, según la curva.
float toSlider(const ParamInfo& info, float value);
float fromSlider(const ParamInfo& info, float t);
// Texto del valor para el panel ("8 %", "45°", "×1,5", "24 px").
std::string formatValue(const ParamInfo& info, float value);

// Nombres de las puntas y los granos: en el panel y en el archivo.
const char* tipName(BrushTip tip);
const char* tipKey(BrushTip tip);
const char* grainName(BrushGrain grain);
const char* grainKey(BrushGrain grain);

// Archivo de ajustes: una línea "clave valor" por cada ajuste que difiere de `base`.
std::string describeChanges(const BrushParams& base, const BrushParams& params);
// Aplica una línea "clave valor". Devuelve false si la clave o el valor no valen.
bool applySetting(BrushParams& params, std::string_view key, std::string_view value);
// Deja los valores dentro de sus márgenes.
void sanitize(BrushParams& params);

} // namespace brushes
