// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/matching_policy.hpp"

#include <algorithm>
#include <atomic>

namespace seabass::domain
{

namespace
{

// Function-local statics rather than namespace-scope ones: this is read
// from translation units whose initialization order relative to this one
// is not defined, and a duplicate scan reading a zero tolerance would
// silently find no duplicates at all rather than fail.
std::atomic<double> &exactMatchStore()
{
    static std::atomic<double> value{MatchingPolicy::DefaultExactMatchSeconds};
    return value;
}

std::atomic<double> &compareAudioStore()
{
    static std::atomic<double> value{MatchingPolicy::DefaultCompareAudioSeconds};
    return value;
}

std::atomic<bool> &ignoreCuesAtStartStore()
{
    static std::atomic<bool> value{MatchingPolicy::DefaultIgnoreCuesAtStart};
    return value;
}

}  // namespace

double MatchingPolicy::exactMatchSeconds()
{
    return exactMatchStore().load(std::memory_order_relaxed);
}

double MatchingPolicy::compareAudioSeconds()
{
    return compareAudioStore().load(std::memory_order_relaxed);
}

double MatchingPolicy::backupIdentitySeconds()
{
    // See the header: capped rather than clamped at set() time, because
    // the exact-match window itself really is whatever the user chose
    // -- it is only the backup stores' row identity that must not widen.
    return std::min(exactMatchSeconds(), DefaultExactMatchSeconds);
}

bool MatchingPolicy::ignoreCuesAtStart()
{
    return ignoreCuesAtStartStore().load(std::memory_order_relaxed);
}

void MatchingPolicy::set(double exactMatchSeconds, double compareAudioSeconds, bool ignoreCuesAtStart)
{
    const double exact = std::clamp(exactMatchSeconds, MinExactMatchSeconds, MaxExactMatchSeconds);
    // Never below the exact-match window. A wider window that is
    // narrower than the exact one describes an empty band, and the
    // audio comparison would be configured on while never running --
    // exactly the kind of quietly-does-nothing setting this project has
    // been bitten by. Clamped up instead, so "compare audio" always
    // means a real band or is plainly off (equal to the exact window).
    const double deeper = std::max(exact, std::clamp(compareAudioSeconds, MinCompareAudioSeconds,
                                                      MaxCompareAudioSeconds));
    exactMatchStore().store(exact, std::memory_order_relaxed);
    compareAudioStore().store(deeper, std::memory_order_relaxed);
    ignoreCuesAtStartStore().store(ignoreCuesAtStart, std::memory_order_relaxed);
}

void MatchingPolicy::reset()
{
    set(DefaultExactMatchSeconds, DefaultCompareAudioSeconds, DefaultIgnoreCuesAtStart);
}

}  // namespace seabass::domain
