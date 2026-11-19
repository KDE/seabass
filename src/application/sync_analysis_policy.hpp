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
// Rekordbox Export and Sync Cue Points read them when they stage a save,
// Create Engine Library when it starts. Both off by default
// (Sebastian, 2026-11-19: "default off (for now)").
//
// A process-wide value set by a composition root, for the reason
// domain::MatchingPolicy gives: AppSettingsController sets it on
// construction and on every change, the command line never does (its
// sync commands do not write Engine's analysis). Lock-free atomics, so
// the save's worker and the Preferences page never race.
//
// Experimental (docs/experimental-features.md): both read off unless
// experimental features are enabled, whatever the two switches hold, so
// switching Experimental off turns the copy off in every workflow that
// reads this. AppSettingsController hands the gate in with
// setExperimentalEnabled() (false in a build without SEABASS_EXPERIMENTAL)
// and keeps the user's two choices stored, so switching it on again
// brings them back. Off until a composition root says otherwise.
class SyncAnalysisPolicy
{
public:
    // What a workflow obeys: the switch, and the experimental gate open.
    static bool copyBeatGrids();
    static bool copyWaveforms();
    static void set(bool copyBeatGrids, bool copyWaveforms);
    static void setExperimentalEnabled(bool enabled);
    static bool experimentalEnabled();

    // Back to the defaults, the gate closed. For tests, which must not
    // inherit whatever a previous case set.
    static void reset();

    static constexpr bool DefaultCopyBeatGrids = false;
    static constexpr bool DefaultCopyWaveforms = false;
};

}  // namespace seabass::application
