// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

namespace seabass::domain
{

// One column of a waveform preview, split into three frequency bands (each
// normalized to 0..1) so the GUI can render a colored waveform the way DJ
// hardware does -- low/bass in one hue, mid in another, high in a third.
// Not every source can populate all three distinctly: see each reader's own
// comment for what its underlying format actually provides.
// How a waveform is drawn: the look of the library whose analysis it is,
// for the richest data that analysis holds. A waveform is drawn as the
// software that wrote it draws it, so its colours say which library it
// came from, in every view alike.
enum class WaveformLook
{
    Unknown,
    // rekordbox's 3Band (PWV6 overview, PWV7 scrolling, in the .2EX):
    // blue lows, amber mids, white highs.
    RekordboxThreeBand,
    // rekordbox's Blue (PWAV overview, PWV3 scrolling): one height a
    // column and a whiteness, carried as low = the height and high = the
    // height times the whiteness (see the rekordbox waveform reader).
    RekordboxBlue,
    // Engine OS's three layers, from Engine's overview or a detail made
    // from the audio the way a Denon player makes its own.
    Engine,
};

// The name QML gets: "rekordbox-3band", "rekordbox-blue", "engine", or
// "" for Unknown.
inline const char *waveformLookName(WaveformLook look)
{
    switch (look) {
    case WaveformLook::RekordboxThreeBand:
        return "rekordbox-3band";
    case WaveformLook::RekordboxBlue:
        return "rekordbox-blue";
    case WaveformLook::Engine:
        return "engine";
    case WaveformLook::Unknown:
        break;
    }
    return "";
}

struct WaveformColumn
{
    double low = 0.0;
    double mid = 0.0;
    double high = 0.0;
};

// A track's waveform at the resolution a player scrolls it by, for a view
// zoomed in to a few bars (the waveform popup, #104): columns evenly
// spaced in time from the start of the track.
struct DetailWaveform
{
    std::vector<WaveformColumn> columns;
    // From one column to the next. 0 where the source does not say: the
    // columns are then spread evenly over the track's length.
    double msPerColumn = 0.0;
    WaveformLook look = WaveformLook::Unknown;
};

}  // namespace seabass::domain
