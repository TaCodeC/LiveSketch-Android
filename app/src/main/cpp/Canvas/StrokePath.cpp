#include "Canvas/StrokePath.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRadiansPerDegree = kPi / 180.0f;

// Estabilización al máximo: el trazo va 36 puntos de pantalla por detrás del puntero.
constexpr float kMaxPull = 36.0f;
// Afinado al máximo: 40 píxeles más 16 veces el radio del pincel.
constexpr float kTaperBase = 40.0f;
constexpr float kTaperPerRadius = 16.0f;
// Por debajo de este radio el sello no encoge más: se aclara según su área.
constexpr float kMinDabRadius = 0.5f;
// Límites del radio de los pinceles de antes.
constexpr float kClassicMinRadius = 1.0f;
constexpr float kClassicMaxRadius = 200.0f;

uint32_t mixBits(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Número al azar en [0, 1) del sello `index` para el uso `channel`. Siempre el mismo:
// un sello provisional y el definitivo que lo sustituye salen iguales.
float random(uint32_t seed, uint32_t index, uint32_t channel) {
    const uint32_t bits = mixBits(seed ^ mixBits(index * 0x9E3779B1U + channel * 0x85EBCA77U));
    return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f);
}

} // namespace

float StrokePath::Taper::factor(float s, float length) const {
    auto curve = [this](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return tip + (1.0f - tip) * (1.0f - (1.0f - x) * (1.0f - x));
    };
    float f = 1.0f;
    if (start > 0.0f && s < start) {
        f = std::min(f, curve(s / start));
    }
    if (end > 0.0f && length - s < end) {
        f = std::min(f, curve((length - s) / end));
    }
    return f;
}

void StrokePath::begin(const BrushParams& params, const Settings& settings, float x, float y, float pressure) {
    m_params = params;
    brushes::sanitize(m_params);
    m_settings = settings;
    m_settings.radius = std::max(settings.radius, 0.0f);
    m_settings.flow = std::clamp(settings.flow, 0.0f, 1.0f);
    m_settings.pixelsPerPoint = std::max(settings.pixelsPerPoint, 0.0f);

    pressure = std::clamp(pressure, 0.0f, 1.0f);
    m_path.clear();
    m_path.push_back({x, y, pressure, 0.0f});
    m_rawX = x;
    m_rawY = y;
    m_rawPressure = pressure;
    m_pull = m_params.streamline * kMaxPull * m_settings.pixelsPerPoint;
    const float taperLength = kTaperBase + kTaperPerRadius * m_settings.radius;
    m_startTaper = m_params.taperStart * taperLength;
    m_endTaper = m_params.taperEnd * taperLength;
    m_active = true;
    m_finished = false;
    m_final = {};
    m_finalDabs.clear();
    m_provisional.clear();
    emit();
}

void StrokePath::moveTo(float x, float y, float pressure) {
    if (!m_active || m_finished) {
        return;
    }
    pressure = std::clamp(pressure, 0.0f, 1.0f);
    m_rawX = x;
    m_rawY = y;
    m_rawPressure = pressure;
    if (m_pull > 0.0f) {
        // Estabilización "de cuerda": el trazo solo avanza cuando el puntero se aleja más
        // de m_pull, y entonces lo sigue en línea recta hasta quedar a esa distancia.
        const Point& last = m_path.back();
        const float dx = x - last.x;
        const float dy = y - last.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= m_pull) {
            return;
        }
        const float f = (distance - m_pull) / distance;
        x = last.x + dx * f;
        y = last.y + dy * f;
    }
    appendPoint(x, y, pressure);
    emit();
}

void StrokePath::reshape(std::span<const glm::vec2> points, float pressure) {
    if (!m_active || m_finished || points.empty()) {
        return;
    }
    pressure = std::clamp(pressure, 0.0f, 1.0f);
    m_path.clear();
    m_path.push_back({points[0].x, points[0].y, pressure, 0.0f});
    for (size_t i = 1; i < points.size(); ++i) {
        appendPoint(points[i].x, points[i].y, pressure);
    }
    m_rawX = points.back().x;
    m_rawY = points.back().y;
    m_rawPressure = pressure;
    m_pull = 0.0f;
    m_final = {};
    m_finalDabs.clear();
    m_provisional.clear();
    emit();
}

void StrokePath::finish() {
    if (!m_active || m_finished) {
        return;
    }
    if (m_pull > 0.0f) {
        appendPoint(m_rawX, m_rawY, m_rawPressure);   // alcanza al puntero en línea recta
    }
    m_finished = true;
    emit();
}

void StrokePath::takeFinal(std::vector<Dab>& out) {
    out.insert(out.end(), m_finalDabs.begin(), m_finalDabs.end());
    m_finalDabs.clear();
}

void StrokePath::appendPoint(float x, float y, float pressure) {
    const Point last = m_path.back();
    const float dx = x - last.x;
    const float dy = y - last.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.0f)) {
        // Sin moverse: cuenta la presión nueva, como hacían los pinceles de antes.
        m_path.back().pressure = pressure;
        return;
    }
    m_path.push_back({x, y, pressure, last.s + length});
}

