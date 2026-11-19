// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

namespace seabass::application
{

// Whether a sync that writes rekordbox's tracks into the Engine library
// also copies rekordbox's beat grid and waveform onto the tracks it
// writes (the Beat Grids & Waveforms page's copy, run inside the sync's
// own save). Preferences, under "Music", writes them; Sync after
// Rekordbox Export reads them when it stages a save. Both off by default
// (Sebastian, 2026-11-19: "default off (for now)").
//
// A process-wide value set by a composition root, for the reason
// domain::MatchingPolicy gives: AppSettingsController sets it on
// construction and on every change, the command line never does (its
// sync commands do not write Engine's analysis). Lock-free atomics, so
// the save's worker and the Preferences page never race.
class SyncAnalysisPolicy
{
public:
    static bool copyBeatGrids();
    static bool copyWaveforms();
    static void set(bool copyBeatGrids, bool copyWaveforms);

    // Back to the defaults. For tests, which must not inherit whatever a
    // previous case set.
    static void reset();

    static constexpr bool DefaultCopyBeatGrids = false;
    static constexpr bool DefaultCopyWaveforms = false;
};

}  // namespace seabass::application
