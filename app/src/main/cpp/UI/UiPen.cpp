// Preferencias del lápiz: la curva de presión (sus puntos se arrastran; un toque añade uno,
// y dos toques o arrastrarlo fuera de la gráfica lo quitan) y el suavizado de todos los
// trazos. La curva se aplica en la app a la presión del lápiz; el suavizado, en el lienzo.
#include "UI/Ui.h"

#include "UI/Anim.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace th = ui::theme;
using ui::Align;
using ui::Weight;
using ui::pt;

namespace {

// --- Medidas (pt) ---
constexpr float kSection = 34.0f;       // el rótulo de la sección
constexpr float kMinGraph = 150.0f;     // alto de la gráfica
constexpr float kMaxGraph = 200.0f;
constexpr float kGraphAspect = 0.62f;   // alto / ancho
constexpr float kHintGap = 8.0f;
constexpr float kSliderGap = 12.0f;
constexpr float kSliderHeight = 44.0f;
constexpr float kBottom = 6.0f;
constexpr float kPointRadius = 6.0f;
constexpr float kGrabbedRadius = 8.5f;
constexpr float kReach = 24.0f;         // un punto se coge a esta distancia
constexpr float kRemoveReach = 28.0f;   // un punto arrastrado así de fuera se quita
constexpr float kTapSlop = 10.0f;
constexpr int kCurveSteps = 72;

constexpr const char* kHint = "Toca para añadir un punto y arrástralo fuera para quitarlo.";

float graphHeight(float width) { return std::clamp(width * kGraphAspect, pt(kMinGraph), pt(kMaxGraph)); }

ImVec2 toVec(glm::vec2 v) { return ImVec2(v.x, v.y); }

} // namespace

float Ui::penSettingsHeight(float width) const {
    const float hint = ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f), width - pt(20.0f),
                                     Align::Left, 0, kHint, 1.35f);
    return pt(kSection) + graphHeight(width - pt(4.0f)) + pt(kHintGap) + hint +
           pt(kSliderGap + kSliderHeight + kBottom);
}

