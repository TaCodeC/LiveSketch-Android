#include "UI/ColorManage.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace ui::gamut {

namespace {

// Vértices de una lista dibujados en otro perfil que el de la interfaz (sRGB).
struct Span {
    ImDrawList* list;
    int begin;
    int end;
    ColorProfile profile;
};

ColorProfile g_display = ColorProfile::Srgb;
ColorProfile g_canvas = ColorProfile::Srgb;
std::vector<Span> g_spans;

} // namespace

void beginFrame(ColorProfile display, ColorProfile canvas) {
    g_display = display;
    g_canvas = canvas;
    g_spans.clear();
}

ColorProfile display() {
    return g_display;
}

ColorProfile canvas() {
    return g_canvas;
}

Scope::Scope(ImDrawList* dl) : Scope(dl, g_canvas) {}

Scope::Scope(ImDrawList* dl, ColorProfile profile)
    : m_dl(dl), m_begin(dl ? dl->VtxBuffer.Size : 0), m_profile(profile) {}

Scope::~Scope() {
    // En sRGB es lo mismo que el resto de la interfaz.
    if (m_dl && m_profile != ColorProfile::Srgb && m_dl->VtxBuffer.Size > m_begin) {
        g_spans.push_back({m_dl, m_begin, m_dl->VtxBuffer.Size, m_profile});
    }
}

void convertVertices(ImDrawList* dl, int begin, int end, ColorProfile from, ColorProfile to) {
    begin = std::max(begin, 0);
    end = std::min(end, dl ? dl->VtxBuffer.Size : 0);
    if (from == to || begin >= end) {
        return;
    }
    const colorspace::Converter8& converter = colorspace::converter8(from, to);
    constexpr ImU32 kAlpha = IM_COL32_A_MASK;
    // Los vértices seguidos suelen ser del mismo color.
    ImU32 lastIn = 0;
    ImU32 lastOut = 0;
    bool cached = false;
    for (int i = begin; i < end; ++i) {
        ImU32& col = dl->VtxBuffer.Data[i].col;
        const ImU32 rgb = col & ~kAlpha;
        if (!cached || rgb != lastIn) {
            uint8_t r = static_cast<uint8_t>((rgb >> IM_COL32_R_SHIFT) & 0xFF);
            uint8_t g = static_cast<uint8_t>((rgb >> IM_COL32_G_SHIFT) & 0xFF);
            uint8_t b = static_cast<uint8_t>((rgb >> IM_COL32_B_SHIFT) & 0xFF);
            converter.apply(r, g, b);
            lastIn = rgb;
            lastOut = (static_cast<ImU32>(r) << IM_COL32_R_SHIFT) | (static_cast<ImU32>(g) << IM_COL32_G_SHIFT) |
                      (static_cast<ImU32>(b) << IM_COL32_B_SHIFT);
            cached = true;
        }
        col = (col & kAlpha) | lastOut;
    }
}

void convert(ImDrawData* data) {
    if (!data) {
        g_spans.clear();
        return;
    }
    const bool spansChange = std::any_of(g_spans.begin(), g_spans.end(),
                                         [](const Span& span) { return span.profile != g_display; });
    if (g_display == ColorProfile::Srgb && !spansChange) {
        g_spans.clear();
        return;
    }
    std::sort(g_spans.begin(), g_spans.end(), [](const Span& a, const Span& b) {
        if (a.list != b.list) {
            return std::less<ImDrawList*>()(a.list, b.list);
        }
        return a.begin < b.begin;
    });
    for (ImDrawList* list : data->CmdLists) {
        auto span = std::lower_bound(g_spans.begin(), g_spans.end(), list, [](const Span& s, ImDrawList* l) {
            return std::less<ImDrawList*>()(s.list, l);
        });
        int cursor = 0;
        for (; span != g_spans.end() && span->list == list; ++span) {
            const int begin = std::max(span->begin, cursor);
            const int end = std::min(span->end, list->VtxBuffer.Size);
            if (begin >= end) {
                continue;
            }
            convertVertices(list, cursor, begin, ColorProfile::Srgb, g_display);
            convertVertices(list, begin, end, span->profile, g_display);
            cursor = end;
        }
        convertVertices(list, cursor, list->VtxBuffer.Size, ColorProfile::Srgb, g_display);
    }
    g_spans.clear();
}

} // namespace ui::gamut
