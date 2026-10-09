// Pantalla de Proyectos: lo primero que se ve al abrir la app y lo que se ve sin lienzo. Una
// ficha por proyecto de la biblioteca (la miniatura, el nombre, el tamaño, la fecha y lo que
// ocupa), del último que cambió al primero. Tocar una lo abre; su botón «⋯» (o el clic
// derecho) da Renombrar, Duplicar, Exportar y Eliminar. Arriba, «Abrir archivo» y «Nuevo
// lienzo». Sin proyectos, en lugar de las fichas, qué es esta pantalla y esos dos botones.
#include "UI/Ui.h"

#include "Canvas/CanvasSpec.h"
#include "Gfx/ColorSpace.h"
#include "Gfx/GL.h"
#include "IO/Library.h"
#include "UI/Anim.h"
#include "UI/Icons.h"
#include "UI/PanelParts.h"

#include <SDL3/SDL_platform_defines.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace th = ui::theme;
using ui::Align;
using ui::Press;
using ui::Weight;
using ui::pt;

namespace {

// Medidas (pt).
constexpr float kCardMin = 204.0f;         // ancho mínimo de una ficha (tableta y ordenador)
constexpr float kCardMinNarrow = 148.0f;   // en un teléfono
constexpr float kCardRadius = 14.0f;
constexpr float kCardPad = 8.0f;           // de la ficha a la caja de la miniatura
constexpr float kThumbAspect = 0.75f;      // alto de la caja de la miniatura / ancho
constexpr float kThumbInset = 10.0f;       // de la caja a la miniatura
constexpr float kCardText = 80.0f;         // alto de los textos de la ficha
constexpr float kMenuWidth = 236.0f;
constexpr float kMenuRow = 44.0f;
constexpr float kHeaderButton = 42.0f;

// Sin proyectos (pt): la ilustración (tres fichas apiladas), el título y el ancho del texto.
constexpr float kStackWidth = 148.0f;
constexpr float kStackCardHeight = 128.0f;
constexpr float kStackRise = 13.0f;        // lo que asoma cada ficha por encima de la siguiente
constexpr float kStackHeight = kStackCardHeight + kStackRise * 2.0f;
constexpr float kEmptyTitle = 20.0f;
constexpr float kEmptyWidth = 400.0f;
constexpr float kEmptyButtonMin = 156.0f;

// Buscando los proyectos: si tarda, se dice.
constexpr uint64_t kLoadingDelayMs = 300;

// Copia `text` en `buffer` (de `size` bytes) sin partir un carácter UTF-8.
void copyText(const std::string& text, char* buffer, size_t size) {
    size_t length = std::min(text.size(), size - 1);
    while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    std::memcpy(buffer, text.data(), length);
    buffer[length] = '\0';
}

std::string displayName(const library::Summary& item) { return item.name.empty() ? "Sin nombre" : item.name; }

std::string projectCount(const std::vector<library::Summary>& items) {
    if (items.empty()) {
        return "Sin proyectos";
    }
    uint64_t bytes = 0;
    for (const library::Summary& item : items) {
        bytes += item.bytes;
    }
    std::string text = std::to_string(items.size()) + (items.size() == 1 ? " proyecto" : " proyectos");
    if (bytes > 0) {
        text += " · " + canvasspec::formatBytes(static_cast<size_t>(bytes));
    }
    return text;
}

// Botón de la cabecera: el icono y el texto centrados (o solo el icono).
bool headerButton(ImDrawList* dl, const char* id, const ImRect& rect, const char* glyph, const char* text,
                  ui::ButtonStyle style) {
    const float radius = pt(12.0f);
    if (style == ui::ButtonStyle::Primary) {
        ui::shadow(dl, rect, radius, pt(24.0f), pt(8.0f), 0.28f, 0, th::kAccent);
    }
    const Press press = ui::buttonFrame(id, rect, style, true, radius);
    const float cy = rect.GetCenter().y;
    const float textWidth = ui::measure(Weight::SemiBold, th::kSubhead, text).x;
    const float total = pt(18.0f + 8.0f) + textWidth;
    if (total + pt(20.0f) > rect.GetWidth()) {
        ui::icon(dl, glyph, rect.GetCenter(), 18.0f, th::kLabel);
        return press.clicked;
    }
    const float x = std::round(rect.GetCenter().x - total * 0.5f);
    ui::icon(dl, glyph, ImVec2(x + pt(9.0f), cy), 18.0f, th::kLabel);
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(x + pt(26.0f), cy), Align::Left, th::kLabel, text);
    return press.clicked;
}

float headerButtonWidth(const char* text) {
    return pt(18.0f + 18.0f + 8.0f) + ui::measure(Weight::SemiBold, th::kSubhead, text).x + pt(20.0f);
}

