#include "UI/Kit.h"

#include "IO/Assets.h"
#include "UI/Anim.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

namespace {

// Inter: (ascendente - descendente) / unidades por em = (1984 + 494) / 2048.
constexpr float kInterLineHeight = 2478.0f / 2048.0f;
// Distancia que puede moverse el dedo en un toque (más, y es un arrastre).
constexpr float kTapSlop = 10.0f;

float g_scale = 1.0f;
float g_pixels = 1.0f;

ImFont* g_fonts[3] = {nullptr, nullptr, nullptr};
ImFont* g_icons = nullptr;

ImTextureID g_backdrop = ImTextureID_Invalid;
ImVec2 g_display(1.0f, 1.0f);

struct ScrollState {
    float offset = 0.0f;
    float velocity = 0.0f;      // unidades por segundo (inercia)
    bool tracking = false;      // la pulsación empezó dentro
    bool dragging = false;
    double lastMove = 0.0;
};
std::unordered_map<ImGuiID, ScrollState> g_scrolls;
struct ScrollFrame {
    ImGuiID id;
    ImRect view;
    float contentHeight;
};
std::vector<ScrollFrame> g_scrollStack;

float lerp(float a, float b, float t) { return a + (b - a) * t; }

ImFont* addFont(const char* asset, const char* name) {
    std::vector<uint8_t> data = io::loadAsset(asset);
    if (data.empty()) {
        return nullptr;
    }
    // El atlas se queda con los datos y los libera con IM_FREE.
    void* copy = IM_ALLOC(data.size());
    std::memcpy(copy, data.data(), data.size());
    ImFontConfig config;
    config.FontDataOwnedByAtlas = true;
    ImFormatString(config.Name, IM_COUNTOF(config.Name), "%s", name);
    ImFont* font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(data.size()), 0.0f, &config);
    if (!font) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Fuente inválida: %s", asset);
    }
    return font;
}

// Contorno de un rectángulo redondeado en sentido horario, empezando por la esquina
// superior izquierda. Cada punto lleva el centro de su esquina y su dirección (normal),
// así se pueden sacar contornos paralelos por fuera o por dentro.
struct ContourPoint {
    ImVec2 center;
    ImVec2 normal;
    float radius;
};

void roundedContour(const ImRect& r, float radius, ImDrawFlags corners, int segments,
                    std::vector<ContourPoint>& out) {
    out.clear();
    radius = std::max(0.0f, std::min(radius, std::min(r.GetWidth(), r.GetHeight()) * 0.5f));
    if (corners == 0) {
        corners = ImDrawFlags_RoundCornersAll;
    }
    struct Corner {
        ImDrawFlags flag;
        ImVec2 point;
        ImVec2 inward;
        float angle;
    };
    const Corner list[4] = {
        {ImDrawFlags_RoundCornersTopLeft, r.Min, ImVec2(1, 1), IM_PI},
        {ImDrawFlags_RoundCornersTopRight, ImVec2(r.Max.x, r.Min.y), ImVec2(-1, 1), IM_PI * 1.5f},
        {ImDrawFlags_RoundCornersBottomRight, r.Max, ImVec2(-1, -1), 0.0f},
        {ImDrawFlags_RoundCornersBottomLeft, ImVec2(r.Min.x, r.Max.y), ImVec2(1, -1), IM_PI * 0.5f},
    };
    for (const Corner& corner : list) {
        const float cornerRadius = (corners & corner.flag) ? radius : 0.0f;
        const ImVec2 center(corner.point.x + corner.inward.x * cornerRadius,
                            corner.point.y + corner.inward.y * cornerRadius);
        for (int i = 0; i <= segments; ++i) {
            const float a = corner.angle + (IM_PI * 0.5f) * static_cast<float>(i) / static_cast<float>(segments);
            out.push_back({center, ImVec2(std::cos(a), std::sin(a)), cornerRadius});
        }
    }
}

// Bandas entre contornos paralelos con un color por contorno (degradado por vértice).
struct Band {
    float expand;   // hacia fuera (> 0) o hacia dentro (< 0) del contorno
    float offsetY;
    ImU32 color;
};

void drawBands(ImDrawList* dl, const std::vector<ContourPoint>& contour, const Band* bands, int bandCount,
               float (*weight)(const ContourPoint&) = nullptr) {
    const int n = static_cast<int>(contour.size());
    if (n < 3 || bandCount < 2) {
        return;
    }
    const int vtxCount = n * bandCount;
    const int idxCount = n * (bandCount - 1) * 6;
    dl->PrimReserve(idxCount, vtxCount);
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const unsigned base = dl->_VtxCurrentIdx;
    for (int b = 0; b < bandCount; ++b) {
        for (const ContourPoint& p : contour) {
            const float r = std::max(0.0f, p.radius + bands[b].expand);
            const ImVec2 pos(p.center.x + p.normal.x * r, p.center.y + p.normal.y * r + bands[b].offsetY);
            ImU32 color = bands[b].color;
            if (weight) {
                color = withAlpha(color, weight(p));
            }
            dl->PrimWriteVtx(pos, uv, color);
        }
    }
    for (int b = 0; b + 1 < bandCount; ++b) {
        for (int i = 0; i < n; ++i) {
            const unsigned j = static_cast<unsigned>((i + 1) % n);
            const unsigned a0 = base + static_cast<unsigned>(b * n + i);
            const unsigned a1 = base + static_cast<unsigned>(b * n) + j;
            const unsigned b0 = base + static_cast<unsigned>((b + 1) * n + i);
            const unsigned b1 = base + static_cast<unsigned>((b + 1) * n) + j;
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a0));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a0));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b0));
        }
    }
}

