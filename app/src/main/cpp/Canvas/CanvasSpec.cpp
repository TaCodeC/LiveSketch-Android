#include "Canvas/CanvasSpec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace canvasspec {
namespace {

constexpr double kMillimetersPerInch = 25.4;

// Tamaños de cada categoría. Los de papel se miden en su unidad de siempre (los A en mm,
// los americanos en pulgadas) para que se vean redondos.
constexpr Preset kVideo[] = {
    {"HD 720p", 1280, 720, LengthUnit::Pixels, 72.0f},
    {"Full HD 1080p", 1920, 1080, LengthUnit::Pixels, 72.0f},
    {"QHD 1440p", 2560, 1440, LengthUnit::Pixels, 72.0f},
    {"4K UHD", 3840, 2160, LengthUnit::Pixels, 72.0f},
    {"Full HD vertical", 1080, 1920, LengthUnit::Pixels, 72.0f},
    {"Pequeño 360p", 640, 360, LengthUnit::Pixels, 72.0f},
};
constexpr Preset kScreen[] = {
    {"Esta pantalla", 0, 0, LengthUnit::Pixels, 72.0f},
    {"Móvil 20:9", 1080, 2400, LengthUnit::Pixels, 72.0f},
    {"Tableta 4:3", 2048, 1536, LengthUnit::Pixels, 72.0f},
    {"Tableta 16:10", 2560, 1600, LengthUnit::Pixels, 72.0f},
    {"Tableta 3:2", 2880, 1920, LengthUnit::Pixels, 72.0f},
    {"Ultrapanorámico", 3440, 1440, LengthUnit::Pixels, 72.0f},
};
constexpr Preset kSocial[] = {
    {"Cuadrado", 1080, 1080, LengthUnit::Pixels, 72.0f},
    {"Vertical 4:5", 1080, 1350, LengthUnit::Pixels, 72.0f},
    {"Historia 9:16", 1080, 1920, LengthUnit::Pixels, 72.0f},
    {"Miniatura de YouTube", 1280, 720, LengthUnit::Pixels, 72.0f},
    {"Cabecera 3:1", 1500, 500, LengthUnit::Pixels, 72.0f},
};
constexpr Preset kPaper[] = {
    {"A6", 105, 148, LengthUnit::Millimeters, 300.0f},
    {"A5", 148, 210, LengthUnit::Millimeters, 300.0f},
    {"A4", 210, 297, LengthUnit::Millimeters, 300.0f},
    {"A3", 297, 420, LengthUnit::Millimeters, 300.0f},
    {"Carta", 8.5, 11, LengthUnit::Inches, 300.0f},
    {"Tabloide", 11, 17, LengthUnit::Inches, 300.0f},
    {"Foto 10 × 15", 10, 15, LengthUnit::Centimeters, 300.0f},
};
constexpr Preset kComic[] = {
    {"Página de cómic", 6.625, 10.25, LengthUnit::Inches, 300.0f},
    {"Manga B5", 182, 257, LengthUnit::Millimeters, 300.0f},
    {"Webtoon", 800, 1280, LengthUnit::Pixels, 72.0f},
    {"Póster A2", 420, 594, LengthUnit::Millimeters, 150.0f},
    {"Carta coleccionable", 63, 88, LengthUnit::Millimeters, 300.0f},
};

// Papeles que se reconocen por su tamaño (en mm), en vertical.
struct Paper {
    const char* name;
    double width;
    double height;
};
constexpr Paper kPapers[] = {
    {"A6", 105, 148},          {"A5", 148, 210},     {"A4", 210, 297},           {"A3", 297, 420},
    {"A2", 420, 594},          {"B5", 182, 257},     {"Carta", 215.9, 279.4},    {"Legal", 215.9, 355.6},
    {"Tabloide", 279.4, 431.8},
};

double unitsPerInch(LengthUnit unit) {
    switch (unit) {
    case LengthUnit::Millimeters:
        return kMillimetersPerInch;
    case LengthUnit::Centimeters:
        return kMillimetersPerInch / 10.0;
    default:
        return 1.0;
    }
}

const char* unitKey(LengthUnit unit) {
    switch (unit) {
    case LengthUnit::Millimeters:
        return "mm";
    case LengthUnit::Centimeters:
        return "cm";
    case LengthUnit::Inches:
        return "in";
    default:
        return "px";
    }
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

size_t layerBytes(int width, int height) {
    // En 64 bits: en Android de 32 bits y en la web, size_t se desbordaría.
    const uint64_t bytes = static_cast<uint64_t>(std::max(width, 0)) * static_cast<uint64_t>(std::max(height, 0)) * 4;
    return static_cast<size_t>(std::min<uint64_t>(bytes, SIZE_MAX));
}

int layerLimit(int width, int height) {
    const size_t bytes = layerBytes(width, height);
    const size_t byBudget = bytes > 0 ? kLayerMemoryBudget / bytes : static_cast<size_t>(kMaxLayerLimit);
    const int limit = static_cast<int>(std::min(byBudget, static_cast<size_t>(kMaxLayerLimit)));
    return std::max(limit, kMinLayerLimit);
}

SizeProblem sizeProblem(int width, int height, int maxSide) {
    if (width < kMinSide || height < kMinSide) {
        return SizeProblem::TooSmall;
    }
    if (maxSide > 0 && std::max(width, height) > maxSide) {
        return SizeProblem::TooWide;
    }
    if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kMaxPixels) {
        return SizeProblem::TooManyPixels;
    }
    return SizeProblem::None;
}

const char* unitSymbol(LengthUnit unit) {
    switch (unit) {
    case LengthUnit::Millimeters:
        return "mm";
    case LengthUnit::Centimeters:
        return "cm";
    case LengthUnit::Inches:
        return "pulg";
    default:
        return "px";
    }
}

int unitDecimals(LengthUnit unit) {
    switch (unit) {
    case LengthUnit::Millimeters:
        return 1;
    case LengthUnit::Centimeters:
    case LengthUnit::Inches:
        return 2;
    default:
        return 0;
    }
}

double toLength(int pixels, LengthUnit unit, double ppi) {
    if (unit == LengthUnit::Pixels || !(ppi > 0.0)) {
        return static_cast<double>(pixels);
    }
    return static_cast<double>(pixels) / ppi * unitsPerInch(unit);
}

int toPixels(double length, LengthUnit unit, double ppi) {
    if (!std::isfinite(length) || length <= 0.0) {
        return 0;
    }
    const double pixels = unit == LengthUnit::Pixels ? length : length / unitsPerInch(unit) * ppi;
    // Más que cualquier lienzo posible, pero sin desbordar un int.
    return static_cast<int>(std::lround(std::min(pixels, 1.0e9)));
}

double convertLength(double value, LengthUnit from, LengthUnit to, double ppi) {
    if (from == to) {
        return value;
    }
    if (to == LengthUnit::Pixels) {
        return static_cast<double>(toPixels(value, from, ppi));
    }
    if (from == LengthUnit::Pixels) {
        return toLength(toPixels(value, LengthUnit::Pixels, ppi), to, ppi);
    }
    return value / unitsPerInch(from) * unitsPerInch(to);
}

std::string formatNumber(double value, int decimals) {
    if (!std::isfinite(value)) {
        return "—";
    }
    decimals = std::clamp(decimals, 0, 6);
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    std::string out(text);
    if (out.find('.') != std::string::npos) {
        while (out.back() == '0') {
            out.pop_back();
        }
        if (out.back() == '.') {
            out.pop_back();
        }
    }
    if (out == "-0") {
        out = "0";
    }
    std::replace(out.begin(), out.end(), '.', ',');
    return out;
}

bool parseNumber(std::string_view text, double* value) {
    text = trim(text);
    if (text.empty() || text.size() > 24) {
        return false;
    }
    double integer = 0.0;
    double fraction = 0.0;
    double scale = 1.0;
    bool separator = false;
    bool digits = false;
    for (const char c : text) {
        if (c == ',' || c == '.') {
            if (separator) {
                return false;
            }
            separator = true;
        } else if (c >= '0' && c <= '9') {
            digits = true;
            if (separator) {
                scale *= 0.1;
                fraction += (c - '0') * scale;
            } else {
                integer = integer * 10.0 + (c - '0');
            }
        } else {
            return false;
        }
    }
    if (!digits) {
        return false;
    }
    *value = integer + fraction;
    return true;
}

std::string formatSize(double width, double height, LengthUnit unit) {
    const int decimals = unitDecimals(unit);
    return formatNumber(width, decimals) + " × " + formatNumber(height, decimals) + " " + unitSymbol(unit);
}

std::string paperSize(int width, int height, LengthUnit unit, double ppi) {
    return formatSize(toLength(width, unit, ppi), toLength(height, unit, ppi), unit);
}

std::string formatBytes(size_t bytes) {
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (megabytes >= 1024.0) {
        return formatNumber(megabytes / 1024.0, 1) + " GB";
    }
    return formatNumber(megabytes, megabytes < 10.0 ? 1 : 0) + " MB";
}

std::string formatPpi(double ppi) { return formatNumber(ppi, 1) + " ppp"; }

const char* orientationName(int width, int height) {
    if (width > height) {
        return "Horizontal";
    }
    return width < height ? "Vertical" : "Cuadrado";
}

namespace {

// Días desde el 1 de enero de 1970 (calendario gregoriano; vale para cualquier año).
int64_t daysFromCivil(int year, int month, int day) {
    const int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yearOfEra = y - era * 400;
    const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}

} // namespace

std::string formatDate(const LocalTime& when, const LocalTime& now) {
    static constexpr const char* kMonths[12] = {"ene", "feb", "mar", "abr", "may", "jun",
                                                "jul", "ago", "sept", "oct", "nov", "dic"};
    char time[16];
    std::snprintf(time, sizeof(time), "%d:%02d", std::clamp(when.hour, 0, 23), std::clamp(when.minute, 0, 59));
    const int64_t days = daysFromCivil(now.year, now.month, now.day) - daysFromCivil(when.year, when.month, when.day);
    if (days == 0) {
        return std::string("Hoy, ") + time;
    }
    if (days == 1) {
        return std::string("Ayer, ") + time;
    }
    // Este año, el día y la hora; de otro año, el día con el año.
    const char* month = kMonths[std::clamp(when.month, 1, 12) - 1];
    char text[48];
    if (when.year == now.year) {
        std::snprintf(text, sizeof(text), "%d %s, %s", when.day, month, time);
    } else {
        std::snprintf(text, sizeof(text), "%d %s %d", when.day, month, when.year);
    }
    return text;
}

std::string formatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 60.0) {
        return "Menos de 1 min";
    }
    const auto minutes = static_cast<int64_t>(seconds / 60.0);
    const int64_t hours = minutes / 60;
    if (hours == 0) {
        return std::to_string(minutes) + " min";
    }
    std::string text = std::to_string(hours) + " h";
    if (minutes % 60 != 0) {
        text += " " + std::to_string(minutes % 60) + " min";
    }
    return text;
}

