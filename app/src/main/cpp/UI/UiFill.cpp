// Relleno arrastrando el color: del botón del color al lienzo.
//
// Arrastrar el botón lleva un círculo con el color. Soltarlo sobre el lienzo rellena la
// zona que hay debajo con el último umbral. Si el puntero se queda quieto un momento
// sobre el lienzo, el relleno se ve ya y deslizar en horizontal cambia el umbral (arriba
// se ve el valor); al soltar se aplica. Soltarlo sobre la interfaz no hace nada.
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace th = ui::theme;
using ui::pt;

namespace {

// Medidas en pt.
constexpr float kDragStart = 10.0f;       // como un toque de la interfaz: más, y es arrastrar
constexpr float kStillSlop = 6.0f;        // quieto: se mueve menos que esto
constexpr float kThresholdSpan = 300.0f;  // deslizar esto cambia el umbral del 0 al 100 %
constexpr float kDiscRadius = 22.0f;      // el color que va con el puntero
constexpr float kMarkRadius = 9.0f;       // el punto de donde sale el relleno
constexpr double kHoldSeconds = 0.4;      // quieto este tiempo: se ve el relleno

float distance(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

} // namespace

void Ui::colorDrop(Canvas& canvas, ImGuiID button) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool held = ImGui::GetActiveID() == button && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (ImGui::IsMousePosValid() && (held || m_drop.dragging)) {
        m_drop.position = io.MousePos;
    }
    if (!held) {
        if (m_drop.dragging && !m_drop.finished) {
            finishDrop(canvas);
        }
        m_drop = {};
        return;
    }
    if (m_drop.finished) {
        return;
    }
    const double now = ImGui::GetTime();
    if (!m_drop.dragging) {
        const float slop = pt(kDragStart);
        if (io.MouseDragMaxDistanceSqr[0] <= slop * slop) {
            return;
        }
        m_drop.dragging = true;
        m_drop.stillAt = m_drop.position;
        m_drop.stillSince = now;
        closePanels();
        return;
    }

    if (m_drop.filling) {
        if (!canvas.filling()) {
            // Algo lo terminó (deshacer): el resto del gesto no hace nada.
            m_drop.filling = false;
            m_drop.finished = true;
            return;
        }
        const float dx = m_drop.position.x - m_drop.fillAt.x;
        m_fillThreshold = std::clamp(m_drop.startThreshold + dx / pt(kThresholdSpan), 0.0f, 1.0f);
        canvas.setFillThreshold(m_fillThreshold);
        // El umbral se muestra donde salen los avisos.
        m_toast.until = std::min(m_toast.until, SDL_GetTicks());
        return;
    }

    // Quieto un momento sobre el lienzo: se ve el relleno y se ajusta el umbral.
    if (distance(m_drop.position, m_drop.stillAt) > pt(kStillSlop)) {
        m_drop.stillAt = m_drop.position;
        m_drop.stillSince = now;
        m_drop.refused = false;
        return;
    }
    if (m_drop.refused || now - m_drop.stillSince < kHoldSeconds || !overCanvas(canvas, m_drop.position)) {
        return;
    }
    if (startFill(canvas, m_drop.position)) {
        m_drop.filling = true;
        m_drop.fillAt = m_drop.position;
        m_drop.startThreshold = m_fillThreshold;
        m_toast.until = std::min(m_toast.until, SDL_GetTicks());
    } else {
        m_drop.refused = true;
    }
}

void Ui::finishDrop(Canvas& canvas) {
    if (m_drop.filling) {
        if (canvas.filling()) {
            fillApplied(canvas, canvas.endFill(true));
        }
        return;
    }
    // Soltado sobre la interfaz o fuera del lienzo: no se rellena nada.
    if (!overCanvas(canvas, m_drop.position) || m_drop.refused) {
        return;
    }
    if (startFill(canvas, m_drop.position)) {
        fillApplied(canvas, canvas.endFill(true));
    }
}

void Ui::cancelDrop(Canvas& canvas) {
    if (m_drop.filling) {
        canvas.endFill(false);
    }
    m_drop.filling = false;
    m_drop.finished = true;
}

bool Ui::startFill(Canvas& canvas, ImVec2 position) {
    const glm::vec2 p = m_status.canvasView.toCanvas(position);
    switch (canvas.beginFill(p.x, p.y, m_fillThreshold)) {
    case Canvas::Edit::Done:
        return true;
    case Canvas::Edit::Hidden:
        notify(canvas.layers().active().visible ? "La capa recorta con una capa oculta" : "La capa activa está oculta",
               Notice::Warning);
        return false;
    case Canvas::Edit::NoMemory:
        notify("No hay memoria para rellenar", Notice::Error);
        return false;
    default:
        return false;
    }
}

void Ui::fillApplied(Canvas& canvas, bool changed) {
    if (changed) {
        pushRecent(canvas.brushSettings().color);
    } else if (canvas.hasSelection()) {
        notify("Esa zona está fuera de la selección", Notice::Info, 2200);
    }
}

bool Ui::overCanvas(const Canvas& canvas, ImVec2 position) const {
    // Las barras, los paneles y la barra de opciones son ventanas de ImGui.
    ImGuiWindow* hovered = nullptr;
    ImGui::FindHoveredWindowEx(position, false, &hovered, nullptr);
    if (hovered) {
        return false;
    }
    const glm::vec2 p = m_status.canvasView.toCanvas(position);
    return p.x >= 0.0f && p.y >= 0.0f && p.x < static_cast<float>(canvas.width()) &&
           p.y < static_cast<float>(canvas.height());
}

void Ui::drawDrop(Canvas& canvas) {
    const ImGuiID id = ImHashStr("##drop");
    const bool shown = m_drop.dragging && !m_drop.finished;
    const float p = ui::anim::followFrom(id, 0.0f, shown ? 1.0f : 0.0f, shown ? 26.0f : 12.0f);
    if (shown) {
        // Mientras se ajusta el umbral, el color se queda en el punto de donde sale el relleno.
        m_dropMark.center = m_drop.filling ? m_drop.fillAt : m_drop.position;
        m_dropMark.small = m_drop.filling;
    }
    if (p <= 0.002f) {
        return;
    }
    const float small = ui::anim::follow(id + 1u, m_dropMark.small ? 1.0f : 0.0f, 20.0f);
    const float radius = pt(kDiscRadius) + (pt(kMarkRadius) - pt(kDiscRadius)) * small;
    const ImVec2 center = m_dropMark.center;
    const float ring = pt(2.5f);
    const float outer = radius + ring;
    const float margin = pt(24.0f);
    const ImRect rect(ImVec2(center.x - outer - margin, center.y - outer - margin),
                      ImVec2(center.x + outer + margin, center.y + outer + margin));

    ui::beginSurface("##drop", rect, false, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark drawn = ui::mark(dl);
    const ImRect circle(ImVec2(center.x - outer, center.y - outer), ImVec2(center.x + outer, center.y + outer));
    ui::pushUnclipped(dl);
    ui::shadow(dl, circle, outer, pt(16.0f), pt(4.0f), 0.3f);
    ui::popUnclipped(dl);
    dl->AddCircleFilled(center, outer, IM_COL32_WHITE, 0);
    dl->AddCircleFilled(center, radius, ui::fromFloat(canvas.brushSettings().color), 0);
    dl->AddCircle(center, outer + ui::hairline() * 0.5f, IM_COL32(0, 0, 0, 60), 0, ui::hairline());
    ui::transform(drawn, center, 0.6f + 0.4f * p, ImVec2(0.0f, 0.0f), p);
    ui::endSurface();
}
