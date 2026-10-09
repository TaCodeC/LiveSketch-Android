#include "Canvas/BrushLibrary.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace {

// Los cuatro pinceles de antes, con el comportamiento de antes: un sello cada 0,8 px, la
// presión escala el radio y el alfa, flujo 0,5 al pintar y 1 al borrar. El tamaño por
// defecto es el radio de 10 px de siempre.
BrushParams classic(BrushTip tip) {
    BrushParams p;
    p.tip = tip;
    p.spacing = 0.0f;
    p.minSpacing = 0.8f;
    p.flow = 0.5f;
    p.eraseFlow = 1.0f;
    p.pressureSize = 1.0f;
    p.pressureOpacity = 1.0f;
    p.minRadius = 1.0f;
    p.maxRadius = 200.0f;
    p.size = std::sqrt(9.0f / 199.0f);
    p.previewSize = 0.12f;
    p.classic = true;
    return p;
}

std::vector<BrushPreset> makeLibrary() {
    using T = BrushTip;
    using G = BrushGrain;
    using B = BrushBuildUp;
    using C = BrushCategory;
    std::vector<BrushPreset> list;
    auto add = [&](const char* id, const char* name, C category, BrushParams params) {
        list.push_back({id, name, category, params});
    };

    // --- Bocetos ---
    add("lapiz-hb", "Lápiz HB", C::Sketching,
        {.tip = T::Pencil, .rotationJitter = 1.0f, .spacing = 0.15f, .taperStart = 0.2f, .taperEnd = 0.2f,
         .taperTip = 0.3f, .grain = G::Paper, .grainScale = 1.0f, .grainDepth = 0.65f, .flow = 0.35f,
         .buildUp = B::Glaze, .pressureSize = 0.45f, .pressureOpacity = 0.85f, .minRadius = 0.75f,
         .maxRadius = 12.0f, .size = 0.4f, .previewSize = 0.08f});
    add("lapiz-6b", "Lápiz 6B", C::Sketching,
        {.tip = T::Pencil, .rotationJitter = 1.0f, .spacing = 0.12f, .taperStart = 0.15f, .taperEnd = 0.15f,
         .taperTip = 0.35f, .grain = G::Paper, .grainScale = 1.4f, .grainDepth = 0.55f, .flow = 0.5f,
         .buildUp = B::Glaze, .pressureSize = 0.5f, .pressureOpacity = 0.8f, .minRadius = 1.0f, .maxRadius = 24.0f,
         .size = 0.4f, .previewSize = 0.11f});
    add("carboncillo", "Carboncillo", C::Sketching,
        {.tip = T::Charcoal, .rotationJitter = 1.0f, .spacing = 0.1f, .scatter = 0.03f, .taperStart = 0.1f,
         .taperEnd = 0.1f, .taperTip = 0.5f, .grain = G::Rough, .grainScale = 1.0f, .grainDepth = 0.7f,
         .flow = 0.45f, .buildUp = B::Glaze, .pressureSize = 0.35f, .pressureOpacity = 0.75f, .sizeJitter = 0.15f,
         .minRadius = 2.0f, .maxRadius = 60.0f, .size = 0.35f, .previewSize = 0.16f});

    // --- Entintado ---
    add("pluma-estudio", "Pluma de estudio", C::Inking,
        {.tip = T::Round, .hardness = 0.92f, .spacing = 0.05f, .streamline = 0.35f, .taperStart = 0.3f,
         .taperEnd = 0.35f, .taperTip = 0.05f, .flow = 1.0f, .buildUp = B::Uniform, .pressureSize = 0.8f,
         .minRadius = 0.75f, .maxRadius = 30.0f, .size = 0.35f, .previewSize = 0.11f});
    add("tinta-tecnica", "Tinta técnica", C::Inking,
        {.tip = T::Round, .hardness = 1.0f, .spacing = 0.04f, .streamline = 0.25f, .taperStart = 0.08f,
         .taperEnd = 0.1f, .taperTip = 0.5f, .flow = 1.0f, .buildUp = B::Uniform, .pressureSize = 0.15f,
         .minRadius = 0.5f, .maxRadius = 20.0f, .size = 0.3f, .previewSize = 0.07f});
    add("rotulador", "Rotulador", C::Inking,
        {.tip = T::Round, .hardness = 0.8f, .spacing = 0.05f, .streamline = 0.2f, .flow = 0.9f,
         .buildUp = B::Uniform, .pressureSize = 0.0f, .minRadius = 1.0f, .maxRadius = 40.0f, .size = 0.4f,
         .opacity = 0.85f, .previewSize = 0.14f});
    add("tinta-seca", "Tinta seca", C::Inking,
        {.tip = T::Round, .hardness = 0.9f, .spacing = 0.06f, .scatter = 0.02f, .streamline = 0.2f,
         .taperStart = 0.15f, .taperEnd = 0.25f, .taperTip = 0.1f, .grain = G::Rough, .grainScale = 0.8f,
         .grainDepth = 0.55f, .flow = 1.0f, .buildUp = B::Uniform, .pressureSize = 0.75f, .sizeJitter = 0.1f,
         .minRadius = 1.0f, .maxRadius = 40.0f, .size = 0.35f, .previewSize = 0.12f});

    // --- Pintura ---
    add("gouache", "Gouache", C::Painting,
        {.tip = T::Flat, .angle = 90.0f, .followStroke = true, .spacing = 0.04f, .taperStart = 0.05f,
         .taperEnd = 0.12f, .taperTip = 0.6f, .grain = G::Paper, .grainScale = 1.5f, .grainDepth = 0.18f,
         .flow = 1.0f, .buildUp = B::Uniform, .pressureSize = 0.4f, .pressureOpacity = 0.2f, .minRadius = 3.0f,
         .maxRadius = 80.0f, .size = 0.4f, .previewSize = 0.2f});
    add("oleo", "Óleo", C::Painting,
        {.tip = T::Bristle, .angle = 90.0f, .followStroke = true, .spacing = 0.03f, .taperStart = 0.05f,
         .taperEnd = 0.1f, .taperTip = 0.6f, .grain = G::Canvas, .grainScale = 1.3f, .grainDepth = 0.15f,
         .flow = 1.0f, .buildUp = B::Uniform, .pressureSize = 0.3f, .pressureOpacity = 0.35f, .sizeJitter = 0.05f,
         .minRadius = 3.0f, .maxRadius = 100.0f, .size = 0.4f, .previewSize = 0.22f});
    add("acrilico", "Acrílico", C::Painting,
        {.tip = T::Flat, .angle = 90.0f, .followStroke = true, .spacing = 0.05f, .grain = G::Canvas,
         .grainScale = 1.0f, .grainDepth = 0.22f, .flow = 0.6f, .buildUp = B::Glaze, .pressureSize = 0.25f,
         .pressureOpacity = 0.3f, .minRadius = 3.0f, .maxRadius = 80.0f, .size = 0.4f, .previewSize = 0.2f});
    add("aguada", "Aguada", C::Painting,
        {.tip = T::Wash, .rotationJitter = 1.0f, .spacing = 0.06f, .grain = G::Watercolor, .grainScale = 1.5f,
         .grainDepth = 0.45f, .flow = 0.12f, .buildUp = B::Glaze, .pressureSize = 0.3f, .pressureOpacity = 0.7f,
         .sizeJitter = 0.1f, .minRadius = 4.0f, .maxRadius = 120.0f, .size = 0.45f, .opacity = 0.85f,
         .previewSize = 0.24f});
    // Húmedos: mezclan su color con lo que ya está pintado y lo arrastran.
    add("oleo-humedo", "Óleo húmedo", C::Painting,
        {.tip = T::Bristle, .angle = 90.0f, .followStroke = true, .spacing = 0.05f, .taperStart = 0.05f,
         .taperEnd = 0.1f, .taperTip = 0.6f, .grain = G::Canvas, .grainScale = 1.3f, .grainDepth = 0.12f,
         .flow = 0.55f, .buildUp = B::Glaze, .wetPull = 0.4f, .wetCharge = 0.7f, .pressureSize = 0.3f,
         .pressureOpacity = 0.3f, .sizeJitter = 0.05f, .minRadius = 3.0f, .maxRadius = 100.0f, .size = 0.4f,
         .previewSize = 0.22f});
    add("acuarela-humeda", "Acuarela húmeda", C::Painting,
        {.tip = T::Wash, .rotationJitter = 1.0f, .spacing = 0.06f, .grain = G::Watercolor, .grainScale = 1.5f,
         .grainDepth = 0.4f, .flow = 0.3f, .buildUp = B::Glaze, .wetPull = 0.3f, .wetCharge = 0.55f,
         .wetDilution = 0.35f, .pressureSize = 0.3f, .pressureOpacity = 0.6f, .sizeJitter = 0.1f,
         .minRadius = 4.0f, .maxRadius = 120.0f, .size = 0.45f, .opacity = 0.9f, .previewSize = 0.24f});
    add("gouache-humedo", "Gouache húmedo", C::Painting,
        {.tip = T::Flat, .angle = 90.0f, .followStroke = true, .spacing = 0.05f, .taperStart = 0.05f,
         .taperEnd = 0.12f, .taperTip = 0.6f, .grain = G::Paper, .grainScale = 1.5f, .grainDepth = 0.15f,
         .flow = 1.0f, .buildUp = B::Glaze, .wetPull = 0.25f, .wetCharge = 0.8f, .pressureSize = 0.4f,
         .pressureOpacity = 0.2f, .minRadius = 3.0f, .maxRadius = 80.0f, .size = 0.4f, .previewSize = 0.2f});

    // --- Aerógrafo ---
    add("aerografo-suave", "Aerógrafo suave", C::Airbrushing,
        {.tip = T::Round, .hardness = 0.0f, .spacing = 0.04f, .flow = 0.05f, .buildUp = B::Glaze,
         .pressureSize = 0.0f, .pressureOpacity = 0.9f, .minRadius = 4.0f, .maxRadius = 300.0f, .size = 0.4f,
         .previewSize = 0.32f});
    add("aerografo-duro", "Aerógrafo duro", C::Airbrushing,
        {.tip = T::Round, .hardness = 0.85f, .spacing = 0.04f, .flow = 0.35f, .buildUp = B::Glaze,
         .pressureSize = 0.0f, .pressureOpacity = 0.8f, .minRadius = 2.0f, .maxRadius = 250.0f, .size = 0.3f,
         .previewSize = 0.2f});
    add("espray", "Espray", C::Airbrushing,
        {.tip = T::Spray, .rotationJitter = 1.0f, .spacing = 0.15f, .scatter = 0.15f, .count = 2.0f, .flow = 0.5f,
         .buildUp = B::Glaze, .pressureSize = 0.2f, .pressureOpacity = 0.6f, .sizeJitter = 0.15f,
         .minRadius = 5.0f, .maxRadius = 200.0f, .size = 0.4f, .previewSize = 0.3f});

    // --- Artísticos ---
    add("tiza", "Tiza", C::Artistic,
        {.tip = T::Chalk, .rotationJitter = 1.0f, .spacing = 0.1f, .scatter = 0.02f, .grain = G::Rough,
         .grainScale = 1.2f, .grainDepth = 0.75f, .flow = 0.7f, .buildUp = B::Glaze, .pressureSize = 0.3f,
         .pressureOpacity = 0.5f, .sizeJitter = 0.1f, .minRadius = 3.0f, .maxRadius = 60.0f, .size = 0.4f,
         .previewSize = 0.18f});
    add("pastel", "Pastel", C::Artistic,
        {.tip = T::Charcoal, .roundness = 0.7f, .angle = 90.0f, .followStroke = true, .spacing = 0.08f,
         .grain = G::Paper, .grainScale = 1.4f, .grainDepth = 0.6f, .flow = 0.55f, .buildUp = B::Glaze,
         .pressureSize = 0.3f, .pressureOpacity = 0.7f, .minRadius = 3.0f, .maxRadius = 60.0f, .size = 0.4f,
         .previewSize = 0.18f});
    add("crayon", "Crayón", C::Artistic,
        {.tip = T::Pencil, .roundness = 0.85f, .rotationJitter = 1.0f, .spacing = 0.1f, .grain = G::Paper,
         .grainScale = 1.1f, .grainDepth = 0.55f, .flow = 0.85f, .buildUp = B::Uniform, .pressureSize = 0.2f,
         .pressureOpacity = 0.4f, .minRadius = 2.0f, .maxRadius = 40.0f, .size = 0.4f, .previewSize = 0.15f});
    add("salpicadura", "Salpicadura", C::Artistic,
        {.tip = T::Splatter, .rotationJitter = 1.0f, .spacing = 0.7f, .scatter = 0.5f, .flow = 0.95f,
         .buildUp = B::Glaze, .pressureSize = 0.3f, .sizeJitter = 0.5f, .opacityJitter = 0.2f, .minRadius = 5.0f,
         .maxRadius = 150.0f, .size = 0.4f, .previewSize = 0.28f});

    // --- Caligrafía ---
    add("plumilla", "Plumilla", C::Calligraphy,
        {.tip = T::Nib, .angle = 45.0f, .spacing = 0.03f, .minSpacing = 0.4f, .streamline = 0.3f,
         .taperStart = 0.05f, .taperEnd = 0.08f, .taperTip = 0.4f, .flow = 1.0f, .buildUp = B::Uniform,
         .pressureSize = 0.2f, .minRadius = 1.0f, .maxRadius = 40.0f, .size = 0.4f, .previewSize = 0.16f});
    add("pincel-caligrafico", "Pincel caligráfico", C::Calligraphy,
        {.tip = T::Round, .hardness = 0.9f, .roundness = 0.85f, .angle = 30.0f, .spacing = 0.04f,
         .streamline = 0.4f, .taperStart = 0.35f, .taperEnd = 0.4f, .taperTip = 0.02f, .flow = 1.0f,
         .buildUp = B::Uniform, .pressureSize = 0.95f, .minRadius = 1.0f, .maxRadius = 40.0f, .size = 0.45f,
         .previewSize = 0.14f});
    add("monolinea", "Monolínea", C::Calligraphy,
        {.tip = T::Round, .hardness = 0.95f, .spacing = 0.04f, .streamline = 0.5f, .flow = 1.0f,
         .buildUp = B::Uniform, .pressureSize = 0.0f, .minRadius = 0.75f, .maxRadius = 40.0f, .size = 0.35f,
         .previewSize = 0.1f});

    // --- Clásicos ---
    add("basico", "Básico", C::Classic, classic(T::Classic0));
    add("texturizado", "Texturizado", C::Classic, classic(T::Classic1));
    add("caligrafico", "Caligráfico", C::Classic, classic(T::Classic2));
    add("acuarela", "Acuarela", C::Classic, classic(T::Classic3));
    return list;
}