// La ilustración de la pantalla sin proyectos, con su borde de arriba en `top`: tres fichas
// apiladas como las de los proyectos; la de delante, con un «+» donde iría la miniatura y
// dos rayas donde irían el nombre y los datos.
void emptyStack(ImDrawList* dl, float cx, float top) {
    const float width = pt(kStackWidth);
    const float height = pt(kStackCardHeight);
    const float rise = pt(kStackRise);
    const float radius = pt(kCardRadius);
    // De atrás adelante, cada una algo más ancha y más clara. Son opacas: tapan las de detrás.
    struct Sheet {
        float scale;
        ImU32 fill;
        ImU32 border;
    };
    const Sheet sheets[3] = {
        {0.80f, IM_COL32(24, 24, 28, 255), IM_COL32(255, 255, 255, 16)},
        {0.90f, IM_COL32(30, 30, 35, 255), IM_COL32(255, 255, 255, 22)},
        {1.00f, IM_COL32(37, 37, 42, 255), th::kControlBorderStrong},
    };
    ImRect front;
    for (int i = 0; i < 3; ++i) {
        const float w = std::round(width * sheets[i].scale);
        const float y = top + rise * static_cast<float>(i);
        const ImRect rect(ImVec2(std::round(cx - w * 0.5f), y), ImVec2(std::round(cx - w * 0.5f) + w, y + height));
        ui::shadow(dl, rect, radius, pt(i == 2 ? 30.0f : 18.0f), pt(i == 2 ? 10.0f : 4.0f), i == 2 ? 0.45f : 0.25f);
        dl->AddRectFilled(rect.Min, rect.Max, sheets[i].fill, radius);
        ui::outline(dl, rect, radius, sheets[i].border, ui::hairline());
        front = rect;
    }
    // La caja de la miniatura, con el «+».
    const float pad = pt(7.0f);
    const ImRect box(ImVec2(front.Min.x + pad, front.Min.y + pad), ImVec2(front.Max.x - pad, front.Min.y + pad + pt(82.0f)));
    dl->AddRectFilled(box.Min, box.Max, IM_COL32(0, 0, 0, 80), radius - pad * 0.5f);
    const ImVec2 center = box.GetCenter();
    dl->AddCircleFilled(center, pt(20.0f), IM_COL32(10, 132, 255, 40), 0);
    dl->AddCircle(center, pt(20.0f), IM_COL32(61, 155, 255, 110), 0, pt(1.5f));
    ui::icon(dl, icon::kPlus, center, 20.0f, th::kAccentText);
    // El nombre y los datos, en gris.
    const float textLeft = front.Min.x + pt(12.0f);
    const float textWidth = front.GetWidth() - pt(24.0f);
    const float line1 = box.Max.y + pt(14.0f);
    const float line2 = line1 + pt(13.0f);
    dl->AddRectFilled(ImVec2(textLeft, line1 - pt(3.5f)), ImVec2(textLeft + std::round(textWidth * 0.58f), line1 + pt(3.5f)),
                      IM_COL32(255, 255, 255, 46), pt(3.5f));
    dl->AddRectFilled(ImVec2(textLeft, line2 - pt(3.0f)), ImVec2(textLeft + std::round(textWidth * 0.36f), line2 + pt(3.0f)),
                      IM_COL32(255, 255, 255, 24), pt(3.0f));
}

// Fila del menú de un proyecto.
bool menuItem(ImDrawList* dl, const char* id, const ImRect& row, const char* glyph, const char* title, ImU32 color,
              bool enabled) {
    const ImGuiID gid = ImGui::GetID(id);
    const Press press = ui::pressable(gid, row, enabled);
    ui::parts::pressFeedback(dl, gid, row, press, pt(th::kControlRadius));
    const float alpha = enabled ? 1.0f : 0.4f;
    const float cy = row.GetCenter().y;
    ui::icon(dl, glyph, ImVec2(row.Min.x + pt(12.0f + 9.0f), cy), 18.0f,
             ui::withAlpha(color == th::kLabel ? th::kMutedIcon : color, alpha));
    ui::label(dl, Weight::Regular, th::kSubhead, ImVec2(row.Min.x + pt(12.0f + 18.0f + 12.0f), cy), Align::Left,
              ui::withAlpha(color, alpha), title, row.GetWidth() - pt(12.0f + 18.0f + 12.0f + 10.0f));
    return press.clicked && enabled;
}