// Relleno del rectángulo redondeado por debajo de la altura `yCut` (intersección de un
// polígono convexo con un semiplano).
void fillRoundedBelow(ImDrawList* dl, const ImRect& r, float radius, float yCut, ImU32 color) {
    if (yCut >= r.Max.y) {
        return;
    }
    if (yCut <= r.Min.y) {
        dl->AddRectFilled(r.Min, r.Max, color, radius);
        return;
    }
    static std::vector<ContourPoint> contour;
    roundedContour(r, radius, 0, 10, contour);
    static std::vector<ImVec2> polygon;
    polygon.clear();
    const int n = static_cast<int>(contour.size());
    auto position = [&](int i) {
        const ContourPoint& p = contour[static_cast<size_t>(i % n)];
        return ImVec2(p.center.x + p.normal.x * p.radius, p.center.y + p.normal.y * p.radius);
    };
    for (int i = 0; i < n; ++i) {
        const ImVec2 a = position(i);
        const ImVec2 b = position(i + 1);
        const bool aIn = a.y >= yCut;
        const bool bIn = b.y >= yCut;
        if (aIn) {
            polygon.push_back(a);
        }
        if (aIn != bIn) {
            const float t = (yCut - a.y) / (b.y - a.y);
            polygon.push_back(ImVec2(a.x + (b.x - a.x) * t, yCut));
        }
    }
    if (polygon.size() >= 3) {
        dl->AddConvexPolyFilled(polygon.data(), static_cast<int>(polygon.size()), color);
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Escala
// -----------------------------------------------------------------------------

void setScale(float unitsPerPoint, float pixelsPerUnit) {
    g_scale = unitsPerPoint > 0.0f ? unitsPerPoint : 1.0f;
    g_pixels = pixelsPerUnit > 0.0f ? pixelsPerUnit : 1.0f;
}

float scale() { return g_scale; }
float hairline() { return 1.0f / g_pixels; }
float snap(float units) { return std::round(units * g_pixels) / g_pixels; }

// -----------------------------------------------------------------------------
// Texto e iconos
// -----------------------------------------------------------------------------

bool loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    g_fonts[0] = addFont("fonts/Inter-Regular.ttf", "Inter Regular");
    g_fonts[1] = addFont("fonts/Inter-SemiBold.ttf", "Inter SemiBold");
    g_fonts[2] = addFont("fonts/Inter-Bold.ttf", "Inter Bold");
    g_icons = addFont("fonts/LucideIcons.ttf", "Lucide");
    if (!g_fonts[0]) {
        g_fonts[0] = io.Fonts->AddFontDefault();
    }
    for (ImFont*& f : g_fonts) {
        if (!f) {
            f = g_fonts[0];
        }
    }
    io.FontDefault = g_fonts[0];
    return g_fonts[0] != nullptr && g_icons != nullptr;
}

ImFont* font(Weight weight) { return g_fonts[static_cast<int>(weight)]; }

float fontSize(float points) { return points * kInterLineHeight * g_scale; }

ImVec2 measure(Weight weight, float points, const char* text, const char* end, float wrapWidth) {
    return font(weight)->CalcTextSizeA(fontSize(points), FLT_MAX, wrapWidth, text, end);
}

void text(ImDrawList* dl, Weight weight, float points, ImVec2 pos, ImU32 color, const char* text, const char* end,
          float wrapWidth) {
    dl->AddText(font(weight), fontSize(points), pos, color, text, end, wrapWidth);
}

std::string fitText(Weight weight, float points, const char* text, float maxWidth) {
    ImFont* f = font(weight);
    const float size = fontSize(points);
    const char* end = text + std::strlen(text);
    if (f->CalcTextSizeA(size, FLT_MAX, 0.0f, text, end).x <= maxWidth) {
        return std::string(text, end);
    }
    const char* ellipsis = "\xe2\x80\xa6";
    const float ellipsisWidth = f->CalcTextSizeA(size, FLT_MAX, 0.0f, ellipsis).x;
    const char* cut = text;
    for (const char* p = text; p < end;) {
        unsigned int c = 0;
        const int length = ImTextCharFromUtf8(&c, p, end);
        const char* next = p + std::max(length, 1);
        if (f->CalcTextSizeA(size, FLT_MAX, 0.0f, text, next).x + ellipsisWidth > maxWidth) {
            break;
        }
        cut = next;
        p = next;
    }
    std::string shown(text, cut);
    while (!shown.empty() && shown.back() == ' ') {
        shown.pop_back();
    }
    return shown + ellipsis;
}

void textFit(ImDrawList* dl, Weight weight, float points, ImVec2 pos, float maxWidth, ImU32 color, const char* text) {
    const std::string shown = fitText(weight, points, text, maxWidth);
    dl->AddText(font(weight), fontSize(points), pos, color, shown.c_str());
}

void textCentered(ImDrawList* dl, Weight weight, float points, ImVec2 center, ImU32 color, const char* text) {
    const ImVec2 size = measure(weight, points, text);
    dl->AddText(font(weight), fontSize(points), ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), color,
                text);
}