std::string formatCount(uint64_t count) {
    const std::string digits = std::to_string(count);
    if (digits.size() <= 4) {
        return digits;
    }
    std::string text;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            text += ' ';
        }
        text += digits[i];
    }
    return text;
}

double strokeSeconds(double start, double end, double previousEnd) {
    if (!std::isfinite(start) || !std::isfinite(end) || end < start) {
        return 0.0;
    }
    double seconds = end - start;
    if (std::isfinite(previousEnd) && previousEnd >= 0.0 && start >= previousEnd && start - previousEnd <= kDrawingPause) {
        seconds += start - previousEnd;
    }
    return seconds;
}

const char* categoryName(PresetCategory category) {
    switch (category) {
    case PresetCategory::Video:
        return "Vídeo y NDI";
    case PresetCategory::Screen:
        return "Pantalla";
    case PresetCategory::Social:
        return "Redes sociales";
    case PresetCategory::Paper:
        return "Papel";
    case PresetCategory::Comic:
        return "Cómic e ilustración";
    case PresetCategory::Saved:
        return "Mis tamaños";
    }
    return "";
}

const char* categoryShortName(PresetCategory category) {
    switch (category) {
    case PresetCategory::Video:
        return "Vídeo";
    case PresetCategory::Social:
        return "Redes";
    case PresetCategory::Comic:
        return "Cómic";
    case PresetCategory::Saved:
        return "Míos";
    default:
        return categoryName(category);
    }
}

