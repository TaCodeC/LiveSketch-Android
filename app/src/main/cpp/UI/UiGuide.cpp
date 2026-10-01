// Guía de dibujo: sus líneas encima del lienzo y su edición, con la barra de abajo (el tipo
// de guía, la opacidad y el tamaño de la cuadrícula o la simetría rotacional) y los
// tiradores del centro y del giro sobre el lienzo. Lo que hace la simetría con los trazos
// está en Canvas.
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"
#include "UI/PanelParts.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;
using ui::parts::pressFeedback;

namespace {

constexpr float kPi = 3.14159265358979f;

// --- Barra de abajo (pt) ---
constexpr float kPad = 6.0f;
constexpr float kHeaderHeight = 44.0f;
constexpr float kCellHeight = 50.0f;
constexpr float kCellWidth = 72.0f;
constexpr float kIconCellWidth = 46.0f;  // sin el nombre (teléfono en horizontal)
constexpr float kCellGap = 4.0f;
constexpr float kGroupGap = 13.0f;       // entre los tipos y los deslizadores, con una línea
constexpr float kSliderHeight = 44.0f;
constexpr float kButtonHeight = 38.0f;
constexpr float kIconButton = 38.0f;
constexpr float kTextButton = 100.0f;
constexpr float kRowRule = 7.0f;         // entre filas, con una línea en medio
constexpr float kGap = 8.0f;
constexpr float kMaxWidth = 760.0f;
constexpr float kMinSlider = 140.0f;     // más estrechos, los deslizadores van en otra fila

// --- Sobre el lienzo (pt) ---
constexpr float kCenterRadius = 9.0f;
constexpr float kTurnRadius = 7.5f;
constexpr float kTurnDistance = 88.0f;   // del centro al tirador del giro
constexpr float kTurnReach = 28.0f;      // el tirador del giro se coge a esta distancia
constexpr float kLineSpacing = 8.0f;     // la cuadrícula, con las líneas al menos así de separadas
constexpr ImU32 kLineColor = IM_COL32(64, 156, 255, 255);
// Imanes: el centro se queda en el del lienzo a menos de esto en cada eje, y el giro en
// los múltiplos de 15°, a menos de 3°.
constexpr float kCenterSnap = 10.0f;
constexpr float kAngleStep = 15.0f * kPi / 180.0f;
constexpr float kAngleSnap = 3.0f * kPi / 180.0f;
// La opacidad no baja de esto: una guía invisible no sirve.
constexpr float kMinOpacity = 0.05f;

struct KindInfo {
    const char* glyph;
    const char* label;
    const char* detail;      // en la cabecera
    GuideKind kind;
    SymmetryKind symmetry;
};
constexpr KindInfo kKinds[5] = {
    {icon::kGrid, "Cuadrícula", "Cuadrícula", GuideKind::Grid, SymmetryKind::Vertical},
    {icon::kSymmetry, "Vertical", "Simetría vertical", GuideKind::Symmetry, SymmetryKind::Vertical},
    {icon::kSymmetryHorizontal, "Horizontal", "Simetría horizontal", GuideKind::Symmetry, SymmetryKind::Horizontal},
    {icon::kQuadrant, "Cuadrante", "Simetría en cuadrante", GuideKind::Symmetry, SymmetryKind::Quadrant},
    {icon::kRadial, "Radial", "Simetría radial", GuideKind::Symmetry, SymmetryKind::Radial},
};

int kindIndex(const DrawingGuide& g) {
    if (g.kind == GuideKind::Grid) {
        return 0;
    }
    switch (g.symmetry) {
    case SymmetryKind::Vertical:
        return 1;
    case SymmetryKind::Horizontal:
        return 2;
    case SymmetryKind::Quadrant:
        return 3;
    case SymmetryKind::Radial:
        return 4;
    }
    return 1;
}

// Tamaño de la cuadrícula en el deslizador: escala logarítmica, en píxeles enteros.
float gridToSlider(float size) {
    const float range = std::log(guide::kMaxGridSize / guide::kMinGridSize);
    return std::log(std::clamp(size, guide::kMinGridSize, guide::kMaxGridSize) / guide::kMinGridSize) / range;
}

float sliderToGrid(float t) {
    const float range = guide::kMaxGridSize / guide::kMinGridSize;
    return std::round(guide::kMinGridSize * std::pow(range, std::clamp(t, 0.0f, 1.0f)));
}

// El tirador del giro sale del centro hacia un lado de la guía: en la simetría vertical,
// por su eje hacia arriba; en las demás, hacia la derecha.
float turnOffset(const DrawingGuide& g) {
    return g.kind == GuideKind::Symmetry && g.symmetry == SymmetryKind::Vertical ? -kPi * 0.5f : 0.0f;
}

ImVec2 turnHandle(const DrawingGuide& g, const ToolView& view) {
    const ImVec2 c = view.toScreen(g.center);
    const float a = g.angle + turnOffset(g);
    const glm::vec2 d = view.axisX * std::cos(a) + view.axisY * std::sin(a);   // en la pantalla
    return ImVec2(c.x + d.x * pt(kTurnDistance), c.y + d.y * pt(kTurnDistance));
}

float distance(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// De -π a π.
float wrapAngle(float angle) {
    angle = std::remainder(angle, 2.0f * kPi);
    return angle <= -kPi ? angle + 2.0f * kPi : angle;
}

Press iconButton(ImDrawList* dl, const char* id, const ImRect& r, const char* glyph, bool enabled) {
    const ImGuiID gid = ImGui::GetID(id);
    const Press press = ui::pressable(gid, r, enabled);
    dl->AddRectFilled(r.Min, r.Max, ui::withAlpha(th::kControl, enabled ? 1.0f : 0.5f), pt(th::kControlRadius));
    pressFeedback(dl, gid, r, press, pt(th::kControlRadius));
    ui::icon(dl, glyph, r.GetCenter(), 18.0f, enabled ? th::kLabel : ui::withAlpha(th::kLabel, 0.32f));
    return press;
}

// Celda de un tipo de guía: icono con su nombre debajo, en el color de acento si es el elegido.
bool kindCell(ImDrawList* dl, const ImRect& r, const KindInfo& info, bool selected, bool enabled, bool named) {
    const ui::Choice c = ui::choice("##kind", r, selected, enabled, false);
    const ImU32 color = ui::mix(IM_COL32(235, 235, 245, 222), th::kAccentText, c.on);
    const float middle = r.GetCenter().y;
    const float height = r.GetHeight();
    if (!named) {
        // El nombre del elegido ya sale en la cabecera.
        ui::icon(dl, info.glyph, r.GetCenter(), 20.0f, color);
        return c.press.clicked;
    }
    ui::icon(dl, info.glyph, ImVec2(r.GetCenter().x, middle - height * 0.15f), 19.0f, color);
    ui::label(dl, Weight::Regular, th::kMicro, ImVec2(r.GetCenter().x, middle + height * 0.26f), Align::Center, color,
              info.label, r.GetWidth() - pt(2.0f));
    return c.press.clicked;
}

} // namespace

// -----------------------------------------------------------------------------
// Empezar y terminar
// -----------------------------------------------------------------------------

void Ui::startGuide(Canvas& canvas) {
    closePanels();
    m_eyedropperArmed = false;
    if (m_canvasTool == CanvasTool::Guide) {
        return;
    }
    // La herramienta a la que se vuelve al terminar. Salir de Transformar la aplica, y salir
    // de un ajuste, también.
    if (m_canvasTool == CanvasTool::Adjust) {
        finishAdjust(canvas, true);
    }
    if (m_canvasTool == CanvasTool::Transform) {
        setCanvasTool(canvas, m_toolBeforeTransform);
    } else if (m_canvasTool == CanvasTool::Select) {
        toolCancel(canvas);
    }
    m_toolBeforeGuide = m_canvasTool;
    m_guideBefore = canvas.guide();
    DrawingGuide guide = canvas.guide();
    guide.enabled = true;
    canvas.setGuide(guide);
    m_guideDrag = {};
    m_canvasTool = CanvasTool::Guide;
}

void Ui::finishGuide(Canvas& canvas, bool keep) {
    m_guideDrag = {};
    if (!keep) {
        canvas.setGuide(m_guideBefore);
    }
    if (m_canvasTool == CanvasTool::Guide) {
        m_canvasTool = m_toolBeforeGuide;
    }
}

// -----------------------------------------------------------------------------
// Tiradores
// -----------------------------------------------------------------------------

void Ui::guidePress(const Canvas& canvas, const ToolView& view, ImVec2 position) {
    // El tirador del giro se coge si se toca cerca; cualquier otro sitio mueve la guía.
    const DrawingGuide& g = canvas.guide();
    const glm::vec2 point = view.toCanvas(position);
    m_guideDrag = {};
    m_guideDrag.before = g;
    if (distance(position, turnHandle(g, view)) <= pt(kTurnReach)) {
        const glm::vec2 d = point - g.center;
        m_guideDrag.handle = 2;
        m_guideDrag.spin = glm::dot(d, d) > 1e-6f ? g.angle - std::atan2(d.y, d.x) : 0.0f;
    } else {
        m_guideDrag.handle = 1;
        m_guideDrag.grab = g.center - point;
    }
}

void Ui::guideDrag(Canvas& canvas, const ToolView& view, ImVec2 position) {
    if (m_guideDrag.handle == 0) {
        return;
    }
    DrawingGuide g = canvas.guide();
    const glm::vec2 point = view.toCanvas(position);
    if (m_guideDrag.handle == 1) {
        g.center = point + m_guideDrag.grab;
        const glm::vec2 middle(static_cast<float>(canvas.width()) * 0.5f, static_cast<float>(canvas.height()) * 0.5f);
        const float reach = view.toCanvasLength(pt(kCenterSnap));
        for (int axis = 0; axis < 2; ++axis) {
            if (std::fabs(g.center[axis] - middle[axis]) <= reach) {
                g.center[axis] = middle[axis];
            }
        }
    } else {
        const glm::vec2 d = point - g.center;
        if (glm::dot(d, d) < 1e-6f) {
            return;
        }
        float angle = wrapAngle(std::atan2(d.y, d.x) + m_guideDrag.spin);
        const float snapped = std::round(angle / kAngleStep) * kAngleStep;
        if (std::fabs(angle - snapped) <= kAngleSnap) {
            angle = snapped;
        }
        g.angle = angle;
    }
    canvas.setGuide(g);
}

void Ui::guideRelease() { m_guideDrag = {}; }

void Ui::guideCancel(Canvas& canvas) {
    if (m_guideDrag.handle != 0) {
        canvas.setGuide(m_guideDrag.before);
    }
    m_guideDrag = {};
}

// -----------------------------------------------------------------------------
// Dibujo
// -----------------------------------------------------------------------------

void Ui::drawGuide(const Canvas& canvas) {
    const DrawingGuide& g = canvas.guide();
    if (!g.enabled) {
        return;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ToolView& view = m_status.canvasView;
    const glm::vec2 size(static_cast<float>(canvas.width()), static_cast<float>(canvas.height()));
    const bool grid = g.kind == GuideKind::Grid;
    const ImU32 color = ui::withAlpha(kLineColor, std::clamp(g.opacity, 0.0f, 1.0f));
    const float thickness = grid ? std::max(ui::hairline(), pt(1.0f)) : pt(1.5f);
    for (const guide::Segment& s : guide::lines(g, size, view.toCanvasLength(pt(kLineSpacing)))) {
        dl->AddLine(view.toScreen(s.a), view.toScreen(s.b), color, thickness);
    }
    if (m_canvasTool != CanvasTool::Guide) {
        return;
    }

    // Tiradores: el centro (azul) y, unido a él, el del giro (verde).
    const ImVec2 c = view.toScreen(g.center);
    const ImVec2 t = turnHandle(g, view);
    const ImGuiID id = ImHashStr("##guide-handles");
    const float moving = ui::anim::follow(id, m_guideDrag.handle == 1 ? 1.0f : 0.0f, 20.0f);
    const float turning = ui::anim::follow(id + 1u, m_guideDrag.handle == 2 ? 1.0f : 0.0f, 20.0f);
    const ImU32 shadow = IM_COL32(0, 0, 0, 70);
    dl->AddLine(ImVec2(c.x, c.y + pt(1.0f)), ImVec2(t.x, t.y + pt(1.0f)), shadow, pt(2.5f));
    dl->AddLine(c, t, IM_COL32(255, 255, 255, 220), pt(1.5f));
    const float centerRadius = pt(kCenterRadius) * (1.0f + 0.25f * moving);
    const float turnRadius = pt(kTurnRadius) * (1.0f + 0.3f * turning);
    for (const auto& [p, r, fill] : {std::tuple{c, centerRadius, th::kAccent}, std::tuple{t, turnRadius, th::kGreen}}) {
        dl->AddCircleFilled(ImVec2(p.x, p.y + pt(1.0f)), r + pt(2.0f), shadow, 0);
        dl->AddCircleFilled(p, r, fill, 0);
        dl->AddCircle(p, r, IM_COL32(255, 255, 255, 255), 0, pt(2.0f));
    }
}

// -----------------------------------------------------------------------------
// Barra de abajo
// -----------------------------------------------------------------------------

void Ui::guideDock(Canvas& canvas) {
    const Layout& L = m_layout;
    const bool open = m_canvasTool == CanvasTool::Guide;
    const float presence =
        ui::anim::followFrom(ImHashStr("##dock-guide"), 0.0f, open ? 1.0f : 0.0f, open ? 18.0f : 24.0f);
    if (!open && presence <= 0.002f) {
        return;
    }
    DrawingGuide g = canvas.guide();
    const bool grid = g.kind == GuideKind::Grid;
    const int current = kindIndex(g);

    // Tableta: cancelar y hecho en la cabecera y, si caben, los tipos y los deslizadores en
    // una fila. Teléfono en vertical: los deslizadores debajo de los tipos y los botones abajo.
    // Teléfono en horizontal: una fila, aunque los tipos tengan que ir sin nombre (con dos
    // filas apenas quedaría lienzo a la vista). La barra solo se estrecha para no tapar la
    // barra lateral cuando esta baja hasta su altura.
    const float pad = pt(kPad);
    const float full = L.right - L.left;
    const bool wide = !L.narrow;
    auto widthFor = [&](float height) {
        if (!wide) {
            return full;
        }
        const bool clear = L.sidebar.Max.y + pt(10.0f) <= L.bottom - height;
        return std::min(pt(kMaxWidth), clear ? full : full - (L.sidebar.GetWidth() + pt(10.0f)) * 2.0f);
    };
    const float namedKinds = pt(kCellWidth) * 5.0f + pt(kCellGap) * 4.0f;
    const float iconKinds = pt(kIconCellWidth) * 5.0f + pt(kCellGap) * 4.0f;
    auto singleWidth = [&](float kinds) {
        return pad * 2.0f + kinds + pt(kGroupGap) + pt(kMinSlider) * 2.0f + pt(kGap);
    };
    const float head = pad * 2.0f + pt(kHeaderHeight) + pt(kRowRule) + pt(kCellHeight);
    float height = head;
    float width = widthFor(height);
    bool single = wide && width >= singleWidth(namedKinds);
    bool named = true;
    if (!single && wide && L.compact && width >= singleWidth(iconKinds)) {
        single = true;
        named = false;
    }
    const float kindsWidth = named ? namedKinds : iconKinds;
    if (!single) {
        height += pt(kRowRule) + pt(kSliderHeight);
        if (!wide) {
            height += pt(kRowRule) + pt(kButtonHeight);
        }
        width = widthFor(height);
    }
    const float x = std::round((L.left + L.right - width) * 0.5f);
    const ImRect rect(x, L.bottom - height, x + width, L.bottom);
    if (open) {
        m_dock = rect;
    }

    ui::beginSurface("##dock-guide", rect, open, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    const float radius = pt(th::kPanelRadius);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(30.0f), pt(8.0f), 0.26f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kPanelTint);

    const float left = rect.Min.x + pad;
    const float right = rect.Max.x - pad;
    float y = rect.Min.y + pad;

    // Cabecera: la guía y cómo es; centrar y (en una tableta) cancelar y hecho.
    const float cy = y + pt(kHeaderHeight) * 0.5f;
    float buttonsLeft = right;
    bool cancelIt = false;
    bool doneIt = false;
    if (wide) {
        const ImRect done(ImVec2(right - pt(kTextButton), cy - pt(kButtonHeight) * 0.5f),
                          ImVec2(right, cy + pt(kButtonHeight) * 0.5f));
        const ImRect cancel(ImVec2(done.Min.x - pt(kGap) - pt(kTextButton), done.Min.y),
                            ImVec2(done.Min.x - pt(kGap), done.Max.y));
        cancelIt = ui::button("##cancel", cancel, "Cancelar", ui::ButtonStyle::Secondary, open);
        doneIt = ui::button("##done", done, "Hecho", ui::ButtonStyle::Primary, open);
        const float divider = std::round(cancel.Min.x - pt(13.0f) * 0.5f);
        dl->AddLine(ImVec2(divider, cy - pt(13.0f)), ImVec2(divider, cy + pt(13.0f)), th::kRule, ui::hairline());
        buttonsLeft = cancel.Min.x - pt(13.0f);
    }
    const glm::vec2 middle(static_cast<float>(canvas.width()) * 0.5f, static_cast<float>(canvas.height()) * 0.5f);
    const bool centered = g.center == middle && g.angle == 0.0f;
    const ImRect centerButton(ImVec2(buttonsLeft - pt(kIconButton), cy - pt(kIconButton) * 0.5f),
                              ImVec2(buttonsLeft, cy + pt(kIconButton) * 0.5f));
    const bool centerIt = iconButton(dl, "##center", centerButton, icon::kCenter, open && !centered).clicked;

    const ImRect badge(ImVec2(left + pt(4.0f), cy - pt(16.0f)), ImVec2(left + pt(36.0f), cy + pt(16.0f)));
    dl->AddRectFilled(badge.Min, badge.Max, th::kControl, pt(9.0f));
    ui::icon(dl, kKinds[current].glyph, badge.GetCenter(), 18.0f, th::kAccentText);
    const float textX = badge.Max.x + pt(10.0f);
    const float textWidth = std::max(pt(40.0f), centerButton.Min.x - pt(10.0f) - textX);
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(textX, cy - pt(8.0f)), Align::Left, th::kLabel,
              "Guía de dibujo", textWidth);
    {
        char detail[64];
        if (grid) {
            std::snprintf(detail, sizeof(detail), "Cuadrícula de %d px", static_cast<int>(std::lround(g.gridSize)));
        } else {
            std::snprintf(detail, sizeof(detail), g.rotational ? "%s · rotacional" : "%s", kKinds[current].detail);
        }
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(textX, cy + pt(10.0f)), Align::Left, th::kSecondaryLabel,
                  detail, textWidth);
    }
    y += pt(kHeaderHeight);
    ui::separator(dl, rect.Min.x + pt(12.0f), rect.Max.x - pt(12.0f), std::round(y + pt(kRowRule) * 0.5f), th::kRule);
    y += pt(kRowRule);