void icon(ImDrawList* dl, const char* glyph, ImVec2 center, float points, ImU32 color) {
    const float size = pt(points);
    if (!g_icons) {
        dl->AddCircle(center, size * 0.3f, color, 0, pt(1.8f));
        return;
    }
    dl->AddText(g_icons, size, ImVec2(center.x - size * 0.5f, center.y - size * 0.5f), color, glyph);
}

float paragraph(ImDrawList* dl, Weight weight, float points, ImVec2 pos, float width, Align align, ImU32 color,
                const char* text, float lineHeight) {
    ImFont* f = font(weight);
    const float size = fontSize(points);
    const float step = pt(points * lineHeight);
    const char* end = text + std::strlen(text);
    float y = pos.y;
    int lines = 0;
    for (const char* line = text; line < end;) {
        const char* newline = static_cast<const char*>(std::memchr(line, '\n', static_cast<size_t>(end - line)));
        const char* limit = newline ? newline : end;
        const char* cut = f->CalcWordWrapPosition(size, line, limit, width);
        if (cut == line) {
            cut = line + std::max(ImTextCharFromUtf8(nullptr, line, limit), 1);   // palabra más ancha que la línea
        }
        const char* drawEnd = cut;
        while (drawEnd > line && (drawEnd[-1] == ' ' || drawEnd[-1] == '\n')) {
            --drawEnd;
        }
        if (dl) {
            float x = pos.x;
            if (align != Align::Left) {
                const float lineWidth = f->CalcTextSizeA(size, FLT_MAX, 0.0f, line, drawEnd).x;
                x += align == Align::Center ? (width - lineWidth) * 0.5f : width - lineWidth;
            }
            // La caja de la línea (fontSize) va centrada en su paso.
            dl->AddText(f, size, ImVec2(x, y + (step - size) * 0.5f), color, line, drawEnd);
        }
        y += step;
        ++lines;
        line = cut;
        while (line < limit && *line == ' ') {
            ++line;
        }
        if (line == limit && newline) {
            ++line;   // salta el salto de línea
        }
    }
    return static_cast<float>(lines) * step;
}

void label(ImDrawList* dl, Weight weight, float points, ImVec2 anchor, Align align, ImU32 color, const char* text,
           float maxWidth) {
    ImFont* f = font(weight);
    const float size = fontSize(points);
    std::string shortened;
    const char* shown = text;
    if (maxWidth > 0.0f && f->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x > maxWidth) {
        shortened = fitText(weight, points, text, maxWidth);
        shown = shortened.c_str();
    }
    float x = anchor.x;
    if (align != Align::Left) {
        const float width = f->CalcTextSizeA(size, FLT_MAX, 0.0f, shown).x;
        x -= align == Align::Center ? width * 0.5f : width;
    }
    dl->AddText(f, size, ImVec2(x, anchor.y - size * 0.5f), color, shown);
}

float trackedWidth(Weight weight, float points, const char* text, float tracking) {
    ImFont* f = font(weight);
    const float size = fontSize(points);
    const float extra = pt(points * tracking);
    float width = 0.0f;
    int count = 0;
    const char* end = text + std::strlen(text);
    for (const char* p = text; p < end;) {
        unsigned int c = 0;
        const char* next = p + std::max(ImTextCharFromUtf8(&c, p, end), 1);
        width += f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, next).x;
        ++count;
        p = next;
    }
    return width + extra * static_cast<float>(std::max(count - 1, 0));
}

void tracked(ImDrawList* dl, Weight weight, float points, ImVec2 anchor, ImU32 color, const char* text,
             float tracking) {
    ImFont* f = font(weight);
    const float size = fontSize(points);
    const float extra = pt(points * tracking);
    float x = anchor.x;
    const float y = anchor.y - size * 0.5f;
    const char* end = text + std::strlen(text);
    for (const char* p = text; p < end;) {
        unsigned int c = 0;
        const char* next = p + std::max(ImTextCharFromUtf8(&c, p, end), 1);
        dl->AddText(f, size, ImVec2(x, y), color, p, next);
        x += f->CalcTextSizeA(size, FLT_MAX, 0.0f, p, next).x + extra;
        p = next;
    }
}

// -----------------------------------------------------------------------------
// Color
// -----------------------------------------------------------------------------

ImU32 withAlpha(ImU32 color, float factor) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(factor, 0.0f, 1.0f);
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    ImU32 out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFF);
        const float cb = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<ImU32>(lerp(ca, cb, t) + 0.5f) << shift;
    }
    return out;
}

ImU32 fromFloat(const float rgb[3], float alpha) {
    auto channel = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(channel(rgb[0]), channel(rgb[1]), channel(rgb[2]), channel(alpha));
}

