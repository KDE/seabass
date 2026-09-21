// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// X3: a file that cannot be read has to become an issue the user can act
// on, and the counters have to admit it.
//
//   rig_file_failure <stick root>
//
// The check plants ONE defect and asks two questions about it. A track's
// audio file is moved aside and a DIRECTORY is created with that exact
// name, so the path the catalog points at still resolves and reading it
// fails. Then:
//
//   1. does the library health check raise an issue that NAMES the file
//   2. does opening it actually fail, so the plant is a real failure and
//      not a simulated one
//
// Question 2 exists because question 1 can pass for the wrong reason. A
// check that plants something harmless and then reports no issue is
// agreeing rather than checking, and this rig has found three tests doing
// exactly that in one day.
//
// WHY A DIRECTORY, since it looks like a trick and the alternatives look
// more obvious:
//
//   chmod 000        does nothing. The sticks are FAT and exFAT, mounted
//                    noowners, and the mode is ignored -- verified on
//                    macOS, where the file stayed readable at -rwx------.
//   a symlink        FAT has none.
//   a truncated file opens fine and fails to PARSE, which is a different
//                    answer to the user ("this file is damaged", not
//                    "this file could not be read") and deserves its own
//                    row rather than making this one mean two things.
//   dm-error / EIO   needs root and a loop device, so it cannot run
//                    against the real sticks this rig exists to test.
//   an exclusive open blocks the reader on Windows and does not on Linux,
//                    so the check would mean something different per
//                    platform.
//
// A directory is the only mechanism that behaves the same everywhere:
// EISDIR on Linux and macOS, access-denied on Windows. It is also a shape
// real sticks produce, via a half-finished copy or a filesystem repair
// that turned a cross-linked chain into a directory entry.
//
// The plant is always removed and the file always put back, including on
// every failure path, because a stick left with a directory where a track
// should be would fail every later check for a reason that has nothing to
// do with them.

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/scan_library.hpp"
#include "application/track_file_presence.hpp"
#include "domain/library_consistency.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

// Reads whichever catalogs the stick carries, the way every other rig
// tool does: rekordbox and Engine, never OneLibrary, because OneLibrary
// mirrors rekordbox and would double every track.
std::vector<domain::Track> readTracks(const fs::path &root)
{
    std::vector<domain::Track> tracks;
    const fs::path pioneer = root / "PIONEER";
    if (fs::exists(pioneer / "rekordbox" / "export.pdb")) {
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer.string());
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        tracks.insert(tracks.end(), read.begin(), read.end());
    }
    const fs::path engine = root / "Engine Library";
    if (fs::exists(engine / "Database2" / "m.db")) {
        infrastructure::engine::LibdjinteropEngineReader reader(engine.string());
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        tracks.insert(tracks.end(), read.begin(), read.end());
    }
    return tracks;
}

// The classification the app itself makes, through the same function the
// app calls. Not a copy: a copy is what let the two disagree in the first
// place, and a check that reimplements the rule it is checking can only
// ever agree with itself.
bool countedAsPresent(const domain::Track &track)
{
    return application::trackFileIsPresent(track);
}

bool opens(const fs::path &path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return false;
    }
    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    char byte = 0;
    const bool read = std::fread(&byte, 1, 1, f) == 1;
    std::fclose(f);
    return read;
}

struct Plant
{
    fs::path target;
    fs::path stashed;
    bool planted = false;

    void undo()
    {
        if (!planted) {
            return;
        }
        std::error_code ec;
        fs::remove_all(target, ec);
        fs::rename(stashed, target, ec);
        planted = false;
        if (ec) {
            std::cout << "  WARNING: could not put " << target.string() << " back: " << ec.message() << "\n";
        }
    }

    ~Plant() { undo(); }
};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cout << "usage: rig_file_failure <stick root>\nRIG RESULT: FAIL\n";
        return 1;
    }
    const fs::path root = argv[1];
    Plant plant;
    bool ok = true;
    try {
        const std::vector<domain::Track> before = readTracks(root);
        std::cout << "read " << before.size() << " track(s) from " << root.string() << "\n";

        // A track whose file is really there, so the plant is the only
        // thing that changes. A library with none would make this check
        // meaningless rather than green.
        const domain::Track *victim = nullptr;
        for (const domain::Track &track : before) {
            if (track.streamingSource.empty() && !track.filePath.empty() && opens(track.filePath)) {
                victim = &track;
                break;
            }
        }
        if (victim == nullptr) {
            std::cout << "no track on this stick has a readable file, so there is nothing to make fail\n"
                         "RIG RESULT: FAIL\n";
            return 1;
        }

        plant.target = victim->filePath;
        plant.stashed = fs::path(victim->filePath).replace_extension(".x3-stashed");
        std::cout << "planting a directory where a file should be:\n  " << plant.target.string() << "\n";
        fs::rename(plant.target, plant.stashed);
        fs::create_directory(plant.target);
        plant.planted = true;

        // The plant has to be a real failure or the rest proves nothing.
        if (opens(plant.target)) {
            std::cout << "  the planted path still opens as a file, so this check cannot mean anything here\n";
            plant.undo();
            std::cout << "RIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "  confirmed: the path no longer opens as a file\n";

        const std::vector<domain::Track> after = readTracks(root);
        std::vector<domain::Track> healthy;
        std::vector<domain::Track> broken;
        for (const domain::Track &track : after) {
            if (!track.streamingSource.empty()) {
                continue;
            }
            (countedAsPresent(track) ? healthy : broken).push_back(track);
        }
        const std::vector<domain::LibraryConsistencyIssue> issues =
            domain::LibraryConsistencyChecker::check(healthy, broken);

        const std::string planted = plant.target.string();
        bool named = false;
        for (const domain::LibraryConsistencyIssue &issue : issues) {
            for (const domain::Track &track : issue.brokenGroup) {
                if (track.filePath == planted) {
                    named = true;
                }
            }
            if (issue.survivor && issue.survivor->filePath == planted) {
                named = true;
            }
        }

        std::cout << "after the plant: " << healthy.size() << " counted present, " << broken.size()
                  << " counted missing, " << issues.size() << " issue(s)\n";
        if (named) {
            std::cout << "  an issue names the unreadable file\n";
        } else {
            std::cout << "  NO issue names " << planted << "\n"
                      << "  the app counts it as present because fs::exists() is true for a directory\n"
                      << "  (library_consistency_controller.cpp: `fs::exists(t.filePath, ec)`), so a file\n"
                      << "  that cannot be read is reported to the user as fine\n";
            ok = false;
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\n";
        ok = false;
    }
    plant.undo();
    std::cout << "RIG RESULT: " << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
