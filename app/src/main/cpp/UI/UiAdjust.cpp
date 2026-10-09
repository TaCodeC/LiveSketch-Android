// Ajustes de imagen: la lista (bajo el botón Ajustes o, en un teléfono, desde Modificar),
// la barra de abajo con los deslizadores de cada ajuste y el arrastre en el lienzo, que
// cambia el valor principal del desenfoque, enfocar y el ruido.
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"
#include "UI/PanelParts.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;
using ui::parts::pressFeedback;

namespace {

// --- Barra de abajo (pt) ---
constexpr float kPad = 6.0f;
constexpr float kHeaderHeight = 44.0f;
constexpr float kRangeHeight = 34.0f;    // sombras, medios tonos y luces
constexpr float kSliderHeight = 44.0f;
constexpr float kButtonHeight = 38.0f;
constexpr float kIconButton = 38.0f;
constexpr float kTextButton = 100.0f;
constexpr float kRowRule = 7.0f;         // entre filas, con una línea en medio
constexpr float kGap = 8.0f;
constexpr float kMaxWidth = 720.0f;
// Por debajo de esto los deslizadores no van en una fila (se cortarían sus textos). Los
// del balance llevan un nombre a cada lado.
constexpr float kMinSlider = 150.0f;
constexpr float kMinBalanceSlider = 180.0f;
// Deslizar esto a los lados sobre el lienzo recorre el deslizador entero.
constexpr float kDragSpan = 300.0f;
// Hasta aquí es un toque (no cambia el valor).
constexpr float kDragSlop = 6.0f;

constexpr const char* kMinus = "\xe2\x88\x92";   // U+2212

struct Info {
    const char* glyph;
    const char* title;
    const char* detail;
};
// En el orden de Adjustment.
constexpr Info kAdjustments[5] = {
    {icon::kSun, "Tono, saturación y brillo", "Cambia el color y la luz"},
    {icon::kScale, "Balance de color", "Sombras, medios tonos y luces"},
    {icon::kAperture, "Desenfoque gaussiano", "Suaviza la capa"},
    {icon::kFocus, "Enfocar", "Marca más los bordes"},
    {icon::kGrain, "Ruido", "Añade grano"},
};
constexpr Adjustment kOrder[5] = {Adjustment::HueSaturation, Adjustment::ColorBalance, Adjustment::Blur,
                                  Adjustment::Sharpen, Adjustment::Noise};

const Info& infoOf(Adjustment kind) { return kAdjustments[static_cast<int>(kind)]; }

// Ejes del balance de color: los extremos y sus colores.
struct Axis {
    const char* left;
    const char* right;
    ImU32 leftColor;
    ImU32 rightColor;
};
constexpr Axis kAxes[3] = {
    {"Cian", "Rojo", IM_COL32(50, 200, 230, 255), IM_COL32(255, 69, 58, 255)},
    {"Magenta", "Verde", IM_COL32(230, 70, 200, 255), IM_COL32(48, 209, 88, 255)},
    {"Amarillo", "Azul", IM_COL32(255, 214, 10, 255), IM_COL32(10, 132, 255, 255)},
};
constexpr const char* kRanges[3] = {"Sombras", "Medios tonos", "Luces"};

// De 0..1 a -1..1.
float centered(float t) { return (std::clamp(t, 0.0f, 1.0f) - 0.5f) * 2.0f; }

// "+12", "−12" o "0", con `unit` detrás.
void signedText(char* out, size_t size, int value, const char* unit) {
    if (value > 0) {
        std::snprintf(out, size, "+%d%s", value, unit);
    } else if (value < 0) {
        std::snprintf(out, size, "%s%d%s", kMinus, -value, unit);
    } else {
        std::snprintf(out, size, "0%s", unit);
    }
}

// Píxeles, con un decimal (coma) por debajo de 10.
void pixelsText(char* out, size_t size, float pixels) {
    if (pixels < 9.95f) {
        const int tenths = static_cast<int>(std::lround(pixels * 10.0f));
        std::snprintf(out, size, "%d,%d px", tenths / 10, tenths % 10);
    } else {
        std::snprintf(out, size, "%d px", static_cast<int>(std::lround(pixels)));
    }
}

void percentText(char* out, size_t size, float t) {
    std::snprintf(out, size, "%d %%", static_cast<int>(std::lround(std::clamp(t, 0.0f, 1.0f) * 100.0f)));
}

// Botón con un icono. `holds`: se resalta mientras se mantiene pulsado (comparar).
Press iconButton(ImDrawList* dl, const char* id, const ImRect& r, const char* glyph, bool enabled, bool holds = false) {
    const ImGuiID gid = ImGui::GetID(id);
    const Press press = ui::pressable(gid, r, enabled);
    dl->AddRectFilled(r.Min, r.Max, ui::withAlpha(th::kControl, enabled ? 1.0f : 0.5f), pt(th::kControlRadius));
    const float lit = ui::anim::follow(gid + 3u, holds && press.held ? 1.0f : 0.0f, 24.0f);
    if (lit > 0.002f) {
        dl->AddRectFilled(r.Min, r.Max, ui::withAlpha(th::kAccentSoft, lit * 2.0f), pt(th::kControlRadius));
    }
    pressFeedback(dl, gid, r, press, pt(th::kControlRadius));
    ImU32 color = ui::mix(th::kLabel, th::kAccentText, lit);
    if (!enabled) {
        color = ui::withAlpha(color, 0.32f);
    }
    ui::icon(dl, glyph, r.GetCenter(), 18.0f, color);
    return press;
}

} // namespace

