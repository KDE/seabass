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
//   tracks:    normalized title + artist + the catalog's duration in
//              whole seconds (a length probed after the read is left out)
//   cues:      per track, its sorted cue positions rounded to 50 ms
//              (the personal part: two DJs rarely place identical cues)
//   playlists: normalized playlist paths
//
// compare() estimates containment from the samples, so a library that
// has grown since the backup still matches. Also the identity for
// exporting and importing cue points and other metadata between copies
// of the same library.
//
// Read in two steps on a stick that has just been plugged in: the
// catalog alone gives the tracks and the playlists at once, rekordbox's
// cues need its ANLZ pass, which takes seconds more. Until that pass the
// fingerprint says so (cuesKnown false) rather than carrying an empty
// cue sample that would read as "no cues at all".
struct LibraryFingerprint
{
    // 2: a track's duration counts only when its catalog gave it (see
    // trackIdentityHash). A version 1 fingerprint, hashed with probed
    // lengths in, parses as nothing rather than as a different library.
    static constexpr int Version = 2;
    static constexpr std::size_t SampleSize = 256;

    std::vector<std::uint64_t> trackHashes;     // sorted ascending, at most SampleSize
    std::vector<std::uint64_t> cueHashes;       // sorted ascending, at most SampleSize
    std::vector<std::uint64_t> playlistHashes;  // sorted ascending, at most SampleSize
    std::size_t trackCount = 0;                 // of the whole library, not the sample
    std::size_t cuedTrackCount = 0;
    std::size_t playlistCount = 0;
    // False for a fingerprint taken before the cue pass: cueHashes and
    // cuedTrackCount are empty then and mean nothing. A fingerprint that
    // is written down always knows its cues, so this is never serialized
    // and parse() always answers true.
    bool cuesKnown = true;

    bool empty() const { return trackCount == 0; }

    // One line, no tabs or newlines, safe inside a manifest field. Empty
    // for a fingerprint whose cues are not known: written into a backup's
    // manifest it would read back as a library without a single cue, and
    // every later comparison would call the stick's cues changed.
    std::string serialize() const;
    static std::optional<LibraryFingerprint> parse(std::string_view text);

    // Identical, cues included: matchFingerprints() == Identical. A
    // fingerprint whose cues are still being read is equal to nothing;
    // a caller that wants "the same as far as both sides know" asks
    // matchFingerprints() for the three-way answer.
    bool operator==(const LibraryFingerprint &other) const;
};

// cuesKnown false: the tracks come from a read that skipped the cue pass,
// so the cue part is left empty and marked unknown, whatever cues some of
// the tracks happen to carry.
LibraryFingerprint fingerprintLibrary(const std::vector<Track> &tracks, bool cuesKnown = true);

// The exact comparison, in the two steps the fingerprint is read in.
// Tracks or playlists that differ decide it at once: no cue can make two
// libraries with different tracks the same. With those equal, the cues
// decide, and while either side's cues are unknown the answer is
// IdenticalSoFar: identical unless the cue pass says otherwise.
enum class FingerprintMatch
{
    Identical,
    IdenticalSoFar,
    Different,
};
FingerprintMatch matchFingerprints(const LibraryFingerprint &a, const LibraryFingerprint &b);

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
    // The verdict is Same only because one side's cues are not read yet:
    // once they are it may still turn into SameCollectionDifferentState.
    // Never set for Different or Unknown, which no cue can change.
    bool cuesPending = false;
};

FingerprintSimilarity compareFingerprints(const LibraryFingerprint &a, const LibraryFingerprint &b);

}  // namespace seabass::domain
