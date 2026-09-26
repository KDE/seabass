// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The fingerprint reader's two decisions about cues:
//
// - fingerprintAfterCuesPass(): what the backup advisor keeps once its
//   second read of a stick is back. Only a whole fingerprint, cues known,
//   of the library the first read saw, replaces the first; a failed or
//   short second read leaves the first standing, still pending.
// - readLibraryFingerprint(): whether the cues are known comes with the
//   tracks from the catalog cache, from one look at its entry. Over a
//   copy of the committed fixture: a Tracks read of a cold rekordbox
//   catalog does not know its cues, and one served from an entry that
//   has read them does, and says the same as the Cues read.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <optional>
#include <vector>

#include <QString>

#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/library_fingerprint_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::fingerprintLibrary;
using seabass::domain::LibraryFingerprint;
using seabass::gui::fingerprintAfterCuesPass;

namespace
{

std::vector<seabass::domain::Track> library(int trackCount, bool withCues)
{
    std::vector<seabass::domain::Track> tracks;
    for (int i = 0; i < trackCount; ++i) {
        seabass::domain::Track track;
        track.sourceId = std::to_string(i);
        track.title = "Title " + std::to_string(i);
        track.artist = "Artist";
        track.durationSeconds = 180 + i;
        track.playlists.push_back({"Set", i});
        if (withCues) {
            seabass::domain::CuePoint cue;
            cue.kind = seabass::domain::CuePoint::Kind::Hot;
            cue.hotCueNumber = 1;
            cue.positionMs = 1000.0 * i;
            track.cues.push_back(cue);
        }
        tracks.push_back(track);
    }
    return tracks;
}

void cuesPassCases()
{
    const LibraryFingerprint first = fingerprintLibrary(library(10, false), /*cuesKnown=*/false);
    const LibraryFingerprint whole = fingerprintLibrary(library(10, true), true);
    const LibraryFingerprint wholeButNoCuesKnown = fingerprintLibrary(library(10, true), false);
    // One catalog of two unreadable by the second read: fewer tracks,
    // and "cues known" because the catalog that was read keeps its own.
    const LibraryFingerprint shortRead = fingerprintLibrary(library(6, true), true);
    // A stick whose tracks changed between the two reads.
    auto changed = library(10, true);
    changed[3].title = "Something else";
    const LibraryFingerprint changedLibrary = fingerprintLibrary(changed, true);

    auto kept = fingerprintAfterCuesPass(first, whole);
    assert(kept && *kept == whole && kept->cuesKnown && "a whole second read replaces the first");

    kept = fingerprintAfterCuesPass(first, std::nullopt);
    assert(kept && kept->trackCount == 10 && !kept->cuesKnown && "a failed second read keeps the first, pending");

    kept = fingerprintAfterCuesPass(first, wholeButNoCuesKnown);
    assert(kept && !kept->cuesKnown && kept->cueHashes.empty() && "a second read without its cues keeps the first");

    kept = fingerprintAfterCuesPass(first, shortRead);
    assert(kept && kept->trackCount == 10 && !kept->cuesKnown && "a short second read keeps the first");

    kept = fingerprintAfterCuesPass(first, changedLibrary);
    assert(kept && !kept->cuesKnown && "a second read of a different library keeps the first");

    kept = fingerprintAfterCuesPass(std::nullopt, whole);
    assert(kept && *kept == whole && "with no first fingerprint, a whole second one is taken");

    kept = fingerprintAfterCuesPass(std::nullopt, wholeButNoCuesKnown);
    assert(!kept && "nothing whole to take and nothing to keep");

    kept = fingerprintAfterCuesPass(std::nullopt, std::nullopt);
    assert(!kept);
    std::cout << "fingerprintAfterCuesPass: every case OK\n";
}

void stageFromTheCacheCases(const fs::path &fixture)
{
    const fs::path stick = seabass::testing::scratchRoot() / "library_fingerprint_reader_test";
    fs::remove_all(stick);
    fs::create_directories(stick);
    fs::copy(fixture / "rekordbox", stick / "PIONEER", fs::copy_options::recursive);
    const QString pioneer = QString::fromStdString(seabass::pathToUtf8(stick / "PIONEER"));

    const auto cold = seabass::gui::readLibraryFingerprint(pioneer, QString(), seabass::gui::FingerprintPass::Tracks);
    assert(cold && cold->trackCount > 0);
    assert(!cold->cuesKnown && "a cold rekordbox Tracks read does not know its cues");

    const auto withCues = seabass::gui::readLibraryFingerprint(pioneer, QString(), seabass::gui::FingerprintPass::Cues);
    assert(withCues && withCues->cuesKnown && withCues->cuedTrackCount > 0);

    const auto served = seabass::gui::readLibraryFingerprint(pioneer, QString(), seabass::gui::FingerprintPass::Tracks);
    assert(served && served->cuesKnown && "a Tracks read served from an entry at Cues knows its cues");
    assert(*served == *withCues && "and says what the Cues read said");
    std::cout << "readLibraryFingerprint: the stage comes from the entry that served the tracks OK ("
              << served->trackCount << " tracks, " << served->cuedTrackCount << " with cues)\n";
    seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
    fs::remove_all(stick);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: library_fingerprint_reader_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    cuesPassCases();
    stageFromTheCacheCases(seabass::pathFromUtf8(argv[1]));
    std::cout << "All library_fingerprint_reader tests passed.\n";
    return 0;
}
