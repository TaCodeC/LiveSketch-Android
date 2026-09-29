#pragma once

#include <algorithm>
#include <cmath>

// Rectángulo entero en píxeles del lienzo: [x0, x1) × [y0, y1), con y hacia abajo.
struct IRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }

    bool operator==(const IRect&) const = default;

    static IRect ofSize(int width, int height) { return {0, 0, width, height}; }

    // Caja que cubre un círculo, con un píxel de margen por el filtrado lineal.
    static IRect around(float cx, float cy, float radius) {
        return {static_cast<int>(std::floor(cx - radius)) - 1, static_cast<int>(std::floor(cy - radius)) - 1,
                static_cast<int>(std::ceil(cx + radius)) + 1, static_cast<int>(std::ceil(cy + radius)) + 1};
    }

    void unite(const IRect& other) {
        if (other.empty()) {
            return;
        }
        if (empty()) {
            *this = other;
            return;
        }
        x0 = std::min(x0, other.x0);
        y0 = std::min(y0, other.y0);
        x1 = std::max(x1, other.x1);
        y1 = std::max(y1, other.y1);
    }

    IRect intersected(const IRect& other) const {
        IRect r{std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1), std::min(y1, other.y1)};
        return r.empty() ? IRect{} : r;
    }
};
