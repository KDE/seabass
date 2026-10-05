// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstdint>

#include "measurement.hpp"

namespace storageprobe
{

// A probe run's progress, in the units its options' onProgress names:
// counted once up front, then only ever advanced. Internal to the probes.
class RunProgress
{
public:
    RunProgress(const ProbeProgress &onProgress, std::uint64_t total) : m_onProgress(onProgress), m_total(total) {}

    void begin(ProbeStep step)
    {
        m_step = step;
        report();
    }
    void advance(std::uint64_t units = 1)
    {
        m_done = std::min(m_total, m_done + units);
        report();
    }
    // A step ending early (nothing to read) hands over its units at once.
    void advanceTo(std::uint64_t done)
    {
        if (done > m_done) {
            advance(done - m_done);
        }
    }
    std::uint64_t done() const { return m_done; }

private:
    void report() const
    {
        if (m_onProgress) {
            m_onProgress(m_step, m_done, m_total);
        }
    }

    const ProbeProgress &m_onProgress;
    std::uint64_t m_total = 0;
    std::uint64_t m_done = 0;
    ProbeStep m_step = ProbeStep::Streaming;
};

}  // namespace storageprobe