float luminance(const float rgb[3]) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }

// -----------------------------------------------------------------------------
// Materiales
// -----------------------------------------------------------------------------

void setBackdrop(ImTextureID texture, ImVec2 displaySize) {
    g_backdrop = texture;
    g_display = ImVec2(std::max(displaySize.x, 1.0f), std::max(displaySize.y, 1.0f));
}

void shadow(ImDrawList* dl, const ImRect& rect, float radius, float blur, float offsetY, float alpha,
            ImDrawFlags corners, ImU32 color) {
    if (alpha <= 0.0f || blur <= 0.0f) {
        return;
    }
    static std::vector<ContourPoint> contour;
    roundedContour(rect, radius, corners, 8, contour);
    // Tres contornos: fuerte junto al borde y una cola larga, como una gaussiana.
    const ImU32 black = color | IM_COL32_A_MASK;
    const Band bands[4] = {
        {0.0f, 0.0f, withAlpha(black, alpha)},
        {blur * 0.18f, offsetY * 0.25f, withAlpha(black, alpha * 0.55f)},
        {blur * 0.5f, offsetY * 0.6f, withAlpha(black, alpha * 0.18f)},
        {blur, offsetY, withAlpha(black, 0.0f)},
    };
    drawBands(dl, contour, bands, 4);
}

void glass(ImDrawList* dl, const ImRect& rect, float radius, ImU32 tint, ImDrawFlags corners, float dim) {
    if (g_backdrop != ImTextureID_Invalid) {
        const ImVec2 uv0(rect.Min.x / g_display.x, 1.0f - rect.Min.y / g_display.y);
        const ImVec2 uv1(rect.Max.x / g_display.x, 1.0f - rect.Max.y / g_display.y);
        dl->AddImageRounded(ImTextureRef(g_backdrop), rect.Min, rect.Max, uv0, uv1, IM_COL32_WHITE, radius, corners);
        if (dim > 0.0f) {
            dl->AddRectFilled(rect.Min, rect.Max, withAlpha(IM_COL32_BLACK, dim), radius, corners);
        }
        dl->AddRectFilled(rect.Min, rect.Max, tint, radius, corners);
    } else {
        // Sin desenfoque (la GPU no pudo), el mismo tinte algo más opaco.
        const float a = static_cast<float>((tint >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        dl->AddRectFilled(rect.Min, rect.Max, withAlpha(tint | IM_COL32_A_MASK, std::min(1.0f, a + 0.25f)), radius,
                          corners);
    }
    // Borde de un píxel y brillo en el canto de arriba (más fuerte donde mira hacia arriba).
    const float h = hairline();
    static std::vector<ContourPoint> contour;
    roundedContour(rect, radius, corners, 10, contour);
    const Band border[2] = {{0.0f, 0.0f, theme::kGlassBorder}, {-h, 0.0f, theme::kGlassBorder}};
    drawBands(dl, contour, border, 2);
    const Band highlight[2] = {{-h, 0.0f, theme::kGlassHighlight}, {-2.5f * h, 0.0f, withAlpha(theme::kGlassHighlight, 0.0f)}};
    drawBands(dl, contour, highlight, 2, [](const ContourPoint& p) { return std::max(0.0f, -p.normal.y); });
}

void pushUnclipped(ImDrawList* dl) {
    dl->PushClipRect(ImVec2(-g_display.x, -g_display.y), ImVec2(g_display.x * 2.0f, g_display.y * 2.0f), false);
}

void popUnclipped(ImDrawList* dl) { dl->PopClipRect(); }

void group(ImDrawList* dl, const ImRect& rect, float radius) {
    dl->AddRectFilled(rect.Min, rect.Max, theme::kGroupFill, radius);
}

void separator(ImDrawList* dl, float x0, float x1, float y, ImU32 color) {
    const float top = snap(y);
    dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + hairline()), color);
}

void iconTile(ImDrawList* dl, const ImRect& rect, ImU32 color, const char* glyph, float iconPoints) {
    dl->AddRectFilled(rect.Min, rect.Max, color, rect.GetWidth() * 0.25f);
    icon(dl, glyph, rect.GetCenter(), iconPoints, IM_COL32_WHITE);
}

void spinner(ImDrawList* dl, ImVec2 center, float radius, ImU32 color) {
    // Como el de iOS: el radio más claro avanza una posición cada 1/12 s.
    constexpr int kSpokes = 8;
    const int head = static_cast<int>(anim::time() * 12.0) % kSpokes;
    const float thickness = radius * 0.24f;
    for (int i = 0; i < kSpokes; ++i) {
        const float angle = static_cast<float>(i) / kSpokes * 2.0f * IM_PI - IM_PI * 0.5f;
        const ImVec2 direction(std::cos(angle), std::sin(angle));
        const ImVec2 a(center.x + direction.x * radius * 0.5f, center.y + direction.y * radius * 0.5f);
        const ImVec2 b(center.x + direction.x * (radius - thickness * 0.5f),
                       center.y + direction.y * (radius - thickness * 0.5f));
        const int behind = (head - i + kSpokes) % kSpokes;
        const ImU32 spoke = withAlpha(color, 1.0f - 0.8f * static_cast<float>(behind) / kSpokes);
        dl->AddLine(a, b, spoke, thickness);
        dl->AddCircleFilled(a, thickness * 0.5f, spoke, 8);
        dl->AddCircleFilled(b, thickness * 0.5f, spoke, 8);
    }
    anim::keepAlive();
}

