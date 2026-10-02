#include "UI/CanvasForm.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// Caracteres que caben en un campo ("123456,78").
constexpr size_t kMaxText = 9;

std::string trimmed(const char* text) {
    std::string s(text);
    const size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

} // namespace

int CanvasForm::pixelWidth() const { return canvasspec::toPixels(m_width, m_unit, m_ppi); }

int CanvasForm::pixelHeight() const { return canvasspec::toPixels(m_height, m_unit, m_ppi); }

void CanvasForm::choose(double width, double height, LengthUnit unit, float ppi) {
    commit();
    m_width = width;
    m_height = height;
    m_unit = unit;
    m_ppi = std::clamp(ppi, canvasspec::kMinPpi, canvasspec::kMaxPpi);
    if (m_locked && m_height > 0.0) {
        m_ratio = m_width / m_height;
    }
}

void CanvasForm::setUnit(LengthUnit unit) {
    commit();
    m_width = canvasspec::convertLength(m_width, m_unit, unit, m_ppi);
    m_height = canvasspec::convertLength(m_height, m_unit, unit, m_ppi);
    m_unit = unit;
}

void CanvasForm::setPpi(float ppi) {
    if (std::isfinite(ppi)) {
        m_ppi = std::clamp(ppi, canvasspec::kMinPpi, canvasspec::kMaxPpi);
    }
}

void CanvasForm::setWidth(double width) {
    m_width = width;
    if (m_locked && m_ratio > 0.0) {
        m_height = width / m_ratio;
    }
}

void CanvasForm::setHeight(double height) {
    m_height = height;
    if (m_locked) {
        m_width = height * m_ratio;
    }
}

void CanvasForm::swap() {
    commit();
    std::swap(m_width, m_height);
    if (m_locked && m_ratio > 0.0) {
        m_ratio = 1.0 / m_ratio;
    }
}

void CanvasForm::setLocked(bool locked) {
    m_locked = locked;
    if (locked && m_height > 0.0) {
        m_ratio = m_width / m_height;
    }
}

void CanvasForm::begin(Field field) {
    commit();
    if (field == Field::None) {
        return;
    }
    m_field = field;
    m_before[0] = m_width;
    m_before[1] = m_height;
    m_before[2] = m_ppi;
    switch (field) {
    case Field::Width:
        m_text = canvasspec::formatNumber(m_width, canvasspec::unitDecimals(m_unit));
        break;
    case Field::Height:
        m_text = canvasspec::formatNumber(m_height, canvasspec::unitDecimals(m_unit));
        break;
    default:
        m_text = canvasspec::formatNumber(m_ppi, 1);
        break;
    }
    m_fresh = true;
}

bool CanvasForm::acceptsComma() const {
    return m_field == Field::Ppi || ((m_field == Field::Width || m_field == Field::Height) && m_unit != LengthUnit::Pixels);
}

int CanvasForm::decimals() const { return m_field == Field::Ppi ? 1 : canvasspec::unitDecimals(m_unit); }

void CanvasForm::type(char c) {
    if (m_field == Field::None) {
        return;
    }
    if (m_fresh) {
        m_text.clear();
        m_fresh = false;
    }
    const size_t comma = m_text.find(',');
    if (c == ',' || c == '.') {
        if (!acceptsComma() || comma != std::string::npos) {
            return;
        }
        if (m_text.empty()) {
            m_text = "0";
        }
        m_text += ',';
    } else if (c >= '0' && c <= '9') {
        if (comma != std::string::npos && static_cast<int>(m_text.size() - comma - 1) >= decimals()) {
            return;
        }
        if (m_text == "0") {
            m_text.clear();   // sin ceros delante
        }
        m_text += c;
    } else {
        return;
    }
    if (m_text.size() > kMaxText) {
        m_text.pop_back();
        return;
    }
    applyText();
}

void CanvasForm::erase() {
    if (m_field == Field::None) {
        return;
    }
    if (m_fresh) {
        m_text.clear();
        m_fresh = false;
    } else if (!m_text.empty()) {
        m_text.pop_back();
    }
    applyText();
}

void CanvasForm::applyText() {
    double value = 0.0;
    if (!canvasspec::parseNumber(m_text, &value) || !(value > 0.0)) {
        return;
    }
    switch (m_field) {
    case Field::Width:
        setWidth(value);
        break;
    case Field::Height:
        setHeight(value);
        break;
    case Field::Ppi:
        setPpi(static_cast<float>(value));
        break;
    case Field::None:
        break;
    }
}

void CanvasForm::restore() {
    m_width = m_before[0];
    m_height = m_before[1];
    m_ppi = static_cast<float>(m_before[2]);
}

bool CanvasForm::commit() {
    if (m_field == Field::None) {
        return true;
    }
    bool ok = true;
    if (!m_fresh) {
        double value = 0.0;
        ok = canvasspec::parseNumber(m_text, &value) && value > 0.0;
        if (!ok) {
            restore();
        }
    }
    m_field = Field::None;
    m_text.clear();
    m_fresh = false;
    return ok;
}

void CanvasForm::cancel() {
    if (m_field == Field::None) {
        return;
    }
    restore();
    m_field = Field::None;
    m_text.clear();
    m_fresh = false;
}

CanvasSpec CanvasForm::spec() const {
    CanvasSpec spec;
    spec.width = pixelWidth();
    spec.height = pixelHeight();
    spec.ppi = m_ppi;
    spec.unit = m_unit;
    spec.background.visible = background != Background::Transparent;
    if (background == Background::Color) {
        std::copy(color, color + 3, spec.background.color);
    }
    spec.name = trimmed(name);
    return spec;
}

canvasspec::SavedSize CanvasForm::size() const {
    canvasspec::SavedSize size;
    size.width = m_width;
    size.height = m_height;
    size.unit = m_unit;
    size.ppi = m_ppi;
    return size;
}