// -----------------------------------------------------------------------------
// Empezar y terminar
// -----------------------------------------------------------------------------

void Ui::startAdjust(Canvas& canvas, Adjustment kind) {
    closePanels();
    m_eyedropperArmed = false;
    // La herramienta a la que se vuelve al terminar. Salir de Transformar la aplica.
    CanvasTool back = m_canvasTool;
    if (back == CanvasTool::Adjust) {
        back = m_toolBeforeAdjust;
    } else if (back == CanvasTool::Transform) {
        setCanvasTool(canvas, m_toolBeforeTransform);
        back = m_canvasTool;
    } else if (back == CanvasTool::Select) {
        toolCancel(canvas);
    }
    m_adjustDrag = {};
    m_adjust = {};
    m_adjustKind = kind;
    // Si había otro ajuste a medias, beginAdjust lo aplica antes.
    const Canvas::Edit edit = canvas.beginAdjust(kind, adjustParams(canvas));
    if (edit != Canvas::Edit::Done) {
        m_canvasTool = back;
        switch (edit) {
        case Canvas::Edit::Hidden:
            notify("La capa activa está oculta", Notice::Warning);
            break;
        case Canvas::Edit::NoMemory:
            notify("No hay memoria suficiente", Notice::Error);
            break;
        default:
            break;
        }
        return;
    }
    m_toolBeforeAdjust = back;
    m_canvasTool = CanvasTool::Adjust;
    if (adjustMain() && !m_adjustHint) {
        m_adjustHint = true;
        notify("También puedes deslizar a los lados sobre el lienzo", Notice::Info, 4000);
    }
}

void Ui::finishAdjust(Canvas& canvas, bool apply) {
    m_adjustDrag = {};
    canvas.endAdjust(apply);
    if (m_canvasTool == CanvasTool::Adjust) {
        m_canvasTool = m_toolBeforeAdjust;
    }
}

