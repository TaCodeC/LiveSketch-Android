// Enrutado de la entrada: qué va a la interfaz y qué al lienzo.
//
// - Lápiz: dibuja (con presión y goma). Para ImGui llega también como ratón emulado.
// - Dedos: un dedo mueve la vista y dos hacen zoom, o dibujan si está activado
//   "Dibujar con el dedo" (y entonces dejar el dedo quieto al empezar abre el
//   cuentagotas). Un toque con dos dedos deshace y con tres rehace. Mientras el lápiz
//   está cerca o tocando se ignoran (palma).
// - Ratón de verdad (escritorio): izquierdo dibuja (con Alt, cuentagotas), derecho o
//   central mueve, rueda zoom.
// - Con el cuentagotas de la barra lateral armado, la siguiente pulsación en el lienzo
//   elige un color en vez de dibujar.
// - Con la herramienta Selección o Transformar, el lápiz, un dedo (dibuje o no con el
//   dedo) y el botón izquierdo del ratón la manejan en vez de pintar. Con el ratón,
//   Mayús suma a la selección y Alt resta.
//
// Cada pulsación se decide una vez, en el punto donde empieza: si cae sobre la interfaz
// (una barra, un panel o, con un panel abierto, cualquier sitio: el toque lo cierra) es
// de la interfaz hasta que se suelta; si no, es del lienzo y ImGui no la ve.
#include "App/App.h"

#include "UI/Kit.h"

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
// pellizco (o de un toque con dos dedos) y se descarta.
constexpr uint64_t kPinchCancelMs = 250;
constexpr float kWheelZoomStep = 1.1f;
// Toque con dos o tres dedos: todos se levantan antes de este tiempo y ninguno se mueve
// más de esta distancia (pt).
constexpr uint64_t kTapMaxMs = 350;
constexpr float kTapSlopPoints = 12.0f;
// Dedo quieto este tiempo al empezar un trazo: cuentagotas.
constexpr uint64_t kLongPressMs = 450;

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

float App::pointsToPixels(float points) const {
    // Escala de la ventana: píxeles por punto.
    return points * std::max(SDL_GetWindowDisplayScale(m_window), 0.01f);
}

bool App::uiWantsPoint(float x, float y) const {
    if (!m_imguiReady) {
        return false;
    }
    if (!m_canvas.ready()) {
        return true;   // pantalla de inicio
    }
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.OpenPopupStack.Size > 0) {
        return true;
    }
    // Las barras, los paneles y los diálogos son ventanas de ImGui. Con un panel o un
    // diálogo abierto hay además una ventana que cubre la pantalla detrás de ellos.
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
    return m_pen.drawing || !m_fingers.empty() || m_mouseDrawing || m_mousePanning ||
           m_pick.source != PickSource::None || m_toolGesture.pointer != ToolPointer::None;
}

// -----------------------------------------------------------------------------
// Selección y Transformar
// -----------------------------------------------------------------------------

ToolView App::toolView() const {
    const float density = std::max(SDL_GetWindowPixelDensity(m_window), 0.01f);
    ToolView view;
    view.offset = m_camera.offset() / density;
    view.zoom = m_camera.zoom() / density;
    return view;
}

void App::beginToolGesture(ToolPointer pointer, SDL_FingerID finger, float x, float y) {
    endToolGesture(true);
    const SDL_Keymod mods = SDL_GetModState();
    SelectOp modifier = SelectOp::Replace;
    if (mods & SDL_KMOD_SHIFT) {
        modifier = SelectOp::Add;
    } else if (mods & SDL_KMOD_ALT) {
        modifier = SelectOp::Subtract;
    }
    m_toolGesture = {pointer, finger, SDL_GetTicks(), x, y};
    m_ui.toolPress(m_canvas, toolView(), ImVec2(x, y), modifier);
}

void App::moveToolGesture(float x, float y) {
    m_toolGesture.x = x;
    m_toolGesture.y = y;
    const bool constrain = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    m_ui.toolDrag(m_canvas, toolView(), ImVec2(x, y), constrain);
}

void App::endToolGesture(bool cancel) {
    if (m_toolGesture.pointer == ToolPointer::None) {
        return;
    }
    if (cancel) {
        m_ui.toolCancel(m_canvas);
    } else {
        m_ui.toolRelease(m_canvas, toolView(), ImVec2(m_toolGesture.x, m_toolGesture.y));
    }
    m_toolGesture = {};
}

