#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Lo que define un lienzo: tamaño, resolución de impresión, unidad, perfil de color, fondo
// y nombre. Aquí van también las cuentas que no necesitan la GPU: pasar de píxeles a
// medidas en papel y al revés, escribir y leer números con coma decimal, los límites de
// tamaño y de capas, y los tamaños que ofrece la tarjeta de lienzo nuevo.

// Unidad en la que se mide el lienzo. En px, el tamaño son los píxeles; en las otras, el
// tamaño en papel a la resolución del lienzo (como en Photoshop: cambiar los ppp conserva
// los píxeles en px y el tamaño en papel en las demás).
enum class LengthUnit : uint8_t { Pixels, Millimeters, Centimeters, Inches };
inline constexpr int kLengthUnitCount = 4;

// Perfil de color del lienzo: el espacio de sus colores. NDI recibe siempre sRGB.
enum class ColorProfile : uint8_t { Srgb, DisplayP3 };

// Color de fondo del lienzo, debajo de todas las capas. Oculto, lo que no está pintado es
// transparente (en el PNG y por NDI).
struct CanvasBackground {
    float color[3] = {1.0f, 1.0f, 1.0f};
    bool visible = true;

    bool operator==(const CanvasBackground&) const = default;
};

struct CanvasSpec {
    int width = 0;                       // píxeles
    int height = 0;
    float ppi = 72.0f;                   // píxeles por pulgada al imprimir
    LengthUnit unit = LengthUnit::Pixels;   // en qué se midió (las propiedades lo muestran así)
    ColorProfile profile = ColorProfile::Srgb;
    CanvasBackground background;
    std::string name;
};

namespace canvasspec {

// --- Límites ---
inline constexpr int kMinSide = 8;
inline constexpr float kMinPpi = 1.0f;
inline constexpr float kMaxPpi = 9600.0f;
// Memoria de GPU que pueden ocupar las capas. En un móvil la GPU usa la RAM del sistema:
// pasarse no da un error limpio, el sistema mata la app y se pierde el dibujo.
inline constexpr size_t kLayerMemoryBudget = size_t{768} * 1024 * 1024;
inline constexpr int kMinLayerLimit = 4;
inline constexpr int kMaxLayerLimit = 64;
// Lienzo más grande: aquel en el que aún caben kMinLayerLimit capas en el presupuesto.
inline constexpr size_t kMaxPixels = kLayerMemoryBudget / (size_t{4} * kMinLayerLimit);

// Memoria de una capa (RGBA de 8 bits).
size_t layerBytes(int width, int height);
// Capas que caben en un lienzo de ese tamaño (el fondo no cuenta: es un color).
int layerLimit(int width, int height);

// Por qué no se puede crear un lienzo de ese tamaño. `maxSide`: el lado máximo que admite
// la GPU (0 si no se sabe).
enum class SizeProblem { None, TooSmall, TooWide, TooManyPixels };
SizeProblem sizeProblem(int width, int height, int maxSide);

// --- Unidades ---
// Símbolo corto ("px", "mm", "cm", "pulg").
const char* unitSymbol(LengthUnit unit);
// Decimales con los que se muestra una medida en esa unidad.
int unitDecimals(LengthUnit unit);
// Medida de `pixels` en `unit` a `ppi` (sin redondear).
double toLength(int pixels, LengthUnit unit, double ppi);
// Píxeles de una medida en `unit` a `ppi` (redondeados).
int toPixels(double length, LengthUnit unit, double ppi);
// Una medida en `from` pasada a `to` a `ppi`: entre medidas en papel, exacta; a px, al
// píxel; desde px, de los píxeles enteros.
double convertLength(double value, LengthUnit from, LengthUnit to, double ppi);

// --- Números con coma decimal ---
// Con `decimals` decimales como mucho y sin ceros de sobra: "29,7", "21", "8,5".
std::string formatNumber(double value, int decimals);
// Acepta coma o punto decimal y espacios alrededor; nada más (ni signo ni letras).
bool parseNumber(std::string_view text, double* value);
// "21 × 29,7 cm" (en px: "1920 × 1080 px").
std::string formatSize(double width, double height, LengthUnit unit);
// Tamaño en papel de un lienzo de píxeles: "21 × 29,7 cm".
std::string paperSize(int width, int height, LengthUnit unit, double ppi);
// "33 MB", "8,3 MB" o "1,2 GB".
std::string formatBytes(size_t bytes);
// "300 ppp", "72 ppp" o "96,5 ppp".
std::string formatPpi(double ppi);

// "Vertical", "Horizontal" o "Cuadrado".
const char* orientationName(int width, int height);

// --- Datos del lienzo (Propiedades) ---
// Fecha y hora local, lo que hace falta para escribirla.
struct LocalTime {
    int year = 1970;
    int month = 1;   // 1..12
    int day = 1;
    int hour = 0;
    int minute = 0;
};
// "Hoy, 19:30", "Ayer, 8:05", "2 oct, 19:30" (de este año) o "2 oct 2025" (hoy, ayer y
// este año, según `now`).
std::string formatDate(const LocalTime& when, const LocalTime& now);
// Tiempo dibujando: "Menos de 1 min", "12 min", "1 h 5 min" o "3 h".
std::string formatDuration(double seconds);
// Un número entero con un espacio cada tres cifras desde las cinco (como pide la RAE):
// "1234", "12 345", "1 234 567".
std::string formatCount(uint64_t count);

// Tiempo dibujando: cada trazo suma lo que dura y, si empieza poco después de acabar el
// anterior (pensar, cambiar de pincel o de color...), también esa pausa. Las pausas más
// largas no cuentan.
inline constexpr double kDrawingPause = 30.0;
// Segundos que suma un trazo de `start` a `end` si el anterior acabó en `previousEnd`
// (negativo: no hubo). En segundos de un reloj que solo avanza.
double strokeSeconds(double start, double end, double previousEnd);

// --- Tamaños de la tarjeta de lienzo nuevo ---
enum class PresetCategory : uint8_t { Video, Screen, Social, Paper, Comic, Saved };
inline constexpr int kPresetCategoryCount = 6;
const char* categoryName(PresetCategory category);
// Nombre corto para las fichas de un teléfono.
const char* categoryShortName(PresetCategory category);

struct Preset {
    const char* name;
    double width;      // en `unit`, en su orientación natural; 0: la pantalla del dispositivo
    double height;
    LengthUnit unit;
    float ppi;
};
// Los tamaños de una categoría (Saved no tiene: son los del usuario).
std::span<const Preset> presets(PresetCategory category);

// Nombre del papel de ese tamaño a esa resolución ("A4", "Carta"...), o null.
const char* paperName(int width, int height, double ppi);

// Tamaño guardado por el usuario ("Mis tamaños"), en una línea de texto:
// "<ancho> <alto> <unidad> <ppp>", con punto decimal.
struct SavedSize {
    double width = 0.0;   // en `unit`
    double height = 0.0;
    LengthUnit unit = LengthUnit::Pixels;
    float ppi = 72.0f;

    bool operator==(const SavedSize&) const = default;
};
std::string toLine(const SavedSize& size);
bool fromLine(std::string_view line, SavedSize* size);
// Lo que muestra su ficha: "21 × 29,7 cm".
std::string savedName(const SavedSize& size);

} // namespace canvasspec