AdjustParams Ui::adjustParams(const Canvas& canvas) const {
    const AdjustSliders& s = m_adjust;
    AdjustParams params;
    params.hue = centered(s.hsb[0]);
    params.saturation = centered(s.hsb[1]);
    params.brightness = centered(s.hsb[2]);
    for (int range = 0; range < 3; ++range) {
        for (int axis = 0; axis < 3; ++axis) {
            params.balance[range][axis] = centered(s.balance[range][axis]);
        }
    }
    // El desenfoque y el tamaño del grano crecen despacio al principio: los valores
    // pequeños, los más usados, se ajustan con precisión.
    params.blur = canvas.maxBlur() * s.blur * s.blur;
    params.sharpen = s.sharpen;
    params.noise = s.noise;
    params.noiseSize = 1.0f + 7.0f * s.noiseSize * s.noiseSize;
    return params;
}

void Ui::updateAdjust(Canvas& canvas) {
    if (canvas.adjusting() && !canvas.setAdjust(adjustParams(canvas))) {
        notify("No hay memoria suficiente para desenfocar tanto", Notice::Error);
    }
}

int Ui::adjustSliders(const Canvas& canvas, AdjustSlider out[3]) {
    const AdjustParams params = adjustParams(canvas);
    ui::SliderStyle signedStyle;
    signedStyle.origin = 0.5f;
    signedStyle.snap = true;
    ui::SliderStyle zeroStyle;
    zeroStyle.snap = true;
    switch (m_adjustKind) {
    case Adjustment::HueSaturation: {
        const char* names[3] = {"Tono", "Saturación", "Brillo"};
        for (int i = 0; i < 3; ++i) {
            out[i].t = &m_adjust.hsb[i];
            out[i].text = names[i];
            out[i].style = signedStyle;
        }
        signedText(out[0].value, sizeof(out[0].value), static_cast<int>(std::lround(params.hue * 180.0f)), "°");
        signedText(out[1].value, sizeof(out[1].value), static_cast<int>(std::lround(params.saturation * 100.0f)), " %");
        signedText(out[2].value, sizeof(out[2].value), static_cast<int>(std::lround(params.brightness * 100.0f)), " %");
        return 3;
    }
    case Adjustment::ColorBalance: {
        const int range = std::clamp(m_balanceRange, 0, 2);
        for (int axis = 0; axis < 3; ++axis) {
            AdjustSlider& slider = out[axis];
            slider.t = &m_adjust.balance[range][axis];
            slider.text = kAxes[axis].right;
            slider.style = signedStyle;
            slider.style.leftText = kAxes[axis].left;
            slider.style.rightText = kAxes[axis].right;
            slider.style.leftColor = kAxes[axis].leftColor;
            slider.style.rightColor = kAxes[axis].rightColor;
            signedText(slider.value, sizeof(slider.value),
                       static_cast<int>(std::lround(params.balance[range][axis] * 100.0f)), "");
        }
        return 3;
    }
    case Adjustment::Blur:
        out[0].t = &m_adjust.blur;
        out[0].text = "Radio";
        out[0].style = zeroStyle;
        pixelsText(out[0].value, sizeof(out[0].value), m_adjust.blur > 0.0f ? params.blur : 0.0f);
        return 1;
    case Adjustment::Sharpen:
        out[0].t = &m_adjust.sharpen;
        out[0].text = "Cantidad";
        out[0].style = zeroStyle;
        percentText(out[0].value, sizeof(out[0].value), m_adjust.sharpen);
        return 1;
    case Adjustment::Noise:
        out[0].t = &m_adjust.noise;
        out[0].text = "Cantidad";
        out[0].style = zeroStyle;
        percentText(out[0].value, sizeof(out[0].value), m_adjust.noise);
        out[1].t = &m_adjust.noiseSize;
        out[1].text = "Tamaño del grano";
        out[1].style = zeroStyle;
        pixelsText(out[1].value, sizeof(out[1].value), params.noiseSize);
        return 2;
    }
    return 0;
}

float* Ui::adjustMain() {
    switch (m_adjustKind) {
    case Adjustment::Blur:
        return &m_adjust.blur;
    case Adjustment::Sharpen:
        return &m_adjust.sharpen;
    case Adjustment::Noise:
        return &m_adjust.noise;
    default:
        return nullptr;
    }
}

