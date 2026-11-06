// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/playback_timeline.hpp"

#include "domain/matching_policy.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"

namespace seabass::infrastructure::audio
{

PlaybackShift playbackShiftFor(const std::optional<domain::Mp3LeadIn> &leadIn, bool correct, GridTimeline grid)
{
    if (!correct || !leadIn) {
        return {};
    }
    // rekordbox minus Engine: taking it off a rekordbox position gives
    // the Engine one, which is where the player hears that instant.
    const double toPlayback = -domain::engineToRekordboxOffsetMs(*leadIn);
    PlaybackShift shift;
    shift.cuesMs = toPlayback;
    shift.beatsMs = grid == GridTimeline::Rekordbox ? toPlayback : 0.0;
    return shift;
}

PlaybackShift playbackShiftFor(const std::string &filePath, GridTimeline grid)
{
    const bool correct = domain::MatchingPolicy::correctMp3CueTiming();
    if (!correct || !domain::hasMp3Extension(filePath)) {
        return {};
    }
    return playbackShiftFor(readMp3LeadIn(filePath), correct, grid);
}

}  // namespace seabass::infrastructure::audio
