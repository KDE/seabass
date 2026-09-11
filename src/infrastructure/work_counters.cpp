// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/work_counters.hpp"

#include <sstream>

namespace seabass::infrastructure
{

std::string WorkCountersSnapshot::describe() const
{
    std::ostringstream out;
    out << "engine opens=" << engineDatabaseOpens << " encrypted opens=" << encryptedDatabaseOpens
        << " pdb parses=" << trackDatabaseParses << " durable writes=" << durableFileWrites;
    return out.str();
}

WorkCounters &WorkCounters::instance()
{
    static WorkCounters counters;
    return counters;
}

WorkCountersSnapshot WorkCounters::snapshot() const
{
    return {m_engineDatabaseOpens.load(std::memory_order_relaxed),
            m_encryptedDatabaseOpens.load(std::memory_order_relaxed),
            m_trackDatabaseParses.load(std::memory_order_relaxed),
            m_durableFileWrites.load(std::memory_order_relaxed)};
}

void WorkCounters::reset()
{
    m_engineDatabaseOpens.store(0, std::memory_order_relaxed);
    m_encryptedDatabaseOpens.store(0, std::memory_order_relaxed);
    m_trackDatabaseParses.store(0, std::memory_order_relaxed);
    m_durableFileWrites.store(0, std::memory_order_relaxed);
}

}  // namespace seabass::infrastructure
