// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The progressive read gives the same library as the one-shot read.
//
// A stick used to be read in one pass: catalog, every ANLZ file, a stat
// per audio file and per artwork. It is now three stages (readTracks,
// fillCues, fillFileSizes) that a cache hands out one by one. This test
// runs the stages over a copy of the committed anonymized fixture, with
// audio files and artwork planted for two thirds of the paths the
// catalogs name, and asserts:
//
// - the three stages together equal readAll() plus fillFileSizes(), field
//   by field, for rekordbox, OneLibrary and Engine, and also when the cue
//   stage runs on a fresh reader that never read the catalog itself;
// - every catalog digest equals the one the one-shot readers produced at
//   the base commit (0bf22cb0), pinned below: the digest covers tracks,
//   metadata and cues, so the split lost or changed nothing a DJ sees;
// - the sizes are the planted ones (and the counts equal what the old
//   readers' own stats produced over the same planting), OneLibrary's
//   catalog sizes are kept, missing files read 0;
// - the catalog stage of rekordbox carries no cue and no edit time, and
//   the cue stage ticks and checks cancellation once per track;
// - artwork is named whether or not the file is there (the readers no
//   longer stat it), and the named count is the catalog's.
//
// Pinned numbers come from running the base commit's readers over the
// same planted copy. If the fixture is regenerated they change, and this
// test says which one moved.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "application/catalog_digest.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/fill_file_sizes.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::CuePoint;
using seabass::domain::Track;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

bool sameCue(const CuePoint &a, const CuePoint &b)
{
    return a.kind == b.kind && a.hotCueNumber == b.hotCueNumber && a.positionMs == b.positionMs
        && a.color == b.color && a.comment == b.comment && a.isLoop == b.isLoop && a.loopEndMs == b.loopEndMs;
}

// Every field of domain::Track, named, so a difference says where it is.
std::string firstDifference(const Track &a, const Track &b)
{
    if (a.sourceId != b.sourceId) return "sourceId";
    if (a.format != b.format) return "format";
    if (a.isUnreferenced != b.isUnreferenced) return "isUnreferenced";
    if (a.title != b.title) return "title";
    if (a.artist != b.artist) return "artist";
    if (a.filename != b.filename) return "filename";
    if (a.filePath != b.filePath) return "filePath";
    if (a.artworkPath != b.artworkPath) return "artworkPath";
    if (a.streamingSource != b.streamingSource) return "streamingSource";
    if (a.fileSizeBytes != b.fileSizeBytes) return "fileSizeBytes";
    if (a.bitrate != b.bitrate) return "bitrate";
    if (a.durationSeconds != b.durationSeconds) return "durationSeconds";
    if (a.durationIsEstimated != b.durationIsEstimated) return "durationIsEstimated";
    if (a.bpm != b.bpm) return "bpm";
    if (a.key != b.key) return "key";
    if (a.cues.size() != b.cues.size()) return "cues (count)";
    for (size_t i = 0; i < a.cues.size(); ++i) {
        if (!sameCue(a.cues[i], b.cues[i])) return "cues[" + std::to_string(i) + "]";
    }
    if (a.rating != b.rating) return "rating";
    if (a.comment != b.comment) return "comment";
    if (a.album != b.album) return "album";
    if (a.catalogRows.size() != b.catalogRows.size()) return "catalogRows";
    if (a.playlists.size() != b.playlists.size()) return "playlists (count)";
    for (size_t i = 0; i < a.playlists.size(); ++i) {
        if (a.playlists[i].name != b.playlists[i].name || a.playlists[i].position != b.playlists[i].position) {
            return "playlists[" + std::to_string(i) + "]";
        }
    }
    if (a.playCount != b.playCount) return "playCount";
    if (a.lastPlayedAt != b.lastPlayedAt) return "lastPlayedAt";
    if (a.metadataModifiedAt != b.metadataModifiedAt) return "metadataModifiedAt";
    return {};
}

