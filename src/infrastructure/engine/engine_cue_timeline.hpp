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

// What a write onto Engine left out because the lead-in shift put it at
// or before Engine's 0 (the cue writer's and Create Engine Library's
// counters), in a sentence for a save's outcome; empty when nothing was.
// "1 memory cue and 2 loops inside the MP3 lead-in were left out: ...".
inline std::string describeLeadInShiftDrops(int memoryCues, int loops)
{
    if (memoryCues <= 0 && loops <= 0) {
        return {};
    }
    const auto count = [](int n, const char *one, const char *many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    std::string text;
    if (memoryCues > 0) {
        text = count(memoryCues, "memory cue", "memory cues");
    }
    if (loops > 0) {
        text += (text.empty() ? "" : " and ") + count(loops, "loop", "loops");
    }
    text += memoryCues + loops == 1 ? " inside the MP3 lead-in was left out" : " inside the MP3 lead-in were left out";
    return text + ": on Engine's timeline they fall at or before the start of the track, where Engine cannot hold "
                  "them";
}

}  // namespace seabass::infrastructure::engine