// -----------------------------------------------------------------------------
// Deslizar en el lienzo
// -----------------------------------------------------------------------------

void Ui::adjustPress(ImVec2 position) {
    m_adjustDrag = {};
    if (float* main = adjustMain()) {
        m_adjustDrag.pressed = true;
        m_adjustDrag.startX = position.x;
        m_adjustDrag.startT = *main;
    }
}

void Ui::adjustDrag(Canvas& canvas, ImVec2 position) {
    float* main = adjustMain();
    if (!m_adjustDrag.pressed || !main) {
        return;
    }
    if (!m_adjustDrag.active) {
        if (std::fabs(position.x - m_adjustDrag.startX) < pt(kDragSlop)) {
            return;
        }
        // Desde aquí, sin saltar lo que se movió para empezar.
        m_adjustDrag.active = true;
        m_adjustDrag.startX = position.x;
        // El valor se muestra donde salen los avisos.
        m_toast.until = std::min(m_toast.until, SDL_GetTicks());
    }
    const float t = std::clamp(m_adjustDrag.startT + (position.x - m_adjustDrag.startX) / pt(kDragSpan), 0.0f, 1.0f);
    if (t != *main) {
        *main = t;
        updateAdjust(canvas);
    }
}

void Ui::adjustRelease() { m_adjustDrag = {}; }

void Ui::adjustCancel(Canvas& canvas) {
    float* main = adjustMain();
    if (m_adjustDrag.active && main && *main != m_adjustDrag.startT) {
        *main = m_adjustDrag.startT;
        updateAdjust(canvas);
    }
    m_adjustDrag = {};
}

void Ui::adjustPill(const Canvas& canvas, const char** glyph, const char** title, char* value, size_t size, float* t) {
    const Info& info = infoOf(m_adjustKind);
    *glyph = info.glyph;
    *title = info.title;
    AdjustSlider sliders[3];
    adjustSliders(canvas, sliders);
    *t = sliders[0].t ? *sliders[0].t : 0.0f;
    std::snprintf(value, size, "%s", sliders[0].value);
}

// -----------------------------------------------------------------------------
// Lista de ajustes
// -----------------------------------------------------------------------------

