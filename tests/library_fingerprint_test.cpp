// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/advise_stick_backup.hpp"
#include "domain/library_fingerprint.hpp"

using namespace seabass::domain;
using seabass::application::adviseStickBackup;
using seabass::application::StickBackupAdvice;
using seabass::application::StickBackupAdviceInput;
using seabass::application::StickBackupDescription;

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

    // The two-step read: a fingerprint from the catalogs alone, before the
    // cue pass, against one that knows its cues. Every combination of
    // tracks, playlists and cues (same or different) and of which side
    // knows its cues. 100 tracks, under SampleSize, so one changed track
    // or cue always reaches the sample.
    {
        const std::vector<Track> base = makeLibrary(100, 20);
        for (int variant = 0; variant < 8; ++variant) {
            const bool tracksDiffer = (variant & 1) != 0;
            const bool playlistsDiffer = (variant & 2) != 0;
            const bool cuesDiffer = (variant & 4) != 0;
            std::vector<Track> other = base;
            if (tracksDiffer) {
                // Joins a playlist the library already has, so only the
                // track part moves.
                Track extra = makeLibrary(1, 21).front();
                extra.playlists = {base.front().playlists.front()};
                other.push_back(extra);
            }
            if (playlistsDiffer) {
                other.front().playlists.push_back({"Sets/Only here", 0});
            }
            if (cuesDiffer) {
                // Every cue moved, so the estimate below sees it too, not
                // only the exact comparison.
                for (Track &track : other) {
                    for (CuePoint &cue : track.cues) {
                        cue.positionMs += 2000.0;
                    }
                }
            }
            for (int known = 0; known < 4; ++known) {
                const bool aKnows = (known & 1) != 0;
                const bool bKnows = (known & 2) != 0;
                const LibraryFingerprint a = fingerprintLibrary(base, aKnows);
                const LibraryFingerprint b = fingerprintLibrary(other, bKnows);
                assert(a.cuesKnown == aKnows && b.cuesKnown == bKnows);

                FingerprintMatch expected = FingerprintMatch::Identical;
                if (tracksDiffer || playlistsDiffer) {
                    expected = FingerprintMatch::Different;  // certain: no cue changes that
                } else if (!aKnows || !bKnows) {
                    expected = FingerprintMatch::IdenticalSoFar;
                } else if (cuesDiffer) {
                    expected = FingerprintMatch::Different;
                }
                assert(matchFingerprints(a, b) == expected);
                assert(matchFingerprints(b, a) == expected);
                assert((a == b) == (expected != FingerprintMatch::Different));

                // The estimate: the same library throughout (one track
                // more at most), so never Different; pending exactly when
                // a side lacks its cues, and then plainly Same.
                const FingerprintSimilarity similarity = compareFingerprints(a, b);
                assert(similarity.cuesPending == (!aKnows || !bKnows));
                if (similarity.cuesPending) {
                    assert(similarity.verdict == FingerprintSimilarity::Verdict::Same);
                    assert(similarity.cueOverlap == -1.0);
                } else {
                    assert(similarity.verdict
                           == (cuesDiffer ? FingerprintSimilarity::Verdict::SameCollectionDifferentState
                                          : FingerprintSimilarity::Verdict::Same));
                }
            }
        }

        // A different library is Different with or without its cues, and
        // never pending; too small to tell stays Unknown, also not pending.
        const FingerprintSimilarity different =
            compareFingerprints(fingerprintLibrary(makeLibrary(300, 22), false), fingerprintLibrary(makeLibrary(300, 23)));
        assert(different.verdict == FingerprintSimilarity::Verdict::Different);
        assert(!different.cuesPending);
        const FingerprintSimilarity tiny =
            compareFingerprints(fingerprintLibrary(makeLibrary(3, 24), false), fingerprintLibrary(makeLibrary(3, 24)));
        assert(tiny.verdict == FingerprintSimilarity::Verdict::Unknown);
        assert(!tiny.cuesPending);

        // Before the cue pass the cue part is empty, whatever cues the
        // tracks carry, and it is never written down: a backup's manifest
        // holding it would read back as a library without cues.
        const LibraryFingerprint early = fingerprintLibrary(base, false);
        assert(early.cueHashes.empty() && early.cuedTrackCount == 0);
        assert(early.trackCount == 100 && early.playlistCount == 9);
        assert(early.serialize().empty());
        const std::optional<LibraryFingerprint> parsed = LibraryFingerprint::parse(fingerprintLibrary(base).serialize());
        assert(parsed && parsed->cuesKnown);
    }

    // The same, as the backup advisor meets it: the stick read before its
    // cues against a backup of it. Unchanged tracks and playlists give the
    // verdict the cues would have to overturn (current); changed tracks
    // give outdated at once; with the cues read, a moved cue is outdated.
    {
        const std::vector<Track> library = makeLibrary(100, 30);
        StickBackupDescription backup;
        backup.archivePath = "/b/MAIN.zip";
        backup.stickIdentifier = "uuid-main";
        backup.stickLabel = "MAIN";
        backup.createdAtUnix = 1000;
        backup.libraryFingerprint = fingerprintLibrary(library).serialize();
        const auto adviceFor = [&backup](const std::vector<Track> &live, bool cuesKnown) {
            StickBackupAdviceInput input;
            input.hasLibrary = true;
            input.stickIdentifier = "uuid-main";
            input.stickLabel = "MAIN";
            input.liveFingerprint = fingerprintLibrary(live, cuesKnown);
            input.catalogModifiedAtUnix = 2000;  // written after the backup
            input.backups = {backup};
            return adviseStickBackup(input);
        };

        std::vector<Track> movedCue = library;
        movedCue.front().cues.front().positionMs += 2000.0;
        std::vector<Track> grown = library;
        grown.push_back(makeLibrary(1, 31).front());

        assert(adviceFor(library, false).state == StickBackupAdvice::State::Current);
        assert(adviceFor(movedCue, false).state == StickBackupAdvice::State::Current);  // until the cues say otherwise
        assert(adviceFor(grown, false).state == StickBackupAdvice::State::Outdated);
        assert(adviceFor(library, true).state == StickBackupAdvice::State::Current);
        assert(adviceFor(movedCue, true).state == StickBackupAdvice::State::Outdated);
        assert(adviceFor(grown, true).state == StickBackupAdvice::State::Outdated);
    }

    std::cout << "All library_fingerprint tests passed." << std::endl;
    return 0;
}
