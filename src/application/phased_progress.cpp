// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/phased_progress.hpp"

#include <algorithm>

namespace seabass::application
{

PhasedProgress::PhasedProgress(ProgressReporter &underlying, const std::string &label, size_t plannedTotal,
                               size_t startAt, bool announce, bool endsOperation)
    : m_underlying(underlying), m_plannedTotal(plannedTotal), m_offset(startAt), m_position(startAt),
      m_endsOperation(endsOperation)
{
    if (announce && plannedTotal > 0) {
        m_underlying.start(label, plannedTotal);
    }
    if (startAt > 0) {
        report(startAt);
    }
}

PhasedProgress::~PhasedProgress()
{
    done();
}

void PhasedProgress::start(const std::string &label, size_t total)
{
    if (m_plannedTotal == 0) {
        m_underlying.start(label, total);
        return;
    }
    endStretch();
    m_offset = m_position;
    m_stretchTotal = total;
    m_inStretch = true;
    m_underlying.phase(label);
    report(m_offset);
}

void PhasedProgress::tick(size_t current)
{
    if (m_plannedTotal == 0) {
        m_underlying.tick(current);
        return;
    }
    if (!m_inStretch || m_stretchTotal == 0) {
        return;
    }
    m_position = m_offset + std::min(current, m_stretchTotal);
    report(m_position);
}

void PhasedProgress::finish()
{
    endStretch();
}

void PhasedProgress::warn(const std::string &message)
{
    m_underlying.warn(message);
}

void PhasedProgress::phase(const std::string &label)
{
    m_underlying.phase(label);
}

void PhasedProgress::done()
{
    if (m_done) {
        return;
    }
    endStretch();
    m_done = true;
    if (m_plannedTotal > 0 && m_endsOperation) {
        m_position = m_plannedTotal;
        report(m_position);
    }
    m_underlying.finish();
}

void PhasedProgress::endStretch()
{
    if (!m_inStretch) {
        return;
    }
    // Whatever the stretch counted, it is over: a stretch of n moves the
    // bar n, one with no count moves it 1.
    m_position = m_offset + (m_stretchTotal == 0 ? 1 : m_stretchTotal);
    m_inStretch = false;
    report(m_position);
}

void PhasedProgress::report(size_t position)
{
    if (m_plannedTotal == 0) {
        return;
    }
    const size_t clamped = std::min(position, m_plannedTotal);
    // Forward only: a stretch beginning at an offset the last one ran
    // past (the plan was short) must not pull the bar back.
    if (clamped <= m_reported) {
        return;
    }
    m_reported = clamped;
    m_underlying.tick(clamped);
}

}  // namespace seabass::application
