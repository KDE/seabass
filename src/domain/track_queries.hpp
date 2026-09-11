// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

class TrackPrioritizer
{
public:
    // Tracks with no cues at all, ordered with the most "worth setting
    // cues on" first: higher rekordbox play count first, else more
    // recently played on Engine first. Tracks with no engagement signal at
    // all sort last (in original order among themselves). limit == 0
    // means no limit.
    static std::vector<Track> tracksNeedingCues(const std::vector<Track> &tracks, size_t limit = 0);
};

}  // namespace seabass::domain
