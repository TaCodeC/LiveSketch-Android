#include "UI/Anim.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ui::anim {

namespace {

struct State {
    float value = 0.0f;
    float velocity = 0.0f;
    unsigned lastFrame = 0;
};

// Un frame más largo que esto (el bucle estaba dormido) cuenta como este.
constexpr float kMaxStep = 1.0f / 20.0f;
// Los valores que no se piden durante tantos frames se olvidan.
constexpr unsigned kForgetFrames = 600;

std::unordered_map<ImGuiID, State> g_states;
unsigned g_frame = 1;
float g_dt = 1.0f / 60.0f;
double g_time = 0.0;
bool g_moving = false;       // algo se movió en el frame en curso
bool g_movedLast = false;    // ... y en el anterior

State& state(ImGuiID id, float initial, bool* created) {
    auto [it, inserted] = g_states.try_emplace(id);
    if (inserted) {
        it->second.value = initial;
    }
    if (created) {
        *created = inserted;
    }
    it->second.lastFrame = g_frame;
    return it->second;
}

} // namespace

void newFrame(float dt) {
    g_dt = std::clamp(dt, 0.0f, kMaxStep);
    g_time += static_cast<double>(dt);
    g_movedLast = g_moving;
    g_moving = false;
    ++g_frame;
    if (g_frame % 120 == 0) {
        for (auto it = g_states.begin(); it != g_states.end();) {
            it = g_frame - it->second.lastFrame > kForgetFrames ? g_states.erase(it) : std::next(it);
        }
    }
}

bool active() { return g_moving || g_movedLast; }
float dt() { return g_dt; }
double time() { return g_time; }

float followFrom(ImGuiID id, float initial, float target, float speed, float epsilon) {
    State& s = state(id, initial, nullptr);
    const float difference = target - s.value;
    if (std::fabs(difference) <= epsilon) {
        s.value = target;
        return s.value;
    }
    s.value += difference * (1.0f - std::exp(-speed * g_dt));
    if (std::fabs(target - s.value) <= epsilon) {
        s.value = target;
    }
    g_moving = true;
    return s.value;
}

float follow(ImGuiID id, float target, float speed, float epsilon) {
    return followFrom(id, target, target, speed, epsilon);
}

float spring(ImGuiID id, float target, float frequency, float damping, float epsilon) {
    State& s = state(id, target, nullptr);
    const float difference = target - s.value;
    if (std::fabs(difference) <= epsilon && std::fabs(s.velocity) <= epsilon * 10.0f) {
        s.value = target;
        s.velocity = 0.0f;
        return s.value;
    }
    // Integración semiimplícita en pasos cortos: estable con cualquier dt.
    const float omega = 2.0f * 3.14159265f * frequency;
    float remaining = g_dt;
    while (remaining > 0.0f) {
        const float h = std::min(remaining, 1.0f / 240.0f);
        const float acceleration = omega * omega * (target - s.value) - 2.0f * damping * omega * s.velocity;
        s.velocity += acceleration * h;
        s.value += s.velocity * h;
        remaining -= h;
    }
    g_moving = true;
    return s.value;
}

void set(ImGuiID id, float value) {
    State& s = state(id, value, nullptr);
    s.value = value;
    s.velocity = 0.0f;
}

void keepAlive() { g_moving = true; }

float value(ImGuiID id, float fallback) {
    const auto it = g_states.find(id);
    return it == g_states.end() ? fallback : it->second.value;
}

} // namespace ui::anim
