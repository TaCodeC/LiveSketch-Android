#include "Canvas/History.h"

void History::setLimits(size_t maxBytes, int maxSteps) {
    m_maxBytes = maxBytes;
    m_maxSteps = maxSteps > 0 ? maxSteps : 1;
    enforceLimits();
}

void History::clear() {
    m_steps.clear();
    m_cursor = 0;
    m_bytes = 0;
}

void History::push(HistoryStep step) {
    while (m_steps.size() > m_cursor) {
        m_bytes -= m_steps.back().bytes;
        m_steps.pop_back();
    }
    if (step.bytes > m_maxBytes) {
        // No cabe ni solo: sin él, los pasos anteriores ya no casarían con el lienzo.
        clear();
        return;
    }
    m_bytes += step.bytes;
    m_steps.push_back(std::move(step));
    m_cursor = m_steps.size();
    enforceLimits();
}

HistoryStep* History::stepToUndo() {
    if (!canUndo()) {
        return nullptr;
    }
    --m_cursor;
    return &m_steps[m_cursor];
}

HistoryStep* History::stepToRedo() {
    if (!canRedo()) {
        return nullptr;
    }
    return &m_steps[m_cursor++];
}

void History::enforceLimits() {
    while (m_cursor > 0 && (m_bytes > m_maxBytes || m_steps.size() > static_cast<size_t>(m_maxSteps))) {
        m_bytes -= m_steps.front().bytes;
        m_steps.pop_front();
        --m_cursor;
    }
}