void Ui::penSettings(ImDrawList* dl, float left, float right, float y) {
    PressureCurve& curve = m_prefs.pressure;
    bool changed = false;

    // Rótulo y, a la derecha, volver a como venía (la recta y sin suavizado).
    const float headerY = y + pt(19.0f);
    const char* resetText = "Restablecer";
    const float resetWidth = ui::measure(Weight::SemiBold, th::kFootnote, resetText).x + pt(16.0f);
    const ImRect reset(ImVec2(right - resetWidth, headerY - pt(14.0f)), ImVec2(right, headerY + pt(14.0f)));
    y += pt(kSection);

    // La gráfica: la presión del lápiz va de izquierda a derecha y la del trazo, de abajo
    // arriba.
    const float graphLeft = left + pt(2.0f);
    const float graphRight = right - pt(2.0f);
    const ImRect graph(ImVec2(graphLeft, y), ImVec2(graphRight, y + graphHeight(graphRight - graphLeft)));
    auto toScreen = [&](glm::vec2 c) {
        return ImVec2(graph.Min.x + c.x * graph.GetWidth(), graph.Max.y - c.y * graph.GetHeight());
    };
    auto toCurve = [&](ImVec2 p) {
        return glm::vec2((p.x - graph.Min.x) / graph.GetWidth(), (graph.Max.y - p.y) / graph.GetHeight());
    };

    // Entrada: coger el punto más cercano y arrastrarlo (fuera, para quitarlo), quitarlo
    // con dos toques o añadir uno con un toque. Un arrastre que no empieza en un punto
    // desplaza la lista. Va antes que Restablecer: donde se tocan, manda la gráfica (el
    // punto de la esquina de arriba se coge también desde un poco más arriba).
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiID id = ImGui::GetID("##pressure-curve");
    const ImRect hit(ImVec2(graph.Min.x - pt(kReach * 0.5f), graph.Min.y - pt(kReach * 0.5f)),
                     ImVec2(graph.Max.x + pt(kReach * 0.5f), graph.Max.y + pt(kReach * 0.5f)));
    ImGui::ItemAdd(hit, id);
    bool hovered = false;
    bool held = false;
    const bool released = ImGui::ButtonBehavior(hit, id, &hovered, &held, ImGuiButtonFlags_NoNavFocus);
    if (ImGui::IsItemActivated()) {
        int best = -1;
        float bestDistance = pt(kReach);
        const std::vector<glm::vec2>& points = curve.points();
        for (size_t i = 0; i < points.size(); ++i) {
            const ImVec2 p = toScreen(points[i]);
            const float d = std::hypot(p.x - io.MouseClickedPos[0].x, p.y - io.MouseClickedPos[0].y);
            if (d < bestDistance) {
                bestDistance = d;
                best = static_cast<int>(i);
            }
        }
        const bool interior = best > 0 && best + 1 < static_cast<int>(points.size());
        if (interior && io.MouseClickedCount[0] >= 2) {
            // Dos toques en un punto de en medio también lo quitan.
            curve.remove(static_cast<size_t>(best));
            changed = true;
        } else if (best >= 0) {
            const ImVec2 p = toScreen(points[static_cast<size_t>(best)]);
            m_curveDrag.index = best;
            m_curveDrag.removing = false;
            m_curveDrag.grab = glm::vec2(p.x - io.MouseClickedPos[0].x, p.y - io.MouseClickedPos[0].y);
            ui::claimDrag();
        } else {
            m_curveDrag.tap = true;   // lejos de los puntos: al soltar sin moverse se añade uno
        }
    }
    if (m_curveDrag.index >= 0) {
        const size_t index = static_cast<size_t>(m_curveDrag.index);
        const bool interior = index > 0 && index + 1 < curve.points().size();
        const ImVec2 target(io.MousePos.x + m_curveDrag.grab.x, io.MousePos.y + m_curveDrag.grab.y);
        const float reach = pt(kRemoveReach);
        const bool outside = target.x < graph.Min.x - reach || target.x > graph.Max.x + reach ||
                             target.y < graph.Min.y - reach || target.y > graph.Max.y + reach;
        if (held) {
            m_curveDrag.removing = interior && outside;
            m_curveDrag.at = glm::vec2(target.x, target.y);
            if (!m_curveDrag.removing) {
                const glm::vec2 before = curve.points()[index];
                curve.move(index, toCurve(target));
                changed = changed || curve.points()[index] != before;
            }
        } else {
            if (m_curveDrag.removing) {
                curve.remove(index);
                changed = true;
            }
            m_curveDrag = {};
        }
    }
    if (m_curveDrag.tap && !held) {
        m_curveDrag.tap = false;
        const float slop = pt(kTapSlop);
        if (released && io.MouseDragMaxDistanceSqr[0] <= slop * slop && graph.Contains(io.MousePos)) {
            if (curve.points().size() >= PressureCurve::kMaxPoints) {
                notify("La curva ya tiene todos sus puntos", Notice::Info, 2200);
            } else if (curve.add(toCurve(io.MousePos)) >= 0) {
                changed = true;
            } else {
                notify("Hay otro punto muy cerca", Notice::Info, 2200);
            }
        }
    }

    const bool canReset = !curve.isDefault() || m_prefs.smoothing > 0.0f;
    const ui::Press resetPress = ui::pressable("##curve-reset", reset, canReset);
    ui::highlight(dl, ImGui::GetID("##curve-reset"), reset, pt(8.0f), false, resetPress);
    ui::label(dl, Weight::SemiBold, th::kFootnote, reset.GetCenter(), Align::Center,
              canReset ? th::kAccentText : th::kTertiaryLabel, resetText);
    ui::sectionLabel(dl, left + pt(10.0f), reset.Min.x - pt(6.0f), headerY, "PRESIÓN Y SUAVIZADO");
    if (resetPress.clicked && canReset) {
        curve = PressureCurve();
        m_prefs.smoothing = 0.0f;
        m_curveDrag = {};
        changed = true;
    }

    // Lo que se dibuja: sin el punto que se va a quitar.
    PressureCurve shown = curve;
    if (m_curveDrag.index >= 0 && m_curveDrag.removing) {
        shown.remove(static_cast<size_t>(m_curveDrag.index));
    }
    const float radius = pt(th::kControlRadius);
    dl->AddRectFilled(graph.Min, graph.Max, th::kControl, radius);
    for (int i = 1; i < 4; ++i) {
        const float t = static_cast<float>(i) * 0.25f;
        const float gx = ui::snap(graph.Min.x + graph.GetWidth() * t);
        const float gy = ui::snap(graph.Max.y - graph.GetHeight() * t);
        dl->AddLine(ImVec2(gx, graph.Min.y), ImVec2(gx, graph.Max.y), th::kRule, ui::hairline());
        dl->AddLine(ImVec2(graph.Min.x, gy), ImVec2(graph.Max.x, gy), th::kRule, ui::hairline());
    }
    if (!shown.isDefault()) {
        // La recta de siempre, de referencia.
        dl->AddLine(toScreen({0.0f, 0.0f}), toScreen({1.0f, 1.0f}), IM_COL32(255, 255, 255, 46), pt(1.0f));
    }
    ImVec2 line[kCurveSteps + 1];
    for (int i = 0; i <= kCurveSteps; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(kCurveSteps);
        line[i] = toScreen({x, shown(x)});
    }
    {
        // Relleno suave bajo la curva, en franjas sin suavizar los bordes (si no, se ven
        // las juntas).
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (int i = 0; i < kCurveSteps; ++i) {
            dl->AddQuadFilled(line[i], line[i + 1], ImVec2(line[i + 1].x, graph.Max.y), ImVec2(line[i].x, graph.Max.y),
                              IM_COL32(10, 132, 255, 34));
        }
        dl->Flags = flags;
    }
    dl->AddPolyline(line, kCurveSteps + 1, th::kAccent, ImDrawFlags_None, pt(2.5f));
    ui::outline(dl, graph, radius, th::kControlBorder, pt(1.0f));

    // La presión del lápiz mientras toca: dónde cae en la curva.
    if (m_status.penPressure >= 0.0f) {
        const float x = std::clamp(m_status.penPressure, 0.0f, 1.0f);
        const ImVec2 top = toScreen({x, 1.0f});
        const ImVec2 bottom = toScreen({x, 0.0f});
        dl->AddLine(top, bottom, ui::withAlpha(th::kAccentText, 0.55f), pt(1.0f));
        const ImVec2 p = toScreen({x, shown(x)});
        dl->AddCircleFilled(p, pt(5.0f), th::kAccent, 0);
        dl->AddCircle(p, pt(5.0f), IM_COL32(255, 255, 255, 255), 0, pt(1.5f));
    }

    // Los puntos.
    const ImU32 shadow = IM_COL32(0, 0, 0, 80);
    const std::vector<glm::vec2>& points = curve.points();
    for (size_t i = 0; i < points.size(); ++i) {
        const bool grabbed = m_curveDrag.index == static_cast<int>(i);
        const float r = pt(grabbed ? kGrabbedRadius : kPointRadius);
        if (grabbed && m_curveDrag.removing) {
            // Va con el puntero, aunque salga del panel.
            const ImVec2 p = toVec(m_curveDrag.at);
            ui::pushUnclipped(dl);
            dl->AddCircleFilled(p, r, IM_COL32(255, 255, 255, 90), 0);
            dl->AddCircle(p, r, th::kRed, 0, pt(2.0f));
            ui::popUnclipped(dl);
            continue;
        }
        const ImVec2 p = toScreen(points[i]);
        dl->AddCircleFilled(ImVec2(p.x, p.y + pt(1.0f)), r + pt(1.5f), shadow, 0);
        dl->AddCircleFilled(p, r, IM_COL32(255, 255, 255, 255), 0);
        if (grabbed) {
            dl->AddCircle(p, r + pt(1.0f), th::kAccent, 0, pt(2.0f));
        }
    }
    y = graph.Max.y + pt(kHintGap);

    const float hintWidth = right - left - pt(20.0f);
    y += ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(left + pt(10.0f), y), hintWidth, Align::Left,
                       IM_COL32(235, 235, 245, 128), kHint, 1.35f);
    y += pt(kSliderGap);

    // Suavizado de todos los trazos.
    float t = m_prefs.smoothing;
    char value[16];
    std::snprintf(value, sizeof(value), "%d %%", static_cast<int>(std::lround(m_prefs.smoothing * 100.0f)));
    bool sliding = false;
    const ImRect slider(ImVec2(left + pt(2.0f), y), ImVec2(right - pt(2.0f), y + pt(kSliderHeight)));
    if (ui::paramSlider("##smoothing", slider, &t, "Suavizado", value, true, &sliding)) {
        m_prefs.smoothing = std::clamp(t, 0.0f, 1.0f);
        changed = true;
    }

    // Se guarda cuando no queda nada a medias.
    if (changed) {
        m_prefsDirty = true;
    }
    if (m_prefsDirty && !sliding && m_curveDrag.index < 0) {
        savePrefs();
        m_prefsDirty = false;
    }
}
