// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/hidden_engine_cues.hpp"

namespace seabass::domain
{

std::vector<HiddenEngineCues> HiddenEngineCueFinder::find(const std::vector<Track> &tracks)
{
    std::vector<HiddenEngineCues> found;
    for (const Track &track : tracks) {
        if (track.format != "engine") {
            continue;
        }
        HiddenEngineCues entry;
        for (const CuePoint &cue : track.cues) {
            if (cue.kind != CuePoint::Kind::Hot || !cue.color.empty()) {
                continue;
            }
            (cue.isLoop ? entry.loops : entry.hotCues)++;
        }
        if (entry.hidden() == 0) {
            continue;
        }
        entry.track = track;
        found.push_back(std::move(entry));
    }
    return found;
}

}  // namespace seabass::domain
