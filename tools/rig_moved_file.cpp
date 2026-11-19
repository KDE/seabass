// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// X5 (#103): a file that moved on the stick has to be found again, with
// the confidence the match deserves, and nothing else may be matched.
//
//   rig_moved_file <stick root>
//
// The check plants ONE defect: a track's audio file is moved into another
// folder under Contents, name and bytes unchanged, which is what a person
// tidying a stick by hand does. Then it runs the same pipeline Library
// Health runs (the missing split through application::trackFileIsPresent,
// the catalog rows against the files on disk through
// application::findUnreferencedFiles, domain::findMovedFiles over both)
// and asks:
//
//   1. is there exactly one finding more than before the plant, and is
//      it for the moved track's path
//   2. does it have exactly one candidate, the moved file, at high
//      confidence (the same name and the same size)
//   3. once the file is back, does the same pipeline find exactly what
//      it found before the plant
//
// "More than before", because the rig's references are not clean: they
// carry rows whose file is missing on purpose, for the checks of that,
// and those are findings with no candidate. Question 3 is the one that
// keeps 1 and 2 honest: a check that reports a finding on an untouched
// stick would pass the first two for the wrong reason, and this rig has
// seen tests do exactly that.
//
// The file is always put back, on every failure path and on SIGINT and
// SIGTERM, the way rig_file_failure does it: a stick left with a track
// moved would fail every later check for a reason that is nothing to do
// with them.

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
#include <set>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/track_file_presence.hpp"
#include "application/use_cases/find_unreferenced_files.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/library_consistency.hpp"
#include "domain/moved_files.hpp"
#include "domain/track.hpp"
#include "infrastructure/cleanup/audio_file_walk.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

// The folder the file is moved into, under Contents so the walk sees it.
constexpr const char *MovedFolder = "X5 moved by the rig";

// Every catalog the stick carries, named one by one, as Library Health
// reads them: OneLibrary included, since a moved file breaks its row too
// and the finding has to carry all three.
application::CatalogTracks readCatalogs(const fs::path &root)
{
    application::CatalogTracks catalogs;
    const fs::path pioneer = root / "PIONEER";
    if (fs::exists(pioneer / "rekordbox" / "export.pdb")) {
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(pioneer));
        catalogs.rekordbox = application::ScanLibrary(reader).execute();
    }
    if (fs::exists(pioneer / "rekordbox" / "exportLibrary.db")) {
        infrastructure::onelibrary::OneLibraryReader reader(pathToUtf8(pioneer));
        catalogs.oneLibrary = application::ScanLibrary(reader).execute();
    }
    const fs::path engine = root / "Engine Library";
    if (fs::exists(engine / "Database2" / "m.db")) {
        infrastructure::engine::LibdjinteropEngineReader reader(pathToUtf8(engine));
        catalogs.engine = application::ScanLibrary(reader).execute();
    }
    return catalogs;
}

std::vector<domain::Track> allRows(const application::CatalogTracks &catalogs)
{
    std::vector<domain::Track> rows;
    for (const auto *catalog : {&catalogs.rekordbox, &catalogs.engine, &catalogs.oneLibrary}) {
        if (*catalog) {
            rows.insert(rows.end(), (*catalog)->begin(), (*catalog)->end());
        }
    }
    return rows;
}

// The check the app makes, through the app's own functions: the rows
// whose file is not there, the files no row names, paired.
std::vector<domain::MovedFileFinding> findMoved(const fs::path &root, const application::CatalogTracks &catalogs)
{
    std::vector<domain::Track> missing;
    for (const domain::Track &row : allRows(catalogs)) {
        if (row.streamingSource.empty() && !row.filePath.empty() && !application::trackFileIsPresent(row)) {
            missing.push_back(row);
        }
    }
    application::CancellationToken never;
    const auto walk = infrastructure::cleanup::walkAudioFiles(pathToUtf8(root / "Contents"), never);
    const auto unreferenced = application::findUnreferencedFiles(walk.files, catalogs);
    // Name and size only, as the app has for a file whose tags cannot be
    // read: the match this check plants needs nothing more.
    std::vector<domain::Track> files;
    for (const auto &file : unreferenced.unreferenced) {
        domain::Track t;
        t.format = "disk";
        t.isUnreferenced = true;
        t.sourceId = file.filePath;
        t.filePath = file.filePath;
        t.filename = pathToUtf8(pathFromUtf8(file.filePath).filename());
        t.fileSizeBytes = file.fileSizeBytes;
        files.push_back(std::move(t));
    }
    std::cout << "  " << missing.size() << " row(s) missing a file, " << unreferenced.unreferenced.size()
              << " file(s) no catalog names\n";
    return domain::findMovedFiles(missing, files,
                                  [](const std::string &p) { return application::normalizedPathKey(p); });
}

