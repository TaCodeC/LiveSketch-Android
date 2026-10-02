// Diálogos: confirmaciones (eliminar y renombrar capa, salir), con la tarjeta de lienzo
// nuevo (UiCanvas.cpp) por encima de lo que haya, y la pantalla de inicio. Las
// confirmaciones son tarjetas alineadas a la izquierda: un icono, el título, la explicación
// y botones abajo a la derecha.
#include "UI/Ui.h"

#include "Canvas/Canvas.h"
#include "UI/Anim.h"
#include "UI/Icons.h"

#include <SDL3/SDL_platform_defines.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;

namespace {

// Opacidad del oscurecido de detrás de los diálogos.
constexpr float kDimAlpha = static_cast<float>((th::kDim >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;

std::string trimmed(const char* text) {
    std::string s(text);
    const size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

// Campo de texto quieto (mientras la tarjeta desaparece), con el aspecto de ui::textField.
void staticField(ImDrawList* dl, const ImRect& rect, const char* text) {
    const float radius = pt(8.0f);
    dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(0, 0, 0, 64), radius);
    ui::outline(dl, rect, radius, IM_COL32(255, 255, 255, 31), pt(1.0f));
    ui::label(dl, Weight::Regular, th::kBody, ImVec2(rect.Min.x + pt(10.0f), rect.GetCenter().y), Align::Left,
              th::kLabel, text, rect.GetWidth() - pt(20.0f));
}

// Mancha de luz difusa (fondo de la pantalla de inicio): degradado radial con muchos
// anillos para que no se vean escalones.
void glow(ImDrawList* dl, ImVec2 center, float radius, ImU32 color, float alpha) {
    constexpr int kRings = 32;
    constexpr int kSegments = 72;
    const ImU32 opaque = color | IM_COL32_A_MASK;
    dl->PrimReserve(kSegments * 3 + (kRings - 1) * kSegments * 6, 1 + kRings * kSegments);
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    const unsigned base = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(center, uv, ui::withAlpha(opaque, alpha));
    for (int ring = 1; ring <= kRings; ++ring) {
        const float t = static_cast<float>(ring) / kRings;
        const float falloff = std::exp(-3.0f * t * t) * (1.0f - t * t);
        for (int i = 0; i < kSegments; ++i) {
            const float angle = 2.0f * IM_PI * static_cast<float>(i) / kSegments;
            const ImVec2 p(center.x + std::cos(angle) * radius * t, center.y + std::sin(angle) * radius * t);
            dl->PrimWriteVtx(p, uv, ui::withAlpha(opaque, alpha * falloff));
        }
    }
    for (int i = 0; i < kSegments; ++i) {
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % kSegments));
    }
    for (int ring = 0; ring + 1 < kRings; ++ring) {
        const unsigned inner = base + 1 + static_cast<unsigned>(ring * kSegments);
        const unsigned outer = inner + kSegments;
        for (int i = 0; i < kSegments; ++i) {
            const unsigned j = static_cast<unsigned>((i + 1) % kSegments);
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(inner + i));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(outer + i));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(outer + j));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(inner + i));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(outer + j));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(inner + j));
        }
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Abrir y cerrar
// -----------------------------------------------------------------------------

void Ui::openDialog(Dialog dialog, const Canvas* canvas) {
    m_dialog = dialog;
    m_dialogShown = dialog;
    m_dialogFocus = true;
    m_layerMenu = false;
    m_hexEditing = false;
    switch (dialog) {
    case Dialog::DeleteLayer:
    case Dialog::RenameLayer:
        if (canvas) {
            const Layer& layer = canvas->layers().active();
            m_dialogLayerId = layer.id;
            if (dialog == Dialog::DeleteLayer) {
                m_dialogTitle = "¿Eliminar «" + layer.name + "»?";
            } else {
                m_dialogTitle = "Renombrar capa";
                std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", layer.name.c_str());
            }
        }
        break;
    case Dialog::Exit:
        m_dialogTitle = "¿Salir de LiveSketch?";
        break;
    case Dialog::NewCanvas:
        // Empieza sin nombre; el tamaño, el fondo y la categoría son los de la última vez.
        closePanels();
        m_canvasForm.commit();
        m_canvasForm.name[0] = '\0';
        m_keypad = false;
        break;
    case Dialog::None:
        break;
    }
}

void Ui::askExit() {
#ifndef SDL_PLATFORM_EMSCRIPTEN
    // En el navegador no se sale de la app: se cierra la pestaña.
    if (m_dialog == Dialog::None) {
        openDialog(Dialog::Exit, nullptr);
    }
#endif
}

// -----------------------------------------------------------------------------
// Confirmaciones
// -----------------------------------------------------------------------------