void App::notifyStrokeBlocked(bool eraserTip) {
    switch (m_canvas.strokeBlock(eraserTip)) {
    case Canvas::StrokeBlock::Hidden:
        m_ui.notify("La capa activa está oculta", Notice::Warning);
        break;
    case Canvas::StrokeBlock::ClipBaseHidden:
        m_ui.notify("La capa recorta con una capa oculta", Notice::Warning);
        break;
    case Canvas::StrokeBlock::AlphaLocked:
        m_ui.notify("Con el alfa bloqueado no se puede borrar", Notice::Warning);
        break;
    case Canvas::StrokeBlock::None:
        break;
    }
}

bool App::beginCanvasStroke(glm::vec2 pixels, float pressure, bool eraserTip) {
    m_ui.prepareStroke(m_canvas, eraserTip);
    const glm::vec2 point = toCanvas(pixels);
    if (!m_canvas.beginStroke(point.x, point.y, pressure, eraserTip)) {
        notifyStrokeBlocked(eraserTip);
        return false;
    }
    m_ui.strokeStarted(m_canvas, eraserTip || m_canvas.brushSettings().eraser);
    return true;
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

    default:
        // Teclado incluido: los atajos y el botón atrás los maneja la interfaz.
        ImGui_ImplSDL3_ProcessEvent(&event);
        return false;
    }
}

// -----------------------------------------------------------------------------
// Cuentagotas
// -----------------------------------------------------------------------------

void App::beginPick(PickSource source, SDL_FingerID finger, float x, float y) {
    m_pick.source = source;
    m_pick.finger = finger;
    m_pick.x = x;
    m_pick.y = y;
    m_pick.valid = false;
    m_pick.dirty = true;
    samplePick();
}

void App::movePick(float x, float y) {
    m_pick.x = x;
    m_pick.y = y;
    m_pick.dirty = true;
}

void App::samplePick() {
    if (m_pick.source == PickSource::None || !m_pick.dirty || !m_canvas.ready()) {
        return;
    }
    m_pick.dirty = false;
    const glm::vec2 point = toCanvas(windowToPixels(m_pick.x, m_pick.y));
    m_pick.valid = m_canvas.pickColor(point.x, point.y, m_pick.rgb);
    m_ui.showPicker(ImVec2(m_pick.x, m_pick.y), m_pick.valid ? m_pick.rgb : nullptr,
                    m_pick.source == PickSource::Finger);
}

void App::endPick(bool apply) {
    if (m_pick.source == PickSource::None) {
        return;
    }
    samplePick();
    if (apply && m_pick.valid && m_canvas.ready()) {
        m_ui.pickColor(m_canvas, m_pick.rgb);
    }
    m_ui.hidePicker();
    m_ui.setEyedropperArmed(false);
    m_pick = {};
}

