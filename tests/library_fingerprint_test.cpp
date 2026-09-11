// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "domain/library_fingerprint.hpp"

using namespace seabass::domain;

namespace
{

// A synthetic library: `count` tracks with cues and playlists, `seed`
// shifts every identity so two seeds never share a track.
std::vector<Track> makeLibrary(std::size_t count, int seed, bool withCues = true)
{
    std::vector<Track> tracks;
    for (std::size_t i = 0; i < count; ++i) {
        Track track;
        track.sourceId = "id-" + std::to_string(i);
        track.format = "rekordbox";
        track.title = "Track " + std::to_string(i + seed * 100000);
        track.artist = "Artist " + std::to_string((i + seed) % 7);
        track.durationSeconds = 180.0 + static_cast<double>(i % 200);
        track.filePath = "/media/A/Contents/" + std::to_string(i) + ".mp3";
        if (withCues) {
            for (int c = 0; c < 3; ++c) {
                CuePoint cue;
                cue.kind = c == 0 ? CuePoint::Kind::Memory : CuePoint::Kind::Hot;
                cue.hotCueNumber = c;
                cue.positionMs = 1000.0 * (c + 1) * static_cast<double>(1 + i % 5);
                track.cues.push_back(cue);
            }
        }
        track.playlists.push_back({"Sets/Playlist " + std::to_string(i % 9), static_cast<int>(i)});
        tracks.push_back(track);
    }
    return tracks;
}

}  // namespace

int main()
{
    // Format, ids and paths do not matter: a re-import keeps the identity.
    {
        std::vector<Track> rekordbox = makeLibrary(300, 1);
        std::vector<Track> engine = makeLibrary(300, 1);
        for (Track &track : engine) {
            track.format = "engine";
            track.sourceId = "e-" + track.sourceId;
            track.filePath = "/media/B/Music/" + track.title + ".flac";
            track.title = " " + track.title + "  ";  // spacing and casing survive normalization
            for (char &c : track.artist) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        LibraryFingerprint a = fingerprintLibrary(rekordbox);
        LibraryFingerprint b = fingerprintLibrary(engine);
        assert(a == b);
        assert(a.trackCount == 300);
        assert(a.cuedTrackCount == 300);
        assert(a.playlistCount == 9);
        assert(a.trackHashes.size() == LibraryFingerprint::SampleSize);
        FingerprintSimilarity same = compareFingerprints(a, b);
        assert(same.verdict == FingerprintSimilarity::Verdict::Same);
        assert(same.trackOverlap == 1.0);
        assert(same.cueOverlap == 1.0);
        assert(same.playlistOverlap == 1.0);
    }

    // A library that grew from 30 to 300 tracks is still the same library.
    {
        LibraryFingerprint small = fingerprintLibrary(makeLibrary(30, 2));
        LibraryFingerprint grown = fingerprintLibrary(makeLibrary(300, 2));
        FingerprintSimilarity similarity = compareFingerprints(small, grown);
        assert(similarity.verdict == FingerprintSimilarity::Verdict::Same);
        assert(similarity.trackOverlap >= 0.95);
        assert(compareFingerprints(grown, small).verdict == FingerprintSimilarity::Verdict::Same);
    }

    // A few removed and a few added: still the same library.
    {
        std::vector<Track> before = makeLibrary(400, 3);
        std::vector<Track> after(before.begin() + 20, before.end());
        std::vector<Track> extra = makeLibrary(30, 4);
        after.insert(after.end(), extra.begin(), extra.end());
        FingerprintSimilarity similarity = compareFingerprints(fingerprintLibrary(before), fingerprintLibrary(after));
        assert(similarity.verdict == FingerprintSimilarity::Verdict::Same);
        assert(similarity.trackOverlap >= 0.85);
    }

    // Same tracks, cues lost in a re-export: same collection, different state.
    {
        FingerprintSimilarity similarity =
            compareFingerprints(fingerprintLibrary(makeLibrary(200, 5)), fingerprintLibrary(makeLibrary(200, 5, false)));
        assert(similarity.verdict == FingerprintSimilarity::Verdict::SameCollectionDifferentState);
        assert(similarity.trackOverlap == 1.0);
        assert(similarity.cueOverlap == -1.0);
    }

    // Same tracks, cues moved: also a different state.
    {
        std::vector<Track> moved = makeLibrary(200, 6);
        for (Track &track : moved) {
            for (CuePoint &cue : track.cues) {
                cue.positionMs += 500.0;
            }
        }
        FingerprintSimilarity similarity = compareFingerprints(fingerprintLibrary(makeLibrary(200, 6)), fingerprintLibrary(moved));
        assert(similarity.verdict == FingerprintSimilarity::Verdict::SameCollectionDifferentState);
        assert(similarity.cueOverlap == 0.0);
    }

    // Cue positions within the 50 ms rounding still match.
    {
        std::vector<Track> jittered = makeLibrary(200, 7);
        for (Track &track : jittered) {
            for (CuePoint &cue : track.cues) {
                cue.positionMs += 10.0;
            }
        }
        assert(fingerprintLibrary(makeLibrary(200, 7)) == fingerprintLibrary(jittered));
    }

    // Two unrelated libraries.
    {
        FingerprintSimilarity similarity = compareFingerprints(fingerprintLibrary(makeLibrary(300, 8)), fingerprintLibrary(makeLibrary(300, 9)));
        assert(similarity.verdict == FingerprintSimilarity::Verdict::Different);
        assert(similarity.trackOverlap == 0.0);
    }

    // Too small to tell, and nothing at all.
    {
        assert(compareFingerprints(fingerprintLibrary(makeLibrary(3, 10)), fingerprintLibrary(makeLibrary(3, 10))).verdict
               == FingerprintSimilarity::Verdict::Unknown);
        LibraryFingerprint empty = fingerprintLibrary({});
        assert(empty.empty());
        assert(compareFingerprints(empty, fingerprintLibrary(makeLibrary(50, 11))).verdict == FingerprintSimilarity::Verdict::Unknown);
    }

    // Serialization round-trips, is one line, and rejects garbage.
    {
        LibraryFingerprint original = fingerprintLibrary(makeLibrary(300, 12));
        std::string text = original.serialize();
        assert(text.find('\t') == std::string::npos && text.find('\n') == std::string::npos);
        std::optional<LibraryFingerprint> parsed = LibraryFingerprint::parse(text);
        assert(parsed.has_value());
        assert(*parsed == original);
        assert(!LibraryFingerprint::parse("").has_value());
        assert(!LibraryFingerprint::parse("v2;1;1;1;;;").has_value());
        assert(!LibraryFingerprint::parse("v1;1;1;1;zz;;").has_value());
        LibraryFingerprint empty = fingerprintLibrary({});
        assert(LibraryFingerprint::parse(empty.serialize()) == empty);
    }

    // The per-track identity is what metadata export/import can key on.
    {
        Track a;
        a.title = "Strobe";
        a.artist = "deadmau5";
        a.durationSeconds = 634.2;
        Track b = a;
        b.title = "STROBE ";
        b.durationSeconds = 634.4;
        assert(trackIdentityHash(a) == trackIdentityHash(b));
        b.artist = "Deadmau5 & Kaskade";
        assert(trackIdentityHash(a) != trackIdentityHash(b));
    }

    std::cout << "All library_fingerprint tests passed." << std::endl;
    return 0;
}