// Los píxeles transparentes toman el color de un vecino que no lo es: al ampliar la miniatura
// con filtro lineal, sus bordes no se oscurecen.
void bleedEdges(std::vector<uint8_t>& rgba, int width, int height) {
    const std::vector<uint8_t> source = rgba;
    auto at = [&](int x, int y) { return (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4; };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = at(x, y);
            if (source[i + 3] != 0) {
                continue;
            }
            const int neighbors[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& n : neighbors) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= width || n[1] >= height) {
                    continue;
                }
                const size_t j = at(n[0], n[1]);
                if (source[j + 3] != 0) {
                    rgba[i] = source[j];
                    rgba[i + 1] = source[j + 1];
                    rgba[i + 2] = source[j + 2];
                    break;
                }
            }
        }
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Lo que pide la app
// -----------------------------------------------------------------------------

void Ui::askLeave(std::string title, std::string message, std::string confirm) {
    closePanels();
    m_dialogTitle = std::move(title);
    m_dialogMessage = std::move(message);
    m_dialogConfirm = std::move(confirm);
    openDialog(Dialog::Leave, nullptr);
}

void Ui::setLastProject(std::string name) {
    if (name == m_prefs.lastProject) {
        return;
    }
    m_prefs.lastProject = std::move(name);
    savePrefs();
}

// -----------------------------------------------------------------------------
// Miniaturas
// -----------------------------------------------------------------------------

GLuint Ui::projectThumbnail(const library::Summary& item) {
    const int width = item.thumbnailWidth;
    const int height = item.thumbnailHeight;
    if (item.thumbnail.empty() || width <= 0 || height <= 0 ||
        item.thumbnail.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
        return 0;
    }
    ProjectThumb& thumb = m_projectThumbs[item.path];
    const ColorProfile display = m_status.displayProfile;
    if (thumb.texture && thumb.fileTime == item.fileTime && thumb.display == display) {
        return thumb.texture.id();
    }
    // Las imágenes de la interfaz van ya en el perfil de la pantalla.
    std::vector<uint8_t> pixels = item.thumbnail;
    if (item.profile != display) {
        const colorspace::Converter8& converter = colorspace::converter8(item.profile, display);
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            converter.apply(pixels[i], pixels[i + 1], pixels[i + 2]);
        }
    }
    bleedEdges(pixels, width, height);
    if (!thumb.texture) {
        thumb.texture = gfx::Texture::create();
    }
    glBindTexture(GL_TEXTURE_2D, thumb.texture.id());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    thumb.fileTime = item.fileTime;
    thumb.display = display;
    return thumb.texture.id();
}

// -----------------------------------------------------------------------------
// La pantalla
// -----------------------------------------------------------------------------