void App::checkLongPress() {
    if (!m_ui.drawWithFinger() || toolActive() || m_pick.source != PickSource::None || m_fingers.size() != 1 ||
        m_tap.fingers.size() != 1 || m_tap.moved || SDL_GetTicks() - m_tap.startMs < kLongPressMs) {
        return;
    }
    // El trazo que había empezado era en realidad el cuentagotas.
    if (m_fingerDrawing) {
        m_canvas.cancelStroke();
        m_fingerDrawing = false;
    }
    const FingerPoint finger = m_fingers.front();
    m_fingers.clear();
    m_gestureDistance = 0.0f;
    m_tap.candidate = false;
    const float density = SDL_GetWindowPixelDensity(m_window);
    beginPick(PickSource::Finger, finger.id, finger.position.x / density, finger.position.y / density);
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
        if (m_pick.source == PickSource::Pen) {
            movePick(m_pen.x, m_pen.y);
        }
        if (m_toolGesture.pointer == ToolPointer::Pen) {
            moveToolGesture(m_pen.x, m_pen.y);
        }
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
        if (m_ui.eyedropperArmed()) {
            beginPick(PickSource::Pen, 0, m_pen.x, m_pen.y);
            break;
        }
        if (toolActive()) {
            beginToolGesture(ToolPointer::Pen, 0, m_pen.x, m_pen.y);
            break;
        }
        m_pen.drawing = true;
        m_pen.strokePending = true;
        m_pen.eraser = event.ptouch.eraser;
        m_pen.pendingSample = true;
        break;

    case SDL_EVENT_PEN_UP:
        m_pen.down = false;
        m_pen.lastActiveMs = now;
        if (m_pick.source == PickSource::Pen) {
            endPick(true);
        }
        if (m_toolGesture.pointer == ToolPointer::Pen) {
            endToolGesture(false);
        }
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
    const glm::vec2 pixels = windowToPixels(m_pen.x, m_pen.y);
    if (m_pen.strokePending) {
        m_pen.strokePending = false;
        m_pen.drawing = beginCanvasStroke(pixels, m_pen.pressure, m_pen.eraser);
    } else {
        const glm::vec2 point = toCanvas(pixels);
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

void App::finishTapGesture(bool canceled) {
    const bool tap = !canceled && m_tap.candidate && m_tap.maxFingers >= 2 && m_tap.maxFingers <= 3 &&
                     SDL_GetTicks() - m_tap.startMs <= kTapMaxMs;
    if (tap && m_canvas.ready()) {
        // Los dedos movieron la vista un poco (pellizco): vuelve a como estaba.
        m_camera.restoreView(m_tap.view);
        m_ui.undoGesture(m_canvas, m_tap.maxFingers == 3);
    }
    m_tap = {};
}

void App::onFingerEvent(const SDL_Event& event) {
    if (!m_canvas.ready()) {
        return;
    }
    const SDL_TouchFingerEvent& touch = event.tfinger;
    const glm::vec2 position = fingerToPixels(touch.x, touch.y);
    const float density = SDL_GetWindowPixelDensity(m_window);
    const uint64_t now = SDL_GetTicks();
    auto finger = std::find_if(m_fingers.begin(), m_fingers.end(),
                               [&](const FingerPoint& f) { return f.id == touch.fingerID; });
    auto tapFinger = std::find_if(m_tap.fingers.begin(), m_tap.fingers.end(),
                                  [&](const TapGesture::Finger& f) { return f.id == touch.fingerID; });
    const bool picking = m_pick.source == PickSource::Finger && m_pick.finger == touch.fingerID;

    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: {
        if (penBlocksFingers() || m_pick.source == PickSource::Finger) {
            return;
        }
        if (m_tap.fingers.empty()) {
            // Primer dedo del gesto: si cae en la interfaz, es de ella.
            if (uiWantsPoint(position.x / density, position.y / density)) {
                return;
            }
            if (m_ui.eyedropperArmed()) {
                beginPick(PickSource::Finger, touch.fingerID, position.x / density, position.y / density);
                return;
            }
            m_tap = {};
            m_tap.candidate = true;
            m_tap.startMs = now;
            m_tap.view = m_camera.view();
        }
        m_tap.fingers.push_back({touch.fingerID, position});
        m_tap.maxFingers = std::max(m_tap.maxFingers, static_cast<int>(m_tap.fingers.size()));
        if (m_tap.fingers.size() > 3) {
            m_tap.candidate = false;
        }
        if (m_fingers.size() >= 2) {
            return;   // el tercer dedo solo cuenta para el toque
        }

        m_fingers.push_back({touch.fingerID, position});
        if (m_fingers.size() == 1) {
            if (toolActive()) {
                beginToolGesture(ToolPointer::Finger, touch.fingerID, position.x / density, position.y / density);
            } else if (m_ui.drawWithFinger()) {
                // La presión de un dedo no es fiable: se dibuja como con presión 1.
                m_fingerDrawing = beginCanvasStroke(position, 1.0f, false);
                m_fingerStrokeStartMs = now;
            }
        } else if (m_toolGesture.pointer == ToolPointer::Finger) {
            // Segundo dedo: como al dibujar, pronto es un pellizco (o deshacer) y lo que
            // hizo el primero no cuenta; si no, se termina.
            if (now - m_toolGesture.startMs < kPinchCancelMs) {
                endToolGesture(true);
            } else {
                endToolGesture(false);
                m_tap.candidate = false;
            }
        } else if (m_fingerDrawing) {
            // Segundo dedo: empieza un pellizco o un toque con dos dedos.
            if (now - m_fingerStrokeStartMs < kPinchCancelMs) {
                m_canvas.cancelStroke();
            } else {
                m_canvas.endStroke();
                m_tap.candidate = false;
            }
            m_fingerDrawing = false;
        }
        resetGestureReference();
        break;
    }

    case SDL_EVENT_FINGER_MOTION: {
        if (picking) {
            movePick(position.x / density, position.y / density);
            return;
        }
        if (tapFinger != m_tap.fingers.end() &&
            glm::length(position - tapFinger->start) > pointsToPixels(kTapSlopPoints)) {
            m_tap.moved = true;
            m_tap.candidate = false;
        }
        if (finger == m_fingers.end()) {
            return;
        }
        finger->position = position;
        if (m_toolGesture.pointer == ToolPointer::Finger && m_toolGesture.finger == touch.fingerID) {
            moveToolGesture(position.x / density, position.y / density);
            return;
        }
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
    case SDL_EVENT_FINGER_CANCELED: {
        const bool canceled = event.type == SDL_EVENT_FINGER_CANCELED;
        if (picking) {
            endPick(!canceled);
        }
        if (m_toolGesture.pointer == ToolPointer::Finger && m_toolGesture.finger == touch.fingerID) {
            endToolGesture(canceled);
        }
        if (finger != m_fingers.end()) {
            if (m_fingerDrawing) {
                if (canceled) {
                    m_canvas.cancelStroke();   // el sistema decidió que era la palma
                } else {
                    m_canvas.endStroke();
                }
                m_fingerDrawing = false;
            }
            m_fingers.erase(finger);
            // Con el dedo que queda, el movimiento sigue desde su posición actual, sin saltos.
            resetGestureReference();
        }
        if (tapFinger != m_tap.fingers.end()) {
            m_tap.fingers.erase(tapFinger);
            if (canceled) {
                m_tap.candidate = false;
            }
            if (m_tap.fingers.empty()) {
                finishTapGesture(false);
            }
        }
        break;
    }

    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// Ratón
// -----------------------------------------------------------------------------

void App::onMouseEvent(const SDL_Event& event) {
    if (!m_canvas.ready()) {
        return;
    }
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const glm::vec2 position = windowToPixels(event.button.x, event.button.y);
        if (event.button.button == SDL_BUTTON_LEFT) {
            // Con Selección, Alt resta en vez de abrir el cuentagotas.
            const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
            if (m_ui.eyedropperArmed() || (alt && !toolActive())) {
                beginPick(PickSource::Mouse, 0, event.button.x, event.button.y);
            } else if (toolActive()) {
                beginToolGesture(ToolPointer::Mouse, 0, event.button.x, event.button.y);
            } else {
                m_mouseDrawing = beginCanvasStroke(position, 1.0f, false);
            }
        } else if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
            m_mousePanning = true;
        }
        m_mouseLast = position;
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        const glm::vec2 position = windowToPixels(event.motion.x, event.motion.y);
        if (m_pick.source == PickSource::Mouse) {
            movePick(event.motion.x, event.motion.y);
        }
        if (m_mouseDrawing) {
            const glm::vec2 point = toCanvas(position);
            m_canvas.strokeTo(point.x, point.y, 1.0f);
        }
        if (m_toolGesture.pointer == ToolPointer::Mouse) {
            moveToolGesture(event.motion.x, event.motion.y);
        }
        if (m_mousePanning) {
            m_camera.pan(position - m_mouseLast);
        }
        m_mouseLast = position;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (m_pick.source == PickSource::Mouse) {
                endPick(true);
            } else if (m_toolGesture.pointer == ToolPointer::Mouse) {
                endToolGesture(false);
            } else if (m_mouseDrawing) {
                m_canvas.endStroke();
                m_mouseDrawing = false;
            }
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

void App::endGestures() {
    endPenStroke();
    endPick(false);
    endToolGesture(true);
    if (m_fingerDrawing) {
        m_canvas.endStroke();
        m_fingerDrawing = false;
    }
    m_fingers.clear();
    m_tap = {};
    m_gestureDistance = 0.0f;
    if (m_mouseDrawing) {
        m_canvas.endStroke();
        m_mouseDrawing = false;
    }
    m_mousePanning = false;
}
