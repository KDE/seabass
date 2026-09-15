// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: the read-only paths on a restored test stick, headless.
//
//   rig_read <stick root> [<archive.zip>]
//
// For each catalog on the stick (rekordbox, OneLibrary, Engine) it reports
// tracks, tracks with cues and playlists -- what Browse Library shows (R1) --
// and runs, timed, the scans behind Library Health, stray cues and Duplicates
// (R3). Given the archive the stick was restored from, it also compares the
// library fingerprint with the one the backup recorded.
//
// It never writes to the stick: the catalogs are read as they are, without the
// duration fill the app runs (that fill caches its results on the stick).
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL" with a matching
// exit code: PASS when every scan finished, and, with an archive given, the
// fingerprint counts match the backup's.

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/library_reader.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/junk_cue.hpp"
#include "domain/library_consistency.hpp"
#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using Clock = std::chrono::steady_clock;

namespace
{

double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Catalog
{
    std::string name;
    std::vector<domain::Track> tracks;
};

void describe(const Catalog &catalog)
{
    std::set<std::string> playlists;
    std::size_t cued = 0;
    std::size_t streaming = 0;
    for (const domain::Track &track : catalog.tracks) {
        if (!track.cues.empty()) {
            ++cued;
        }
        if (!track.streamingSource.empty()) {
            ++streaming;
        }
        for (const domain::PlaylistMembership &membership : track.playlists) {
            playlists.insert(membership.name);
        }
    }
    std::cout << "  " << catalog.name << ": " << catalog.tracks.size() << " tracks, " << cued << " with cues, "
              << playlists.size() << " playlists" << (streaming ? ", " + std::to_string(streaming) + " streaming" : "")
              << "\n";
}

// Library Health, as the app runs it per catalog: tracks whose file exists
// are the healthy side, the rest broken; streaming tracks are neither.
void health(const Catalog &catalog)
{
    const auto start = Clock::now();
    std::vector<domain::Track> healthy;
    std::vector<domain::Track> broken;
    for (const domain::Track &track : catalog.tracks) {
        if (!track.streamingSource.empty()) {
            continue;
        }
        std::error_code ec;
        const bool exists = !track.filePath.empty() && fs::exists(track.filePath, ec);
        (exists ? healthy : broken).push_back(track);
    }
    const std::vector<domain::LibraryConsistencyIssue> issues = domain::LibraryConsistencyChecker::check(healthy, broken);
    const std::vector<domain::JunkCueIssue> junk = domain::JunkCueFinder::find(catalog.tracks);
    std::cout << "  " << catalog.name << ": " << broken.size() << " tracks without their file, " << issues.size()
              << " health issues, " << junk.size() << " stray cue findings (" << secondsSince(start) << " s)\n";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: rig_read <stick root> [<archive.zip>]\n";
        return 2;
    }
    const fs::path root = argv[1];
    bool pass = true;
    try {
        std::vector<Catalog> catalogs;
        const fs::path pioneer = root / "PIONEER";
        const fs::path engine = root / "Engine Library";
        const auto read = [&](const std::string &name, application::LibraryReader &reader) {
            const auto start = Clock::now();
            Catalog catalog{name, application::ScanLibrary(reader).execute()};
            std::cout << "  read " << name << " in " << secondsSince(start) << " s\n";
            catalogs.push_back(std::move(catalog));
        };
        std::cout << "reading catalogs:\n";
        if (fs::exists(pioneer / "rekordbox" / "export.pdb")) {
            infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer.string());
            read("rekordbox", reader);
        }
        if (fs::exists(pioneer / "rekordbox" / "exportLibrary.db")) {
            infrastructure::onelibrary::OneLibraryReader reader(pioneer.string());
            read("onelibrary", reader);
        }
        if (fs::exists(engine / "Database2" / "m.db") || fs::exists(engine / "m.db")) {
            infrastructure::engine::LibdjinteropEngineReader reader(engine.string());
            read("engine", reader);
        }
        if (catalogs.empty()) {
            std::cout << "no catalog on the stick\nRIG RESULT: FAIL\n";
            return 1;
        }

        std::cout << "browse (R1):\n";
        for (const Catalog &catalog : catalogs) {
            describe(catalog);
        }

        std::cout << "library health and stray cues (R3):\n";
        for (const Catalog &catalog : catalogs) {
            health(catalog);
        }

        std::cout << "duplicates across rekordbox and Engine (R3):\n";
        {
            std::vector<domain::Track> both;
            for (const Catalog &catalog : catalogs) {
                if (catalog.name != "onelibrary") {
                    both.insert(both.end(), catalog.tracks.begin(), catalog.tracks.end());
                }
            }
            const auto start = Clock::now();
            const std::vector<domain::DuplicateGroup> groups = domain::DuplicateTrackFinder::find(both);
            std::cout << "  " << groups.size() << " duplicate groups over " << both.size() << " tracks ("
                      << secondsSince(start) << " s)\n";
        }

        if (argc == 3) {
            const application::StickBackupDescription description = application::RestoreStickBackup::describe(argv[2]);
            const auto expected = domain::LibraryFingerprint::parse(description.libraryFingerprint);
            std::vector<domain::Track> fingerprinted;
            for (const Catalog &catalog : catalogs) {
                if (catalog.name != "onelibrary") {
                    fingerprinted.insert(fingerprinted.end(), catalog.tracks.begin(), catalog.tracks.end());
                }
            }
            const domain::LibraryFingerprint live = domain::fingerprintLibrary(fingerprinted);
            std::cout << "fingerprint against " << fs::path(argv[2]).filename().string() << ":\n";
            if (!expected) {
                std::cout << "  the backup recorded no fingerprint\n";
                pass = false;
            } else {
                const bool same = expected->trackCount == live.trackCount && expected->cuedTrackCount == live.cuedTrackCount
                    && expected->playlistCount == live.playlistCount;
                std::cout << "  backup: " << expected->trackCount << " tracks, " << expected->cuedTrackCount << " cued, "
                          << expected->playlistCount << " playlists\n"
                          << "  stick:  " << live.trackCount << " tracks, " << live.cuedTrackCount << " cued, "
                          << live.playlistCount << " playlists -> " << (same ? "same" : "DIFFERENT") << "\n";
                pass = pass && same;
            }
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }
    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
