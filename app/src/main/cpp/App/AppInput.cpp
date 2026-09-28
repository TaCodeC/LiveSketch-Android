// Enrutado de la entrada: qué va a ImGui y qué al lienzo.
//
// - Lápiz: dibuja (con presión y goma). Para ImGui llega también como ratón emulado.
// - Dedos: un dedo mueve la vista y dos hacen zoom, o dibujan si está activado
//   "Dibujar con el dedo". Mientras el lápiz está cerca o tocando se ignoran (palma).
// - Ratón de verdad (escritorio): izquierdo dibuja, derecho o central mueve, rueda zoom.
//
// Cada pulsación se decide una vez, en el punto donde empieza: si cae sobre una ventana
// de ImGui, o hay un popup abierto, es de la interfaz hasta que se suelta; si no, es del
// lienzo y ImGui no la ve.
#include "App/App.h"

#include <glm/geometric.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace {

// Tras levantar o alejar el lápiz, los dedos siguen ignorados un momento (palma).
constexpr uint64_t kPalmGraceMs = 300;
// Si el segundo dedo llega tan pronto, el trazo con el primer dedo era el inicio de un
// pellizco y se descarta.
constexpr uint64_t kPinchCancelMs = 250;
constexpr float kWheelZoomStep = 1.1f;

bool isEmulatedMouse(SDL_MouseID which) {
    return which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID;
}

} // namespace

glm::vec2 App::windowToPixels(float x, float y) const {
    const float density = SDL_GetWindowPixelDensity(m_window);
    return {x * density, y * density};
}

glm::vec2 App::fingerToPixels(float x, float y) const {
    return {x * static_cast<float>(m_pixelWidth), y * static_cast<float>(m_pixelHeight)};
}

bool App::uiWantsPoint(float x, float y) const {
    if (!m_imguiReady) {
        return false;
    }
    if (!m_canvas.ready()) {
        return true;   // pantalla de inicio
    }
    // Con un combo, un menú o un diálogo abierto, el toque es para la interfaz (fuera
    // de un popup, lo cierra) y no debe pintar.
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.OpenPopupStack.Size > 0) {
        return true;
    }
    ImGuiWindow* hovered = nullptr;
    ImGui::FindHoveredWindowEx(ImVec2(x, y), false, &hovered, nullptr);
    return hovered != nullptr;
}

bool App::penBlocksFingers() const {
    if (m_pen.down || m_pen.inProximity) {
        return true;
    }
    return m_pen.lastActiveMs != 0 && SDL_GetTicks() - m_pen.lastActiveMs < kPalmGraceMs;
}

bool App::canvasInteractionActive() const {
    return m_pen.drawing || !m_fingers.empty() || m_mouseDrawing || m_mousePanning;
}

void App::notifyHiddenLayer() {
    if (m_canvas.ready() && !m_canvas.layers().active().visible) {
        m_menu.notify("La capa activa está oculta");
    }
}

bool App::routeEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const uint32_t bit = 1u << event.button.button;
        if (uiWantsPoint(event.button.x, event.button.y)) {
            m_uiMouseButtons |= bit;
            ImGui_ImplSDL3_ProcessEvent(&event);
            return true;
        }
        if (!isEmulatedMouse(event.button.which)) {
            onMouseEvent(event);
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const uint32_t bit = 1u << event.button.button;
        if (m_uiMouseButtons & bit) {
            m_uiMouseButtons &= ~bit;
            ImGui_ImplSDL3_ProcessEvent(&event);
            return true;
        }
        if (!isEmulatedMouse(event.button.which)) {
            onMouseEvent(event);
        }
        return false;
    }
    case SDL_EVENT_MOUSE_MOTION:
        // Mientras el lienzo tiene un puntero pulsado, ImGui no ve el movimiento (así no
        // resalta botones al pasar por encima dibujando).
        if (m_uiMouseButtons != 0 || !canvasInteractionActive()) {
            ImGui_ImplSDL3_ProcessEvent(&event);
        }
        if (!isEmulatedMouse(event.motion.which)) {
            onMouseEvent(event);
        }
        return m_uiMouseButtons != 0;
    case SDL_EVENT_MOUSE_WHEEL:
        if (uiWantsPoint(event.wheel.mouse_x, event.wheel.mouse_y)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            return true;
        }
        if (!isEmulatedMouse(event.wheel.which)) {
            onMouseEvent(event);
        }
        return false;

    case SDL_EVENT_PEN_PROXIMITY_IN:
    case SDL_EVENT_PEN_PROXIMITY_OUT:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_AXIS:
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
        onPenEvent(event);
        return false;

    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        onFingerEvent(event);
        return false;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        ImGui_ImplSDL3_ProcessEvent(&event);
        onKeyEvent(event);
        return true;

    default:
        ImGui_ImplSDL3_ProcessEvent(&event);
        return false;
    }
}

