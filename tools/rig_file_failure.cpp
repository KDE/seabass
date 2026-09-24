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

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/scan_library.hpp"
#include "application/track_file_presence.hpp"
#include "domain/library_consistency.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
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
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(pioneer));
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        tracks.insert(tracks.end(), read.begin(), read.end());
    }
    const fs::path engine = root / "Engine Library";
    if (fs::exists(engine / "Database2" / "m.db")) {
        infrastructure::engine::LibdjinteropEngineReader reader(pathToUtf8(engine));
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
    // Opened with the path itself: fopen() would take its bytes through
    // the ANSI code page on Windows.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char byte = 0;
    return static_cast<bool>(in.read(&byte, 1));
}

// APPENDED to the whole filename, never replacing its extension:
// "track.mp3" stashes as "track.mp3.x3-stashed", so removing the suffix
// gives the original name back exactly.
//
// Replacing the extension was the first version and it was wrong in a
// way the happy path could not show. undo() holds the real target in
// memory, so a normal run restored correctly; only sweepLeftovers(),
// which has nothing but the stash filename to work from, had to
// reconstruct it -- and "track.x3-stashed" reconstructs as "track",
// with no extension. The sweep then left an extensionless orphan beside
// the empty directory it was supposed to remove, which is worse damage
// than the interrupted run it was cleaning up after. Caught by planting
// damage by hand and sweeping it, not by any run of the check itself.
constexpr const char *StashSuffix = ".x3-stashed";

// Set while a plant is in the ground, for the signal handler. Raw paths
// rather than std::string because a handler may only call
// async-signal-safe things, and that rules out allocating. They hold the
// path's native characters (wchar_t on Windows, so the wide CRT calls
// below take a Japanese folder name as it is, where the narrow ones would
// read it in the ANSI code page).
#ifdef _WIN32
using PlantChar = wchar_t;
#else
using PlantChar = char;
#endif
constexpr std::size_t PlantPathCapacity = 4096;
PlantChar g_plantedTarget[PlantPathCapacity];
PlantChar g_plantedStash[PlantPathCapacity];
volatile sig_atomic_t g_planted = 0;

void rememberForSignal(PlantChar *buffer, const fs::path &path)
{
    const auto &native = path.native();
    const std::size_t length = std::min(native.size(), PlantPathCapacity - 1);
    std::copy_n(native.data(), length, buffer);
    buffer[length] = 0;
}

// SIGINT and SIGTERM skip destructors, and a killed check would leave a
// DIRECTORY where a track's audio should be. That is worse than an
// ordinary failure: the stick then carries a defect this tool invented,
// Library Health reports it as a real one, and the next run of this
// check picks a different victim because the first no longer opens.
//
// rmdir(2) and rename(2) are both async-signal-safe, which is why the
// plant is an EMPTY directory and the file is moved aside rather than
// copied: undoing it needs exactly those two calls and no allocation.
// _wrmdir() and _wrename() are the same direct CRT syscall wrappers on
// Windows -- no allocation, no exceptions -- so they keep that guarantee
// there too; std::filesystem::remove() does not belong in a signal
// handler.
extern "C" void restoreOnSignal(int sig)
{
    if (g_planted) {
#ifdef _WIN32
        ::_wrmdir(g_plantedTarget);
        ::_wrename(g_plantedStash, g_plantedTarget);
#else
        ::rmdir(g_plantedTarget);
        ::rename(g_plantedStash, g_plantedTarget);
#endif
        g_planted = 0;
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// Damage left by a run that was killed before it could undo itself, on
// this stick or any earlier one. Swept at startup rather than left for a
// person to find: the alternative is a stick that fails Library Health
// for a reason nobody can explain and that no code in Seabass caused.
void sweepLeftovers(const fs::path &root)
{
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        const fs::path stashed = it->path();
        if (stashed.extension() != StashSuffix) {
            continue;
        }
        // Drop the appended suffix, which gives the original name back
        // including its extension: the suffix is the path's last
        // extension, so "track.mp3.x3-stashed" becomes "track.mp3".
        fs::path original = stashed;
        original.replace_extension();
        std::cout << "  sweeping up after an interrupted run: " << pathToUtf8(original.filename()) << "\n";
        std::error_code undoEc;
        if (fs::is_directory(original, undoEc)) {
            fs::remove(original, undoEc);  // remove, not remove_all: it should be empty
        }
        fs::rename(stashed, original, undoEc);
        if (undoEc) {
            std::cout << "    WARNING: could not put it back: " << undoEc.message() << "\n";
        }
    }
}

struct Plant
{
    fs::path target;
    fs::path stashed;
    bool planted = false;

    void arm()
    {
        rememberForSignal(g_plantedTarget, target);
        rememberForSignal(g_plantedStash, stashed);
        g_planted = 1;
        planted = true;
        ::signal(SIGINT, restoreOnSignal);
        ::signal(SIGTERM, restoreOnSignal);
    }

    void undo()
    {
        if (!planted) {
            return;
        }
        std::error_code ec;
        fs::remove_all(target, ec);
        fs::rename(stashed, target, ec);
        planted = false;
        g_planted = 0;
        if (ec) {
            std::cout << "  WARNING: could not put " << pathToUtf8(target) << " back: " << ec.message() << "\n";
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
    const fs::path root = pathFromUtf8(argv[1]);
    Plant plant;
    bool ok = true;
    try {
        sweepLeftovers(root);
        const std::vector<domain::Track> before = readTracks(root);
        std::cout << "read " << before.size() << " track(s) from " << pathToUtf8(root) << "\n";

        // A track whose file is really there, so the plant is the only
        // thing that changes. A library with none would make this check
        // meaningless rather than green.
        const domain::Track *victim = nullptr;
        for (const domain::Track &track : before) {
            if (track.streamingSource.empty() && !track.filePath.empty() && opens(pathFromUtf8(track.filePath))) {
                victim = &track;
                break;
            }
        }
        if (victim == nullptr) {
            std::cout << "no track on this stick has a readable file, so there is nothing to make fail\n"
                         "RIG RESULT: FAIL\n";
            return 1;
        }

        plant.target = pathFromUtf8(victim->filePath);
        plant.stashed = plant.target;
        plant.stashed += StashSuffix;
        std::cout << "planting a directory where a file should be:\n  " << pathToUtf8(plant.target) << "\n";
        fs::rename(plant.target, plant.stashed);
        fs::create_directory(plant.target);
        plant.arm();

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

        // Compared against Track::filePath, which is UTF-8 by the rule.
        const std::string planted = pathToUtf8(plant.target);
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
