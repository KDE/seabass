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
// - An Engine catalog knows its cues from its Tracks read, and a row the
//   player has not analysed (no sample rate recorded, a 48 kHz file) gives
//   one fingerprint whether its cues were read at the 44.1 kHz guess or
//   at the rate its file gave: the fingerprint takes that row's cue kinds
//   and pads, never its positions, so no audio file needs opening.

#include <cassert>
#include <chrono>
#include <string>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

#include <QString>

#include <djinterop/djinterop.hpp>

#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/library_fingerprint_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
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
// track reads 0), the Full stage probes them, and the fingerprint names
// the same tracks either way, since a probed length is not part of a
// track's identity. Its cues are known from the Tracks stage on. The fixture has no audio, so a short MP3 is planted where
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
        // As paths, not strings: the cache hands back the platform's own
        // spelling (backslashes on Windows), so no one separator in a
        // string prefix matches everywhere.
        const fs::path inside = seabass::pathFromUtf8(path).lexically_normal().lexically_relative(stick);
        assert(!inside.empty() && *inside.begin() != "..");
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
    // Engine's cues are in its catalog: known from the Tracks read, and
    // the same as the Full read's.
    assert(fromTracks && fromFull && fromTracks->cuesKnown && fromFull->cuesKnown);
    assert(*fromTracks == *fromFull && "the cue sample does not move between the stages");
    assert(fromTracks->trackCount == fromFull->trackCount && fromTracks->trackHashes == fromFull->trackHashes
           && fromTracks->playlistCount == fromFull->playlistCount
           && fromTracks->playlistHashes == fromFull->playlistHashes
           && "a fingerprint from a Tracks read names the tracks one from a Full read names");
    std::cout << "the Tracks stage opens no audio file, Full probes " << probed << " rows of " << untimed.size()
              << " files, and the fingerprint names the same tracks at both OK\n";
    cache.invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
    fs::remove_all(stick);
}

// See the top of the file: an analysed row (48 kHz recorded) and an
// unanalysed one (none recorded, its hot cue at 48000 samples).
void unverifiedRateCases()
{
    using seabass::infrastructure::engine::LibdjinteropEngineReader;
    const fs::path stick = seabass::testing::scratchRoot() / "library_fingerprint_reader_test_rates";
    fs::remove_all(stick);
    fs::create_directories(stick);
    const std::string engine = seabass::pathToUtf8(stick / "Engine Library");
    {
        auto db = djinterop::engine::create_database(engine);
        djinterop::track_snapshot analysed;
        analysed.title = "analysed";
        analysed.artist = "Artist";
        analysed.relative_path = "../Contents/analysed.mp3";
        analysed.sample_rate = 48000.0;
        analysed.main_cue = 48000.0;
        db.create_track(analysed);
        djinterop::track_snapshot unanalysed;
        unanalysed.title = "unanalysed";
        unanalysed.artist = "Artist";
        unanalysed.relative_path = "../Contents/unanalysed.mp3";
        unanalysed.hot_cues.resize(8);
        unanalysed.hot_cues[0] = djinterop::hot_cue{"", 48000.0, djinterop::pad_color{}};
        db.create_track(unanalysed);
    }
    const auto unanalysedOf = [](std::vector<seabass::domain::Track> &tracks) -> seabass::domain::Track & {
        for (auto &track : tracks) {
            if (track.title == "unanalysed") {
                return track;
            }
        }
        assert(false && "the unanalysed row was read");
        return tracks.front();
    };

    LibdjinteropEngineReader plain(engine);
    auto guessed = plain.readTracks();
    LibdjinteropEngineReader asking(engine);
    asking.setSampleRateSource([](const std::string &) -> std::optional<double> { return 48000.0; });
    auto probed = asking.readTracks();
    assert(guessed.size() == 2 && probed.size() == 2);
    const double atGuess = unanalysedOf(guessed).cues.at(0).positionMs;
    const double atRate = unanalysedOf(probed).cues.at(0).positionMs;
    std::cout << "  the unanalysed hot cue: " << atGuess << " ms at the guess, " << atRate << " ms at 48 kHz\n";
    assert(std::llround(atGuess / 50.0) != std::llround(atRate / 50.0) && "the precondition: the two reads differ");
    const LibraryFingerprint fromGuess = fingerprintLibrary(guessed);
    const LibraryFingerprint fromRate = fingerprintLibrary(probed);
    assert(fromGuess.cuedTrackCount == 2);
    assert(fromGuess.serialize() == fromRate.serialize()
           && "one fingerprint with and without the file's rate");
    {
        // What the row holds still counts: the cue on another pad is
        // another library.
        auto moved = guessed;
        unanalysedOf(moved).cues.at(0).hotCueNumber = 2;
        assert(!(fingerprintLibrary(moved) == fromGuess) && "an unanalysed row's pads are in the fingerprint");
    }

    // Through the cache: a cold Tracks read of the Engine catalog knows
    // its cues, and is the Cues read's fingerprint.
    const QString enginePath = QString::fromStdString(engine);
    seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(seabass::pathToUtf8(stick));
    const auto fromTracks =
        seabass::gui::readLibraryFingerprint(QString(), enginePath, seabass::gui::FingerprintPass::Tracks);
    assert(fromTracks && fromTracks->cuesKnown && "an Engine Tracks read knows its cues");
    const auto fromCues =
        seabass::gui::readLibraryFingerprint(QString(), enginePath, seabass::gui::FingerprintPass::Cues);
    assert(fromCues && *fromCues == *fromTracks && fromTracks->serialize() == fromGuess.serialize());
    std::cout << "an unanalysed 48 kHz row: one fingerprint at the guess and at the file's rate, known from "
                 "the Tracks read OK\n";
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
    // The cache vouches for the cues it read for analysisCheckWindow(),
    // measured from when it read the analysis files' state, near the
    // start of the cue pass. A pass that takes longer than that -- a
    // Debug build on a Windows laptop does -- leaves the next Tracks read
    // told "cues not checked lately", and the stage test fails on the
    // clock rather than on the code. Stood still here, as
    // library_catalog_cache_test does with its own: nothing in this test
    // is about the window.
    seabass::gui::LibraryCatalogCache::instance().setNowFnForTesting(
        [] { return std::chrono::steady_clock::time_point(std::chrono::seconds(1000)); });
    cuesPassCases();
    stageFromTheCacheCases(seabass::pathFromUtf8(argv[1]));
    unreadableCatalogCases(seabass::pathFromUtf8(argv[1]));
    unverifiedRateCases();
#ifdef SEABASS_TEST_HAVE_PROBE
    durationStageCases(seabass::pathFromUtf8(argv[1]));
#else
    std::cout << "SKIP durationStageCases: this build has no duration probe\n";
#endif
    std::cout << "All library_fingerprint_reader tests passed.\n";
    return 0;
}