    // Tipos de guía.
    bool changed = false;
    const float cellWidth =
        single ? pt(named ? kCellWidth : kIconCellWidth) : (right - left - pt(kCellGap) * 4.0f) / 5.0f;
    for (int i = 0; i < 5; ++i) {
        const float x0 = left + (cellWidth + pt(kCellGap)) * static_cast<float>(i);
        const ImRect r(ImVec2(x0, y), ImVec2(x0 + cellWidth, y + pt(kCellHeight)));
        ImGui::PushID(i);
        if (kindCell(dl, r, kKinds[i], i == current, open, named) && open && i != current) {
            g.kind = kKinds[i].kind;
            g.symmetry = kKinds[i].symmetry;
            changed = true;
        }
        ImGui::PopID();
    }

    // Opacidad y, según el tipo, el tamaño de la cuadrícula o la simetría rotacional.
    float slidersLeft = left;
    float sliderTop = 0.0f;
    if (single) {
        const float divider = std::round(left + kindsWidth + pt(kGroupGap) * 0.5f);
        dl->AddLine(ImVec2(divider, y + pt(9.0f)), ImVec2(divider, y + pt(kCellHeight) - pt(9.0f)), th::kRule,
                    ui::hairline());
        slidersLeft = left + kindsWidth + pt(kGroupGap);
        sliderTop = y + (pt(kCellHeight) - pt(kSliderHeight)) * 0.5f;
        y += pt(kCellHeight);
    } else {
        y += pt(kCellHeight);
        ui::separator(dl, rect.Min.x + pt(12.0f), rect.Max.x - pt(12.0f), std::round(y + pt(kRowRule) * 0.5f),
                      th::kRule);
        y += pt(kRowRule);
        sliderTop = y;
        y += pt(kSliderHeight);
    }
    const float sliderWidth = (right - slidersLeft - pt(kGap)) * 0.5f;
    const ImRect opacityRect(ImVec2(slidersLeft, sliderTop), ImVec2(slidersLeft + sliderWidth, sliderTop + pt(kSliderHeight)));
    const ImRect secondRect(ImVec2(right - sliderWidth, sliderTop), ImVec2(right, sliderTop + pt(kSliderHeight)));
    {
        float t = g.opacity;
        char value[16];
        std::snprintf(value, sizeof(value), "%d %%", static_cast<int>(std::lround(g.opacity * 100.0f)));
        if (ui::paramSlider("##opacity", opacityRect, &t, "Opacidad", value, open) && open) {
            g.opacity = std::clamp(t, kMinOpacity, 1.0f);
            changed = true;
        }
    }
    if (grid) {
        float t = gridToSlider(g.gridSize);
        char value[16];
        std::snprintf(value, sizeof(value), "%d px", static_cast<int>(std::lround(g.gridSize)));
        if (ui::paramSlider("##grid-size", secondRect, &t, "Tamaño", value, open) && open) {
            const float size = sliderToGrid(t);
            if (size != g.gridSize) {
                g.gridSize = size;
                changed = true;
            }
        }
    } else {
        // Las copias van giradas alrededor del centro en vez de reflejadas.
        const ui::Choice c = ui::choice("##rotational", secondRect, g.rotational, open, true);
        const ImU32 color = ui::mix(IM_COL32(235, 235, 245, 222), th::kAccentText, c.on);
        const float midY = secondRect.GetCenter().y;
        ui::icon(dl, icon::kRotational, ImVec2(secondRect.Min.x + pt(12.0f + 9.0f), midY), 18.0f, color);
        ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(secondRect.Min.x + pt(12.0f + 18.0f + 10.0f), midY),
                  Align::Left, color, "Rotacional", secondRect.GetWidth() - pt(12.0f + 18.0f + 10.0f + 8.0f));
        if (c.press.clicked && open) {
            g.rotational = !g.rotational;
            changed = true;
        }
    }

    // Teléfono: cancelar y hecho abajo.
    if (!wide) {
        ui::separator(dl, rect.Min.x + pt(12.0f), rect.Max.x - pt(12.0f), std::round(y + pt(kRowRule) * 0.5f),
                      th::kRule);
        y += pt(kRowRule);
        const float half = (right - left - pt(kGap)) * 0.5f;
        const ImRect cancel(ImVec2(left, y), ImVec2(left + half, y + pt(kButtonHeight)));
        const ImRect done(ImVec2(right - half, y), ImVec2(right, y + pt(kButtonHeight)));
        cancelIt = ui::button("##cancel", cancel, "Cancelar", ui::ButtonStyle::Secondary, open);
        doneIt = ui::button("##done", done, "Hecho", ui::ButtonStyle::Primary, open);
    }

    ui::transform(mark, ImVec2(rect.GetCenter().x, rect.Max.y), 1.0f, ImVec2(0.0f, pt(18.0f) * (1.0f - presence)),
                  std::min(1.0f, presence * 1.4f));
    ui::endSurface();

    if (!open) {
        return;
    }
    if (centerIt) {
        g.center = middle;
        g.angle = 0.0f;
        changed = true;
    }
    if (changed) {
        canvas.setGuide(g);
    }
    if (cancelIt || doneIt) {
        finishGuide(canvas, doneIt);
    }
}