void checkSameLibrary(const std::vector<Track> &got, const std::vector<Track> &want, const std::string &what)
{
    check(got.size() == want.size(), what + ": " + std::to_string(got.size()) + " tracks, expected "
                                         + std::to_string(want.size()));
    int reported = 0;
    for (size_t i = 0; i < std::min(got.size(), want.size()); ++i) {
        const std::string diff = firstDifference(got[i], want[i]);
        if (!diff.empty() && reported++ < 5) {
            check(false, what + ": track " + want[i].sourceId + " differs in " + diff);
        }
    }
    check(reported == 0, what + ": " + std::to_string(reported) + " tracks differ");
}

struct CountingProgress : seabass::application::ProgressReporter
{
    std::vector<std::string> labels;
    size_t lastTotal = 0;
    size_t ticks = 0;
    size_t cancelAtTick = 0;
    seabass::application::CancellationToken token;

    void start(const std::string &label, size_t total) override
    {
        labels.push_back(label);
        lastTotal = total;
        ticks = 0;
    }
    void tick(size_t) override
    {
        if (++ticks == cancelAtTick) {
            token.cancel();
        }
    }
    void finish() override {}
    void warn(const std::string &) override {}
};

// The base commit's readAll() digests over this fixture (see the header).
const std::string RekordboxDigest = "b5b7775ec3990ee5a7e9b6e4d4597082347a18e592dbf219382ada1cc8fdce65";
const std::string OneLibraryDigest = "d8c5556ad1e4e18bbd08bcb89dc9dbadf112dbfc40aa52e637e2c7423161bd76";
const std::string EngineDigest = "70ee572caca6bd8c04e524289b54f1c9a306130befde89b824e5fd1b882d2373";

// What the base commit's readers returned over the same planting: rows
// with a size (their own stat per audio file) and rows with a cue.
constexpr int RekordboxRowsSized = 770;
constexpr int EngineRowsSized = 1043;
constexpr int OneLibraryRowsSized = 1644;  // all from the catalog, no stat
constexpr int RekordboxRowsWithCues = 222;
constexpr int OneLibraryRowsWithCues = 17;
constexpr int EngineRowsWithCues = 125;
// Rows whose catalog names artwork, whether or not the file is there.
constexpr int EngineRowsWithArtwork = 1467;
constexpr int RekordboxRowsWithArtwork = 1160;

