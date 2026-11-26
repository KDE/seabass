// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

namespace musicanalyze
{

// A musical key: the tonic as a pitch class (0 is C, 1 is D flat, up to
// 11, B) and whether it is minor.
struct MusicalKey
{
    int tonic = 0;
    bool minor = false;
    bool operator==(const MusicalKey &other) const = default;
};

// The key as rekordbox spells it: "C", "Db", "F#m", "Bbm". Flats but for
// F sharp, as in the libraries the references were read from.
std::string keyName(MusicalKey key);

// The lowest sample rate a key is read at: under it the top of the five
// octaves would be missing or folded. No file a DJ plays is sampled that
// low.
constexpr int KeyLowestSampleRateHz = 8000;

struct KeyAnalysis
{
    // Empty when no key could be read: audio shorter than one frame (a
    // second to a second and a half, by the sample rate), or nothing
    // with a pitch in it.
    std::optional<MusicalKey> key;
    // How far the best key's score is above the second best's: larger is
    // surer. Not a probability; for telling a clear case from a close one.
    double margin = 0.0;
    // Why there is no key, where that is not the music's doing: no
    // samples, or a sample rate under KeyLowestSampleRateHz.
    std::string error;
};

// The key detector, a first version.
//
// What it listens to: the track at about 11 kHz, a second and a half at a
// time, a third of a second apart. In each such frame the peaks of the
// spectrum between C2 and C7 are found, each put at its true frequency
// (a parabola through the peak and its neighbours) and gathered into
// thirds of a semitone. Every frame counts the same, loud or quiet. Over
// the track that gives how much of each third of a semitone there is in
// each of five octaves; the third that the track's tuning sits on is
// taken as the centre of the notes.
//
// How it decides: for each of the 24 keys the picture is turned so that
// the key's tonic comes first, and scored with one weight per octave and
// note for major and one set for minor. The weights are learned: fitted
// to rekordbox's keys on the tuning halves of four private collections
// (1,300 tracks). On the other halves, which the fit never saw, it has
// rekordbox's key on 68 percent of 1,245 tracks, where Mixxx's detector
// has it on 54 and Engine DJ on 64 to 69 percent of the same tracks.
//
// `samples` are mono, full scale 1.0, read once and not kept. The
// weights were fitted at 11025 Hz, which is what 44.1 kHz comes down to
// exactly; other rates come down to between 8 and 16.5 kHz and are read
// with the frequencies of that rate (measured: the seven tracks of the
// test music that exist at 44.1 and at 48 kHz).
KeyAnalysis keyFromSamples(std::span<const float> samples, int sampleRateHz);

// What the decision is made from, for a test or a comparison to look at:
// how much of each note there is in each of five octaves from C2 (entry
// octave * 12 + note, C first), all 60 adding up to 1. Empty where there
// is no key to be read.
std::optional<std::array<double, 60>> pitchPicture(std::span<const float> samples, int sampleRateHz);

}  // namespace musicanalyze