#ifdef _WIN32
using PlantChar = wchar_t;
#else
using PlantChar = char;
#endif
constexpr std::size_t PlantPathCapacity = 4096;
PlantChar g_plantedTarget[PlantPathCapacity];
PlantChar g_plantedMoved[PlantPathCapacity];
volatile sig_atomic_t g_planted = 0;

void rememberForSignal(PlantChar *buffer, const fs::path &path)
{
    const auto &native = path.native();
    const std::size_t length = std::min(native.size(), PlantPathCapacity - 1);
    std::copy_n(native.data(), length, buffer);
    buffer[length] = 0;
}

// rename(2) is async-signal-safe; see rig_file_failure for why the undo
// is designed to need nothing else.
extern "C" void restoreOnSignal(int sig)
{
    if (g_planted) {
#ifdef _WIN32
        ::_wrename(g_plantedMoved, g_plantedTarget);
#else
        ::rename(g_plantedMoved, g_plantedTarget);
#endif
        g_planted = 0;
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// A file a killed run left in the rig's folder goes back by its name:
// the folder holds one file at a time, and its original place is the one
// catalog row whose file is missing and shares its name. Swept before
// the check so an earlier interruption is not this run's finding.
void sweepLeftovers(const fs::path &root, const application::CatalogTracks &catalogs)
{
    const fs::path folder = root / "Contents" / MovedFolder;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        return;
    }
    for (const auto &entry : fs::directory_iterator(folder, ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        for (const domain::Track &row : allRows(catalogs)) {
            if (row.filePath.empty() || application::trackFileIsPresent(row)
                || pathToUtf8(pathFromUtf8(row.filePath).filename()) != name) {
                continue;
            }
            std::cout << "  sweeping up after an interrupted run: " << name << " back to " << row.filePath << "\n";
            std::error_code undoEc;
            fs::rename(entry.path(), pathFromUtf8(row.filePath), undoEc);
            if (undoEc) {
                std::cout << "    WARNING: could not put it back: " << undoEc.message() << "\n";
            }
            break;
        }
    }
    fs::remove(folder, ec);  // remove, not remove_all: it should be empty
}

struct Plant
{
    fs::path target;
    fs::path moved;
    bool planted = false;

    void arm()
    {
        rememberForSignal(g_plantedTarget, target);
        rememberForSignal(g_plantedMoved, moved);
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
        fs::rename(moved, target, ec);
        planted = false;
        g_planted = 0;
        if (ec) {
            std::cout << "  WARNING: could not put " << pathToUtf8(target) << " back: " << ec.message() << "\n";
        }
        std::error_code removeEc;
        fs::remove(moved.parent_path(), removeEc);
    }

    ~Plant() { undo(); }
};

bool opens(const fs::path &path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    char byte = 0;
    return in && static_cast<bool>(in.read(&byte, 1));
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cout << "usage: rig_moved_file <stick root>\nRIG RESULT: FAIL\n";
        return 1;
    }
    const fs::path root = pathFromUtf8(argv[1]);
    Plant plant;
    bool ok = true;
    try {
        application::CatalogTracks catalogs = readCatalogs(root);
        if (catalogs.present().empty()) {
            std::cout << "no catalog on the stick\nRIG RESULT: FAIL\n";
            return 1;
        }
        sweepLeftovers(root, catalogs);
        const std::vector<domain::Track> rows = allRows(catalogs);
        std::cout << "read " << rows.size() << " row(s) from " << catalogs.present().size() << " catalog(s) on "
                  << pathToUtf8(root) << "\n";

        // The stick as it is, before the plant: what the reference carries
        // of its own, which the plant must add to and the put-back must
        // leave exactly as it was.
        std::cout << "before the plant:\n";
        const auto keysOf = [](const std::vector<domain::MovedFileFinding> &findings) {
            std::set<std::string> keys;
            for (const auto &finding : findings) {
                keys.insert(application::normalizedPathKey(finding.expectedPath));
            }
            return keys;
        };
        const auto before = findMoved(root, catalogs);
        const std::set<std::string> beforeKeys = keysOf(before);
        std::cout << "  " << before.size() << " track(s) already point at a file that is not there";
        for (const auto &finding : before) {
            if (!finding.candidates.empty()) {
                std::cout << "\n  " << finding.expectedPath << " already has " << finding.candidates.size()
                          << " candidate(s) on this stick, which this check cannot tell from its own\nRIG RESULT: "
                             "FAIL\n";
                return 1;
            }
        }
        std::cout << ", none with a candidate\n";

        // A track whose file is really there, with the size the catalog
        // recorded, so the plant is the only thing that changes and the
        // match is the high tier.
        const domain::Track *victim = nullptr;
        for (const domain::Track &row : rows) {
            if (!row.streamingSource.empty() || row.filePath.empty() || row.catalogFileBytes == 0
                || !opens(pathFromUtf8(row.filePath))) {
                continue;
            }
            std::error_code ec;
            if (fs::file_size(pathFromUtf8(row.filePath), ec) == row.catalogFileBytes && !ec) {
                victim = &row;
                break;
            }
        }
        if (victim == nullptr) {
            std::cout << "no track on this stick has a readable file of the size its catalog recorded, so there "
                         "is nothing to move\nRIG RESULT: FAIL\n";
            return 1;
        }

        plant.target = pathFromUtf8(victim->filePath);
        plant.moved = root / "Contents" / MovedFolder / plant.target.filename();
        std::cout << "moving a track's file into another folder:\n  " << pathToUtf8(plant.target) << "\n  -> "
                  << pathToUtf8(plant.moved) << "\n";
        fs::create_directories(plant.moved.parent_path());
        fs::rename(plant.target, plant.moved);
        plant.arm();
        if (opens(plant.target) || !opens(plant.moved)) {
            std::cout << "  the file did not move, so this check cannot mean anything here\nRIG RESULT: FAIL\n";
            return 1;
        }

        std::cout << "after the plant:\n";
        const auto after = findMoved(root, catalogs);
        const std::string expectedKey = application::normalizedPathKey(victim->filePath);
        const std::string movedKey = application::normalizedPathKey(pathToUtf8(plant.moved));
        if (after.size() != before.size() + 1) {
            std::cout << "  " << after.size() << " finding(s), expected exactly one more than the " << before.size()
                      << " before the plant\n";
            ok = false;
        }
        for (const auto &finding : after) {
            if (beforeKeys.contains(application::normalizedPathKey(finding.expectedPath))) {
                continue;  // the reference's own, unchanged by the plant
            }
            std::cout << "  finding: " << finding.expectedPath << " (" << finding.rows.size() << " row(s), "
                      << finding.candidates.size() << " candidate(s))\n";
            for (const auto &candidate : finding.candidates) {
                std::cout << "    " << candidate.file.filePath << ": "
                          << (candidate.confidence == domain::MovedFileCandidate::Confidence::High ? "high"
                                                                                                    : "confirm")
                          << ", " << candidate.reason << "\n";
            }
            if (application::normalizedPathKey(finding.expectedPath) != expectedKey) {
                std::cout << "    NOT the moved track's path\n";
                ok = false;
            }
            if (finding.rows.size() != catalogs.present().size()) {
                // Every catalog that lists the file lost it. A catalog
                // that never listed it is not a failure of the check,
                // but it is worth seeing in the log.
                std::cout << "    note: " << finding.rows.size() << " row(s) for " << catalogs.present().size()
                          << " catalog(s)\n";
            }
            if (!finding.oneClick()) {
                std::cout << "    expected exactly one high-confidence candidate\n";
                ok = false;
            } else if (application::normalizedPathKey(finding.candidates.front().file.filePath) != movedKey) {
                std::cout << "    the candidate is not the moved file\n";
                ok = false;
            }
        }

        // Put back by hand, which is what repair A does with a save round
        // it: the same pipeline must then find exactly what it found
        // before the plant.
        plant.undo();
        std::cout << "after the file is back:\n";
        const auto back = findMoved(root, catalogs);
        if (keysOf(back) != beforeKeys) {
            std::cout << "  " << back.size() << " finding(s) with the file back where it was; expected the "
                      << before.size() << " from before the plant\n";
            ok = false;
        } else {
            std::cout << "  as before the plant: the file is where the catalogs expect it\n";
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\n";
        ok = false;
    }
    plant.undo();
    std::cout << "RIG RESULT: " << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
