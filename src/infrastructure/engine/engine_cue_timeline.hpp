// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "domain/matching_policy.hpp"
#include "domain/mp3_lead_in.hpp"
#include "domain/track.hpp"

namespace seabass::infrastructure::engine
{

// Seabass keeps cue positions on rekordbox's timeline (#50): ANLZ,
// DeviceLibrary and OneLibrary share it, and Engine's are moved onto it
// here, at the Engine adapters, by domain::engineToRekordboxOffsetMs().
// The reader adds the shift, the cue writer and Create Engine Library
// take it off again, and both read the lead-in from the same bytes of
// the same file (infrastructure::audio::readMp3LeadIn), so a cue read
// and written back lands where it was.

// Whether a row's cues are moved at all: an MP3 with a file on this
// computer, and the correction switched on (MatchingPolicy).
inline bool shiftsCueTimeline(const std::string &filePath, const std::string &streamingSource)
{
    return domain::MatchingPolicy::correctMp3CueTiming() && streamingSource.empty() && !filePath.empty()
        && domain::hasMp3Extension(filePath);
}

// The milliseconds to add to an Engine position: 0 for a file whose
// lead-in could not be read, so a guess is never worse than no
// correction.
inline double engineCueShiftMs(const std::optional<domain::Mp3LeadIn> &leadIn)
{
    return leadIn ? domain::engineToRekordboxOffsetMs(*leadIn) : 0.0;
}

// Moves every position, both ends of a loop, by `shiftMs`.
inline void shiftCuePositions(std::vector<domain::CuePoint> &cues, double shiftMs)
{
    if (shiftMs == 0.0) {
        return;
    }
    for (auto &cue : cues) {
        cue.positionMs += shiftMs;
        if (cue.isLoop) {
            cue.loopEndMs += shiftMs;
        }
    }
}

}  // namespace seabass::infrastructure::engine