void Ui::drawDialogs(Canvas* canvas, UiRequests& requests) {
    const bool open = m_dialog != Dialog::None;
    if (open) {
        m_dialogShown = m_dialog;
    }
    const float p = ui::anim::followFrom(ImHashStr("##dialog"), 0.0f, open ? 1.0f : 0.0f, open ? 18.0f : 24.0f);
    if (!open && p <= 0.002f) {
        m_dialogShown = Dialog::None;
        return;
    }
    if (m_dialogShown == Dialog::None) {
        return;
    }
    const Layout& L = m_layout;

    // Oscurece lo de detrás y se queda con los toques de fuera. Tocar fuera cierra la
    // tarjeta de lienzo nuevo; una confirmación espera a que se elija un botón.
    const ImRect screen(ImVec2(0.0f, 0.0f), L.display);
    ui::beginSurface("##dim", screen, open, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(screen.Min, screen.Max, ui::withAlpha(th::kDim, p));
    if (open) {
        const ImGuiID outside = ImGui::GetID("##outside");
        ImGui::ItemAdd(screen, outside);
        bool hovered = false;
        bool held = false;
        if (ImGui::ButtonBehavior(screen, outside, &hovered, &held, ImGuiButtonFlags_NoNavFocus) &&
            m_dialog == Dialog::NewCanvas) {
            m_dialog = Dialog::None;
        }
    }
    ui::endSurface();

    if (m_dialogShown == Dialog::NewCanvas) {
        newCanvasCard(true, p, open, requests);
        return;
    }

    // Cada confirmación lleva su icono, su explicación y el botón que la acepta.
    const bool rename = m_dialogShown == Dialog::RenameLayer;
    const char* glyph = icon::kPencil;
    ImU32 tone = th::kAccent;
    const char* message = nullptr;
    const char* confirmLabel = "Aceptar";
    ui::ButtonStyle confirmStyle = ui::ButtonStyle::Primary;
    if (m_dialogShown == Dialog::DeleteLayer) {
        glyph = icon::kTrash;
        tone = th::kRed;
        message = "La capa y su dibujo desaparecen. Puedes recuperarla con Deshacer.";
        confirmLabel = "Eliminar";
        confirmStyle = ui::ButtonStyle::Destructive;
    } else if (m_dialogShown == Dialog::Exit) {
        glyph = icon::kLogOut;
        tone = th::kRed;
        message = "Lo que no hayas guardado como PNG se perderá.";
        confirmLabel = "Salir";
        confirmStyle = ui::ButtonStyle::Destructive;
    }

    const float width = std::min(pt(340.0f), L.display.x - L.margin * 2.0f);
    const float pad = pt(20.0f);
    const float textWidth = width - pad * 2.0f;
    const float badge = pt(40.0f);
    const float titleHeight = ui::paragraph(nullptr, Weight::SemiBold, th::kHeadline, ImVec2(0.0f, 0.0f), textWidth,
                                            Align::Left, 0, m_dialogTitle.c_str(), 1.25f);
    const float messageHeight = message ? ui::paragraph(nullptr, Weight::Regular, th::kCallout, ImVec2(0.0f, 0.0f),
                                                        textWidth, Align::Left, 0, message, 1.45f)
                                        : 0.0f;
    const float fieldHeight = pt(40.0f);
    const float buttonHeight = pt(42.0f);
    const float height = pad + badge + pt(14.0f) + titleHeight + (message ? pt(6.0f) + messageHeight : 0.0f) +
                         (rename ? pt(14.0f) + fieldHeight : 0.0f) + pt(20.0f) + buttonHeight + pad;
    // Renombrar va más arriba: en un teléfono el teclado tapa la mitad de abajo.
    const float centerY = L.display.y * (rename ? 0.36f : 0.5f);
    const float x = std::round((L.display.x - width) * 0.5f);
    const float y = std::round(std::clamp(centerY - height * 0.5f, L.top, std::max(L.top, L.bottom - height)));
    const ImRect rect(x, y, x + width, y + height);
    const float radius = pt(th::kDialogRadius);

    ui::beginSurface("##alert", rect, open, true);
    dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(60.0f), pt(24.0f), 0.45f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kDialogTint, 0, kDimAlpha * p);

    const float left = rect.Min.x + pad;
    float top = rect.Min.y + pad;
    ui::iconBadge(dl, ImRect(ImVec2(left, top), ImVec2(left + badge, top + badge)), tone, glyph, 20.0f);
    top += badge + pt(14.0f);
    ui::paragraph(dl, Weight::SemiBold, th::kHeadline, ImVec2(left, top), textWidth, Align::Left, th::kLabel,
                  m_dialogTitle.c_str(), 1.25f);
    top += titleHeight;
    if (message) {
        top += pt(6.0f);
        ui::paragraph(dl, Weight::Regular, th::kCallout, ImVec2(left, top), textWidth, Align::Left, th::kSecondaryLabel,
                      message, 1.45f);
        top += messageHeight;
    }
    bool enter = false;
    if (rename) {
        top += pt(14.0f);
        const ImRect field(ImVec2(left, top), ImVec2(left + textWidth, top + fieldHeight));
        if (open) {
            enter = ui::textField("##rename", field, m_renameBuffer, sizeof(m_renameBuffer), m_dialogFocus,
                                  "Nombre de la capa");
            m_dialogFocus = false;
        } else {
            staticField(dl, field, m_renameBuffer);
        }
    }

    // Botones abajo a la derecha, la acción al final. Si no caben a su ancho, se reparten
    // la fila.
    const std::string newName = rename ? trimmed(m_renameBuffer) : std::string();
    const bool canConfirm = !rename || !newName.empty();
    const float gap = pt(10.0f);
    auto buttonWidth = [](const char* text) {
        return std::max(pt(104.0f), ui::measure(Weight::SemiBold, th::kSubhead, text).x + pt(32.0f));
    };
    float cancelWidth = buttonWidth("Cancelar");
    float confirmWidth = buttonWidth(confirmLabel);
    if (cancelWidth + gap + confirmWidth > textWidth) {
        cancelWidth = (textWidth - gap) * 0.5f;
        confirmWidth = cancelWidth;
    }
    const float buttonsTop = rect.Max.y - pad - buttonHeight;
    const float right = rect.Max.x - pad;
    const ImRect confirmRect(ImVec2(right - confirmWidth, buttonsTop), ImVec2(right, buttonsTop + buttonHeight));
    const ImRect cancelRect(ImVec2(confirmRect.Min.x - gap - cancelWidth, buttonsTop),
                            ImVec2(confirmRect.Min.x - gap, buttonsTop + buttonHeight));
    const bool cancel = ui::button("##cancel", cancelRect, "Cancelar", ui::ButtonStyle::Secondary);
    const bool confirm = ui::button("##confirm", confirmRect, confirmLabel, confirmStyle, canConfirm);

    // Aparece subiendo un poco; al cerrarse baja mientras se desvanece.
    const float e = ui::anim::easeOutCubic(std::clamp(p, 0.0f, 1.0f));
    ui::transform(mark, rect.GetCenter(), 0.98f + 0.02f * e, ImVec2(0.0f, pt(12.0f) * (1.0f - e)), p);
    ui::endSurface();

    if (!open) {
        return;
    }
    if (confirm || (enter && canConfirm)) {
        LayerStack* layers = canvas ? &canvas->layers() : nullptr;
        const int index = layers ? layers->indexOf(m_dialogLayerId) : -1;
        switch (m_dialogShown) {
        case Dialog::DeleteLayer:
            if (index >= 0) {
                canvas->removeLayer(index);
            }
            break;
        case Dialog::RenameLayer:
            if (index >= 0) {
                canvas->renameLayer(index, newName);
            }
            break;
        case Dialog::Exit:
            requests.quit = true;
            break;
        default:
            break;
        }
        m_dialog = Dialog::None;
    } else if (cancel) {
        m_dialog = Dialog::None;
    }
}