void Ui::adjustPanel(Canvas& canvas) {
    const Layout& L = m_layout;
    const float row = pt(60.0f);
    const float gap = pt(4.0f);
    const float content = pt(th::kHeaderHeight + 8.0f) + row * 5.0f + gap * 4.0f + pt(12.0f);
    const bool anchored = m_adjustButton.GetWidth() > 0.0f && !L.narrow;
    const float anchorX = anchored ? m_adjustButton.GetCenter().x : L.leftBar.GetCenter().x;
    const float x = anchored ? m_adjustButton.Min.x - pt(12.0f) : L.leftBar.Min.x;
    PanelFrame f;
    if (!beginPanel(f, Panel::Adjust, "##panel-adjust", x, pt(340.0f), content, anchorX, true)) {
        return;
    }
    ImDrawList* dl = f.dl;
    float y = f.content.Min.y - f.scroll;
    const float cy = ui::parts::panelHeader(dl, f.content, y, "Ajustes");
    // A qué se aplica: la capa activa (y solo lo seleccionado, si hay selección).
    {
        const std::string& name = canvas.layers().active().name;
        char target[96];
        std::snprintf(target, sizeof(target), canvas.hasSelection() ? "%s · selección" : "%s", name.c_str());
        const float titleRight = f.content.Min.x + pt(16.0f) +
                                 ui::measure(Weight::SemiBold, th::kPanelTitle, "Ajustes").x + pt(16.0f);
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(f.content.Max.x - pt(16.0f), cy), Align::Right,
                  th::kSecondaryLabel, target, std::max(pt(40.0f), f.content.Max.x - pt(16.0f) - titleRight));
    }
    y += pt(th::kHeaderHeight + 8.0f);

    const bool adjusting = m_canvasTool == CanvasTool::Adjust && canvas.adjusting();
    int picked = -1;
    for (int i = 0; i < 5; ++i) {
        const Adjustment kind = kOrder[i];
        const Info& info = infoOf(kind);
        const ImRect r(ImVec2(f.content.Min.x + pt(8.0f), y), ImVec2(f.content.Max.x - pt(8.0f), y + row));
        const bool active = adjusting && m_adjustKind == kind;
        ImGui::PushID(i);
        const ui::Choice c = ui::choice("##adjustment", r, active, f.open, false);
        ImGui::PopID();
        const float rowY = r.GetCenter().y;
        const ImRect badge(ImVec2(r.Min.x + pt(10.0f), rowY - pt(18.0f)), ImVec2(r.Min.x + pt(46.0f), rowY + pt(18.0f)));
        dl->AddRectFilled(badge.Min, badge.Max, th::kControl, pt(9.0f));
        const ImU32 color = ui::mix(th::kLabel, th::kAccentText, c.on);
        ui::icon(dl, info.glyph, badge.GetCenter(), 20.0f, color);
        const float textX = badge.Max.x + pt(12.0f);
        const float maxWidth = r.Max.x - pt(40.0f) - textX;
        ui::label(dl, Weight::SemiBold, th::kBody, ImVec2(textX, rowY - pt(9.0f)), Align::Left, color, info.title,
                  maxWidth);
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(textX, rowY + pt(11.0f)), Align::Left, th::kSecondaryLabel,
                  info.detail, maxWidth);
        if (active) {
            ui::icon(dl, icon::kCheck, ImVec2(r.Max.x - pt(22.0f), rowY), 18.0f, th::kAccentText);
        }
        if (c.press.clicked && f.open) {
            picked = i;
        }
        y += row + gap;
    }
    endPanel(f);
    if (picked >= 0) {
        if (adjusting && m_adjustKind == kOrder[picked]) {
            closePanels();   // ya es este: se sigue ajustando
        } else {
            startAdjust(canvas, kOrder[picked]);
        }
    }
}

// -----------------------------------------------------------------------------
// Barra de abajo
// -----------------------------------------------------------------------------