void StrokePath::emit() {
    const float length = m_path.back().s;
    const float nominal = m_startTaper + m_endTaper;
    // Mientras el trazo es más corto que los dos afinados juntos, los dos se acortan en
    // proporción y la punta engorda: un toque deja un punto entero y un trazo corto no
    // desaparece.
    const float amount = nominal > 0.0f ? std::min(1.0f, length / nominal) : 1.0f;
    Taper taper;
    taper.start = m_startTaper * amount;
    taper.end = m_endTaper * amount;
    taper.tip = 1.0f + (m_params.taperTip - 1.0f) * amount;
    const bool settled = amount >= 1.0f;

    m_provisional.clear();
    Cursor cursor = m_final;
    bool final = true;
    while (cursor.s <= length) {
        // Siguiendo el trazo hace falta saber hacia dónde sigue.
        if (m_params.followStroke && !m_finished && cursor.s >= length) {
            break;
        }
        // Un sello ya no cambia cuando el afinado del final no puede alcanzarlo: el trazo
        // solo crece. Como `s` sube, a partir del primer provisional todos lo son.
        const bool settledHere = m_finished || !tapered() || (settled && cursor.s <= length - m_endTaper);
        if (!settledHere) {
            final = false;
        }
        const Sample sample = sampleAt(cursor);
        stamp(cursor, sample, taper, length, final ? m_finalDabs : m_provisional);
        if (final) {
            m_final = cursor;
        }
    }
    ++m_revision;
}

StrokePath::Sample StrokePath::sampleAt(Cursor& cursor) const {
    const size_t count = m_path.size();
    if (count == 1) {
        const Point& p = m_path.front();
        return {p.x, p.y, p.pressure, 1.0f, 0.0f, false};
    }
    while (cursor.segment + 2 < count && m_path[cursor.segment + 1].s <= cursor.s) {
        ++cursor.segment;
    }
    const Point& a = m_path[cursor.segment];
    const Point& b = m_path[cursor.segment + 1];
    const float span = b.s - a.s;
    const float f = std::clamp((cursor.s - a.s) / span, 0.0f, 1.0f);
    return {a.x + (b.x - a.x) * f,
            a.y + (b.y - a.y) * f,
            a.pressure + (b.pressure - a.pressure) * f,
            (b.x - a.x) / span,
            (b.y - a.y) / span,
            true};
}

void StrokePath::stamp(Cursor& cursor, const Sample& sample, const Taper& taper, float length,
                       std::vector<Dab>& out) const {
    const BrushParams& p = m_params;
    const float pressure = std::clamp(sample.pressure, 0.0f, 1.0f);
    const float sizeFactor = 1.0f - p.pressureSize * (1.0f - pressure);
    const float alphaFactor = 1.0f - p.pressureOpacity * (1.0f - pressure);
    const float radius = m_settings.radius * sizeFactor * taper.factor(cursor.s, length);

    float angle = p.angle * kRadiansPerDegree;
    if (p.followStroke) {
        if (sample.hasDir) {
            if (!cursor.hasDir) {
                cursor.dirX = sample.dirX;
                cursor.dirY = sample.dirY;
                cursor.hasDir = true;
            } else {
                // La dirección gira poco a poco: el ruido del lápiz no hace temblar la punta.
                const float smoothing = std::max(2.0f, 0.3f * m_settings.radius);
                const float k = 1.0f - std::exp(-(cursor.s - cursor.lastS) / smoothing);
                const float x = cursor.dirX + (sample.dirX - cursor.dirX) * k;
                const float y = cursor.dirY + (sample.dirY - cursor.dirY) * k;
                const float norm = std::sqrt(x * x + y * y);
                if (norm > 1e-4f) {
                    cursor.dirX = x / norm;
                    cursor.dirY = y / norm;
                } else {
                    cursor.dirX = sample.dirX;
                    cursor.dirY = sample.dirY;
                }
            }
        }
        if (cursor.hasDir) {
            angle += std::atan2(cursor.dirY, cursor.dirX);
        }
    }

    const int count = std::max(1, static_cast<int>(std::lround(p.count)));
    for (int i = 0; i < count; ++i) {
        const uint32_t index = cursor.index++;
        const uint32_t seed = m_settings.seed;
        Dab dab;
        dab.x = sample.x;
        dab.y = sample.y;
        dab.radius = radius * (1.0f - p.sizeJitter * random(seed, index, 0));
        dab.alpha = m_settings.flow * alphaFactor * (1.0f - p.opacityJitter * random(seed, index, 1));
        dab.angle = angle + p.rotationJitter * kPi * (2.0f * random(seed, index, 2) - 1.0f);
        dab.distance = cursor.s;
        if (p.scatter > 0.0f) {
            // Repartido por igual en un círculo de `scatter` diámetros de radio.
            const float distance = p.scatter * 2.0f * radius * std::sqrt(random(seed, index, 3));
            const float direction = 2.0f * kPi * random(seed, index, 4);
            dab.x += distance * std::cos(direction);
            dab.y += distance * std::sin(direction);
        }
        if (p.classic) {
            dab.radius = std::clamp(dab.radius, kClassicMinRadius, kClassicMaxRadius);
        } else if (dab.radius < kMinDabRadius) {
            const float f = dab.radius / kMinDabRadius;
            dab.alpha *= f * f;
            dab.radius = kMinDabRadius;
        }
        out.push_back(dab);
    }

    const float spacingRadius =
        p.classic ? std::clamp(radius, kClassicMinRadius, kClassicMaxRadius) : std::max(radius, kMinDabRadius);
    cursor.lastS = cursor.s;
    cursor.s += std::max(p.spacing * 2.0f * spacingRadius, p.minSpacing);
}