constexpr brushes::ParamInfo kParams[] = {
    {"espaciado", "Espaciado", &BrushParams::spacing, 0.0f, 1.5f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Square},
    {"dispersion", "Dispersión", &BrushParams::scatter, 0.0f, 2.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Square},
    {"sellos", "Sellos por paso", &BrushParams::count, 1.0f, 8.0f, brushes::ParamFormat::Count,
     brushes::ParamCurve::Linear},
    {"estabilizacion", "Estabilización", &BrushParams::streamline, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"afinado-inicio", "Al empezar", &BrushParams::taperStart, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"afinado-final", "Al terminar", &BrushParams::taperEnd, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"afinado-punta", "Grosor de la punta", &BrushParams::taperTip, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"dureza", "Dureza", &BrushParams::hardness, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"redondez", "Redondez", &BrushParams::roundness, 0.1f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"angulo", "Ángulo", &BrushParams::angle, 0.0f, 360.0f, brushes::ParamFormat::Degrees,
     brushes::ParamCurve::Linear},
    {"giro", "Giro al azar", &BrushParams::rotationJitter, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"grano-escala", "Escala", &BrushParams::grainScale, 0.25f, 4.0f, brushes::ParamFormat::Scale,
     brushes::ParamCurve::Log},
    {"grano-intensidad", "Intensidad", &BrushParams::grainDepth, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"flujo", "Flujo", &BrushParams::flow, 0.01f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Square},
    {"arrastre", "Arrastre", &BrushParams::wetPull, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"carga", "Carga", &BrushParams::wetCharge, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"dilucion", "Dilución", &BrushParams::wetDilution, 0.0f, 1.0f, brushes::ParamFormat::Percent,
     brushes::ParamCurve::Linear},
    {"presion-tamano", "Tamaño con la presión", &BrushParams::pressureSize, 0.0f, 1.0f,
     brushes::ParamFormat::Percent, brushes::ParamCurve::Linear},
    {"presion-opacidad", "Opacidad con la presión", &BrushParams::pressureOpacity, 0.0f, 1.0f,
     brushes::ParamFormat::Percent, brushes::ParamCurve::Linear},
    {"variacion-tamano", "Variación del tamaño", &BrushParams::sizeJitter, 0.0f, 1.0f,
     brushes::ParamFormat::Percent, brushes::ParamCurve::Linear},
    {"variacion-opacidad", "Variación de la opacidad", &BrushParams::opacityJitter, 0.0f, 1.0f,
     brushes::ParamFormat::Percent, brushes::ParamCurve::Linear},
    {"tamano-minimo", "Tamaño mínimo", &BrushParams::minRadius, 0.5f, 100.0f, brushes::ParamFormat::Diameter,
     brushes::ParamCurve::Log},
    {"tamano-maximo", "Tamaño máximo", &BrushParams::maxRadius, 2.0f, 400.0f, brushes::ParamFormat::Diameter,
     brushes::ParamCurve::Log},
};