void Ui::projectsScreen(UiRequests& requests) {
    const library::Library* library = m_status.library;
    const bool loading = library && library->loading();
    // Sin dónde guardar proyectos, la tarjeta de lienzo nuevo: se puede dibujar igual.
    const bool start = !library;

    // Atrás y Escape cierran lo último que se abrió (o dejan de escribir en la tarjeta); con
    // todo cerrado, atrás sale de la app: aquí no se pierde nada.
    const ImGuiIO& io = ImGui::GetIO();
    if (m_status.busy == Busy::None) {
        const bool back = ImGui::IsKeyPressed(ImGuiKey_AppBack, false);
        if (back || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (closeTopmost(nullptr)) {
            } else if (!m_projectMenu.empty()) {
                m_projectMenu.clear();
            } else if (start && cancelCanvasEdit()) {
            } else if (back) {
                requests.quit = true;
            }
        } else if (m_dialog == Dialog::None && !io.WantTextInput && (io.KeyCtrl || io.KeySuper)) {
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
                requests.openProject = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_N, false) && !start) {
                m_projectMenu.clear();
                openDialog(Dialog::NewCanvas, nullptr);
            }
        }
    }

    if (!loading) {
        m_libraryLoadingSince = 0;
    }
    startBackground();
    if (start) {
        m_projectMenu.clear();
        startScreen(requests);
        return;
    }
    const Layout& L = m_layout;
    const ImRect screen(ImVec2(0.0f, 0.0f), L.display);
    // Buscando los proyectos (suele ser un momento): solo el fondo y, si tarda, un indicador.
    if (loading) {
        const uint64_t now = SDL_GetTicks();
        if (m_libraryLoadingSince == 0) {
            m_libraryLoadingSince = now;
        }
        if (now - m_libraryLoadingSince >= kLoadingDelayMs) {
            ui::beginSurface("##projects-loading", screen, false, false);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 center = screen.GetCenter();
            ui::spinner(dl, ImVec2(center.x, center.y - pt(14.0f)), pt(11.0f), th::kSecondaryLabel);
            ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(center.x, center.y + pt(16.0f)), Align::Center,
                      th::kSecondaryLabel, "Buscando proyectos…");
            ui::endSurface();
        } else {
            ui::anim::keepAlive();
        }
        return;
    }

    // Cabecera: la marca, «Proyectos», cuántos hay y lo que ocupan, y los botones (en un
    // teléfono, en una fila debajo).
    const bool narrow = L.narrow;
    const float padX = pt(narrow ? 16.0f : 28.0f);
    const float left = L.safe[3] + padX;
    const float right = L.display.x - L.safe[1] - padX;
    const float width = right - left;
    const std::vector<library::Summary>& items = library->items();
    const bool empty = items.empty();
    const std::string count = projectCount(items);
    const float titleHeight = ui::fontSize(th::kTitle);
    const float footnote = ui::fontSize(th::kFootnote);
    const float top = L.safe[0] + pt(narrow ? 14.0f : 22.0f);
    const float titleCy = top + pt(14.0f + 8.0f) + titleHeight * 0.5f;
    const float countCy = titleCy + titleHeight * 0.5f + pt(4.0f) + footnote * 0.5f;
    const float buttonHeight = pt(kHeaderButton);
    float headerBottom = countCy + footnote * 0.5f + pt(16.0f);
    ImRect openRect;
    ImRect newRect;
    const float gap = pt(10.0f);
    if (empty) {
        // Sin proyectos, los botones van en el centro, con lo que se explica.
    } else if (narrow) {
        const float buttonsTop = countCy + footnote * 0.5f + pt(14.0f);
        const float half = (width - gap) * 0.5f;
        openRect = ImRect(ImVec2(left, buttonsTop), ImVec2(left + half, buttonsTop + buttonHeight));
        newRect = ImRect(ImVec2(right - half, buttonsTop), ImVec2(right, buttonsTop + buttonHeight));
        headerBottom = buttonsTop + buttonHeight + pt(14.0f);
    } else {
        const float middle = (titleCy - titleHeight * 0.5f + countCy + footnote * 0.5f) * 0.5f;
        const float newWidth = headerButtonWidth("Nuevo lienzo");
        const float openWidth = headerButtonWidth("Abrir archivo");
        newRect = ImRect(ImVec2(right - newWidth, middle - buttonHeight * 0.5f), ImVec2(right, middle + buttonHeight * 0.5f));
        openRect = ImRect(ImVec2(newRect.Min.x - gap - openWidth, newRect.Min.y), ImVec2(newRect.Min.x - gap, newRect.Max.y));
    }

    ui::beginSurface("##projects", screen, true, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::parts::brand(dl, left, top + pt(7.0f));
    const float titleRight = narrow || empty ? right : openRect.Min.x - pt(16.0f);
    ui::label(dl, Weight::Bold, th::kTitle, ImVec2(left, titleCy), Align::Left, th::kLabel, "Proyectos",
              titleRight - left);
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(left, countCy), Align::Left, th::kSecondaryLabel,
              count.c_str(), titleRight - left);
    if (!empty && headerButton(dl, "##open-file", openRect, icon::kFolderOpen, "Abrir archivo",
                               ui::ButtonStyle::Secondary)) {
        m_projectMenu.clear();
        requests.openProject = true;
    }
    if (!empty && headerButton(dl, "##new-canvas", newRect, icon::kPlus, "Nuevo lienzo", ui::ButtonStyle::Primary)) {
        m_projectMenu.clear();
        openDialog(Dialog::NewCanvas, nullptr);
    }
    ui::separator(dl, 0.0f, L.display.x, headerBottom, th::kRule);

    // Las fichas, en columnas que llenan el ancho (o, sin proyectos, cómo empezar).
    const ImRect view(ImVec2(0.0f, headerBottom + ui::hairline()), L.display);
    if (empty) {
        projectsEmpty(dl, view, left, right, requests);
    } else {
        projectGrid(dl, view, left, right, requests);
    }
    ui::endSurface();

    projectMenu(requests);
    // Lo que ya no está en la biblioteca no necesita miniatura.
    if (m_projectThumbs.size() > items.size()) {
        for (auto it = m_projectThumbs.begin(); it != m_projectThumbs.end();) {
            it = library->find(it->first) ? std::next(it) : m_projectThumbs.erase(it);
        }
    }
}

