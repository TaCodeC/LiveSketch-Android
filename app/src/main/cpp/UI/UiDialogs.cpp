// Diálogos: alertas al estilo de iOS (eliminar y renombrar capa, salir) y la tarjeta de
// lienzo nuevo, que también es la pantalla de inicio.
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

struct CanvasPreset {
    const char* name;
    int width;    // 0: el tamaño de la pantalla
    int height;
    ImU32 color;
};

constexpr CanvasPreset kCanvasPresets[] = {
    {"Pantalla completa", 0, 0, th::kAccent},
    {"HD 720p", 1280, 720, th::kIndigo},
    {"Full HD 1080p", 1920, 1080, th::kGreen},
    {"QHD 1440p", 2560, 1440, th::kOrange},
    {"4K UHD", 3840, 2160, IM_COL32(255, 55, 95, 255)},
    {"Pequeño", 640, 360, th::kGray},
};
constexpr int kCanvasPresetCount = static_cast<int>(sizeof(kCanvasPresets) / sizeof(kCanvasPresets[0]));

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

// Campo de texto quieto (mientras la alerta desaparece), con el aspecto de ui::textField.
void staticField(ImDrawList* dl, const ImRect& rect, const char* text) {
    dl->AddRectFilled(rect.Min, rect.Max, IM_COL32(0, 0, 0, 90), pt(9.0f));
    dl->AddRect(rect.Min, rect.Max, IM_COL32(255, 255, 255, 30), pt(9.0f), 0, ui::hairline());
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
        closePanels();
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
// Alertas
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
    // tarjeta de lienzo nuevo; las alertas, como en iOS, no.
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

    const bool rename = m_dialogShown == Dialog::RenameLayer;
    const char* message = nullptr;
    const char* confirmLabel = "Aceptar";
    bool destructive = false;
    if (m_dialogShown == Dialog::DeleteLayer) {
        message = "Podrás recuperarla con Deshacer.";
        confirmLabel = "Eliminar";
        destructive = true;
    } else if (m_dialogShown == Dialog::Exit) {
        message = "Lo que no hayas guardado como PNG se perderá.";
        confirmLabel = "Salir";
        destructive = true;
    }

    const float width = std::min(pt(290.0f), L.display.x - L.margin * 2.0f);
    const float pad = pt(20.0f);
    const float textWidth = width - pad * 2.0f;
    const float titleHeight = ui::paragraph(nullptr, Weight::SemiBold, th::kHeadline, ImVec2(0.0f, 0.0f), textWidth,
                                            Align::Center, 0, m_dialogTitle.c_str(), 1.25f);
    const float messageHeight = message ? ui::paragraph(nullptr, Weight::Regular, th::kFootnote, ImVec2(0.0f, 0.0f),
                                                        textWidth, Align::Center, 0, message)
                                        : 0.0f;
    const float fieldHeight = pt(36.0f);
    const float buttonsHeight = pt(46.0f);
    const float height = pad + titleHeight + (message ? pt(4.0f) + messageHeight : 0.0f) +
                         (rename ? pt(14.0f) + fieldHeight : 0.0f) + pt(18.0f) + buttonsHeight;
    // Renombrar va más arriba: en un teléfono el teclado tapa la mitad de abajo.
    const float centerY = L.display.y * (rename ? 0.36f : 0.5f);
    const float x = std::round((L.display.x - width) * 0.5f);
    const float y = std::round(std::clamp(centerY - height * 0.5f, L.top, std::max(L.top, L.bottom - height)));
    const ImRect rect(x, y, x + width, y + height);
    const float radius = pt(20.0f);

    ui::beginSurface("##alert", rect, open, true);
    dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(44.0f), pt(16.0f), 0.36f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kDialogTint, 0, kDimAlpha * p);

    float top = rect.Min.y + pad;
    ui::paragraph(dl, Weight::SemiBold, th::kHeadline, ImVec2(rect.Min.x + pad, top), textWidth, Align::Center,
                  th::kLabel, m_dialogTitle.c_str(), 1.25f);
    top += titleHeight;
    if (message) {
        top += pt(4.0f);
        ui::paragraph(dl, Weight::Regular, th::kFootnote, ImVec2(rect.Min.x + pad, top), textWidth, Align::Center,
                      IM_COL32(235, 235, 245, 200), message);
        top += messageHeight;
    }
    bool enter = false;
    if (rename) {
        top += pt(14.0f);
        const ImRect field(ImVec2(rect.Min.x + pad, top), ImVec2(rect.Max.x - pad, top + fieldHeight));
        if (open) {
            enter = ui::textField("##rename", field, m_renameBuffer, sizeof(m_renameBuffer), m_dialogFocus,
                                  "Nombre de la capa");
            m_dialogFocus = false;
        } else {
            staticField(dl, field, m_renameBuffer);
        }
    }

    // Botones: Cancelar a la izquierda, como en iOS.
    const float buttonsTop = rect.Max.y - buttonsHeight;
    const float middle = ui::snap(rect.GetCenter().x);
    ui::separator(dl, rect.Min.x, rect.Max.x, buttonsTop, IM_COL32(255, 255, 255, 36));
    dl->AddRectFilled(ImVec2(middle, buttonsTop), ImVec2(middle + ui::hairline(), rect.Max.y),
                      IM_COL32(255, 255, 255, 36));
    const ImRect cancelRect(ImVec2(rect.Min.x, buttonsTop), ImVec2(middle, rect.Max.y));
    const ImRect confirmRect(ImVec2(middle, buttonsTop), rect.Max);
    const std::string newName = rename ? trimmed(m_renameBuffer) : std::string();
    const bool canConfirm = !rename || !newName.empty();
    const Press cancel = ui::pressable("##cancel", cancelRect, open);
    const Press confirm = ui::pressable("##confirm", confirmRect, open && canConfirm);
    auto pressedFill = [&](const Press& press, const ImRect& r, ImDrawFlags corners) {
        if (press.held || press.hovered) {
            dl->AddRectFilled(r.Min, r.Max, press.held ? th::kPressed : th::kHover, radius, corners);
        }
    };
    pressedFill(cancel, cancelRect, ImDrawFlags_RoundCornersBottomLeft);
    pressedFill(confirm, confirmRect, ImDrawFlags_RoundCornersBottomRight);
    // La acción preferida va en negrita: Aceptar al renombrar; si no, Cancelar.
    ui::label(dl, rename ? Weight::Regular : Weight::SemiBold, th::kHeadline, cancelRect.GetCenter(), Align::Center,
              th::kAccent, "Cancelar");
    ImU32 confirmColor = destructive ? th::kRed : th::kAccent;
    if (!canConfirm) {
        confirmColor = ui::withAlpha(confirmColor, 0.35f);
    }
    ui::label(dl, destructive ? Weight::Regular : Weight::SemiBold, th::kHeadline, confirmRect.GetCenter(),
              Align::Center, confirmColor, confirmLabel);

    // Aparece creciendo un poco desde más grande; desaparece solo con un fundido.
    ui::transform(mark, rect.GetCenter(), open ? 1.1f - 0.1f * p : 1.0f, ImVec2(0.0f, 0.0f), p);
    ui::endSurface();

    if (!open) {
        return;
    }
    if (confirm.clicked || (enter && canConfirm)) {
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
    } else if (cancel.clicked) {
        m_dialog = Dialog::None;
    }
}

