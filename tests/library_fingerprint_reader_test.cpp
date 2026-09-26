// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The fingerprint reader's two decisions about cues:
//
// - fingerprintAfterCuesPass(): what the backup advisor keeps once its
//   second read of a stick is back. A fingerprint that knows its cues
//   replaces the first; a failed second read leaves the first standing,
//   still pending.
// - An unreadable catalog, or a cancelled read, gives no fingerprint at
//   all, never one of the other catalog alone.
// - readLibraryFingerprint(): whether the cues are known comes with the
//   tracks from the catalog cache, from one look at its entry. Over a
//   copy of the committed fixture: a Tracks read of a cold rekordbox
//   catalog does not know its cues, and one served from an entry that
//   has read them does, and says the same as the Cues read.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

#include <QString>

#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/library_fingerprint_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "mp3_fixture.hpp"
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
    // Fewer tracks than the first read, cues known.
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

    // Anything the second read says with its cues known is taken, even
    // where its tracks differ from the first: the Full stage may have
    // filled a length in between, and a stick changed in between is best
    // described by the newer read. (A short read cannot arrive here:
    // readLibraryFingerprint() gives nothing rather than one catalog of
    // two, see unreadableCatalogCases().)
    kept = fingerprintAfterCuesPass(first, shortRead);
    assert(kept && *kept == shortRead && "a second read with its cues known is taken, whatever its tracks");

    kept = fingerprintAfterCuesPass(first, changedLibrary);
    assert(kept && *kept == changedLibrary && kept->cuesKnown);

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

// A stick with both catalogs whose rekordbox catalog cannot be read: no
// fingerprint, not Engine's alone (which, against a backup of both,
// reads as a different library). A cancelled read likewise gives none.
void unreadableCatalogCases(const fs::path &fixture)
{
    const fs::path stick = seabass::testing::scratchRoot() / "library_fingerprint_reader_test_unreadable";
    fs::remove_all(stick);
    fs::create_directories(stick);
    fs::copy(fixture / "rekordbox", stick / "PIONEER", fs::copy_options::recursive);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    const QString pioneer = QString::fromStdString(seabass::pathToUtf8(stick / "PIONEER"));
    const QString engine = QString::fromStdString(seabass::pathToUtf8(stick / "Engine Library"));

    const auto whole = seabass::gui::readLibraryFingerprint(pioneer, engine, seabass::gui::FingerprintPass::Cues);
    assert(whole && whole->trackCount > 0 && "both catalogs read: the precondition");
    const auto engineAlone = seabass::gui::readLibraryFingerprint(QString(), engine, seabass::gui::FingerprintPass::Cues);
    assert(engineAlone && engineAlone->trackCount > 0 && engineAlone->trackCount < whole->trackCount);

    seabass::application::CancellationToken cancel;
    cancel.cancel();
    seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
    assert(!seabass::gui::readLibraryFingerprint(pioneer, engine, seabass::gui::FingerprintPass::Tracks, cancel)
           && "a cancelled read gives no fingerprint");

    // Garbage where export.pdb was: the rekordbox read throws.
    {
        std::ofstream pdb(stick / "PIONEER" / "rekordbox" / "export.pdb", std::ios::binary | std::ios::trunc);
        pdb << "not a pdb";
    }
    seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
    for (const auto pass : {seabass::gui::FingerprintPass::Tracks, seabass::gui::FingerprintPass::Cues}) {
        const auto read = seabass::gui::readLibraryFingerprint(pioneer, engine, pass);
        assert(!read && "an unreadable catalog gives no fingerprint, not the other one's alone");
    }
    std::cout << "readLibraryFingerprint: an unreadable or cancelled catalog gives no fingerprint OK\n";
    seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
    fs::remove_all(stick);
}

// The lengths the Engine catalog does not record, with the stick's
// duration cache empty: the Tracks stage opens no audio file (every such
// track reads 0), the Full stage probes them, and the fingerprint is the
// same either way, since a probed length is not part of a track's
// identity. The fixture has no audio, so a short MP3 is planted where
// each untimed track's file should be: without it the probe would fail
// at every stage and the comparison would prove nothing.
void durationStageCases(const fs::path &fixture)
{
    using seabass::gui::LibraryCatalogCache;
    auto &cache = LibraryCatalogCache::instance();
    const fs::path stick = seabass::testing::scratchRoot() / "library_fingerprint_reader_test_durations";
    fs::remove_all(stick);
    fs::create_directories(stick);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    const std::string engine = seabass::pathToUtf8(stick / "Engine Library");
    const QString enginePath = QString::fromStdString(engine);

    std::set<std::string> untimed;
    for (const auto &track : cache.tracksFor("engine", engine, LibraryCatalogCache::Detail::Tracks)) {
        if (track.durationSeconds <= 0.0 && !track.filePath.empty() && track.streamingSource.empty()) {
            untimed.insert(track.filePath);
        }
    }
    assert(!untimed.empty() && "the fixture has Engine tracks without a catalog length");
    for (const std::string &path : untimed) {
        // Only ever inside the scratch stick.
        const std::string under = seabass::pathToUtf8(stick) + "/";
        assert(path.rfind(under, 0) == 0 && path.find("/../") == std::string::npos);
        fs::create_directories(seabass::pathFromUtf8(path).parent_path());
        seabass::test_fixture::mp3::writeMp3(seabass::pathFromUtf8(path), 400, true);
    }
    cache.invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
    fs::remove_all(stick / "Seabass");  // no duration cache on this stick

    const auto atTracks = cache.stagedTracksFor("engine", engine, LibraryCatalogCache::Detail::Tracks);
    assert(atTracks.stage == LibraryCatalogCache::Detail::Tracks);
    for (const auto &track : atTracks.tracks) {
        if (untimed.count(track.filePath) > 0) {
            assert(track.durationSeconds == 0.0 && !track.durationIsProbed
                   && "the Tracks stage probes nothing, even with the duration cache empty");
        }
    }
    assert(!fs::exists(stick / "Seabass" / "caches") && "and so caches nothing");
    const auto fromTracks = seabass::gui::readLibraryFingerprint(QString(), enginePath, seabass::gui::FingerprintPass::Tracks);

    std::size_t probed = 0;
    for (const auto &track : cache.tracksFor("engine", engine, LibraryCatalogCache::Detail::Full)) {
        if (untimed.count(track.filePath) > 0 && track.durationSeconds > 0.0 && track.durationIsProbed) {
            ++probed;
        }
    }
    assert(probed > 0 && "the Full stage probed the planted files: the precondition of the comparison below");
    const auto fromFull = seabass::gui::readLibraryFingerprint(QString(), enginePath, seabass::gui::FingerprintPass::Cues);
    assert(fromTracks && fromFull && fromTracks->cuesKnown && fromFull->cuesKnown);
    assert(*fromTracks == *fromFull && "a fingerprint from a Tracks read equals one from a Full read");
    std::cout << "the Tracks stage opens no audio file, Full probes " << probed << " rows of " << untimed.size()
              << " files, and the fingerprint is the same at both OK\n";
    cache.invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
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
    unreadableCatalogCases(seabass::pathFromUtf8(argv[1]));
#ifdef SEABASS_TEST_HAVE_PROBE
    durationStageCases(seabass::pathFromUtf8(argv[1]));
#else
    std::cout << "SKIP durationStageCases: this build has no duration probe\n";
#endif
    std::cout << "All library_fingerprint_reader tests passed.\n";
    return 0;
}
