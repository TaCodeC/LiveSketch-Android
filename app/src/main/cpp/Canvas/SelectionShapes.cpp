#include "Canvas/SelectionShapes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace selection {

namespace {

constexpr float kPi = 3.14159265358979f;

// Acumulación de área con signo (como el rasterizador de font-rs): cada segmento suma en
// su fila, a cada píxel que cruza, la parte del área que queda a su derecha; la suma de
// la fila de izquierda a derecha da la cobertura. `acc` tiene filas de `stride`
// (el ancho más dos) y `x` va de 0 a `width`.
void accumulateLine(std::vector<float>& acc, int width, int height, size_t stride, glm::vec2 a, glm::vec2 b) {
    if (a.y == b.y) {
        return;
    }
    float dir = 1.0f;
    if (a.y > b.y) {
        std::swap(a, b);
        dir = -1.0f;
    }
    const float dxdy = (b.x - a.x) / (b.y - a.y);
    float x = a.x;
    int y0 = static_cast<int>(std::floor(a.y));
    if (a.y < 0.0f) {
        x -= a.y * dxdy;
        y0 = 0;
    }
    const int y1 = std::min(height, static_cast<int>(std::ceil(b.y)));
    const float w = static_cast<float>(width);
    for (int y = y0; y < y1; ++y) {
        float* row = acc.data() + static_cast<size_t>(y) * stride;
        const float dy = std::min(static_cast<float>(y + 1), b.y) - std::max(static_cast<float>(y), a.y);
        const float xnext = x + dxdy * dy;
        const float d = dy * dir;
        const float x0 = std::clamp(std::min(x, xnext), 0.0f, w);
        const float x1 = std::clamp(std::max(x, xnext), 0.0f, w);
        const float x0floor = std::floor(x0);
        const int x0i = static_cast<int>(x0floor);
        const float x1ceil = std::ceil(x1);
        const int x1i = static_cast<int>(x1ceil);
        if (x1i <= x0i + 1) {
            // El segmento no sale de un píxel en esta fila.
            const float xmf = std::clamp(0.5f * (x0 + x1) - x0floor, 0.0f, 1.0f);
            row[x0i] += d - d * xmf;
            row[x0i + 1] += d * xmf;
        } else {
            const float s = 1.0f / (x1 - x0);
            const float x0f = x0 - x0floor;
            const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
            const float x1f = x1 - x1ceil + 1.0f;
            const float am = 0.5f * s * x1f * x1f;
            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.0f - a0 - am);
            } else {
                const float a1 = s * (1.5f - x0f);
                row[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; ++xi) {
                    row[xi] += d * s;
                }
                const float a2 = a1 + static_cast<float>(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.0f - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xnext;
    }
}

} // namespace

bool rasterize(std::span<const glm::vec2> polygon, const IRect& clip, std::vector<uint8_t>& coverage,
               IRect& bounds) {
    coverage.clear();
    bounds = {};
    if (polygon.size() < 3 || clip.empty()) {
        return false;
    }
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    for (const glm::vec2& p : polygon) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            return false;
        }
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    // Sin pasarse del tamaño de un entero con polígonos disparatados.
    constexpr float kLimit = 1.0e7f;
    const IRect box = IRect{static_cast<int>(std::floor(std::max(minX, -kLimit))),
                            static_cast<int>(std::floor(std::max(minY, -kLimit))),
                            static_cast<int>(std::ceil(std::min(maxX, kLimit))),
                            static_cast<int>(std::ceil(std::min(maxY, kLimit)))}
                          .intersected(clip);
    if (box.empty()) {
        return false;
    }

    const int width = box.width();
    const int height = box.height();
    const size_t stride = static_cast<size_t>(width) + 2;
    std::vector<float> acc(stride * static_cast<size_t>(height), 0.0f);
    // Lo que se sale por los lados se aplasta contra el borde: la cobertura de dentro no
    // cambia (los cruces siguen contando, solo que en el borde).
    auto local = [&](const glm::vec2& p) {
        return glm::vec2(std::clamp(p.x - static_cast<float>(box.x0), 0.0f, static_cast<float>(width)),
                         p.y - static_cast<float>(box.y0));
    };
    for (size_t i = 0; i < polygon.size(); ++i) {
        accumulateLine(acc, width, height, stride, local(polygon[i]), local(polygon[(i + 1) % polygon.size()]));
    }

    coverage.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    bool any = false;
    for (int y = 0; y < height; ++y) {
        const float* row = acc.data() + static_cast<size_t>(y) * stride;
        uint8_t* out = coverage.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        float sum = 0.0f;
        for (int x = 0; x < width; ++x) {
            sum += row[x];
            const float c = std::min(std::fabs(sum), 1.0f);
            const auto value = static_cast<uint8_t>(c * 255.0f + 0.5f);
            out[x] = value;
            any = any || value != 0;
        }
    }
    bounds = box;
    return any;
}