int countIf(const std::vector<Track> &tracks, bool (*pred)(const Track &))
{
    return static_cast<int>(std::count_if(tracks.begin(), tracks.end(), pred));
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: progressive_catalog_read_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    const fs::path fixture = seabass::pathFromUtf8(argv[1]);

    const fs::path scratch = seabass::testing::scratchRoot() / "progressive_catalog_read";
    seabass::testing::sandboxSeabassHome(scratch / "home");
    const fs::path stick = scratch / "stick";
    const fs::path pioneer = stick / "PIONEER";
    const fs::path engineLibrary = stick / "Engine Library";
    fs::remove_all(stick);
    fs::create_directories(stick);
    // A copy, never the fixture itself: the OneLibrary and Engine opens
    // may roll back a journal, and planting writes next to the catalogs.
    fs::copy(fixture / "rekordbox", pioneer, fs::copy_options::recursive);
    fs::copy(fixture / "engine", engineLibrary, fs::copy_options::recursive);
    const std::string pioneerUtf8 = seabass::pathToUtf8(pioneer);
    const std::string engineUtf8 = seabass::pathToUtf8(engineLibrary);

    // Plant two thirds of every audio file and artwork the catalogs name,
    // by sorted position, with sizes that differ from file to file.
    std::map<std::string, std::uint64_t> plantedSize;
    {
        std::set<std::string> audio;
        std::set<std::string> artwork;
        auto collect = [&](const std::vector<Track> &tracks) {
            for (const auto &t : tracks) {
                if (!t.filePath.empty()) audio.insert(t.filePath);
                if (!t.artworkPath.empty()) artwork.insert(t.artworkPath);
            }
        };
        collect(seabass::infrastructure::rekordbox::KaitaiRekordboxReader(pioneerUtf8).readTracks());
        collect(seabass::infrastructure::onelibrary::OneLibraryReader(pioneerUtf8).readTracks());
        collect(seabass::infrastructure::engine::LibdjinteropEngineReader(engineUtf8).readTracks());
        size_t i = 0;
        for (const auto &path : audio) {
            if (i % 3 != 0) {
                const fs::path file = seabass::pathFromUtf8(path);
                fs::create_directories(file.parent_path());
                std::ofstream(file, std::ios::binary).put('x');
                const std::uint64_t size = 1000 + (i * 37) % 5000;
                fs::resize_file(file, size);
                plantedSize[path] = size;
            }
            ++i;
        }
        i = 0;
        for (const auto &path : artwork) {
            if (i++ % 3 != 0) {
                const fs::path file = seabass::pathFromUtf8(path);
                fs::create_directories(file.parent_path());
                std::ofstream(file, std::ios::binary) << "jpg";
            }
        }
        check(audio.size() == 3015, "distinct audio paths: " + std::to_string(audio.size()) + ", expected 3015");
    }

    auto expectPlantedSizes = [&](const std::vector<Track> &tracks, const std::string &what) {
        int wrong = 0;
        for (const auto &t : tracks) {
            auto it = plantedSize.find(t.filePath);
            const std::uint64_t want = it == plantedSize.end() ? 0 : it->second;
            if (t.fileSizeBytes != want && wrong++ < 3) {
                check(false, what + ": track " + t.sourceId + " size " + std::to_string(t.fileSizeBytes)
                                 + ", planted " + std::to_string(want));
            }
        }
        check(wrong == 0, what + ": " + std::to_string(wrong) + " sizes wrong");
    };
    auto sized = [](const Track &t) { return t.fileSizeBytes > 0; };
    auto withCues = [](const Track &t) { return !t.cues.empty(); };
    auto withArtwork = [](const Track &t) { return !t.artworkPath.empty(); };
    auto artworkMissing = [](const Track &t) {
        return !t.artworkPath.empty() && !fs::exists(seabass::pathFromUtf8(t.artworkPath));
    };

    // rekordbox: the one format whose cues are a stage of their own.
    {
        using seabass::infrastructure::rekordbox::KaitaiRekordboxReader;
        KaitaiRekordboxReader reader(pioneerUtf8);
        CountingProgress progress;
        reader.setProgressReporter(progress);
        auto tracks = reader.readTracks();
        check(progress.labels.size() == 1 && progress.ticks == tracks.size(),
              "rekordbox readTracks ticks once per track");
        check(countIf(tracks, withCues) == 0, "rekordbox readTracks carries no cues");
        check(std::none_of(tracks.begin(), tracks.end(), [](const Track &t) { return t.metadataModifiedAt != 0; }),
              "rekordbox readTracks carries no ANLZ edit time");
        check(countIf(tracks, sized) == 0, "rekordbox readTracks stats no audio file");

        reader.fillCues(tracks);
        check(progress.labels.size() == 2 && progress.labels[1] == "Reading rekordbox cues",
              "rekordbox fillCues is its own progress pass");
        check(progress.lastTotal == tracks.size() && progress.ticks == tracks.size(),
              "rekordbox fillCues ticks once per track");
        check(countIf(tracks, sized) == 0, "rekordbox fillCues stats no audio file");
        seabass::application::fillFileSizes(tracks);

        auto reference = KaitaiRekordboxReader(pioneerUtf8).readAll();
        seabass::application::fillFileSizes(reference);
        checkSameLibrary(tracks, reference, "rekordbox stages vs readAll");

        // The cue stage on a reader that never read the catalog: it finds
        // the analysis paths in export.pdb itself.
        auto split = KaitaiRekordboxReader(pioneerUtf8).readTracks();
        KaitaiRekordboxReader(pioneerUtf8).fillCues(split);
        seabass::application::fillFileSizes(split);
        checkSameLibrary(split, reference, "rekordbox cues on a fresh reader vs readAll");

        check(seabass::application::catalogDigest(tracks) == RekordboxDigest, "rekordbox digest equals the base commit's");
        check(countIf(tracks, withCues) == RekordboxRowsWithCues,
              "rekordbox rows with cues: " + std::to_string(countIf(tracks, withCues)));
        check(countIf(tracks, sized) == RekordboxRowsSized,
              "rekordbox rows sized: " + std::to_string(countIf(tracks, sized)));
        check(std::all_of(tracks.begin(), tracks.end(), [](const Track &t) { return t.metadataModifiedAt > 0; }),
              "rekordbox: every row has its .EXT edit time after fillCues");
        check(countIf(tracks, withArtwork) == RekordboxRowsWithArtwork, "rekordbox rows naming artwork");
        expectPlantedSizes(tracks, "rekordbox");

        // readAll() keeps the one progress bar it always had.
        CountingProgress oneShot;
        KaitaiRekordboxReader oneShotReader(pioneerUtf8);
        oneShotReader.setProgressReporter(oneShot);
        oneShotReader.readAll();
        check(oneShot.labels == std::vector<std::string>{"Scanning rekordbox tracks"} && oneShot.ticks == tracks.size(),
              "rekordbox readAll shows one pass under its old label");

        // Cancelled in the middle of the cue pass: it stops at that track.
        KaitaiRekordboxReader cancelled(pioneerUtf8);
        CountingProgress cancelling;
        cancelling.cancelAtTick = 10;
        auto partial = cancelled.readTracks();
        cancelled.setProgressReporter(cancelling);
        cancelled.setCancellationToken(cancelling.token);
        bool threw = false;
        try {
            cancelled.fillCues(partial);
        } catch (const seabass::application::OperationCancelled &) {
            threw = true;
        }
        check(threw && cancelling.ticks == 10, "rekordbox fillCues stops at the track it was cancelled on");
    }

    // OneLibrary: cues are in the catalog; sizes come from the catalog too.
    {
        using seabass::infrastructure::onelibrary::OneLibraryReader;
        OneLibraryReader reader(pioneerUtf8);
        auto tracks = reader.readTracks();
        const auto catalogSizes = tracks;
        reader.fillCues(tracks);
        seabass::application::fillFileSizes(tracks);
        auto reference = OneLibraryReader(pioneerUtf8).readAll();
        seabass::application::fillFileSizes(reference);
        checkSameLibrary(tracks, reference, "onelibrary stages vs readAll");
        checkSameLibrary(tracks, catalogSizes, "onelibrary sizes are the catalog's, kept by fillFileSizes");
        check(seabass::application::catalogDigest(tracks) == OneLibraryDigest, "onelibrary digest equals the base commit's");
        check(countIf(tracks, sized) == OneLibraryRowsSized, "onelibrary rows sized");
        check(countIf(tracks, withCues) == OneLibraryRowsWithCues, "onelibrary rows with cues");
    }

    // Engine: cues in the catalog, no stat of audio or artwork.
    {
        using seabass::infrastructure::engine::LibdjinteropEngineReader;
        LibdjinteropEngineReader reader(engineUtf8);
        auto tracks = reader.readTracks();
        check(countIf(tracks, sized) == 0, "engine readTracks stats no audio file");
        reader.fillCues(tracks);
        seabass::application::fillFileSizes(tracks);
        auto reference = LibdjinteropEngineReader(engineUtf8).readAll();
        seabass::application::fillFileSizes(reference);
        checkSameLibrary(tracks, reference, "engine stages vs readAll");
        check(seabass::application::catalogDigest(tracks) == EngineDigest, "engine digest equals the base commit's");
        check(countIf(tracks, withCues) == EngineRowsWithCues, "engine rows with cues");
        check(countIf(tracks, sized) == EngineRowsSized, "engine rows sized: " + std::to_string(countIf(tracks, sized)));
        expectPlantedSizes(tracks, "engine");
        check(countIf(tracks, withArtwork) == EngineRowsWithArtwork,
              "engine rows naming artwork: " + std::to_string(countIf(tracks, withArtwork)));
        check(countIf(tracks, artworkMissing) > 0, "engine names artwork whose file is missing");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all progressive_catalog_read_test checks passed\n";
    return 0;
}