constexpr const char* kTipNames[kBrushTipCount] = {
    "Redonda", "Lápiz", "Carbón", "Tiza", "Cerdas", "Plana", "Plumilla",
    "Espray", "Salpicadura", "Aguada", "Clásica 1", "Clásica 2", "Clásica 3", "Clásica 4",
};
constexpr const char* kTipKeys[kBrushTipCount] = {
    "redonda", "lapiz", "carbon", "tiza", "cerdas", "plana", "plumilla",
    "espray", "salpicadura", "aguada", "clasica-1", "clasica-2", "clasica-3", "clasica-4",
};
constexpr const char* kGrainNames[kBrushGrainCount] = {"Ninguno", "Papel", "Lienzo", "Rugoso", "Acuarela", "Ruido"};
constexpr const char* kGrainKeys[kBrushGrainCount] = {"ninguno", "papel", "lienzo", "rugoso", "acuarela", "ruido"};

bool parseFloat(std::string_view text, float& out) {
    // std::from_chars de float no está en todas las bibliotecas de C++ de Android.
    std::string copy(text);
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

// Número corto y con punto, igual en cualquier idioma del sistema.
std::string number(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", static_cast<double>(value));
    std::string text(buffer);
    std::replace(text.begin(), text.end(), ',', '.');
    return text;
}

} // namespace