void Ui::adjustDock(Canvas& canvas) {
    const Layout& L = m_layout;
    const bool open = m_canvasTool == CanvasTool::Adjust && canvas.adjusting();
    const ImGuiID id = ImHashStr("##dock-adjust");
    const float presence = ui::anim::followFrom(id, 0.0f, open ? 1.0f : 0.0f, open ? 18.0f : 24.0f);
    if (!open && presence <= 0.002f) {
        return;
    }
    // Mientras se cierra sigue mostrando el último ajuste.
    AdjustSlider sliders[3];
    const int count = adjustSliders(canvas, sliders);
    const bool balance = m_adjustKind == Adjustment::ColorBalance;

    // Si caben (tableta, teléfono en horizontal), los deslizadores van en una fila y cancelar
    // y aplicar, en la cabecera. Si no (teléfono en vertical, tableta pequeña en vertical),
    // un deslizador por fila y los botones abajo. La barra solo se estrecha para no tapar la
    // barra lateral cuando esta baja hasta su altura.
    const float pad = pt(kPad);
    const float full = L.right - L.left;
    float head = pad * 2.0f + pt(kHeaderHeight) + pt(kRowRule);
    if (balance) {
        head += pt(kRangeHeight) + pt(kGap);
    }
    auto widthFor = [&](float height) {
        if (L.narrow) {
            return full;
        }
        const bool clear = L.sidebar.Max.y + pt(10.0f) <= L.bottom - height;
        return std::min(pt(kMaxWidth), clear ? full : full - (L.sidebar.GetWidth() + pt(10.0f)) * 2.0f);
    };
    const float n = static_cast<float>(count);
    float height = head + pt(kSliderHeight);
    float width = widthFor(height);
    const float rowSlider = (width - pad * 2.0f - pt(kGap) * (n - 1.0f)) / n;
    const bool wide = !L.narrow && rowSlider >= pt(balance ? kMinBalanceSlider : kMinSlider);
    if (!wide) {
        height = head + pt(kSliderHeight) * n + pt(kGap) * (n - 1.0f) + pt(kRowRule) + pt(kButtonHeight);
        width = widthFor(height);
    }
    const float x = std::round((L.left + L.right - width) * 0.5f);
    const ImRect rect(x, L.bottom - height, x + width, L.bottom);
    if (open) {
        m_dock = rect;
    }

    ui::beginSurface("##dock-adjust", rect, open, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    const float radius = pt(th::kPanelRadius);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(30.0f), pt(8.0f), 0.26f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kPanelTint);
    ImGui::PushID(static_cast<int>(m_adjustKind));

    const float left = rect.Min.x + pad;
    const float right = rect.Max.x - pad;
    float y = rect.Min.y + pad;

    // Cabecera: el ajuste y a qué se aplica; comparar, restablecer y (en una fila) cancelar
    // y aplicar.
    const Info& info = infoOf(m_adjustKind);
    const float cy = y + pt(kHeaderHeight) * 0.5f;
    float buttonsLeft = right;
    bool cancelIt = false;
    bool applyIt = false;
    if (wide) {
        const ImRect apply(ImVec2(right - pt(kTextButton), cy - pt(kButtonHeight) * 0.5f),
                           ImVec2(right, cy + pt(kButtonHeight) * 0.5f));
        const ImRect cancel(ImVec2(apply.Min.x - pt(kGap) - pt(kTextButton), apply.Min.y),
                            ImVec2(apply.Min.x - pt(kGap), apply.Max.y));
        cancelIt = ui::button("##cancel", cancel, "Cancelar", ui::ButtonStyle::Secondary, open);
        applyIt = ui::button("##apply", apply, "Aplicar", ui::ButtonStyle::Primary, open);
        const float divider = std::round(cancel.Min.x - pt(13.0f) * 0.5f);
        dl->AddLine(ImVec2(divider, cy - pt(13.0f)), ImVec2(divider, cy + pt(13.0f)), th::kRule, ui::hairline());
        buttonsLeft = cancel.Min.x - pt(13.0f);
    }
    const bool neutral = canvas.adjustParams().neutral(m_adjustKind);
    const ImRect reset(ImVec2(buttonsLeft - pt(kIconButton), cy - pt(kIconButton) * 0.5f),
                       ImVec2(buttonsLeft, cy + pt(kIconButton) * 0.5f));
    const ImRect compare(ImVec2(reset.Min.x - pt(kGap) - pt(kIconButton), reset.Min.y),
                         ImVec2(reset.Min.x - pt(kGap), reset.Max.y));
    const Press comparePress = iconButton(dl, "##compare", compare, icon::kEye, open && !neutral, true);
    const bool resetIt = iconButton(dl, "##reset", reset, icon::kRotateCcw, open && !neutral).clicked;

    const ImRect badge(ImVec2(left + pt(4.0f), cy - pt(16.0f)), ImVec2(left + pt(36.0f), cy + pt(16.0f)));
    dl->AddRectFilled(badge.Min, badge.Max, th::kControl, pt(9.0f));
    ui::icon(dl, info.glyph, badge.GetCenter(), 18.0f, th::kAccentText);
    const float textX = badge.Max.x + pt(10.0f);
    const float textWidth = std::max(pt(40.0f), compare.Min.x - pt(10.0f) - textX);
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(textX, cy - pt(8.0f)), Align::Left, th::kLabel, info.title,
              textWidth);
    {
        char target[96];
        std::snprintf(target, sizeof(target), canvas.hasSelection() ? "%s · selección" : "%s",
                      canvas.layers().active().name.c_str());
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(textX, cy + pt(10.0f)), Align::Left, th::kSecondaryLabel,
                  target, textWidth);
    }
    y += pt(kHeaderHeight);
    ui::separator(dl, rect.Min.x + pt(12.0f), rect.Max.x - pt(12.0f), std::round(y + pt(kRowRule) * 0.5f), th::kRule);
    y += pt(kRowRule);

    // Balance de color: el rango que cambian los deslizadores.
    if (balance) {
        const float segWidth = wide ? std::min(right - left, pt(390.0f)) : right - left;
        const float segLeft = std::round((left + right - segWidth) * 0.5f);
        const ImRect strip(ImVec2(segLeft, y), ImVec2(segLeft + segWidth, y + pt(kRangeHeight)));
        dl->AddRectFilled(strip.Min, strip.Max, th::kControlSoft, pt(th::kControlRadius));
        const float cell = segWidth / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const ImRect r(ImVec2(strip.Min.x + cell * static_cast<float>(i), strip.Min.y),
                           ImVec2(strip.Min.x + cell * static_cast<float>(i + 1), strip.Max.y));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##range", r, m_balanceRange == i, open, false);
            ImGui::PopID();
            ui::label(dl, Weight::SemiBold, th::kFootnote, r.GetCenter(), Align::Center,
                      ui::mix(th::kSecondaryLabel, th::kAccentText, c.on), kRanges[i], cell - pt(8.0f));
            if (c.press.clicked && open) {
                m_balanceRange = i;
            }
        }
        y += pt(kRangeHeight) + pt(kGap);
    }

    // Deslizadores.
    bool changed = false;
    if (wide) {
        const float w = (right - left - pt(kGap) * static_cast<float>(count - 1)) / static_cast<float>(count);
        for (int i = 0; i < count; ++i) {
            const float x0 = left + (w + pt(kGap)) * static_cast<float>(i);
            const ImRect r(ImVec2(x0, y), ImVec2(x0 + w, y + pt(kSliderHeight)));
            ImGui::PushID(i + 10 * m_balanceRange);
            changed |= ui::paramSlider("##slider", r, sliders[i].t, sliders[i].text, sliders[i].value, sliders[i].style,
                                       open);
            ImGui::PopID();
        }
    } else {
        for (int i = 0; i < count; ++i) {
            const ImRect r(ImVec2(left, y), ImVec2(right, y + pt(kSliderHeight)));
            ImGui::PushID(i + 10 * m_balanceRange);
            changed |= ui::paramSlider("##slider", r, sliders[i].t, sliders[i].text, sliders[i].value, sliders[i].style,
                                       open);
            ImGui::PopID();
            y += pt(kSliderHeight) + (i + 1 < count ? pt(kGap) : 0.0f);
        }
        ui::separator(dl, rect.Min.x + pt(12.0f), rect.Max.x - pt(12.0f), std::round(y + pt(kRowRule) * 0.5f),
                      th::kRule);
        y += pt(kRowRule);
        const float half = (right - left - pt(kGap)) * 0.5f;
        const ImRect cancel(ImVec2(left, y), ImVec2(left + half, y + pt(kButtonHeight)));
        const ImRect apply(ImVec2(right - half, y), ImVec2(right, y + pt(kButtonHeight)));
        cancelIt = ui::button("##cancel", cancel, "Cancelar", ui::ButtonStyle::Secondary, open);
        applyIt = ui::button("##apply", apply, "Aplicar", ui::ButtonStyle::Primary, open);
    }

    ImGui::PopID();
    ui::transform(mark, ImVec2(rect.GetCenter().x, rect.Max.y), 1.0f, ImVec2(0.0f, pt(18.0f) * (1.0f - presence)),
                  std::min(1.0f, presence * 1.4f));
    ui::endSurface();

    if (!open) {
        return;
    }
    // Comparar: mientras se mantiene pulsado, la capa se ve como era.
    canvas.showAdjustOriginal(comparePress.held);
    if (resetIt) {
        m_adjust = {};
        changed = true;
    }
    if (changed) {
        updateAdjust(canvas);
    }
    if (cancelIt || applyIt) {
        finishAdjust(canvas, applyIt);
    }
}
