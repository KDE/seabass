// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace seabass::domain
{

// What a Denon player keeps as a track's overview waveform, worked out
// from what rekordbox already analysed, so that a track exported from
// rekordbox can reach the player looking analysed without the player
// analysing it again.
//
// None of this is a format specification. rekordbox and the player
// analyse the audio each in their own way; the conversions here are
// least-squares fits measured on one real library (see the .cpp), close
// enough that the picture on the player looks like the player's own,
// not equal to it.

// One point of a three-band waveform: low, mid and high, 0..255 each.
struct BandTriple
{
    std::uint8_t low = 0;
    std::uint8_t mid = 0;
    std::uint8_t high = 0;

    bool operator==(const BandTriple &) const = default;
};

// A three-band overview as the player stores it: exactly PointCount
// points spread evenly over the whole track, then one more triple. In
// the overview the player writes into its database that last triple is
// the per-band maximum of the points (measured on 349 of 349 tracks).
// Empty (no points) where there was nothing to convert.
struct EngineOverview
{
    static constexpr std::size_t PointCount = 1024;

    std::vector<BandTriple> points;
    BandTriple finalTriple;

    bool empty() const { return points.empty(); }
};

// The per-band maximum of `points`: the overview's final triple.
BandTriple perBandMaximum(const std::vector<BandTriple> &points);

// The player's overview from rekordbox's three-band preview (PWV6, in
// the .2EX analysis file): `entries` is the section's body, three bytes
// per entry, as many entries as it holds (1200 in every export seen).
// Byte i of an entry feeds the player's band i (low, mid, high): that is
// what the measurement found, whatever the published notes call them.
//
// `sourceSeconds` is how long the preview covers, which is the length of
// rekordbox's scrolling waveform (entries / 150); `targetSeconds` is the
// length the player will count, samples / sample rate. Empty if either
// is not positive or `entries` is not whole three-byte entries.
EngineOverview overviewFromThreeBandPreview(std::string_view entries, double sourceSeconds, double targetSeconds);

// The same from rekordbox's colour preview (PWV4, in the .EXT file, six
// bytes an entry), for an export that has no .2EX. A worse fit than the
// three-band one: a median error of about 12 on the 0..255 scale against
// about 9.
EngineOverview overviewFromColourPreview(std::string_view entries, double sourceSeconds, double targetSeconds);

// The second three-band waveform the player keeps per track, in a file
// of its own (OverviewData/<uuid>/<trackId>.rgb), worked out from the
// overview above. Empty for an empty overview.
EngineOverview secondWaveformFromOverview(const EngineOverview &overview);

// How many samples the player will count for a track, from the number
// of entries in rekordbox's scrolling waveform (PWV7, PWV5 or PWV3, 150
// entries a second, the last one partly filled). 0 for no entries or a
// sample rate that is not positive.
std::uint64_t engineSampleCount(std::uint32_t scrollEntries, double sampleRateHz, bool isMp3);

// The three average loudness values (low, mid, high) of the player's
// analysis record, estimated from an overview. An estimate only: the
// player derives them from the audio in a way the overview does not
// pin down. Zeros for an empty overview.
std::array<double, 3> loudnessFromOverview(const EngineOverview &overview);

}  // namespace seabass::domain