void Ui::projectsEmpty(ImDrawList* dl, const ImRect& view, float left, float right, UiRequests& requests) {
    const Layout& L = m_layout;
    const float width = std::min(right - left, pt(kEmptyWidth));
    const float x0 = std::round((left + right - width) * 0.5f);
    const float cx = x0 + width * 0.5f;
#ifdef SDL_PLATFORM_EMSCRIPTEN
    const char* text = "Cada lienzo se guarda solo en este navegador mientras dibujas. Crea uno nuevo o abre un "
                       "archivo .lvskt.";
#else
    const char* text = "Cada lienzo se guarda solo mientras dibujas. Crea uno nuevo o abre un archivo .lvskt.";
#endif
    const float titleHeight = ui::fontSize(kEmptyTitle);
    const float textHeight =
        ui::paragraph(nullptr, Weight::Regular, th::kSubhead, ImVec2(x0, 0.0f), width, Align::Center, th::kSecondaryLabel, text);
    const float buttonHeight = pt(kHeaderButton);
    // La ilustración, si cabe con lo demás.
    const float words = titleHeight + pt(10.0f) + textHeight + pt(24.0f) + buttonHeight;
    const float padY = pt(L.narrow ? 20.0f : 32.0f);
    const float room = view.GetHeight() - L.safe[2] - padY * 2.0f;
    const float art = pt(kStackHeight) + pt(28.0f);
    const bool illustrated = words + art <= room;
    const float block = words + (illustrated ? art : 0.0f);
    const float content = padY * 2.0f + block + L.safe[2];
    const bool scrolls = content > view.GetHeight() + 0.5f;
    const float scroll = scrolls ? ui::beginScroll("##projects-empty", view, content) : 0.0f;

    // Aparece subiendo un poco; un poco por encima del centro.
    const float p = ui::anim::followFrom(ImHashStr("##projects-empty"), 0.0f, 1.0f, 9.0f);
    const float e = ui::anim::easeOutCubic(std::clamp(p, 0.0f, 1.0f));
    const ui::DrawMark mark = ui::mark(dl);
    float y = view.Min.y + padY + std::max(0.0f, room - block) * 0.42f - scroll + std::round(pt(12.0f) * (1.0f - e));
    if (illustrated) {
        emptyStack(dl, cx, y);
        y += art;
    }
    ui::label(dl, Weight::SemiBold, kEmptyTitle, ImVec2(cx, y + titleHeight * 0.5f), Align::Center, th::kLabel,
              "Aquí aparecerán tus proyectos", width);
    y += titleHeight + pt(10.0f);
    ui::paragraph(dl, Weight::Regular, th::kSubhead, ImVec2(x0, y), width, Align::Center, th::kSecondaryLabel, text);
    y += textHeight + pt(24.0f);

    // «Abrir archivo» y «Nuevo lienzo», como en la cabecera.
    const float gap = pt(10.0f);
    float openWidth = std::max(headerButtonWidth("Abrir archivo"), pt(kEmptyButtonMin));
    float newWidth = std::max(headerButtonWidth("Nuevo lienzo"), pt(kEmptyButtonMin));
    if (openWidth + gap + newWidth > width) {
        openWidth = newWidth = std::floor((width - gap) * 0.5f);
    }
    const float bx = std::round(cx - (openWidth + gap + newWidth) * 0.5f);
    const ImRect openRect(ImVec2(bx, y), ImVec2(bx + openWidth, y + buttonHeight));
    const ImRect newRect(ImVec2(openRect.Max.x + gap, y), ImVec2(openRect.Max.x + gap + newWidth, y + buttonHeight));
    if (headerButton(dl, "##open-file", openRect, icon::kFolderOpen, "Abrir archivo", ui::ButtonStyle::Secondary)) {
        m_projectMenu.clear();
        requests.openProject = true;
    }
    if (headerButton(dl, "##new-canvas", newRect, icon::kPlus, "Nuevo lienzo", ui::ButtonStyle::Primary)) {
        m_projectMenu.clear();
        openDialog(Dialog::NewCanvas, nullptr);
    }
    // Solo la transparencia; la subida ya va en `y` (mover lo dibujado movería también el
    // recorte de la cabecera).
    ui::transform(mark, ImVec2(cx, y), 1.0f, ImVec2(0.0f, 0.0f), p);
    if (scrolls) {
        ui::endScroll();
    }
}