// -----------------------------------------------------------------------------
// Lápiz
// -----------------------------------------------------------------------------

void App::onPenEvent(const SDL_Event& event) {
    const uint64_t now = SDL_GetTicks();
    switch (event.type) {
    case SDL_EVENT_PEN_PROXIMITY_IN:
        m_pen.inProximity = true;
        m_pen.lastActiveMs = now;
        break;
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        m_pen.inProximity = false;
        m_pen.lastActiveMs = now;
        break;

    case SDL_EVENT_PEN_MOTION:
        // La muestra anterior ya tiene su presión: se aplica antes de pisarla.
        flushPenSample();
        m_pen.x = event.pmotion.x;
        m_pen.y = event.pmotion.y;
        m_pen.pendingSample = m_pen.drawing;
        m_pen.lastActiveMs = now;
        break;

    case SDL_EVENT_PEN_AXIS:
        if (event.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
            m_pen.pressure = event.paxis.value;
        }
        break;

    case SDL_EVENT_PEN_DOWN:
        m_pen.down = true;
        m_pen.lastActiveMs = now;
        m_pen.x = event.ptouch.x;
        m_pen.y = event.ptouch.y;
        if (!m_canvas.ready() || uiWantsPoint(m_pen.x, m_pen.y)) {
            break;   // lo maneja ImGui con el ratón emulado
        }
        // El lápiz manda: lo que estuvieran haciendo los dedos (la palma) se termina.
        endGestures();
        m_pen.drawing = true;
        m_pen.strokePending = true;
        m_pen.eraser = event.ptouch.eraser;
        m_pen.pendingSample = true;
        break;

    case SDL_EVENT_PEN_UP:
        m_pen.down = false;
        m_pen.lastActiveMs = now;
        endPenStroke();
        break;

    default:
        break;
    }
}

void App::flushPenSample() {
    if (!m_pen.pendingSample) {
        return;
    }
    m_pen.pendingSample = false;
    if (!m_pen.drawing) {
        return;
    }
    const glm::vec2 point = toCanvas(windowToPixels(m_pen.x, m_pen.y));
    if (m_pen.strokePending) {
        m_pen.strokePending = false;
        m_pen.drawing = m_canvas.beginStroke(point.x, point.y, m_pen.pressure, m_pen.eraser);
        if (!m_pen.drawing) {
            notifyHiddenLayer();
        }
    } else {
        m_canvas.strokeTo(point.x, point.y, m_pen.pressure);
    }
}

void App::endPenStroke() {
    flushPenSample();
    if (m_pen.drawing) {
        m_canvas.endStroke();
        m_pen.drawing = false;
    }
}

// -----------------------------------------------------------------------------
// Dedos
// -----------------------------------------------------------------------------

void App::resetGestureReference() {
    if (m_fingers.empty()) {
        m_gestureDistance = 0.0f;
        return;
    }
    glm::vec2 center{0.0f};
    for (const FingerPoint& finger : m_fingers) {
        center += finger.position;
    }
    m_gestureCenter = center / static_cast<float>(m_fingers.size());
    m_gestureDistance = m_fingers.size() >= 2
                            ? glm::length(m_fingers[0].position - m_fingers[1].position)
                            : 0.0f;
}