// -----------------------------------------------------------------------------
// Pantalla de inicio
// -----------------------------------------------------------------------------

void Ui::startScreen(UiRequests& requests) {
    const Layout& L = m_layout;

    // Fondo: casi negro con dos luces muy suaves detrás de la tarjeta, del azul de los
    // controles y del rojo de la marca.
    const ImRect screen(ImVec2(0.0f, 0.0f), L.display);
    ui::beginSurface("##start-background", screen, false, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(screen.Min, screen.Max, IM_COL32(12, 12, 14, 255));
    const float extent = std::max(L.display.x, L.display.y);
    glow(dl, ImVec2(L.display.x * 0.34f, L.display.y * 0.2f), extent * 0.5f, th::kAccent, 0.2f);
    glow(dl, ImVec2(L.display.x * 0.72f, L.display.y * 0.9f), extent * 0.42f, th::kRed, 0.12f);
    ui::endSurface();

    // Escape o atrás dejan de escribir un número o un texto. Si no, atrás (Android) aquí no
    // pierde nada: sale sin preguntar.
    const bool back = ImGui::IsKeyPressed(ImGuiKey_AppBack, false);
    if (back || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (!cancelCanvasEdit() && back) {
            requests.quit = true;
        }
    }
    const float p = ui::anim::followFrom(ImHashStr("##start-card"), 0.0f, 1.0f, 9.0f);
    newCanvasCard(false, p, true, requests);
}
