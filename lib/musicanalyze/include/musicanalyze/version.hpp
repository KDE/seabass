// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// The library's version: what its interface is. The only place it is
// written down; the build reads it from these three lines.
#define MUSICANALYZE_VERSION_MAJOR 0
#define MUSICANALYZE_VERSION_MINOR 3
#define MUSICANALYZE_VERSION_PATCH 0

namespace musicanalyze
{

// What made a result, one number a part: it goes up with every change
// after which the same audio can give another result, and with no other
// (a faster analyzer that finds the same beats keeps its number). Kept
// beside a stored result, it says whether an analysis of today would
// still give it, and which code a report of a wrong grid is about.
//
// Raising one: tests/results.txt holds what each part makes of generated
// audio under the numbers it was written with, and the library's test
// fails when a result has changed and the number has not. Raise the
// number here, then write the file again (musicanalyze_test --write).
// That is a reminder and not a proof: it sees a change on the audio the
// test makes, and music has more in it than that.
// Grid 2: the audio is listened to in blocks (the same grid on 2,828 of
// 2,831 tracks, see grid.cpp), and no grid is looked for under 8 kHz.
inline constexpr int GridRevision = 2;
inline constexpr int KeyRevision = 1;
inline constexpr int WaveformRevision = 1;

struct Revisions
{
    int grid = 0;
    int key = 0;
    int waveform = 0;
    bool operator==(const Revisions &other) const = default;
};

// The version and the revisions of the library that is linked, where the
// lines above are those of the headers a caller was built with.
const char *version();
Revisions revisions();

}  // namespace musicanalyze
