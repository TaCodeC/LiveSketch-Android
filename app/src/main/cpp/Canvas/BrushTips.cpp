#include "Canvas/BrushTips.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr float kPi = 3.14159265358979f;

uint32_t mixBits(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// [0, 1) a partir de 24 bits del hash.
float unit(uint32_t bits) { return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f); }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// 0 en `from`, 1 en `to` (sirve en los dos sentidos), con curva suave.
float ramp(float x, float from, float to) {
    const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Valor del retículo en (x, y), que se repite cada `period` celdas.
float lattice(uint32_t seed, int x, int y, int period) {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    const uint32_t h = mixBits(static_cast<uint32_t>(x) * 0x9E3779B1U ^ mixBits(static_cast<uint32_t>(y) + 0x85EBCA77U));
    return unit(mixBits(h ^ seed));
}

// Ruido de valor en [0, 1], periódico: (u, v) en fracción de la imagen, `period` celdas.
float valueNoise(uint32_t seed, float u, float v, int period) {
    const float x = u * static_cast<float>(period);
    const float y = v * static_cast<float>(period);
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const float tx = fade(x - fx);
    const float ty = fade(y - fy);
    const float top = lerp(lattice(seed, ix, iy, period), lattice(seed, ix + 1, iy, period), tx);
    const float bottom = lerp(lattice(seed, ix, iy + 1, period), lattice(seed, ix + 1, iy + 1, period), tx);
    return lerp(top, bottom, ty);
}

// Octavas de ruido (cada una con el doble de celdas), normalizadas a [0, 1].
float fbm(uint32_t seed, float u, float v, int period, int octaves, float gain) {
    float sum = 0.0f;
    float total = 0.0f;
    float amplitude = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * valueNoise(seed + static_cast<uint32_t>(i) * 7919U, u, v, period << i);
        total += amplitude;
        amplitude *= gain;
    }
    return sum / total;
}

