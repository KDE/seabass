// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

namespace seabass::domain
{

// The three numbers that decide whether two rows describe the same
// recording, and whether a cue at the very start of a track counts as
// one a DJ placed. Preferences, under "Music", writes them.
//
// A process-wide value rather than a parameter, for the same reason
// paths::setLocalRootOverride() is one: the settings that reach the
// Qt-free layers are set once by a composition root (the GUI's
// AppSettingsController on construction and on every change; the CLI's
// defaults) and read from a dozen places across four layers, several of
// them deep inside loops that have no business carrying a settings
// object down to them. Threading three numbers through every one of
// those signatures buys no behaviour that this does not.
//
// Every accessor is safe to call from any thread. Scans run on
// QtConcurrent threads while the Preferences page lives on the GUI
// thread, so each number is a lock-free atomic and never a struct read
// field by field. A change made *during* a scan is therefore picked up
// by whatever part of that scan has not run yet, which is harmless (the
// answer is recomputed on the next scan either way) but is the reason
// not to treat a scan's numbers as one consistent snapshot.
class MatchingPolicy
{
public:
    // Two lengths this close are the same recording, full stop. The
    // default of 2 s is what every duration comparison in this codebase
    // used as a hardcoded constant before it became a setting: stored
    // lengths genuinely disagree by a second or so between rekordbox and
    // Engine for one and the same file.
    static double exactMatchSeconds();

    // The wider window in which a length difference is not decisive on
    // its own and the audio itself is compared instead -- see
    // domain::AudioContentProbe. Lengths further apart than this are
    // different recordings and nothing is decoded. Equal to or below
    // exactMatchSeconds() means "never compare audio", which is what a
    // build with no decoder available falls back to.
    static double compareAudioSeconds();

    // Whether a cue inside the first second counts as junk (see
    // domain::isJunkCue). On by default: on real sticks these are stray
    // presses and format sentinels, not markers. Off leaves them alone
    // everywhere -- Library Health stops offering them for cleanup, and
    // they start counting as real cues in every comparison.
    static bool ignoreCuesAtStart();

    // Set by a composition root. Values are clamped into the ranges
    // Preferences offers, so a hand-edited settings file cannot widen
    // the window past what the UI would allow.
    static void set(double exactMatchSeconds, double compareAudioSeconds, bool ignoreCuesAtStart);

    // Back to the defaults above. For tests, which must not inherit
    // whatever a previous case set.
    static void reset();

    // The ranges Preferences clamps to, so the QML spin boxes and this
    // cannot drift apart.
    static constexpr double MinExactMatchSeconds = 0.0;
    static constexpr double MaxExactMatchSeconds = 30.0;
    static constexpr double MinCompareAudioSeconds = 0.0;
    static constexpr double MaxCompareAudioSeconds = 120.0;

    static constexpr double DefaultExactMatchSeconds = 2.0;
    static constexpr double DefaultCompareAudioSeconds = 10.0;
    static constexpr bool DefaultIgnoreCuesAtStart = true;
};

}  // namespace seabass::domain
