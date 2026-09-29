// Diálogos: confirmaciones (eliminar y renombrar capa, salir) y la tarjeta de lienzo
// nuevo, que también es la pantalla de inicio. Son tarjetas alineadas a la izquierda:
// un icono, el título, la explicación y botones abajo a la derecha.
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
};

constexpr CanvasPreset kCanvasPresets[] = {
    {"Pantalla completa", 0, 0},
    {"HD 720p", 1280, 720},
    {"Full HD 1080p", 1920, 1080},
    {"QHD 1440p", 2560, 1440},
    {"4K UHD", 3840, 2160},
    {"Pequeño", 640, 360},
};
constexpr int kCanvasPresetCount = static_cast<int>(sizeof(kCanvasPresets) / sizeof(kCanvasPresets[0]));
// Área del tamaño más grande (4K): la forma de cada ficha crece con sus píxeles.
constexpr float kLargestArea = 3840.0f * 2160.0f;

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

// Rectángulo hueco de `size` centrado en `center`: la forma de un lienzo.
ImRect frameRect(ImVec2 center, ImVec2 size) {
    return ImRect(ImVec2(ui::snap(center.x - size.x * 0.5f), ui::snap(center.y - size.y * 0.5f)),
                  ImVec2(ui::snap(center.x + size.x * 0.5f), ui::snap(center.y + size.y * 0.5f)));
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

    // Medidas: cabecera, rejilla de tamaños, orientación y la barra de crear.
    const bool compact = L.compact;
    const float pad = pt(20.0f);
    const float width = std::min(pt(520.0f), L.right - L.left);
    const float inner = width - pad * 2.0f;
    const float closeSize = pt(32.0f);
    const float headerWidth = inner - (modal ? closeSize + pt(12.0f) : 0.0f);
    const char* description =
        modal ? "El dibujo actual se descartará: guárdalo antes como PNG si quieres conservarlo."
              : "NDI y el PNG usan el lienzo entero a este tamaño, sin importar el zoom.";
    const float descriptionHeight = ui::paragraph(nullptr, Weight::Regular, th::kCallout, ImVec2(0.0f, 0.0f),
                                                  headerWidth, Align::Left, 0, description, 1.45f);
    constexpr float kBrand = 14.0f;
    const float titleHeight = ui::fontSize(th::kTitle);
    const float headerHeight = pt(kBrand + 6.0f) + titleHeight + pt(6.0f) + descriptionHeight;
    constexpr float kSectionGap = 18.0f;
    constexpr float kSectionLabel = 14.0f + 10.0f;   // rótulo de sección y su hueco
    const float tileGap = pt(8.0f);
    // Tres columnas si las fichas caben holgadas; si no (un teléfono), dos.
    const int columns = (inner - tileGap * 2.0f) / 3.0f >= pt(128.0f) ? 3 : 2;
    const int gridRows = (kCanvasPresetCount + columns - 1) / columns;
    const float tileWidth = (inner - tileGap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float tileHeight = pt(compact ? 92.0f : 108.0f);
    const float gridHeight = tileHeight * static_cast<float>(gridRows) + tileGap * static_cast<float>(gridRows - 1);
    const float chipHeight = pt(42.0f);
    const float createHeight = pt(50.0f);
    const float content = pad + headerHeight + pt(kSectionGap + kSectionLabel) + gridHeight +
                          pt(kSectionGap + kSectionLabel) + chipHeight + pt(kSectionGap) + createHeight + pad;
    const float height = std::min(content, L.bottom - L.top);
    const float x = std::round((L.left + L.right - width) * 0.5f);
    const float y = std::round((L.top + L.bottom - height) * 0.5f);
    const ImRect rect(x, y, x + width, y + height);
    const float radius = pt(th::kDialogRadius);

    ui::beginSurface(modal ? "##new-canvas" : "##start-card", rect, interactive, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(70.0f), pt(26.0f), 0.5f);
    ui::popUnclipped(dl);
    if (modal) {
        ui::glass(dl, rect, radius, IM_COL32(24, 24, 28, 230), 0, kDimAlpha * presence);
    } else {
        // Al empezar no hay lienzo que desenfocar: deja ver un poco las luces del fondo.
        ui::glass(dl, rect, radius, IM_COL32(24, 24, 28, 170));
    }

    // Si no cabe, el contenido se desplaza (por debajo de las esquinas redondeadas no).
    const bool scrolls = content > height + 0.5f;
    const ImRect view(ImVec2(rect.Min.x, rect.Min.y + pt(8.0f)), ImVec2(rect.Max.x, rect.Max.y - pt(8.0f)));
    const float scroll = scrolls ? ui::beginScroll("##card", view, content - pt(16.0f)) : 0.0f;
    const float left = rect.Min.x + pad;
    const float right = rect.Max.x - pad;
    float top = rect.Min.y + pad - scroll;

    // Cabecera: la marca, el título, la explicación y (desde Acciones) cerrar.
    {
        const float headerTop = top;
        const float brandCy = top + pt(kBrand * 0.5f);
        dl->AddRectFilled(ImVec2(left, brandCy - pt(4.0f)), ImVec2(left + pt(8.0f), brandCy + pt(4.0f)), th::kRed,
                          pt(2.0f));
        ui::tracked(dl, Weight::Bold, th::kMicro, ImVec2(left + pt(16.0f), brandCy), IM_COL32(235, 235, 245, 140),
                    "LIVESKETCH", 0.12f);
        top += pt(kBrand + 6.0f);
        ui::label(dl, Weight::Bold, th::kTitle, ImVec2(left, top + titleHeight * 0.5f), Align::Left, th::kLabel,
                  "Nuevo lienzo", headerWidth);
        top += titleHeight + pt(6.0f);
        ui::paragraph(dl, Weight::Regular, th::kCallout, ImVec2(left, top), headerWidth, Align::Left,
                      IM_COL32(235, 235, 245, 153), description, 1.45f);
        top += descriptionHeight;
        if (modal) {
            const ImRect close(ImVec2(right - closeSize, headerTop), ImVec2(right, headerTop + closeSize));
            const Press press = ui::buttonFrame("##close", close, ui::ButtonStyle::Secondary, true, pt(9.0f));
            ui::icon(dl, icon::kX, close.GetCenter(), 16.0f, IM_COL32(235, 235, 245, 204));
            if (press.clicked) {
                m_dialog = Dialog::None;
            }
        }
    }

    // Tamaños: cada ficha dibuja la forma del lienzo a escala (su proporción, y más grande
    // cuantos más píxeles tiene).
    top += pt(kSectionGap);
    ui::sectionLabel(dl, left, right, top + pt(7.0f), "TAMAÑO");
    top += pt(kSectionLabel);
    {
        const float boxHeight = pt(compact ? 40.0f : 48.0f);
        const ImVec2 shapeMax = compact ? pt(44.0f, 36.0f) : pt(48.0f, 44.0f);
        const float stack = boxHeight + pt(8.0f + 16.0f + 2.0f + 15.0f);   // forma, nombre y medidas
        for (int i = 0; i < kCanvasPresetCount; ++i) {
            const Size size = sizeOf(i);
            const float x0 = left + (tileWidth + tileGap) * static_cast<float>(i % columns);
            const float y0 = top + (tileHeight + tileGap) * static_cast<float>(i / columns);
            const ImRect tile(ImVec2(x0, y0), ImVec2(x0 + tileWidth, y0 + tileHeight));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##preset", tile, i == m_preset, size.supported);
            ImGui::PopID();
            const float alpha = size.supported ? 1.0f : 0.4f;
            const float cx = tile.GetCenter().x;
            const float stackTop = tile.Min.y + (tileHeight - stack) * 0.5f;

            const float w = static_cast<float>(size.width);
            const float h = static_cast<float>(size.height);
            const float grow = 0.55f + 0.45f * std::sqrt(std::min(w * h / kLargestArea, 1.0f));
            const float fit = std::min(shapeMax.x / w, shapeMax.y / h) * grow;
            const ImRect shape = frameRect(ImVec2(cx, stackTop + boxHeight * 0.5f),
                                           ImVec2(std::max(w * fit, pt(8.0f)), std::max(h * fit, pt(8.0f))));
            if (c.on > 0.002f) {
                dl->AddRectFilled(shape.Min, shape.Max, ui::withAlpha(th::kAccent, 0.22f * c.on), pt(4.0f));
            }
            ui::outline(dl, shape, pt(4.0f), ui::withAlpha(ui::mix(IM_COL32(235, 235, 245, 140), th::kAccent, c.on), alpha),
                        pt(2.0f));

            const float nameCy = stackTop + boxHeight + pt(8.0f + 8.0f);
            ui::label(dl, Weight::SemiBold, 13.0f, ImVec2(cx, nameCy), Align::Center, ui::withAlpha(th::kLabel, alpha),
                      kCanvasPresets[i].name, tileWidth - pt(12.0f));
            char detail[48];
            if (size.supported) {
                std::snprintf(detail, sizeof(detail), "%d × %d", size.width, size.height);
            } else {
                std::snprintf(detail, sizeof(detail), "No admitido");
            }
            ui::label(dl, Weight::Regular, th::kCaption, ImVec2(cx, nameCy + pt(8.0f + 2.0f + 7.5f)), Align::Center,
                      size.supported ? ui::mix(IM_COL32(235, 235, 245, 128), th::kAccentText, c.on) : th::kTertiaryLabel,
                      detail, tileWidth - pt(12.0f));
            if (c.press.clicked) {
                m_preset = i;
            }
        }
        top += gridHeight;
    }

    // Orientación (la pantalla completa sigue la de la pantalla: se muestra sin poder
    // cambiarla).
    top += pt(kSectionGap);
    ui::sectionLabel(dl, left, right, top + pt(7.0f), "ORIENTACIÓN");
    top += pt(kSectionLabel);
    {
        const bool full = kCanvasPresets[m_preset].width == 0;
        const Size size = sizeOf(m_preset);
        const int shown = full ? (size.width >= size.height ? 0 : 1) : m_orientation;
        const float chipWidth = (inner - tileGap) * 0.5f;
        const char* labels[2] = {"Horizontal", "Vertical"};
        for (int i = 0; i < 2; ++i) {
            const float x0 = left + (chipWidth + tileGap) * static_cast<float>(i);
            const ImRect chip(ImVec2(x0, top), ImVec2(x0 + chipWidth, top + chipHeight));
            ImGui::PushID(i);
            const ui::Choice c = ui::choice("##orientation", chip, shown == i, !full);
            ImGui::PopID();
            const ImU32 color = ui::withAlpha(ui::mix(IM_COL32(235, 235, 245, 191), th::kLabel, c.on), full ? 0.4f : 1.0f);
            const ImVec2 glyph = i == 0 ? pt(20.0f, 14.0f) : pt(14.0f, 20.0f);
            const float textWidth = ui::measure(Weight::SemiBold, th::kCallout, labels[i]).x;
            const float groupX = chip.GetCenter().x - (glyph.x + pt(10.0f) + textWidth) * 0.5f;
            const float cy = chip.GetCenter().y;
            ui::outline(dl, frameRect(ImVec2(groupX + glyph.x * 0.5f, cy), glyph), pt(3.0f), color, pt(2.0f));
            ui::label(dl, Weight::SemiBold, th::kCallout, ImVec2(groupX + glyph.x + pt(10.0f), cy), Align::Left, color,
                      labels[i]);
            if (c.press.clicked && !full) {
                m_orientation = i;
            }
        }
        top += chipHeight;
    }

    // Crear: una barra con el tamaño que saldrá.
    top += pt(kSectionGap);
    {
        const ImRect bar(ImVec2(left, top), ImVec2(right, top + createHeight));
        const float barRadius = pt(12.0f);
        ui::shadow(dl, bar, barRadius, pt(26.0f), pt(10.0f), 0.3f, 0, th::kAccent);
        const Press press = ui::buttonFrame("##create", bar, ui::ButtonStyle::Primary, true, barRadius);
        const Size size = sizeOf(m_preset);
        const float cy = bar.GetCenter().y;
        const ImVec2 arrow(bar.Max.x - pt(16.0f + 9.0f), cy);
        ui::icon(dl, icon::kArrowRight, arrow, 18.0f, th::kLabel);
        char detail[48];
        std::snprintf(detail, sizeof(detail), "%d × %d", size.width, size.height);
        const float detailRight = arrow.x - pt(9.0f + 10.0f);
        ui::label(dl, Weight::Regular, th::kCallout, ImVec2(detailRight, cy), Align::Right, IM_COL32(255, 255, 255, 199),
                  detail);
        const float textX = bar.Min.x + pt(18.0f);
        ui::label(dl, Weight::SemiBold, th::kBody, ImVec2(textX, cy), Align::Left, th::kLabel, "Crear lienzo",
                  detailRight - ui::measure(Weight::Regular, th::kCallout, detail).x - pt(10.0f) - textX);
        const bool enter = interactive && !ImGui::GetIO().WantTextInput &&
                           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        if ((press.clicked || enter) && size.supported) {
            requests.canvasWidth = size.width;
            requests.canvasHeight = size.height;
            if (modal) {
                m_dialog = Dialog::None;
            }
        }
    }
    if (scrolls) {
        ui::endScroll();
    }

    const float e = ui::anim::easeOutCubic(std::clamp(presence, 0.0f, 1.0f));
    ui::transform(mark, rect.GetCenter(), 0.97f + 0.03f * e, ImVec2(0.0f, pt(16.0f) * (1.0f - e)),
                  std::min(1.0f, presence * 1.4f));
    ui::endSurface();
}

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

    // Atrás (Android) aquí no pierde nada: sale sin preguntar.
    if (ImGui::IsKeyPressed(ImGuiKey_AppBack, false)) {
        requests.quit = true;
    }
    const float p = ui::anim::followFrom(ImHashStr("##start-card"), 0.0f, 1.0f, 9.0f);
    newCanvasCard(false, p, true, requests);
}