void linearGradient(ImDrawList* dl, const ImRect& rect, float radius, ImVec2 from, ImVec2 to, ImU32 colorFrom,
                    ImU32 colorTo) {
    static std::vector<ContourPoint> contour;
    roundedContour(rect, radius, 0, 8, contour);
    const int n = static_cast<int>(contour.size());
    const ImVec2 axis(to.x - from.x, to.y - from.y);
    const float length2 = std::max(axis.x * axis.x + axis.y * axis.y, 1e-6f);
    auto colorAt = [&](ImVec2 p) {
        const float t = ((p.x - from.x) * axis.x + (p.y - from.y) * axis.y) / length2;
        return mix(colorFrom, colorTo, t);
    };
    // Abanico desde el centro (el color es lineal: interpolarlo por triángulos es exacto)
    // y un borde de un píxel que se desvanece hacia fuera.
    const float aa = hairline();
    dl->PrimReserve(n * 3 + n * 6, 1 + n * 2);
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const unsigned base = dl->_VtxCurrentIdx;
    const ImVec2 middle = rect.GetCenter();
    dl->PrimWriteVtx(middle, uv, colorAt(middle));
    for (const ContourPoint& p : contour) {
        const ImVec2 pos(p.center.x + p.normal.x * p.radius, p.center.y + p.normal.y * p.radius);
        dl->PrimWriteVtx(pos, uv, colorAt(pos));
    }
    for (const ContourPoint& p : contour) {
        const ImVec2 pos(p.center.x + p.normal.x * (p.radius + aa), p.center.y + p.normal.y * (p.radius + aa));
        dl->PrimWriteVtx(pos, uv, withAlpha(colorAt(pos), 0.0f));
    }
    for (int i = 0; i < n; ++i) {
        const unsigned j = static_cast<unsigned>((i + 1) % n);
        const unsigned a = base + 1 + static_cast<unsigned>(i);
        const unsigned b = base + 1 + j;
        const unsigned oa = base + 1 + static_cast<unsigned>(n + i);
        const unsigned ob = base + 1 + static_cast<unsigned>(n) + j;
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(a));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(b));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(a));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(oa));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(ob));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(a));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(ob));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(b));
    }
}

void hueRing(ImDrawList* dl, ImVec2 center, float innerRadius, float outerRadius) {
    // Cuatro anillos de vértices: borde suave, interior, exterior y borde suave.
    constexpr int kSegments = 120;
    const float aa = hairline();
    const float radii[4] = {innerRadius - aa, innerRadius, outerRadius, outerRadius + aa};
    const float alphas[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    dl->PrimReserve(kSegments * 3 * 6, (kSegments + 1) * 4);
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const unsigned base = dl->_VtxCurrentIdx;
    for (int ring = 0; ring < 4; ++ring) {
        for (int i = 0; i <= kSegments; ++i) {
            const float t = static_cast<float>(i) / kSegments;
            const float angle = t * 2.0f * IM_PI;
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            ImGui::ColorConvertHSVtoRGB(t, 1.0f, 1.0f, r, g, b);
            const float rgb[3] = {r, g, b};
            const ImVec2 pos(center.x + std::sin(angle) * radii[ring], center.y - std::cos(angle) * radii[ring]);
            dl->PrimWriteVtx(pos, uv, fromFloat(rgb, alphas[ring]));
        }
    }
    const unsigned stride = kSegments + 1;
    for (int ring = 0; ring < 3; ++ring) {
        for (int i = 0; i < kSegments; ++i) {
            const unsigned a0 = base + static_cast<unsigned>(ring) * stride + static_cast<unsigned>(i);
            const unsigned a1 = a0 + 1;
            const unsigned b0 = a0 + stride;
            const unsigned b1 = b0 + 1;
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a0));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a0));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(b0));
        }
    }
}

// -----------------------------------------------------------------------------
// Superficies
// -----------------------------------------------------------------------------

void beginSurface(const char* name, const ImRect& rect, bool interactive, bool front) {
    // ImGui redondea la posición y el tamaño de las ventanas a unidades enteras y recorta
    // lo que se dibuja a ellas: con un margen de una unidad nada queda cortado.
    const ImVec2 min(std::floor(rect.Min.x) - 1.0f, std::floor(rect.Min.y) - 1.0f);
    const ImVec2 max(std::ceil(rect.Max.x) + 1.0f, std::ceil(rect.Max.y) + 1.0f);
    ImGui::SetNextWindowPos(min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(max.x - min.x, 1.0f), std::max(max.y - min.y, 1.0f)), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!interactive) {
        flags |= ImGuiWindowFlags_NoInputs;
    }
    ImGui::Begin(name, nullptr, flags);
    if (front) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    }
}

void endSurface() { ImGui::End(); }