void Ui::projectGrid(ImDrawList* dl, const ImRect& view, float left, float right, UiRequests& requests) {
    const Layout& L = m_layout;
    const library::Library& library = *m_status.library;
    const std::vector<library::Summary>& items = library.items();
    const float width = right - left;
    const float gap = pt(L.narrow ? 12.0f : 18.0f);
    const float minCard = pt(L.narrow ? kCardMinNarrow : kCardMin);
    const int columns = std::max(1, static_cast<int>((width + gap) / (minCard + gap)));
    const float cardWidth = std::floor((width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const float thumbHeight = std::round((cardWidth - pt(kCardPad) * 2.0f) * kThumbAspect);
    const float cardHeight = pt(kCardPad) + thumbHeight + pt(kCardText);
    const int rows = (static_cast<int>(items.size()) + columns - 1) / columns;
    const float padTop = pt(L.narrow ? 14.0f : 20.0f);
    const float content = padTop + static_cast<float>(rows) * cardHeight + static_cast<float>(std::max(rows - 1, 0)) * gap +
                          pt(24.0f) + L.safe[2];
    const bool scrolls = content > view.GetHeight() + 0.5f;
    const float scroll = scrolls ? ui::beginScroll("##projects-grid", view, content) : 0.0f;
    if (!scrolls) {
        ImGui::PushClipRect(view.Min, view.Max, true);
    }
    const float y0 = view.Min.y + padTop - scroll;
    for (size_t i = 0; i < items.size(); ++i) {
        const int column = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const float x = left + (cardWidth + gap) * static_cast<float>(column);
        const float y = y0 + (cardHeight + gap) * static_cast<float>(row);
        const ImRect rect(ImVec2(x, y), ImVec2(x + cardWidth, y + cardHeight));
        if (rect.Max.y < view.Min.y || rect.Min.y > view.Max.y) {
            continue;
        }
        projectCard(dl, items[i], rect, library.busy(items[i].path), requests);
    }
    if (scrolls) {
        ui::endScroll();
    } else {
        ImGui::PopClipRect();
    }
}

void Ui::projectCard(ImDrawList* dl, const library::Summary& item, const ImRect& rect, bool busy,
                     UiRequests& requests) {
    ImGui::PushID(item.path.c_str());
    const float radius = pt(kCardRadius);
    const float pad = pt(kCardPad);
    const ImRect box(ImVec2(rect.Min.x + pad, rect.Min.y + pad), ImVec2(rect.Max.x - pad, rect.Max.y - pt(kCardText)));
    const float textTop = box.Max.y + pt(10.0f);
    const float nameCy = textTop + pt(12.0f);
    const float detailCy = textTop + pt(34.0f);
    const float dateCy = textTop + pt(53.0f);
    const bool menuOpen = m_projectMenu == item.path;

    // El botón del menú va antes que la ficha: así se queda con los toques que caen en él.
    const ImRect menuRect(ImVec2(rect.Max.x - pt(4.0f + 36.0f), nameCy - pt(17.0f)),
                          ImVec2(rect.Max.x - pt(4.0f), nameCy + pt(17.0f)));
    const ImGuiID menuId = ImGui::GetID("##menu");
    const Press menuPress = ui::pressable(menuId, menuRect, !busy);
    const ImGuiID cardId = ImGui::GetID("##card");
    const Press press = ui::pressable(cardId, rect, !busy);

    // La ficha: se aclara al tocarla.
    const float touch = ui::anim::follow(cardId + 7u, press.held ? 1.0f : (press.hovered ? 0.55f : 0.0f), 22.0f);
    dl->AddRectFilled(rect.Min, rect.Max, ui::mix(IM_COL32(255, 255, 255, 12), IM_COL32(255, 255, 255, 26), touch), radius);
    const float selected = ui::anim::follow(cardId + 8u, menuOpen ? 1.0f : 0.0f, 20.0f);
    ui::outline(dl, rect, radius, ui::mix(th::kControlBorder, th::kAccentBorder, selected),
                selected > 0.5f ? pt(1.5f) : ui::hairline());

    // La miniatura, entera dentro de su caja.
    const float boxRadius = radius - pad * 0.5f;
    dl->AddRectFilled(box.Min, box.Max, IM_COL32(0, 0, 0, 72), boxRadius);
    const GLuint texture = item.readable ? projectThumbnail(item) : 0;
    const float inset = pt(kThumbInset);
    const float availableWidth = std::max(1.0f, box.GetWidth() - inset * 2.0f);
    const float availableHeight = std::max(1.0f, box.GetHeight() - inset * 2.0f);
    if (item.readable && item.width > 0 && item.height > 0) {
        const float scale = std::min(availableWidth / static_cast<float>(item.width),
                                     availableHeight / static_cast<float>(item.height));
        const float w = std::max(pt(4.0f), static_cast<float>(item.width) * scale);
        const float h = std::max(pt(4.0f), static_cast<float>(item.height) * scale);
        const ImVec2 center = box.GetCenter();
        const ImRect image(ImVec2(ui::snap(center.x - w * 0.5f), ui::snap(center.y - h * 0.5f)),
                           ImVec2(ui::snap(center.x + w * 0.5f), ui::snap(center.y + h * 0.5f)));
        ui::shadow(dl, image, 0.0f, pt(14.0f), pt(4.0f), 0.4f);
        if (texture) {
            ui::checkerboard(dl, image, 0.0f, pt(6.0f));
            dl->AddImage(ui::parts::textureId(texture), image.Min, image.Max);
        } else {
            dl->AddRectFilled(image.Min, image.Max, IM_COL32(255, 255, 255, 20));
        }
        ui::outline(dl, image, 0.0f, IM_COL32(255, 255, 255, 30), ui::hairline());
        if (item.profile == ColorProfile::DisplayP3) {
            ui::parts::tag(dl, box.Max.x - pt(6.0f), box.Min.y + pt(17.0f), "P3", IM_COL32(0, 0, 0, 150),
                           IM_COL32(235, 235, 245, 220));
        }
    } else {
        // No se puede leer: el aviso en lugar de la miniatura.
        const ImVec2 center = box.GetCenter();
        ui::icon(dl, icon::kFileX, ImVec2(center.x, center.y - pt(10.0f)), 28.0f, th::kOrange);
        ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(center.x, center.y + pt(18.0f)), Align::Center,
                  th::kSecondaryLabel, "No se puede leer", box.GetWidth() - pt(16.0f));
    }
    if (busy) {
        dl->AddRectFilled(box.Min, box.Max, IM_COL32(0, 0, 0, 120), boxRadius);
        ui::spinner(dl, box.GetCenter(), pt(10.0f), th::kLabel);
    }

    // Los textos: el nombre, el tamaño y las capas, y la fecha y lo que ocupa.
    const float textLeft = rect.Min.x + pt(12.0f);
    const float textRight = rect.Max.x - pt(12.0f);
    const std::string name = displayName(item);
    ui::label(dl, Weight::SemiBold, th::kSubhead, ImVec2(textLeft, nameCy), Align::Left,
              item.name.empty() ? th::kSecondaryLabel : th::kLabel, name.c_str(), menuRect.Min.x - textLeft);
    char detail[96];
    if (item.readable) {
        std::snprintf(detail, sizeof(detail), "%d × %d px · %d %s", item.width, item.height, item.layers,
                      item.layers == 1 ? "capa" : "capas");
    } else {
        std::snprintf(detail, sizeof(detail), "%s", "Archivo dañado o de otra versión");
    }
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(textLeft, detailCy), Align::Left, th::kSecondaryLabel, detail,
              textRight - textLeft);
    const std::string when = ui::parts::dateText(item.modified) + " · " + canvasspec::formatBytes(static_cast<size_t>(item.bytes));
    ui::label(dl, Weight::Regular, th::kFootnote, ImVec2(textLeft, dateCy), Align::Left, th::kTertiaryLabel,
              when.c_str(), textRight - textLeft);
    ui::highlight(dl, menuId, menuRect, pt(10.0f), menuOpen, menuPress);
    ui::icon(dl, icon::kEllipsis, menuRect.GetCenter(), 18.0f, busy ? th::kDisabledLabel : th::kMutedIcon);

    if (menuPress.clicked) {
        if (menuOpen) {
            m_projectMenu.clear();
        } else {
            openProjectMenu(item.path, menuRect);
        }
    } else if (press.clicked) {
        m_projectMenu.clear();
        requests.project = {UiRequests::ProjectAction::Kind::Open, item.path, {}};
    } else if (!busy && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered() &&
               ImGui::IsMouseHoveringRect(rect.Min, rect.Max)) {
        // Clic derecho: el menú donde está el ratón.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        openProjectMenu(item.path, ImRect(mouse, mouse));
    }
    ImGui::PopID();
}

