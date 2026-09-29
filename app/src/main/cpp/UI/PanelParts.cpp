#include "UI/PanelParts.h"

#include "UI/Anim.h"
#include "UI/Icons.h"

#include <algorithm>

namespace th = ui::theme;

namespace ui::parts {

void pressFeedback(ImDrawList* dl, ImGuiID id, const ImRect& rect, const Press& press, float radius) {
    const float t = anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
    if (t > 0.002f) {
        dl->AddRectFilled(rect.Min, rect.Max, withAlpha(th::kPressed, t), radius);
    }
}

float panelHeader(ImDrawList* dl, const ImRect& content, float top, const char* title) {
    const float cy = top + pt(th::kHeaderHeight * 0.5f);
    label(dl, Weight::SemiBold, th::kPanelTitle, ImVec2(content.Min.x + pt(16.0f), cy), Align::Left, th::kLabel, title);
    separator(dl, content.Min.x, content.Max.x, top + pt(th::kHeaderHeight) - hairline(), th::kRule);
    return cy;
}

float backHeader(ImDrawList* dl, const ImRect& content, float top, const char* id, const char* title, bool* back,
                 float* titleRight) {
    const float cy = top + pt(th::kHeaderHeight * 0.5f);
    const ImRect button(ImVec2(content.Min.x + pt(8.0f), cy - pt(16.0f)), ImVec2(content.Min.x + pt(8.0f + 32.0f), cy + pt(16.0f)));
    const ImGuiID gid = ImGui::GetID(id);
    const Press press = pressable(gid, button);
    pressFeedback(dl, gid, button, press, pt(9.0f));
    icon(dl, icon::kChevronLeft, button.GetCenter(), 20.0f, th::kLabel);
    const float x = button.Max.x + pt(4.0f);
    label(dl, Weight::SemiBold, th::kPanelTitle, ImVec2(x, cy), Align::Left, th::kLabel, title);
    separator(dl, content.Min.x, content.Max.x, top + pt(th::kHeaderHeight) - hairline(), th::kRule);
    if (back) {
        *back = press.clicked;
    }
    if (titleRight) {
        *titleRight = x + measure(Weight::SemiBold, th::kPanelTitle, title).x;
    }
    return cy;
}

void tag(ImDrawList* dl, float right, float cy, const char* text, ImU32 fill, ImU32 color, ImU32 dot) {
    constexpr float kPoints = 10.5f;
    constexpr float kTracking = 0.08f;
    const float dotSpace = dot ? pt(6.0f + 6.0f) : 0.0f;
    const float width = pt(8.0f) + dotSpace + trackedWidth(Weight::Bold, kPoints, text, kTracking) + pt(8.0f);
    const ImRect r(ImVec2(right - width, cy - pt(11.0f)), ImVec2(right, cy + pt(11.0f)));
    dl->AddRectFilled(r.Min, r.Max, fill, pt(6.0f));
    float x = r.Min.x + pt(8.0f);
    if (dot) {
        dl->AddCircleFilled(ImVec2(x + pt(3.0f), cy), pt(3.0f), dot, 0);
        x += dotSpace;
    }
    tracked(dl, Weight::Bold, kPoints, ImVec2(x, cy), color, text, kTracking);
}

bool toggleRow(ImDrawList* dl, const char* id, const ImRect& row, const char* glyph, const char* title, bool* value) {
    const float cy = row.GetCenter().y;
    const ImVec2 size = toggleSize();
    icon(dl, glyph, ImVec2(row.Min.x + pt(10.0f + 9.0f), cy), 18.0f, th::kMutedIcon);
    const float x = row.Min.x + pt(10.0f + 18.0f + 12.0f);
    const float toggleX = row.Max.x - pt(10.0f) - size.x;
    label(dl, Weight::Regular, th::kSubhead, ImVec2(x, cy), Align::Left, th::kLabel, title,
          std::max(pt(40.0f), toggleX - pt(10.0f) - x));
    return toggle(id, ImVec2(toggleX, cy - size.y * 0.5f), value);
}

} // namespace ui::parts