std::span<const Preset> presets(PresetCategory category) {
    switch (category) {
    case PresetCategory::Video:
        return kVideo;
    case PresetCategory::Screen:
        return kScreen;
    case PresetCategory::Social:
        return kSocial;
    case PresetCategory::Paper:
        return kPaper;
    case PresetCategory::Comic:
        return kComic;
    case PresetCategory::Saved:
        break;
    }
    return {};
}

const char* paperName(int width, int height, double ppi) {
    if (!(ppi > 0.0)) {
        return nullptr;
    }
    const int shortSide = std::min(width, height);
    const int longSide = std::max(width, height);
    for (const Paper& paper : kPapers) {
        const int w = toPixels(paper.width, LengthUnit::Millimeters, ppi);
        const int h = toPixels(paper.height, LengthUnit::Millimeters, ppi);
        if (std::abs(w - shortSide) <= 1 && std::abs(h - longSide) <= 1) {
            return paper.name;
        }
    }
    return nullptr;
}

std::string toLine(const SavedSize& size) {
    char line[96];
    std::snprintf(line, sizeof(line), "%.6g %.6g %s %.6g", size.width, size.height, unitKey(size.unit),
                  static_cast<double>(size.ppi));
    return line;
}

bool fromLine(std::string_view line, SavedSize* size) {
    std::array<std::string_view, 4> fields;
    size_t count = 0;
    line = trim(line);
    while (!line.empty() && count < fields.size()) {
        const size_t end = line.find(' ');
        fields[count++] = line.substr(0, end);
        line = end == std::string_view::npos ? std::string_view() : trim(line.substr(end + 1));
    }
    if (count != fields.size() || !line.empty()) {
        return false;
    }
    SavedSize parsed;
    double ppi = 0.0;
    if (!parseNumber(fields[0], &parsed.width) || !parseNumber(fields[1], &parsed.height) ||
        !parseNumber(fields[3], &ppi)) {
        return false;
    }
    bool knownUnit = false;
    for (int i = 0; i < kLengthUnitCount; ++i) {
        const LengthUnit unit = static_cast<LengthUnit>(i);
        if (fields[2] == unitKey(unit)) {
            parsed.unit = unit;
            knownUnit = true;
        }
    }
    if (!knownUnit || ppi < kMinPpi || ppi > kMaxPpi || parsed.width <= 0.0 || parsed.height <= 0.0) {
        return false;
    }
    parsed.ppi = static_cast<float>(ppi);
    const int width = toPixels(parsed.width, parsed.unit, ppi);
    const int height = toPixels(parsed.height, parsed.unit, ppi);
    if (sizeProblem(width, height, 0) != SizeProblem::None) {
        return false;
    }
    *size = parsed;
    return true;
}

std::string savedName(const SavedSize& size) { return formatSize(size.width, size.height, size.unit); }

} // namespace canvasspec