namespace brushes {

std::span<const BrushPreset> library() {
    static const std::vector<BrushPreset> list = makeLibrary();
    return list;
}

const BrushParams& basic() {
    static const BrushParams params = library()[static_cast<size_t>(indexOf("basico"))].params;
    return params;
}

int indexOf(std::string_view id) {
    const auto list = library();
    for (size_t i = 0; i < list.size(); ++i) {
        if (id == list[i].id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* categoryName(BrushCategory category) {
    switch (category) {
    case BrushCategory::Sketching:
        return "Bocetos";
    case BrushCategory::Inking:
        return "Entintado";
    case BrushCategory::Painting:
        return "Pintura";
    case BrushCategory::Airbrushing:
        return "Aerógrafo";
    case BrushCategory::Artistic:
        return "Artísticos";
    case BrushCategory::Calligraphy:
        return "Caligrafía";
    case BrushCategory::Classic:
        return "Clásicos";
    }
    return "";
}

float radiusFor(const BrushParams& params, float size) {
    const float lo = std::min(params.minRadius, params.maxRadius);
    const float hi = std::max(params.minRadius, params.maxRadius);
    const float t = std::clamp(size, 0.0f, 1.0f);
    return lo + (hi - lo) * t * t;
}

float sizeFor(const BrushParams& params, float radius) {
    const float lo = std::min(params.minRadius, params.maxRadius);
    const float hi = std::max(params.minRadius, params.maxRadius);
    if (hi - lo <= 0.0f) {
        return 0.0f;
    }
    return std::sqrt(std::clamp((radius - lo) / (hi - lo), 0.0f, 1.0f));
}

bool isWet(const BrushParams& params) {
    return params.wetPull > 0.0f || params.wetCharge < 1.0f || params.wetDilution > 0.0f;
}

float wetPaint(const BrushParams& params, float radius, float distance) {
    // La dilución deja siempre algo de pigmento: con toda el agua, un 5 %.
    const float amount = 1.0f - 0.95f * std::clamp(params.wetDilution, 0.0f, 1.0f);
    const float charge = std::clamp(params.wetCharge, 0.0f, 1.0f);
    if (charge >= 1.0f) {
        return amount;
    }
    // La pintura baja a un tercio en un diámetro con carga 0, en unos 13 con carga 0,5 y en
    // más de 100 con 0,9.
    const float length = 2.0f * std::max(radius, 0.5f) * (1.0f + 12.0f * charge / (1.0f - charge));
    return amount * std::exp(-std::max(distance, 0.0f) / length);
}

std::span<const ParamInfo> params() { return kParams; }

const ParamInfo* findParam(std::string_view key) {
    for (const ParamInfo& info : kParams) {
        if (key == info.key) {
            return &info;
        }
    }
    return nullptr;
}

float toSlider(const ParamInfo& info, float value) {
    value = std::clamp(value, info.min, info.max);
    switch (info.curve) {
    case ParamCurve::Linear:
        return (value - info.min) / (info.max - info.min);
    case ParamCurve::Square:
        return std::sqrt((value - info.min) / (info.max - info.min));
    case ParamCurve::Log:
        return std::log(value / info.min) / std::log(info.max / info.min);
    }
    return 0.0f;
}

float fromSlider(const ParamInfo& info, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float value = info.min;
    switch (info.curve) {
    case ParamCurve::Linear:
        value = info.min + (info.max - info.min) * t;
        break;
    case ParamCurve::Square:
        value = info.min + (info.max - info.min) * t * t;
        break;
    case ParamCurve::Log:
        value = info.min * std::pow(info.max / info.min, t);
        break;
    }
    if (info.format == ParamFormat::Count) {
        value = std::round(value);
    }
    return std::clamp(value, info.min, info.max);
}

std::string formatValue(const ParamInfo& info, float value) {
    char buffer[32];
    switch (info.format) {
    case ParamFormat::Percent:
        std::snprintf(buffer, sizeof(buffer), "%d %%", static_cast<int>(std::lround(value * 100.0f)));
        break;
    case ParamFormat::Degrees:
        std::snprintf(buffer, sizeof(buffer), "%d°", static_cast<int>(std::lround(value)));
        break;
    case ParamFormat::Count:
        std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(std::lround(value)));
        break;
    case ParamFormat::Scale: {
        // Coma decimal, como se escribe en español.
        const int tenths = static_cast<int>(std::lround(value * 100.0f));
        std::snprintf(buffer, sizeof(buffer), "×%d,%02d", tenths / 100, tenths % 100);
        break;
    }
    case ParamFormat::Diameter: {
        const float diameter = value * 2.0f;
        if (diameter < 10.0f) {
            const int tenths = static_cast<int>(std::lround(diameter * 10.0f));
            std::snprintf(buffer, sizeof(buffer), "%d,%d px", tenths / 10, tenths % 10);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%d px", static_cast<int>(std::lround(diameter)));
        }
        break;
    }
    }
    return buffer;
}

const char* tipName(BrushTip tip) { return kTipNames[static_cast<int>(tip)]; }
const char* tipKey(BrushTip tip) { return kTipKeys[static_cast<int>(tip)]; }
const char* grainName(BrushGrain grain) { return kGrainNames[static_cast<int>(grain)]; }
const char* grainKey(BrushGrain grain) { return kGrainKeys[static_cast<int>(grain)]; }

std::string describeChanges(const BrushParams& base, const BrushParams& params) {
    std::ostringstream out;
    if (params.tip != base.tip) {
        out << "punta " << tipKey(params.tip) << '\n';
    }
    if (params.grain != base.grain) {
        out << "grano " << grainKey(params.grain) << '\n';
    }
    if (params.buildUp != base.buildUp) {
        out << "acumulacion " << (params.buildUp == BrushBuildUp::Uniform ? "uniforme" : "acumula") << '\n';
    }
    if (params.followStroke != base.followStroke) {
        out << "sigue-trazo " << (params.followStroke ? 1 : 0) << '\n';
    }
    for (const ParamInfo& info : kParams) {
        if (params.*info.member != base.*info.member) {
            out << info.key << ' ' << number(params.*info.member) << '\n';
        }
    }
    return out.str();
}

bool applySetting(BrushParams& params, std::string_view key, std::string_view value) {
    if (key == "punta") {
        for (int i = 0; i < kBrushTipCount; ++i) {
            if (value == kTipKeys[i]) {
                params.tip = static_cast<BrushTip>(i);
                return true;
            }
        }
        return false;
    }
    if (key == "grano") {
        for (int i = 0; i < kBrushGrainCount; ++i) {
            if (value == kGrainKeys[i]) {
                params.grain = static_cast<BrushGrain>(i);
                return true;
            }
        }
        return false;
    }
    if (key == "acumulacion") {
        if (value == "uniforme" || value == "acumula") {
            params.buildUp = value == "uniforme" ? BrushBuildUp::Uniform : BrushBuildUp::Glaze;
            return true;
        }
        return false;
    }
    if (key == "sigue-trazo") {
        if (value == "0" || value == "1") {
            params.followStroke = value == "1";
            return true;
        }
        return false;
    }
    const ParamInfo* info = findParam(key);
    float number = 0.0f;
    if (!info || !parseFloat(value, number)) {
        return false;
    }
    params.*info->member = std::clamp(number, info->min, info->max);
    return true;
}

void sanitize(BrushParams& params) {
    for (const ParamInfo& info : kParams) {
        float& value = params.*info.member;
        value = std::isfinite(value) ? std::clamp(value, info.min, info.max) : info.min;
    }
    if (params.minRadius > params.maxRadius) {
        std::swap(params.minRadius, params.maxRadius);
    }
    params.count = std::round(params.count);
    params.minSpacing = std::max(params.minSpacing, 0.1f);
    params.flow = std::clamp(params.flow, 0.01f, 1.0f);
    params.eraseFlow = std::clamp(params.eraseFlow, 0.0f, 1.0f);
}

} // namespace brushes