DrawMark mark(ImDrawList* dl) { return {dl, dl->VtxBuffer.Size, std::max(dl->CmdBuffer.Size - 1, 0)}; }

void transform(const DrawMark& m, ImVec2 anchor, float scale, ImVec2 offset, float alpha) {
    ImDrawList* dl = m.dl;
    if (!dl || (scale == 1.0f && offset.x == 0.0f && offset.y == 0.0f && alpha >= 1.0f)) {
        return;
    }
    auto move = [&](ImVec2 p) {
        return ImVec2(anchor.x + (p.x - anchor.x) * scale + offset.x, anchor.y + (p.y - anchor.y) * scale + offset.y);
    };
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    for (int i = m.vertex; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos = move(v.pos);
        if (a < 1.0f) {
            v.col = withAlpha(v.col, a);
        }
    }
    for (int c = m.command; c < dl->CmdBuffer.Size; ++c) {
        ImVec4& clip = dl->CmdBuffer[c].ClipRect;
        const ImVec2 min = move(ImVec2(clip.x, clip.y));
        const ImVec2 max = move(ImVec2(clip.z, clip.w));
        clip = ImVec4(min.x, min.y, max.x, max.y);
    }
    // El cristal muestra lo que hay detrás en su posición final.
    if (g_backdrop == ImTextureID_Invalid) {
        return;
    }
    for (int c = m.command; c < dl->CmdBuffer.Size; ++c) {
        const ImDrawCmd& cmd = dl->CmdBuffer[c];
        if (cmd.TexRef._TexData != nullptr || cmd.TexRef._TexID != g_backdrop) {
            continue;
        }
        for (unsigned k = 0; k < cmd.ElemCount; ++k) {
            const int index = static_cast<int>(cmd.VtxOffset + dl->IdxBuffer[static_cast<int>(cmd.IdxOffset + k)]);
            if (index < m.vertex || index >= dl->VtxBuffer.Size) {
                continue;
            }
            ImDrawVert& v = dl->VtxBuffer[index];
            v.uv = ImVec2(v.pos.x / g_display.x, 1.0f - v.pos.y / g_display.y);
        }
    }
}

// -----------------------------------------------------------------------------
// Controles
// -----------------------------------------------------------------------------

Press pressable(ImGuiID id, const ImRect& bb, bool enabled) {
    Press press;
    if (!ImGui::ItemAdd(bb, id, nullptr, enabled ? ImGuiItemFlags_None : ImGuiItemFlags_Disabled)) {
        return press;
    }
    bool hovered = false;
    bool held = false;
    const bool clicked = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_NoNavFocus);
    if (!enabled) {
        return press;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float slop = pt(kTapSlop);
    const bool dragged = io.MouseDragMaxDistanceSqr[0] > slop * slop;
    press.clicked = clicked && !dragged;
    press.held = held && !dragged;
    press.hovered = hovered && io.MouseSource != ImGuiMouseSource_TouchScreen;
    return press;
}

Press pressable(const char* id, const ImRect& bb, bool enabled) { return pressable(ImGui::GetID(id), bb, enabled); }

void highlight(ImDrawList* dl, ImGuiID id, const ImRect& rect, float radius, bool on, const Press& press,
               ImU32 onColor) {
    const float target = (on || press.held) ? 1.0f : (press.hovered ? 0.55f : 0.0f);
    const float t = anim::follow(id + 0x51u, target, 24.0f);
    if (t > 0.002f) {
        dl->AddRectFilled(rect.Min, rect.Max, withAlpha(on ? onColor : theme::kPressed, t), radius);
    }
}

bool toggle(const char* strId, ImVec2 pos, bool* value, bool enabled) {
    const ImGuiID id = ImGui::GetID(strId);
    const ImRect bb(pos, ImVec2(pos.x + toggleSize().x, pos.y + toggleSize().y));
    const Press press = pressable(id, bb, enabled);
    if (press.clicked) {
        *value = !*value;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = anim::follow(id, *value ? 1.0f : 0.0f, 16.0f);
    const float squeeze = anim::follow(id + 1u, press.held ? 1.0f : 0.0f, 20.0f);
    const float radius = bb.GetHeight() * 0.5f;
    ImU32 track = mix(IM_COL32(120, 120, 128, 92), theme::kGreen, t);
    if (!enabled) {
        track = withAlpha(track, 0.45f);
    }
    dl->AddRectFilled(bb.Min, bb.Max, track, radius);

    // La bolita se estira un poco mientras se mantiene pulsada, como en iOS.
    const float inset = pt(2.0f);
    const float knobRadius = radius - inset;
    const float stretch = pt(7.0f) * squeeze;
    const float left = lerp(bb.Min.x + inset, bb.Max.x - inset - knobRadius * 2.0f - stretch, t);
    const ImRect knob(ImVec2(left, bb.Min.y + inset), ImVec2(left + knobRadius * 2.0f + stretch, bb.Max.y - inset));
    shadow(dl, knob, knobRadius, pt(6.0f), pt(2.0f), enabled ? 0.28f : 0.12f);
    dl->AddRectFilled(knob.Min, knob.Max, enabled ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 170), knobRadius);
    return press.clicked;
}