// -----------------------------------------------------------------------------
// Menú de un proyecto
// -----------------------------------------------------------------------------

void Ui::openProjectMenu(const std::string& path, const ImRect& anchor) {
    m_projectMenu = path;
    m_projectMenuShown = path;
    m_projectMenuAnchor = anchor;
}

void Ui::projectMenu(UiRequests& requests) {
    const bool open = !m_projectMenu.empty();
    const float p = ui::anim::followFrom(ImHashStr("##project-menu"), 0.0f, open ? 1.0f : 0.0f, open ? 22.0f : 26.0f);
    if (!open && p <= 0.002f) {
        m_projectMenuShown.clear();
        return;
    }
    const library::Summary* item = m_status.library ? m_status.library->find(m_projectMenuShown) : nullptr;
    if (!item) {
        m_projectMenu.clear();
        m_projectMenuShown.clear();
        return;
    }
    const Layout& L = m_layout;
    // Un toque fuera lo cierra (y no hace nada más).
    if (open) {
        const ImRect screen(ImVec2(0.0f, 0.0f), L.display);
        ui::beginSurface("##project-menu-outside", screen, true, true);
        const ImGuiID id = ImGui::GetID("##outside");
        ImGui::ItemAdd(screen, id);
        bool hovered = false;
        bool held = false;
        const ImGuiButtonFlags buttons = ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight;
        if (ImGui::ButtonBehavior(screen, id, &hovered, &held,
                                  buttons | ImGuiButtonFlags_PressedOnClick | ImGuiButtonFlags_NoNavFocus)) {
            m_projectMenu.clear();
        }
        ui::endSurface();
    }

    // Exportar: en la web se descarga; en Android va a Descargas (o, con otra fila, adonde se
    // elija); en escritorio se elige dónde.
    using Kind = UiRequests::ProjectAction::Kind;
#ifdef SDL_PLATFORM_EMSCRIPTEN
    const char* exportTitle = "Descargar";
    const char* exportGlyph = icon::kDownload;
    constexpr Kind kExportKind = Kind::Export;
    constexpr bool kExportTo = false;
#elif defined(SDL_PLATFORM_ANDROID)
    const char* exportTitle = "Exportar a Descargas";
    const char* exportGlyph = icon::kDownload;
    constexpr Kind kExportKind = Kind::Export;
    constexpr bool kExportTo = true;
#else
    const char* exportTitle = "Exportar…";
    const char* exportGlyph = icon::kFileOutput;
    constexpr Kind kExportKind = Kind::ExportTo;
    constexpr bool kExportTo = false;
#endif
    const int rows = 4 + (kExportTo ? 1 : 0);
    const float row = pt(kMenuRow);
    const float pad = pt(6.0f);
    const float divider = pt(9.0f);   // antes de Eliminar
    const float width = std::min(pt(kMenuWidth), L.right - L.left);
    const float height = pad * 2.0f + row * static_cast<float>(rows) + divider;
    const ImRect& anchor = m_projectMenuAnchor;
    float x = anchor.Max.x - width;
    float y = anchor.Max.y + pt(6.0f);
    bool above = false;
    if (y + height > L.bottom && anchor.Min.y - pt(6.0f) - height >= L.top) {
        y = anchor.Min.y - pt(6.0f) - height;
        above = true;
    }
    x = std::clamp(x, L.left, std::max(L.left, L.right - width));
    y = std::clamp(y, L.top, std::max(L.top, L.bottom - height));
    const ImRect rect(ImVec2(x, y), ImVec2(x + width, y + height));
    const float radius = pt(14.0f);

    ui::beginSurface("##project-menu", rect, open, true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::DrawMark mark = ui::mark(dl);
    ui::pushUnclipped(dl);
    ui::shadow(dl, rect, radius, pt(40.0f), pt(14.0f), 0.38f);
    ui::popUnclipped(dl);
    ui::glass(dl, rect, radius, th::kDialogTint);
    const float left = rect.Min.x + pad;
    const float right = rect.Max.x - pad;
    float top = rect.Min.y + pad;
    const bool readable = item->readable;
    const std::string path = item->path;
    const std::string name = item->name;
    auto next = [&]() {
        const ImRect r(ImVec2(left, top), ImVec2(right, top + row));
        top += row;
        return r;
    };
    if (menuItem(dl, "##rename", next(), icon::kPencil, "Renombrar", th::kLabel, open && readable)) {
        m_projectMenu.clear();
        m_dialogProject = path;
        m_dialogTitle = "Renombrar proyecto";
        m_dialogMessage.clear();
        copyText(name, m_renameBuffer, sizeof(m_renameBuffer));
        openDialog(Dialog::RenameProject, nullptr);
    }
    if (menuItem(dl, "##duplicate", next(), icon::kCopy, "Duplicar", th::kLabel, open && readable)) {
        m_projectMenu.clear();
        requests.project = {Kind::Duplicate, path, (name.empty() ? std::string("Sin nombre") : name) + " (copia)"};
    }
    if (menuItem(dl, "##export", next(), exportGlyph, exportTitle, th::kLabel, open)) {
        m_projectMenu.clear();
        requests.project = {kExportKind, path, name};
    }
    if (kExportTo && menuItem(dl, "##export-to", next(), icon::kFileOutput, "Exportar a…", th::kLabel, open)) {
        m_projectMenu.clear();
        requests.project = {Kind::ExportTo, path, name};
    }
    ui::separator(dl, left + pt(8.0f), right - pt(8.0f), top + divider * 0.5f, th::kRule);
    top += divider;
    if (menuItem(dl, "##remove", next(), icon::kTrash, "Eliminar", th::kRed, open)) {
        m_projectMenu.clear();
        m_dialogProject = path;
        m_dialogTitle = "¿Eliminar " + (name.empty() ? std::string("el proyecto") : "«" + name + "»") + "?";
#ifdef SDL_PLATFORM_EMSCRIPTEN
        m_dialogMessage = "Se borra de este navegador y no se puede deshacer. Si quieres conservar una copia, "
                          "descárgalo antes.";
#else
        m_dialogMessage = "Se borra de este dispositivo y no se puede deshacer. Si quieres conservar una copia, "
                          "expórtalo antes.";
#endif
        openDialog(Dialog::DeleteProject, nullptr);
    }
    // Sale de su botón.
    const float e = ui::anim::easeOutCubic(std::clamp(p, 0.0f, 1.0f));
    const ImVec2 origin(std::clamp(anchor.GetCenter().x, rect.Min.x, rect.Max.x), above ? rect.Max.y : rect.Min.y);
    ui::transform(mark, origin, 0.92f + 0.08f * e, ImVec2(0.0f, 0.0f), std::min(1.0f, p * 1.4f));
    ui::endSurface();
}