// -----------------------------------------------------------------------------
// Lienzo nuevo
// -----------------------------------------------------------------------------

void Ui::newCanvasCard(bool modal, float presence, bool interactive, UiRequests& requests) {
    const Layout& L = m_layout;
    const int maxSize = m_status.maxCanvasSize;
    struct Size {
        int width;
        int height;
        bool supported;
    };
    auto sizeOf = [&](int index) {
        const CanvasPreset& preset = kCanvasPresets[index];
        Size size{preset.width, preset.height, true};
        if (preset.width == 0) {
            size.width = std::max(1, m_status.screenWidth);
            size.height = std::max(1, m_status.screenHeight);
            if (maxSize > 0) {
                size.width = std::min(size.width, maxSize);
                size.height = std::min(size.height, maxSize);
            }
        } else {
            if (m_orientation == 1) {
                std::swap(size.width, size.height);
            }
            size.supported = maxSize <= 0 || std::max(size.width, size.height) <= maxSize;
        }
        return size;
    };
    m_preset = std::clamp(m_preset, 0, kCanvasPresetCount - 1);
    if (!sizeOf(m_preset).supported) {
        m_preset = 0;
    }

    const bool compact = L.compact;
    const float pad = pt(compact ? 20.0f : 24.0f);
    const float width = std::min(pt(468.0f), L.right - L.left);
    const float inner = width - pad * 2.0f;
    const char* description =
        modal ? "Elige el tamaño del lienzo nuevo. El dibujo actual se descartará: guárdalo antes como PNG si "
                "quieres conservarlo."
              : "Elige el tamaño. NDI y el PNG usan el lienzo entero a este tamaño, sin importar el zoom.";
    const float descriptionHeight =
        ui::paragraph(nullptr, Weight::Regular, th::kSubhead, ImVec2(0.0f, 0.0f), inner, Align::Left, 0, description,
                      1.35f);
    const float row = pt(58.0f);
    const float listHeight = row * static_cast<float>(kCanvasPresetCount);
    const float content = pad + pt(16.0f + 10.0f + 36.0f + 8.0f) + descriptionHeight + pt(18.0f) + listHeight +
                          pt(16.0f + 32.0f + 20.0f + 50.0f) + pad;
    const float height = std::min(content, L.bottom - L.top);
    const float x = std::round((L.left + L.right - width) * 0.5f);
    const float y = std::round((L.top + L.bottom - height) * 0.5f);
    const ImRect rect(x, y, x + width, y + height);
    const float radius = pt(30.0f);

    ui::beginSurface(modal ? "##new-canvas" : "##start-card", rect, interactive, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(70.0f), pt(26.0f), 0.5f);
    ui::popUnclipped(dl);
    // Desde Acciones va sobre el oscurecido: más opaca, como una hoja de iOS.
    if (modal) {
        ui::glass(dl, rect, radius, IM_COL32(34, 34, 38, 226), 0, kDimAlpha * presence);
    } else {
        ui::glass(dl, rect, radius, IM_COL32(40, 40, 44, 184));
    }

    // Si no cabe, el contenido se desplaza (por debajo de las esquinas redondeadas no).
    const bool scrolls = content > height + 0.5f;
    const ImRect view(ImVec2(rect.Min.x, rect.Min.y + pt(8.0f)), ImVec2(rect.Max.x, rect.Max.y - pt(8.0f)));
    const float scroll = scrolls ? ui::beginScroll("##card", view, content - pt(16.0f)) : 0.0f;
    const float left = rect.Min.x + pad;
    const float right = rect.Max.x - pad;
    float top = rect.Min.y + pad - scroll;

    // Marca, título y explicación.
    dl->AddCircleFilled(ImVec2(left + pt(4.0f), top + pt(8.0f)), pt(4.0f), th::kLive, 0);
    ui::tracked(dl, Weight::Bold, th::kFootnote, ImVec2(left + pt(14.0f), top + pt(8.0f)), th::kSecondaryLabel,
                "LIVESKETCH", 0.08f);
    top += pt(16.0f + 10.0f);
    ui::label(dl, Weight::Bold, 30.0f, ImVec2(left, top + pt(18.0f)), Align::Left, th::kLabel, "Nuevo lienzo");
    top += pt(36.0f + 8.0f);
    ui::paragraph(dl, Weight::Regular, th::kSubhead, ImVec2(left, top), inner, Align::Left, th::kSecondaryLabel,
                  description, 1.35f);
    top += descriptionHeight + pt(18.0f);

    // Tamaños.
    const ImRect list(ImVec2(left, top), ImVec2(right, top + listHeight));
    dl->AddRectFilled(list.Min, list.Max, th::kGroupFill, pt(18.0f));
    for (int i = 0; i < kCanvasPresetCount; ++i) {
        const CanvasPreset& preset = kCanvasPresets[i];
        const Size size = sizeOf(i);
        const ImRect bounds(ImVec2(left, top), ImVec2(right, top + row));
        const ImRect inset(ImVec2(bounds.Min.x + pt(4.0f), bounds.Min.y + pt(4.0f)),
                           ImVec2(bounds.Max.x - pt(4.0f), bounds.Max.y - pt(4.0f)));
        ImGui::PushID(i);
        const ImGuiID id = ImGui::GetID("##preset");
        const Press press = ui::pressable(id, bounds, interactive && size.supported);
        ImGui::PopID();
        const bool selected = i == m_preset;
        const float on = ui::anim::follow(id + 7u, selected ? 1.0f : 0.0f, 20.0f);
        if (on > 0.002f) {
            dl->AddRectFilled(inset.Min, inset.Max, ui::withAlpha(th::kAccent, 0.2f * on), pt(14.0f));
        }
        const float touch = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
        if (touch > 0.002f) {
            dl->AddRectFilled(inset.Min, inset.Max, ui::withAlpha(th::kPressed, touch), pt(14.0f));
        }
        // Entre filas, salvo junto a la elegida (que ya tiene su fondo).
        if (i > 0 && i != m_preset && i - 1 != m_preset) {
            ui::separator(dl, bounds.Min.x + pt(60.0f), bounds.Max.x - pt(14.0f), bounds.Min.y);
        }

        const float alpha = size.supported ? 1.0f : 0.4f;
        const float cy = bounds.GetCenter().y;
        // Icono: un rectángulo con la proporción del lienzo.
        const ImRect tile(ImVec2(bounds.Min.x + pt(14.0f), cy - pt(16.0f)), ImVec2(bounds.Min.x + pt(46.0f), cy + pt(16.0f)));
        dl->AddRectFilled(tile.Min, tile.Max, ui::withAlpha(preset.color, alpha), pt(8.0f));
        const float aspect = static_cast<float>(size.width) / static_cast<float>(std::max(size.height, 1));
        const float longSide = pt(18.0f);
        const float shortSide = std::max(pt(8.0f), longSide / std::max(aspect, 1.0f / aspect));
        const ImVec2 shape = aspect >= 1.0f ? ImVec2(longSide, shortSide) : ImVec2(shortSide, longSide);
        const ImVec2 c = tile.GetCenter();
        dl->AddRect(ImVec2(c.x - shape.x * 0.5f, c.y - shape.y * 0.5f), ImVec2(c.x + shape.x * 0.5f, c.y + shape.y * 0.5f),
                    ui::withAlpha(IM_COL32_WHITE, alpha), pt(2.5f), 0, pt(1.6f));

        char detail[48];
        std::snprintf(detail, sizeof(detail), "%d × %d", size.width, size.height);
        const float textX = bounds.Min.x + pt(60.0f);
        const float textRight = bounds.Max.x - pt(size.supported ? 44.0f : 96.0f);
        ui::label(dl, Weight::Regular, th::kBody, ImVec2(textX, cy - pt(9.0f)), Align::Left,
                  ui::withAlpha(th::kLabel, alpha), preset.name, textRight - textX);
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(textX, cy + pt(11.0f)), Align::Left,
                  ui::withAlpha(th::kSecondaryLabel, alpha), detail, textRight - textX);
        if (!size.supported) {
            ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(bounds.Max.x - pt(14.0f), cy), Align::Right,
                      th::kTertiaryLabel, "No admitido");
        } else if (on > 0.002f) {
            ui::icon(dl, icon::kCheck, ImVec2(bounds.Max.x - pt(14.0f + 10.0f), cy), 20.0f,
                     ui::withAlpha(th::kAccent, on));
        }
        if (press.clicked) {
            m_preset = i;
        }
        top += row;
    }
    top += pt(16.0f);

    // Orientación (la pantalla completa ya tiene la suya).
    {
        const bool full = kCanvasPresets[m_preset].width == 0;
        const ImRect control(ImVec2(right - std::min(pt(220.0f), inner * 0.62f), top), ImVec2(right, top + pt(32.0f)));
        ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(left + pt(2.0f), control.GetCenter().y), Align::Left,
                  full ? th::kSecondaryLabel : th::kLabel, "Orientación", control.Min.x - left - pt(10.0f));
        const char* labels[2] = {"Horizontal", "Vertical"};
        const Size size = sizeOf(m_preset);
        int shown = full ? (size.width >= size.height ? 0 : 1) : m_orientation;
        if (ui::segmented("##orientation", control, labels, 2, &shown, interactive && !full) && !full) {
            m_orientation = shown;
        }
        top += pt(32.0f + 20.0f);
    }

    // Crear.
    {
        const ImRect button(ImVec2(left, top), ImVec2(right, top + pt(50.0f)));
        const ImGuiID id = ImGui::GetID("##create");
        const Press press = ui::pressable(id, button, interactive);
        const float held = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.4f : 0.0f), 24.0f);
        ui::shadow(dl, button, pt(25.0f), pt(26.0f), pt(8.0f), 0.42f * (1.0f - 0.5f * held), 0, th::kAccent);
        dl->AddRectFilled(button.Min, button.Max, ui::mix(th::kAccent, IM_COL32(0, 60, 150, 255), 0.35f * held),
                          pt(25.0f));
        ui::label(dl, Weight::SemiBold, th::kHeadline, button.GetCenter(), Align::Center, th::kLabel, "Crear lienzo");
        const bool enter = interactive && !ImGui::GetIO().WantTextInput &&
                           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        if (press.clicked || enter) {
            const Size size = sizeOf(m_preset);
            if (size.supported) {
                requests.canvasWidth = size.width;
                requests.canvasHeight = size.height;
                if (modal) {
                    m_dialog = Dialog::None;
                }
            }
        }
    }
    if (scrolls) {
        ui::endScroll();
    }

    // Cerrar (solo desde Acciones: al empezar hay que elegir un lienzo).
    if (modal) {
        const ImVec2 center(rect.Max.x - pt(16.0f + 15.0f), rect.Min.y + pt(16.0f + 15.0f));
        const ImRect close(ImVec2(center.x - pt(15.0f), center.y - pt(15.0f)), ImVec2(center.x + pt(15.0f), center.y + pt(15.0f)));
        const ImGuiID id = ImGui::GetID("##close");
        const Press press = ui::pressable(id, close, interactive);
        const float t = ui::anim::follow(id + 5u, press.held ? 1.0f : (press.hovered ? 0.5f : 0.0f), 24.0f);
        dl->AddCircleFilled(center, pt(15.0f), ui::mix(IM_COL32(118, 118, 128, 61), IM_COL32(118, 118, 128, 110), t), 0);
        ui::icon(dl, icon::kX, center, 16.0f, IM_COL32(235, 235, 245, 180));
        if (press.clicked) {
            m_dialog = Dialog::None;
        }
    }

    const float e = ui::anim::easeOutCubic(std::clamp(presence, 0.0f, 1.0f));
    ui::transform(mark, rect.GetCenter(), 0.94f + 0.06f * e, ImVec2(0.0f, pt(24.0f) * (1.0f - e)), std::min(1.0f, presence * 1.4f));
    ui::endSurface();
}

void Ui::startScreen(UiRequests& requests) {
    const Layout& L = m_layout;

    // Fondo: casi negro con dos luces de color muy suaves detrás de la tarjeta.
    const ImRect screen(ImVec2(0.0f, 0.0f), L.display);
    ui::beginSurface("##start-background", screen, false, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(screen.Min, screen.Max, IM_COL32(12, 12, 14, 255));
    const float extent = std::max(L.display.x, L.display.y);
    glow(dl, ImVec2(L.display.x * 0.28f, L.display.y * 0.25f), extent * 0.5f, th::kAccent, 0.2f);
    glow(dl, ImVec2(L.display.x * 0.76f, L.display.y * 0.8f), extent * 0.45f, IM_COL32(191, 90, 242, 255), 0.14f);
    ui::endSurface();

    // Atrás (Android) aquí no pierde nada: sale sin preguntar.
    if (ImGui::IsKeyPressed(ImGuiKey_AppBack, false)) {
        requests.quit = true;
    }
    const float p = ui::anim::followFrom(ImHashStr("##start-card"), 0.0f, 1.0f, 9.0f);
    newCanvasCard(false, p, true, requests);
}