std::vector<glm::vec2> rectangle(glm::vec2 a, glm::vec2 b) {
    const glm::vec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
    const glm::vec2 hi(std::max(a.x, b.x), std::max(a.y, b.y));
    return {lo, glm::vec2(hi.x, lo.y), hi, glm::vec2(lo.x, hi.y)};
}

std::vector<glm::vec2> ellipse(glm::vec2 a, glm::vec2 b) {
    const glm::vec2 center = (a + b) * 0.5f;
    const glm::vec2 radius(std::fabs(b.x - a.x) * 0.5f, std::fabs(b.y - a.y) * 0.5f);
    // Un vértice cada dos píxeles de perímetro, más o menos: el error queda muy por debajo
    // de un píxel.
    const float perimeter = 2.0f * kPi * std::sqrt((radius.x * radius.x + radius.y * radius.y) * 0.5f);
    const int count = std::clamp(static_cast<int>(std::ceil(perimeter * 0.5f)), 16, 4096);
    std::vector<glm::vec2> points(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float t = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(count);
        points[static_cast<size_t>(i)] = center + glm::vec2(std::cos(t) * radius.x, std::sin(t) * radius.y);
    }
    return points;
}

bool autoLevels(const uint8_t* rgba, int width, int height, int x, int y, AutoLevels& out) {
    out.width = 0;
    out.height = 0;
    out.levels.clear();
    out.bounds.fill(IRect{});
    if (!rgba || width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height) {
        return false;
    }
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    constexpr uint8_t kUnvisited = 255;
    out.levels.assign(count, kUnvisited);
    out.width = width;
    out.height = height;

    const size_t seedIndex = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
    const uint8_t* seed = rgba + seedIndex * 4;
    auto difference = [&](size_t index) {
        const uint8_t* p = rgba + index * 4;
        int d = 0;
        for (int c = 0; c < 4; ++c) {
            d = std::max(d, std::abs(static_cast<int>(p[c]) - static_cast<int>(seed[c])));
        }
        return static_cast<uint8_t>(std::min(d, 254));
    };

    // Inundación por niveles: siempre se sigue por el píxel pendiente de nivel más bajo, así
    // que cada uno recibe el menor "peor escalón" con el que se puede llegar a él.
    std::array<std::vector<uint32_t>, 255> buckets;
    buckets[0].push_back(static_cast<uint32_t>(seedIndex));
    out.levels[seedIndex] = 0;
    IRect box;
    for (int level = 0; level < 255; ++level) {
        std::vector<uint32_t>& bucket = buckets[static_cast<size_t>(level)];
        while (!bucket.empty()) {
            const uint32_t index = bucket.back();
            bucket.pop_back();
            const int px = static_cast<int>(index % static_cast<uint32_t>(width));
            const int py = static_cast<int>(index / static_cast<uint32_t>(width));
            box.unite(IRect{px, py, px + 1, py + 1});
            auto visit = [&](size_t next) {
                if (out.levels[next] != kUnvisited) {
                    return;
                }
                const uint8_t nextLevel = std::max(static_cast<uint8_t>(level), difference(next));
                out.levels[next] = nextLevel;
                buckets[nextLevel].push_back(static_cast<uint32_t>(next));
            };
            if (px > 0) {
                visit(index - 1);
            }
            if (px + 1 < width) {
                visit(index + 1);
            }
            if (py > 0) {
                visit(index - static_cast<uint32_t>(width));
            }
            if (py + 1 < height) {
                visit(index + static_cast<uint32_t>(width));
            }
        }
        std::vector<uint32_t>().swap(bucket);
        out.bounds[static_cast<size_t>(level)] = box;
    }
    out.bounds[255] = box;
    return true;
}

int autoCutoff(float threshold) {
    return static_cast<int>(std::lround(std::clamp(threshold, 0.0f, 1.0f) * 254.0f));
}

IRect tightBounds(const uint8_t* rgba, int width, int height, int channel, const uint8_t* mask, int maskChannel) {
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < height; ++y) {
        const size_t rowStart = static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        const uint8_t* row = rgba + rowStart;
        const uint8_t* maskRow = mask ? mask + rowStart : nullptr;
        int first = -1;
        int last = -1;
        for (int x = 0; x < width; ++x) {
            if (row[x * 4 + channel] != 0 && (!maskRow || maskRow[x * 4 + maskChannel] != 0)) {
                if (first < 0) {
                    first = x;
                }
                last = x;
            }
        }
        if (first >= 0) {
            minX = std::min(minX, first);
            maxX = std::max(maxX, last);
            minY = std::min(minY, y);
            maxY = y;
        }
    }
    if (maxX < 0) {
        return {};
    }
    return {minX, minY, maxX + 1, maxY + 1};
}

} // namespace selection