void App::onFingerEvent(const SDL_Event& event) {
    if (!m_canvas.ready()) {
        return;
    }
    const SDL_TouchFingerEvent& touch = event.tfinger;
    const glm::vec2 position = fingerToPixels(touch.x, touch.y);
    auto finger = std::find_if(m_fingers.begin(), m_fingers.end(),
                               [&](const FingerPoint& f) { return f.id == touch.fingerID; });
    const uint64_t now = SDL_GetTicks();

    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: {
        if (penBlocksFingers() || m_fingers.size() >= 2) {
            return;
        }
        int windowWidth = 0;
        int windowHeight = 0;
        SDL_GetWindowSize(m_window, &windowWidth, &windowHeight);
        if (uiWantsPoint(touch.x * static_cast<float>(windowWidth), touch.y * static_cast<float>(windowHeight))) {
            return;
        }

        m_fingers.push_back({touch.fingerID, position});
        if (m_fingers.size() == 1) {
            if (m_ui.drawWithFinger) {
                // La presión de un dedo no es fiable: se dibuja como con presión 1.
                const glm::vec2 point = toCanvas(position);
                m_fingerDrawing = m_canvas.beginStroke(point.x, point.y, 1.0f);
                m_fingerStrokeStartMs = now;
                if (!m_fingerDrawing) {
                    notifyHiddenLayer();
                }
            }
        } else if (m_fingerDrawing) {
            // Segundo dedo: empieza un pellizco.
            if (now - m_fingerStrokeStartMs < kPinchCancelMs) {
                m_canvas.cancelStroke();
            } else {
                m_canvas.endStroke();
            }
            m_fingerDrawing = false;
        }
        resetGestureReference();
        break;
    }

    case SDL_EVENT_FINGER_MOTION: {
        if (finger == m_fingers.end()) {
            return;
        }
        finger->position = position;
        if (m_fingerDrawing) {
            const glm::vec2 point = toCanvas(position);
            m_canvas.strokeTo(point.x, point.y, 1.0f);
            return;
        }

        const glm::vec2 previousCenter = m_gestureCenter;
        const float previousDistance = m_gestureDistance;
        resetGestureReference();
        m_camera.pan(m_gestureCenter - previousCenter);
        if (m_fingers.size() >= 2 && previousDistance > 0.0f && m_gestureDistance > 0.0f) {
            m_camera.zoomAt(m_gestureCenter, m_gestureDistance / previousDistance);
        }
        break;
    }

    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (finger == m_fingers.end()) {
            return;
        }
        if (m_fingerDrawing) {
            if (event.type == SDL_EVENT_FINGER_CANCELED) {
                m_canvas.cancelStroke();   // el sistema decidió que era la palma
            } else {
                m_canvas.endStroke();
            }
            m_fingerDrawing = false;
        }
        m_fingers.erase(finger);
        // Con el dedo que queda, el movimiento sigue desde su posición actual, sin saltos.
        resetGestureReference();
        break;

    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// Ratón y teclado
// -----------------------------------------------------------------------------

void App::onMouseEvent(const SDL_Event& event) {
    if (!m_canvas.ready()) {
        return;
    }
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const glm::vec2 position = windowToPixels(event.button.x, event.button.y);
        if (event.button.button == SDL_BUTTON_LEFT) {
            const glm::vec2 point = toCanvas(position);
            m_mouseDrawing = m_canvas.beginStroke(point.x, point.y, 1.0f);
            if (!m_mouseDrawing) {
                notifyHiddenLayer();
            }
        } else if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
            m_mousePanning = true;
        }
        m_mouseLast = position;
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        const glm::vec2 position = windowToPixels(event.motion.x, event.motion.y);
        if (m_mouseDrawing) {
            const glm::vec2 point = toCanvas(position);
            m_canvas.strokeTo(point.x, point.y, 1.0f);
        }
        if (m_mousePanning) {
            m_camera.pan(position - m_mouseLast);
        }
        m_mouseLast = position;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT && m_mouseDrawing) {
            m_canvas.endStroke();
            m_mouseDrawing = false;
        } else if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
            m_mousePanning = false;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float steps = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            steps = -steps;
        }
        m_camera.zoomAt(windowToPixels(event.wheel.mouse_x, event.wheel.mouse_y), std::pow(kWheelZoomStep, steps));
        break;
    }
    default:
        break;
    }
}

void App::onKeyEvent(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
        return;
    }
    if (event.key.key == SDLK_AC_BACK) {
        m_menu.askExit();
    }
}

void App::endGestures() {
    endPenStroke();
    if (m_fingerDrawing) {
        m_canvas.endStroke();
        m_fingerDrawing = false;
    }
    m_fingers.clear();
    m_gestureDistance = 0.0f;
    if (m_mouseDrawing) {
        m_canvas.endStroke();
        m_mouseDrawing = false;
    }
    m_mousePanning = false;
}
