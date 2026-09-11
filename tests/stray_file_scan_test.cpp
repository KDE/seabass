// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The unreferenced-file half of a Clean Up scan, end to end and without
// Qt: real audio files in a temp stick, real catalog Tracks to subtract
// them from, the real walk, probe and metadata cache, then the same
// DuplicateTrackFinder + DuplicateCleanupPlanner the page runs.
//
// The property worth proving here is not any one of those steps -- each
// has its own test -- but that the composition still ends where it must:
// a file no catalog references is offered for deletion only when a
// catalogued copy of the same recording survives it, and the answer is
// refused outright when the catalogs it was subtracted from are not
// fully known. Every past mistake in this area has been a partial answer
// presented as a complete one.
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include "domain/duplicate_cleanup.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "infrastructure/cleanup/stray_file_scan.hpp"
#include "mp3_fixture.hpp"

#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::test_fixture::mp3;
namespace fs = std::filesystem;

namespace
{

// An MP3 under Contents/ carrying the given tags. Without a Xing header
// its length can only be estimated, which is what `exactLength` picks.
fs::path writeTrackFile(const fs::path &stickRoot, const std::string &name, const std::string &title,
                         const std::string &artist, int frames, bool exactLength)
{
    fs::path path = stickRoot / "Contents" / name;
    writeMp3(path, frames, exactLength);
    TagLib::FileRef file(path.string().c_str());
    assert(!file.isNull());
    file.tag()->setTitle(TagLib::String(title, TagLib::String::UTF8));
    file.tag()->setArtist(TagLib::String(artist, TagLib::String::UTF8));
    assert(file.save());
    return path;
}

// A catalog row for a file that is really there, as a reader would
// produce it.
domain::Track catalogRow(const std::string &format, const std::string &sourceId, const fs::path &path,
                          const std::string &title, const std::string &artist, double duration, int bitrate)
{
    domain::Track t;
    t.sourceId = sourceId;
    t.format = format;
    t.title = title;
    t.artist = artist;
    t.filename = path.filename().string();
    t.filePath = path.string();
    t.durationSeconds = duration;
    t.bitrate = bitrate;
    t.fileSizeBytes = fs::file_size(path);
    return t;
}

const domain::Track *findStray(const std::vector<domain::Track> &tracks, const std::string &filename)
{
    for (const auto &t : tracks) {
        if (t.filename == filename) {
            return &t;
        }
    }
    return nullptr;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_stray_file_scan_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // 40 frames is ~1.04 s; the grouping tolerance is 2 s, so two files
    // of the same frame count are the same length as far as the finder
    // is concerned.
    const int Frames = 40;
    const double Seconds = expectedSeconds(Frames);

    fs::path kept = writeTrackFile(root, "01_kept.mp3", "Flaschenpost", "Kollektiv Turmstrasse", Frames, true);
    fs::path stray = writeTrackFile(root, "33_stray.mp3", "Flaschenpost", "Kollektiv Turmstrasse", Frames, true);
    fs::path lonely = writeTrackFile(root, "lonely.mp3", "No Goodbye", "Paul Kalkbrenner", Frames, true);
    fs::path guessed = writeTrackFile(root, "guessed.mp3", "Sky and Sand", "Fritz Kalkbrenner", Frames, false);
    fs::path guessedTwin = writeTrackFile(root, "guessed_twin.mp3", "Sky and Sand", "Fritz Kalkbrenner", Frames, true);

    application::CatalogTracks catalogs;
    catalogs.rekordbox = std::vector<domain::Track>{
        catalogRow("rekordbox", "1", kept, "Flaschenpost", "Kollektiv Turmstrasse", Seconds, 128)};
    catalogs.engine = std::vector<domain::Track>{
        catalogRow("engine", "2", guessedTwin, "Sky and Sand", "Fritz Kalkbrenner", Seconds, 128)};

    // A catalog on the stick that could not be read refuses the whole
    // answer. Absent is fine, unreadable is not: a file that catalog
    // still needs would otherwise be presented as unreferenced, and the
    // review it enters ends in deleting it.
    {
        auto refused = infrastructure::cleanup::scanStrayFiles(root.string(), catalogs, {"engine"},
                                                                application::CancellationToken::none());
        assert(!refused.usable);
        assert(refused.tracks.empty());
        assert(refused.filesFound == 0);
        assert(refused.refusal.find("engine") != std::string::npos);
        std::cout << "case 1 (an unreadable catalog refuses the whole scan) OK\n";
    }

    // No catalog at all is the same refusal, not "everything on this
    // stick is unreferenced".
    {
        auto refused = infrastructure::cleanup::scanStrayFiles(root.string(), application::CatalogTracks{}, {},
                                                                application::CancellationToken::none());
        assert(!refused.usable);
        assert(refused.tracks.empty());
        std::cout << "case 2 (no catalog at all -> refused, never 'all unreferenced') OK\n";
    }

    auto scan = infrastructure::cleanup::scanStrayFiles(root.string(), catalogs, {},
                                                         application::CancellationToken::none());
    assert(scan.usable);
    assert(scan.metadataProbeAvailable);

    // Three of the five files are referenced by nothing; both catalogs
    // are named in the answer, because which ones were consulted is what
    // the answer is worth.
    {
        assert(scan.filesFound == 3);
        assert(scan.unreadable == 0);
        assert(scan.tracks.size() == 3);
        assert(scan.catalogsConsulted.size() == 2);
        assert(!scan.walkIncomplete);
        assert(findStray(scan.tracks, "33_stray.mp3") != nullptr);
        assert(findStray(scan.tracks, "lonely.mp3") != nullptr);
        assert(findStray(scan.tracks, "guessed.mp3") != nullptr);
        assert(findStray(scan.tracks, "01_kept.mp3") == nullptr);      // rekordbox has it
        assert(findStray(scan.tracks, "guessed_twin.mp3") == nullptr);  // Engine has it
        for (const auto &t : scan.tracks) {
            assert(t.isUnreferenced);
            assert(t.format == "disk");
            assert(t.sourceId == t.filePath);  // no row, so the path is the identity
        }
        std::cout << "case 3 (only the files no catalog references, with the catalogs named) OK\n";
    }

    // The file whose length had to be estimated says so, and the ones
    // read from a Xing header do not.
    {
        assert(findStray(scan.tracks, "guessed.mp3")->durationIsEstimated);
        assert(!findStray(scan.tracks, "33_stray.mp3")->durationIsEstimated);
        std::cout << "case 4 (an estimated length survives the trip out of the probe) OK\n";
    }

    // Now the part the page actually runs: strays alongside catalogued
    // rows, through the same finder and planner.
    std::vector<domain::Track> all;
    if (catalogs.rekordbox) {
        all.insert(all.end(), catalogs.rekordbox->begin(), catalogs.rekordbox->end());
    }
    if (catalogs.engine) {
        all.insert(all.end(), catalogs.engine->begin(), catalogs.engine->end());
    }
    all.insert(all.end(), scan.tracks.begin(), scan.tracks.end());

    bool sawFlaschenpost = false;
    bool sawSkyAndSand = false;
    for (const auto &group : domain::DuplicateTrackFinder::find(all)) {
        auto plan = domain::DuplicateCleanupPlanner::plan(group);
        if (plan.survivor.title == "Flaschenpost") {
            sawFlaschenpost = true;
            // The catalogued copy survives even though the stray is
            // identical: keeping the stray would leave rekordbox
            // pointing at a file we then delete.
            assert(!plan.survivor.isUnreferenced);
            assert(plan.survivor.sourceId == "1");
            assert(plan.unreferencedFilesToDelete.size() == 1);
            // walkAudioFiles() stores generic_string() (forward slashes)
            // for a stray file's path, deliberately -- not native
            // separators. Identical to .string() on Linux (where this
            // test was first written), so compare against it explicitly
            // rather than the platform-dependent one.
            assert(plan.unreferencedFilesToDelete[0].filePath == stray.generic_string());
            assert(plan.unreferencedFilesHeldBack.empty());
        } else if (plan.survivor.title == "Sky and Sand") {
            sawSkyAndSand = true;
            // One copy's length was guessed, so this group might not be
            // one recording at all: nothing here is deleted.
            assert(plan.unreferencedFilesToDelete.empty());
            assert(plan.unreferencedFilesHeldBack.size() == 1);
            // See the same fix on unreferencedFilesToDelete above.
            assert(plan.unreferencedFilesHeldBack[0].filePath == guessed.generic_string());
        } else {
            assert(false && "no other group should exist");
        }
    }
    assert(sawFlaschenpost && sawSkyAndSand);
    // "lonely.mp3" matches nothing, so it is in no group at all: never
    // touched, only counted. That is the honest answer for real music
    // that fell out of the database -- a re-import candidate, not a
    // deletion candidate.
    std::cout << "case 5 (a stray is proposed only behind a surviving catalogued copy) OK\n";

    // The cache is on the stick, so a second scan needs no file reads --
    // and must not change a single answer.
    {
        assert(fs::exists(root / "Seabass" / "caches" / "metadata.jsonl"));
        auto again = infrastructure::cleanup::scanStrayFiles(root.string(), catalogs, {},
                                                              application::CancellationToken::none());
        assert(again.filesFound == scan.filesFound);
        assert(again.tracks.size() == scan.tracks.size());
        assert(findStray(again.tracks, "guessed.mp3")->durationIsEstimated);
        std::cout << "case 6 (a second scan, served from the on-stick cache, answers identically) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all stray_file_scan_test cases passed\n";
    return 0;
}
