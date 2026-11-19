// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/sync_analysis_policy.hpp"

#include <atomic>

namespace seabass::application
{

namespace
{

// Function-local statics, as domain::MatchingPolicy has them: read from
// translation units whose initialization order is not defined.
std::atomic<bool> &beatGridsStore()
{
    static std::atomic<bool> value{SyncAnalysisPolicy::DefaultCopyBeatGrids};
    return value;
}

std::atomic<bool> &waveformsStore()
{
    static std::atomic<bool> value{SyncAnalysisPolicy::DefaultCopyWaveforms};
    return value;
}

std::atomic<bool> &experimentalStore()
{
    static std::atomic<bool> value{false};
    return value;
}

}  // namespace

bool SyncAnalysisPolicy::copyBeatGrids()
{
    return experimentalEnabled() && beatGridsStore().load(std::memory_order_relaxed);
}

bool SyncAnalysisPolicy::copyWaveforms()
{
    return experimentalEnabled() && waveformsStore().load(std::memory_order_relaxed);
}

void SyncAnalysisPolicy::setExperimentalEnabled(bool enabled)
{
    experimentalStore().store(enabled, std::memory_order_relaxed);
}

bool SyncAnalysisPolicy::experimentalEnabled()
{
    return experimentalStore().load(std::memory_order_relaxed);
}

void SyncAnalysisPolicy::set(bool copyBeatGrids, bool copyWaveforms)
{
    beatGridsStore().store(copyBeatGrids, std::memory_order_relaxed);
    waveformsStore().store(copyWaveforms, std::memory_order_relaxed);
}

void SyncAnalysisPolicy::reset()
{
    set(DefaultCopyBeatGrids, DefaultCopyWaveforms);
    setExperimentalEnabled(false);
}

}  // namespace seabass::application