// Azar con semilla fija para colocar cerdas, gotas y manchas.
struct Random {
    uint32_t state;
    float next() {
        state = state * 1664525U + 1013904223U;
        return unit(mixBits(state));
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

// Imagen en coma flotante mientras se genera.
struct Field {
    int size;
    std::vector<float> values;

    explicit Field(int n) : size(n), values(static_cast<size_t>(n) * static_cast<size_t>(n), 0.0f) {}

    // Llama a f(x, y) con el centro de cada píxel en [-1, 1] (y hacia abajo) y guarda el
    // resultado.
    void eval(const std::function<float(float, float)>& f) {
        for (int j = 0; j < size; ++j) {
            const float y = (static_cast<float>(j) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            for (int i = 0; i < size; ++i) {
                const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                values[static_cast<size_t>(j) * static_cast<size_t>(size) + static_cast<size_t>(i)] = f(x, y);
            }
        }
    }

    // Suma una mancha redonda en (cx, cy) de radio r (en [-1, 1]) con `strength`, como
    // capas que se tapan: v = 1 − (1 − v)(1 − mancha). `hard`: borde nítido (antialias
    // de un píxel); si no, cae suave hasta el borde. `wobble`: borde irregular.
    void splat(float cx, float cy, float r, float strength, bool hard, uint32_t wobbleSeed = 0, float wobble = 0.0f) {
        const float pixel = 2.0f / static_cast<float>(size);
        const float reach = r * (1.0f + wobble) + pixel;
        const int i0 = std::max(0, static_cast<int>(std::floor((cx - reach + 1.0f) / pixel)));
        const int i1 = std::min(size - 1, static_cast<int>(std::ceil((cx + reach + 1.0f) / pixel)));
        const int j0 = std::max(0, static_cast<int>(std::floor((cy - reach + 1.0f) / pixel)));
        const int j1 = std::min(size - 1, static_cast<int>(std::ceil((cy + reach + 1.0f) / pixel)));
        for (int j = j0; j <= j1; ++j) {
            const float y = (static_cast<float>(j) + 0.5f) * pixel - 1.0f;
            for (int i = i0; i <= i1; ++i) {
                const float x = (static_cast<float>(i) + 0.5f) * pixel - 1.0f;
                const float dx = x - cx;
                const float dy = y - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                float radius = r;
                if (wobble > 0.0f && d > 0.0f) {
                    // Radio según el ángulo, con ruido periódico alrededor del círculo.
                    const float angle = std::atan2(dy, dx) / (2.0f * kPi) + 0.5f;
                    radius *= 1.0f + wobble * (valueNoise(wobbleSeed, angle, 0.5f, 7) * 2.0f - 1.0f);
                }
                const float value = hard ? std::clamp((radius - d) / pixel + 0.5f, 0.0f, 1.0f)
                                         : ramp(d, radius, radius * 0.25f);
                float& v = values[static_cast<size_t>(j) * static_cast<size_t>(size) + static_cast<size_t>(i)];
                v = 1.0f - (1.0f - v) * (1.0f - value * strength);
            }
        }
    }

    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out(values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            out[i] = static_cast<uint8_t>(std::lround(std::clamp(values[i], 0.0f, 1.0f) * 255.0f));
        }
        return out;
    }
};

// (x, y) de [-1, 1] a [0, 1], para el ruido.
float to01(float v) { return v * 0.5f + 0.5f; }

void pencil(Field& f) {
    f.eval([](float x, float y) {
        const float d = std::sqrt(x * x + y * y);
        const float u = to01(x);
        const float v = to01(y);
        const float radius = 0.84f + 0.12f * (valueNoise(11, u, v, 6) - 0.5f);
        const float shape = ramp(d, radius, radius - 0.14f);
        const float grain = ramp(fbm(12, u, v, 24, 3, 0.55f), 0.3f, 0.72f);
        return shape * (0.4f + 0.6f * grain);
    });
}

void charcoal(Field& f) {
    f.eval([](float x, float y) {
        const float d = std::sqrt(x * x + y * y);
        const float u = to01(x);
        const float v = to01(y);
        const float radius = 0.8f + 0.3f * (valueNoise(21, u, v, 4) - 0.5f);
        const float shape = ramp(d, radius, radius - 0.12f);
        const float holes = ramp(fbm(22, u, v, 10, 3, 0.55f), 0.3f, 0.6f);
        const float fine = fbm(23, u, v, 40, 2, 0.5f);
        return shape * std::pow(holes, 0.8f) * (0.65f + 0.35f * fine);
    });
}

void chalk(Field& f) {
    f.eval([](float x, float y) {
        const float u = to01(x);
        const float v = to01(y);
        // Bloque de esquinas redondeadas, con el borde mordido.
        const float q = std::pow(std::pow(std::abs(x), 4.0f) + std::pow(std::abs(y), 4.0f), 0.25f);
        const float edge = 0.78f + 0.16f * (valueNoise(31, u, v, 8) - 0.5f);
        const float shape = ramp(q, edge, edge - 0.08f);
        const float speckle = ramp(fbm(32, u, v, 32, 3, 0.5f), 0.36f, 0.56f);
        return shape * speckle;
    });
}

void bristle(Field& f) {
    // Pincel de cerdas visto de frente (se usa girado 90°, a lo ancho del trazo): unas
    // columnas de cerdas de distinta carga a lo largo de x. Arrastrado deja vetas.
    Random random{41};
    f.eval([](float x, float y) {
        const float u = to01(x);
        const float edge = 0.4f + 0.05f * (valueNoise(42, u, 0.5f, 5) - 0.5f);
        const float shape = ramp(x * x / 0.81f + y * y / (edge * edge), 1.0f, 0.7f);
        return 0.7f * shape;
    });
    constexpr int kColumns = 22;
    for (int k = 0; k < kColumns; ++k) {
        const float x = -0.86f + 1.72f * (static_cast<float>(k) + random.range(0.2f, 0.8f)) / kColumns;
        const float half = 0.4f * std::sqrt(std::max(0.0f, 1.0f - x * x / 0.81f));
        const float load = random.range(0.6f, 1.0f);
        const int splats = 3 + static_cast<int>(random.next() * 3.0f);
        for (int i = 0; i < splats; ++i) {
            const float y = random.range(-half, half) * 0.8f;
            f.splat(x + random.range(-0.012f, 0.012f), y, random.range(0.04f, 0.06f), load, false);
        }
    }
}

void flat(Field& f) {
    f.eval([](float x, float y) {
        const float u = to01(x);
        const float v = to01(y);
        // Los extremos, algo deshilachados.
        const float frayed = 0.9f + 0.06f * (valueNoise(54, u, v, 12) - 0.5f);
        const float ends = ramp(std::abs(x), frayed, frayed - 0.14f);
        const float width = 0.3f + 0.04f * (valueNoise(51, u, 0.5f, 6) - 0.5f);
        const float sides = ramp(std::abs(y), width, width - 0.1f);
        // Densidad de cerdas a lo ancho: al arrastrar deja vetas suaves en el sentido del
        // trazo.
        const float bristles = ramp(valueNoise(52, u, 0.5f, 48), 0.1f, 0.8f);
        const float along = 0.9f + 0.1f * valueNoise(53, u, v, 16);
        return ends * sides * (0.82f + 0.18f * bristles) * along;
    });
}

void nib(Field& f) {
    const float pixel = 2.0f / static_cast<float>(f.size);
    f.eval([pixel](float x, float y) {
        const float across = std::clamp((0.95f - std::abs(x)) / pixel + 0.5f, 0.0f, 1.0f);
        const float thick = std::clamp((0.2f - std::abs(y)) / pixel + 0.5f, 0.0f, 1.0f);
        return across * thick;
    });
}

void spray(Field& f) {
    Random random{61};
    for (int i = 0; i < 320; ++i) {
        // Posición gaussiana (Box-Muller), más densa en el centro.
        const float a = std::max(random.next(), 1e-6f);
        const float b = random.next();
        const float r = 0.36f * std::sqrt(-2.0f * std::log(a));
        if (r > 0.9f) {
            continue;
        }
        const float angle = 2.0f * kPi * b;
        const float size = 0.012f + 0.03f * random.next() * random.next();
        f.splat(r * std::cos(angle), r * std::sin(angle), size, random.range(0.6f, 1.0f), true);
    }
}

void splatter(Field& f) {
    Random random{71};
    f.splat(random.range(-0.1f, 0.1f), random.range(-0.1f, 0.1f), 0.28f, 1.0f, true, 72, 0.18f);
    for (int i = 0; i < 12; ++i) {
        const float r = random.range(0.35f, 0.82f);
        const float angle = random.range(0.0f, 2.0f * kPi);
        const float size = random.range(0.025f, 0.11f);
        f.splat(r * std::cos(angle), r * std::sin(angle), std::min(size, 0.9f - r), 1.0f, true,
                73 + static_cast<uint32_t>(i), 0.2f);
    }
}

void wash(Field& f) {
    f.eval([](float x, float y) {
        const float d = std::sqrt(x * x + y * y);
        const float u = to01(x);
        const float v = to01(y);
        const float body = ramp(d, 0.95f, 0.3f);
        const float rim = std::exp(-((d - 0.8f) / 0.09f) * ((d - 0.8f) / 0.09f));
        const float fade = ramp(d, 0.97f, 0.88f);
        const float cloud = 0.8f + 0.2f * fbm(81, u, v, 4, 3, 0.5f);
        return (0.55f * body + 0.45f * rim) * fade * cloud;
    });
}

void disk(Field& f) {
    const float pixel = 2.0f / static_cast<float>(f.size);
    f.eval([pixel](float x, float y) {
        return std::clamp((0.94f - std::sqrt(x * x + y * y)) / pixel + 0.5f, 0.0f, 1.0f);
    });
}

} // namespace

namespace brushtips {

std::vector<uint8_t> makeTip(BrushTip tip, int size) {
    Field field(std::max(size, 8));
    switch (tip) {
    case BrushTip::Round:
        disk(field);
        break;
    case BrushTip::Pencil:
        pencil(field);
        break;
    case BrushTip::Charcoal:
        charcoal(field);
        break;
    case BrushTip::Chalk:
        chalk(field);
        break;
    case BrushTip::Bristle:
        bristle(field);
        break;
    case BrushTip::Flat:
        flat(field);
        break;
    case BrushTip::Nib:
        nib(field);
        break;
    case BrushTip::Spray:
        spray(field);
        break;
    case BrushTip::Splatter:
        splatter(field);
        break;
    case BrushTip::Wash:
        wash(field);
        break;
    case BrushTip::Classic0:
    case BrushTip::Classic1:
    case BrushTip::Classic2:
    case BrushTip::Classic3:
        return {};
    }
    return field.bytes();
}

std::vector<uint8_t> makeGrain(BrushGrain grain, int size) {
    Field field(std::max(size, 8));
    const int n = field.size;
    // Coordenadas en fracción de la textura: el ruido se repite con ella.
    auto each = [&](const std::function<float(float, float, int, int)>& f) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                const float v = (static_cast<float>(j) + 0.5f) / static_cast<float>(n);
                field.values[static_cast<size_t>(j) * static_cast<size_t>(n) + static_cast<size_t>(i)] = f(u, v, i, j);
            }
        }
    };
    switch (grain) {
    case BrushGrain::None:
        each([](float, float, int, int) { return 1.0f; });
        break;
    case BrushGrain::Paper:
        // Diente del papel: poros finos (unos 5 píxeles), repartidos por igual.
        each([](float u, float v, int, int) { return ramp(fbm(101, u, v, 48, 3, 0.5f), 0.3f, 0.7f); });
        break;
    case BrushGrain::Canvas:
        each([n](float u, float v, int, int) {
            // Tejido: hilos de 8 píxeles que se cruzan por arriba y por abajo, cada tramo
            // con su propio relieve para que no parezca una malla.
            const int cells = std::max(1, n / 8);
            const float cx = u * static_cast<float>(cells);
            const float cy = v * static_cast<float>(cells);
            const int ix = static_cast<int>(std::floor(cx));
            const int iy = static_cast<int>(std::floor(cy));
            const float fx = cx - static_cast<float>(ix);
            const float fy = cy - static_cast<float>(iy);
            const bool horizontal = ((ix + iy) & 1) == 0;
            const float across = horizontal ? fy : fx;
            const float along = horizontal ? fx : fy;
            const float tone = 0.75f + 0.25f * lattice(112, ix, iy, cells);
            const float bump = std::sin(kPi * across) * (0.7f + 0.3f * std::sin(kPi * along)) * tone;
            const float noise = fbm(111, u, v, 64, 2, 0.5f);
            return std::clamp(0.12f + 0.88f * bump + 0.3f * (noise - 0.5f), 0.0f, 1.0f);
        });
        break;
    case BrushGrain::Rough:
        // Papel de grano grueso: poros de unos 8 píxeles y algo de relieve más ancho, sin
        // manchas grandes que corten el trazo en tramos.
        each([](float u, float v, int, int) {
            const float tooth = fbm(121, u, v, 32, 3, 0.55f);
            const float relief = fbm(122, u, v, 8, 2, 0.5f);
            return ramp(0.8f * tooth + 0.2f * relief, 0.28f, 0.72f);
        });
        break;
    case BrushGrain::Watercolor:
        each([](float u, float v, int, int) {
            const float cloud = ramp(fbm(131, u, v, 4, 5, 0.55f), 0.22f, 0.78f);
            const float granulation = valueNoise(132, u, v, 128);
            return std::clamp(0.25f + 0.75f * cloud - 0.18f * granulation, 0.0f, 1.0f);
        });
        break;
    case BrushGrain::Noise:
        each([n](float, float, int i, int j) { return lattice(141, i, j, n); });
        break;
    }
    return field.bytes();
}

} // namespace brushtips
