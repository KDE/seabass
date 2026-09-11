// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// A content identity for a DJ library that survives re-import, re-index
// and a move between formats: nothing in it depends on ids, file paths,
// database state or the stick it lives on. Three bottom-k samples (the k
// smallest hashes of each set, so two libraries sample the same members
// rather than random ones):
//
//   tracks:    normalized title + artist + duration in whole seconds
//   cues:      per track, its sorted cue positions rounded to 50 ms
//              (the personal part: two DJs rarely place identical cues)
//   playlists: normalized playlist paths
//
// compare() estimates containment from the samples, so a library that
// has grown since the backup still matches. Also the identity for
// exporting and importing cue points and other metadata between copies
// of the same library.
struct LibraryFingerprint
{
    static constexpr int Version = 1;
    static constexpr std::size_t SampleSize = 256;

    std::vector<std::uint64_t> trackHashes;     // sorted ascending, at most SampleSize
    std::vector<std::uint64_t> cueHashes;       // sorted ascending, at most SampleSize
    std::vector<std::uint64_t> playlistHashes;  // sorted ascending, at most SampleSize
    std::size_t trackCount = 0;                 // of the whole library, not the sample
    std::size_t cuedTrackCount = 0;
    std::size_t playlistCount = 0;

    bool empty() const { return trackCount == 0; }

    // One line, no tabs or newlines, safe inside a manifest field.
    std::string serialize() const;
    static std::optional<LibraryFingerprint> parse(std::string_view text);

    bool operator==(const LibraryFingerprint &) const = default;
};

LibraryFingerprint fingerprintLibrary(const std::vector<Track> &tracks);

// Stable 64-bit hash of a track's identity, shared with the fingerprint
// so metadata export/import can address tracks the same way.
std::uint64_t trackIdentityHash(const Track &track);

struct FingerprintSimilarity
{
    enum class Verdict
    {
        Same,                          // the same library, possibly changed a little or grown
        SameCollectionDifferentState,  // the same tracks, but the cues differ: a re-export that lost them, or a copy cued differently
        Different,
        Unknown,                       // one side is empty or too small to tell
    };
    Verdict verdict = Verdict::Unknown;
    // Estimated containment of the smaller side in the larger, 0..1
    // (-1 when there is nothing to compare on that axis).
    double trackOverlap = -1.0;
    double cueOverlap = -1.0;
    double playlistOverlap = -1.0;
};

FingerprintSimilarity compareFingerprints(const LibraryFingerprint &a, const LibraryFingerprint &b);

}  // namespace seabass::domain