bool segmented(const char* strId, const ImRect& rect, const char* const* labels, int count, int* selected,
               bool enabled) {
    if (count <= 0) {
        return false;
    }
    const float alpha = enabled ? 1.0f : 0.4f;
    ImGui::PushID(strId);
    const ImGuiID id = ImGui::GetID("##track");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(rect.Min, rect.Max, theme::kFillSoft, pt(9.0f));

    const float pad = pt(2.0f);
    const float width = (rect.GetWidth() - 2.0f * pad) / static_cast<float>(count);
    const float x = anim::follow(id, static_cast<float>(*selected), 20.0f, 0.001f);
    const ImRect pill(ImVec2(rect.Min.x + pad + x * width, rect.Min.y + pad),
                      ImVec2(rect.Min.x + pad + (x + 1.0f) * width, rect.Max.y - pad));
    shadow(dl, pill, pt(7.0f), pt(8.0f), pt(3.0f), 0.22f * alpha);
    dl->AddRectFilled(pill.Min, pill.Max, withAlpha(theme::kGray2, alpha), pt(7.0f));

    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const ImRect segment(ImVec2(rect.Min.x + pad + static_cast<float>(i) * width, rect.Min.y),
                             ImVec2(rect.Min.x + pad + static_cast<float>(i + 1) * width, rect.Max.y));
        ImGui::PushID(i);
        const Press press = pressable("##segment", segment, enabled);
        ImGui::PopID();
        const bool on = i == *selected;
        if (!on && press.held) {
            dl->AddRectFilled(ImVec2(segment.Min.x, segment.Min.y + pad), ImVec2(segment.Max.x, segment.Max.y - pad),
                              theme::kHover, pt(7.0f));
        }
        textCentered(dl, on ? Weight::SemiBold : Weight::Regular, theme::kFootnote, segment.GetCenter(),
                     withAlpha(theme::kLabel, alpha), labels[i]);
        if (press.clicked && !on) {
            *selected = i;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

bool slider(const char* strId, const ImRect& rect, float* value, float min, float max, bool* active) {
    const ImGuiID id = ImGui::GetID(strId);
    ImGui::ItemAdd(rect, id);
    bool hovered = false;
    bool held = false;
    ImGui::ButtonBehavior(rect, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus);

    const float knobRadius = pt(14.0f);
    const float x0 = rect.Min.x + knobRadius;
    const float x1 = rect.Max.x - knobRadius;
    bool changed = false;
    if (held && x1 > x0) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        const float v = min + (max - min) * t;
        changed = v != *value;
        *value = v;
    }
    if (active) {
        *active = held;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = max > min ? std::clamp((*value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    const float cy = rect.GetCenter().y;
    const float track = pt(2.0f);
    const float knobX = x0 + (x1 - x0) * t;
    dl->AddRectFilled(ImVec2(rect.Min.x, cy - track), ImVec2(rect.Max.x, cy + track), IM_COL32(120, 120, 128, 102),
                      track);
    dl->AddRectFilled(ImVec2(rect.Min.x, cy - track), ImVec2(knobX, cy + track), theme::kAccent, track);
    const float grow = anim::follow(id, held ? 1.0f : 0.0f, 20.0f);
    const float r = knobRadius * (1.0f + 0.06f * grow);
    const ImRect knob(ImVec2(knobX - r, cy - r), ImVec2(knobX + r, cy + r));
    shadow(dl, knob, r, pt(8.0f), pt(2.0f), 0.3f);
    dl->AddCircleFilled(ImVec2(knobX, cy), r, IM_COL32_WHITE, 0);
    return changed;
}

bool fillSlider(const char* strId, const ImRect& rect, float* t, bool* active) {
    const ImGuiID id = ImGui::GetID(strId);
    ImGui::ItemAdd(rect, id);
    bool hovered = false;
    bool held = false;
    ImGui::ButtonBehavior(rect, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus);
    bool changed = false;
    const ImGuiIO& io = ImGui::GetIO();
    if (held && io.MouseDelta.y != 0.0f && rect.GetHeight() > 0.0f) {
        const float v = std::clamp(*t - io.MouseDelta.y / rect.GetHeight(), 0.0f, 1.0f);
        changed = v != *t;
        *t = v;
    }
    if (active) {
        *active = held;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float radius = rect.GetWidth() * 0.5f;
    const float focus = anim::follow(id, held ? 1.0f : 0.0f, 20.0f);
    dl->AddRectFilled(rect.Min, rect.Max, mix(theme::kFill, IM_COL32(120, 120, 128, 110), focus), radius);
    const float shown = anim::follow(id + 1u, *t, 30.0f, 0.0005f);
    const float yCut = rect.Max.y - rect.GetHeight() * shown;
    fillRoundedBelow(dl, rect, radius, yCut, IM_COL32(255, 255, 255, 235));
    return changed;
}

bool textField(const char* id, const ImRect& rect, char* buffer, size_t size, bool focus, const char* placeholder) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(0, 0, 0, 90), pt(9.0f));
    dl->AddRect(rect.Min, rect.Max, IM_COL32(255, 255, 255, 30), pt(9.0f), 0, hairline());

    const float size16 = fontSize(theme::kBody);
    ImGui::PushFont(font(Weight::Regular), size16);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pt(10.0f), (rect.GetHeight() - size16) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, pt(9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLabel);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, withAlpha(theme::kAccent, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, theme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    ImGui::SetCursorScreenPos(rect.Min);
    ImGui::SetNextItemWidth(rect.GetWidth());
    if (focus) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool enter = ImGui::InputText(id, buffer, size, ImGuiInputTextFlags_EnterReturnsTrue |
                                                             ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    if (buffer[0] == '\0' && placeholder) {
        text(dl, Weight::Regular, theme::kBody, ImVec2(rect.Min.x + pt(10.0f), rect.GetCenter().y - size16 * 0.5f),
             theme::kTertiaryLabel, placeholder);
    }
    return enter;
}

float beginScroll(const char* strId, const ImRect& view, float contentHeight) {
    const ImGuiID id = ImGui::GetID(strId);
    ScrollState& s = g_scrolls[id];
    const ImGuiIO& io = ImGui::GetIO();
    const float maxOffset = std::max(0.0f, contentHeight - view.GetHeight());
    const bool hovered = ImGui::IsWindowHovered() && view.Contains(io.MousePos);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered) {
        s.tracking = true;
        s.dragging = false;
        s.velocity = 0.0f;
    }
    if (s.tracking) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!s.dragging && maxOffset > 0.0f &&
                std::fabs(io.MousePos.y - io.MouseClickedPos[0].y) > pt(kTapSlop)) {
                s.dragging = true;
            }
            if (s.dragging && io.MouseDelta.y != 0.0f) {
                s.offset -= io.MouseDelta.y;
                const float dt = std::max(io.DeltaTime, 1.0f / 240.0f);
                s.velocity = lerp(s.velocity, -io.MouseDelta.y / dt, 0.5f);
                s.lastMove = ImGui::GetTime();
            }
        } else {
            if (!s.dragging || ImGui::GetTime() - s.lastMove > 0.08) {
                s.velocity = 0.0f;
            }
            s.tracking = false;
            s.dragging = false;
        }
    }
    if (hovered && io.MouseWheel != 0.0f) {
        s.velocity = 0.0f;
        s.offset -= io.MouseWheel * pt(48.0f);
    }
    if (!s.tracking && s.velocity != 0.0f) {
        const float dt = anim::dt();
        s.offset += s.velocity * dt;
        s.velocity *= std::exp(-5.0f * dt);
        if (std::fabs(s.velocity) < pt(8.0f)) {
            s.velocity = 0.0f;
        }
        anim::follow(id, s.offset, 1.0f);   // mantiene el bucle despierto mientras frena
    }
    if (s.offset < 0.0f || s.offset > maxOffset) {
        s.offset = std::clamp(s.offset, 0.0f, maxOffset);
        s.velocity = 0.0f;
    }
    g_scrollStack.push_back({id, view, contentHeight});
    ImGui::PushClipRect(view.Min, view.Max, true);
    return s.offset;
}

void endScroll() {
    ImGui::PopClipRect();
    if (g_scrollStack.empty()) {
        return;
    }
    const ScrollFrame frame = g_scrollStack.back();
    g_scrollStack.pop_back();
    const ScrollState& s = g_scrolls[frame.id];
    const float viewHeight = frame.view.GetHeight();
    if (frame.contentHeight <= viewHeight + 0.5f) {
        return;
    }
    // Indicador de posición mientras se mueve.
    const float visible = anim::follow(frame.id + 7u, (s.dragging || s.velocity != 0.0f) ? 1.0f : 0.0f, 10.0f);
    if (visible <= 0.01f) {
        return;
    }
    const float barHeight = std::max(pt(24.0f), viewHeight * viewHeight / frame.contentHeight);
    const float maxOffset = frame.contentHeight - viewHeight;
    const float y = frame.view.Min.y + (viewHeight - barHeight) * (s.offset / maxOffset);
    const float x = frame.view.Max.x - pt(4.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x - pt(3.0f), y), ImVec2(x, y + barHeight),
                                              withAlpha(IM_COL32(255, 255, 255, 110), visible), pt(1.5f));
}

void scrollIntoView(const char* strId, float top, float bottom, float viewHeight) {
    ScrollState& s = g_scrolls[ImGui::GetID(strId)];
    if (top < s.offset) {
        s.offset = top;
    } else if (bottom > s.offset + viewHeight) {
        s.offset = bottom - viewHeight;
    }
}

bool swatch(const char* strId, ImVec2 center, float radius, ImU32 color, bool selected) {
    const ImRect bb(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius));
    const Press press = pressable(strId, bb);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = press.held ? radius * 0.92f : radius;
    dl->AddCircleFilled(center, r, color, 0);
    dl->AddCircle(center, r - hairline() * 0.5f, IM_COL32(255, 255, 255, 41), 0, hairline());
    if (selected) {
        dl->AddCircle(center, radius + pt(3.0f), IM_COL32_WHITE, 0, pt(2.0f));
    }
    return press.clicked;
}

} // namespace ui
