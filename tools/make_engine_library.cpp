// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Create Engine Library, headless, against a real stick.
//
//   make_engine_library <stick root> <output dir> [1|2|3]
//
// Reads the rekordbox export on <stick root> exactly as the app does and
// runs EngineLibraryCreator into <output dir>, which must not already hold
// an Engine Library. The schema generation defaults to 3 (what current
// Denon hardware writes for itself); 1 and 2 are there because firmware
// acceptance per generation is not something this project has a verified
// matrix for, and the only way to find out is to put one in front of a
// player.
//
// Exists because that feature is otherwise reachable only through the GUI,
// and it is the feature that needs the most testing against hardware that
// no automated test can stand in for: a Prime 4 rejected the first library
// it was ever given as corrupt. Writing a candidate to a folder rather than
// straight onto a stick keeps the slow, destructive part -- overwriting a
// stick's library -- a separate, deliberate step.
//
// Prints what was created and then reads the result back with this
// project's own Engine reader, so a library that cannot be read is a
// failure here rather than a surprise on the player. The last line is
// "RESULT: PASS" or "RESULT: FAIL", with a matching exit code.

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using infrastructure::engine::EngineSchemaGeneration;

namespace
{

// One line per phase rather than a live bar: this runs unattended and its
// output is read afterwards, in a log.
class PrintingReporter : public application::ProgressReporter
{
public:
    void start(const std::string &label, size_t total) override
    {
        m_label = label;
        std::cout << "  " << label << " (" << total << ")" << std::flush;
    }
    void tick(size_t) override {}
    void finish() override { std::cout << " done\n" << std::flush; }
    void warn(const std::string &message) override { std::cout << "\n  warning: " << message << "\n" << std::flush; }

private:
    std::string m_label;
};

EngineSchemaGeneration generationFrom(const std::string &argument, bool &ok)
{
    ok = true;
    if (argument == "1") {
        return EngineSchemaGeneration::V1;
    }
    if (argument == "2") {
        return EngineSchemaGeneration::V2;
    }
    if (argument == "3") {
        return EngineSchemaGeneration::V3;
    }
    ok = false;
    return EngineSchemaGeneration::V3;
}

// Points every track at where its file will be once the library is on the
// stick, rather than where it is relative to this staging folder.
//
// EngineLibraryCreator stores each path relative to the directory it is
// creating the library in, which is right when that directory is the
// stick. Staging a candidate somewhere else makes every path relative to
// the build tree instead, and they then climb out of the stick entirely:
// "../../../../../../../media/sebas/RV2/Contents/...". A player given
// that lists all the tracks perfectly and cannot play a single one,
// because not one of those paths resolves once the stick is in a deck.
// Exactly that happened on a Prime 4, and nothing about the library
// itself was wrong.
//
// Rebasing here rather than string-surgery on a prefix: reconstruct the
// absolute file each row points at, then re-express it relative to the
// library's real home. Returns the number of paths changed, or -1 on
// failure.
int rebasePathsFor(const fs::path &stagedLibrary, const fs::path &finalLibrary)
{
    if (fs::weakly_canonical(stagedLibrary) == fs::weakly_canonical(finalLibrary)) {
        return 0;  // created in place; the creator's own paths are already right
    }
    fs::path dbFile = stagedLibrary / "Database2" / "m.db";
    if (!fs::exists(dbFile)) {
        dbFile = stagedLibrary / "m.db";
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(pathToUtf8(dbFile).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    std::vector<std::pair<long long, std::string>> updates;
    sqlite3_stmt *read = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, path FROM Track;", -1, &read, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    while (sqlite3_step(read) == SQLITE_ROW) {
        const long long id = sqlite3_column_int64(read, 0);
        const char *stored = reinterpret_cast<const char *>(sqlite3_column_text(read, 1));
        if (stored == nullptr) {
            continue;
        }
        // operator/ with an absolute right-hand side yields that path, so
        // a row the creator could not make relative is handled too. The
        // column is UTF-8, as everything SQLite stores is.
        const fs::path absolute = fs::weakly_canonical(stagedLibrary / pathFromUtf8(stored));
        std::error_code ec;
        const fs::path rebased = fs::relative(absolute, finalLibrary, ec);
        if (ec || rebased.empty()) {
            continue;
        }
        const std::string next = pathToGenericUtf8(rebased);
        if (next != stored) {
            updates.emplace_back(id, next);
        }
    }
    sqlite3_finalize(read);

    int changed = 0;
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    for (const auto &[id, path] : updates) {
        sqlite3_stmt *write = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE Track SET path = ? WHERE id = ?;", -1, &write, nullptr) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            return -1;
        }
        sqlite3_bind_text(write, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(write, 2, id);
        const bool ok = sqlite3_step(write) == SQLITE_DONE;
        sqlite3_finalize(write);
        if (!ok) {
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
            return -1;
        }
        ++changed;
    }
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return changed;
}

// Every track's file must be there once the stick is in a deck, and must
// be *on the stick*.
//
// Both halves matter, and the second one is the half that bites. A path
// like "../../../../../../../media/sebas/RV2/Contents/x.mp3" resolves
// perfectly well on the machine that wrote it, because that machine
// really does have the stick mounted at that location -- so "does this
// file exist" answers yes and the check passes. Put the stick in a
// player, where it is mounted somewhere else entirely, and every one of
// those paths walks off the device into nothing: "file unavailable" on
// every track. So containment is what is actually asserted here, with
// existence as the weaker second condition.
bool everyTrackResolves(const fs::path &stagedLibrary, const fs::path &finalLibrary, const fs::path &stickRoot)
{
    fs::path dbFile = stagedLibrary / "Database2" / "m.db";
    if (!fs::exists(dbFile)) {
        dbFile = stagedLibrary / "m.db";
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(pathToUtf8(dbFile).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT path FROM Track;", -1, &stmt, nullptr);
    int missing = 0;
    int escaping = 0;
    int total = 0;
    std::string firstMissing;
    std::string firstEscaping;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *stored = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        if (stored == nullptr) {
            continue;
        }
        ++total;
        // Judged relative to the stick, not to this machine's filesystem.
        // "Engine Library" is one level down, so "../Contents/x.mp3"
        // normalises to "Contents/x.mp3" and stays on the device, while a
        // path with seven "../" normalises to something still starting
        // with "..", which is off the device wherever it is mounted.
        // Resolving against this machine's absolute paths cannot see that:
        // the leading "../" run collapses at the filesystem root and lands
        // back on the stick purely because that is where it happens to be
        // mounted here. A deck mounts it somewhere else and finds nothing.
        // fs::path(stored) would construct straight from SQLite's UTF-8
        // bytes, which on Windows a narrow-string fs::path constructor
        // reads as the system's ANSI codepage instead -- any non-ASCII
        // byte (an umlaut, a stroke through an o) comes out corrupted,
        // and fs::exists() below then compares that mangled path against
        // the real file and reports it "missing" though it is sitting
        // right there. pathFromUtf8() reads the bytes as UTF-8 instead,
        // which is unambiguous either way.
        const fs::path withinStick = (fs::path("Engine Library") / pathFromUtf8(stored)).lexically_normal();
        const bool onTheStick = withinStick.empty() || *withinStick.begin() != "..";
        if (!onTheStick) {
            if (escaping == 0) {
                firstEscaping = stored;
            }
            ++escaping;
        } else if (!fs::exists(stickRoot / withinStick)) {
            if (missing == 0) {
                firstMissing = stored;
            }
            ++missing;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    std::cout << "  " << (total - missing - escaping) << " of " << total << " track file(s) are on the stick at "
              << stickRoot << "\n";
    if (escaping > 0) {
        std::cout << "  " << escaping << " path(s) point off the stick, e.g. " << firstEscaping << "\n";
        std::cout << "  (these resolve on this machine and on no player)\n";
    }
    if (missing > 0) {
        std::cout << "  " << missing << " file(s) missing, e.g. " << firstMissing << "\n";
    }
    return missing == 0 && escaping == 0;
}

// What the player looks at first, and what this project now knows to get
// wrong: one Information row, at id 1. Reported here in the tool's own
// output so a candidate library can be checked without opening it by hand.
bool informationRowIsWhereEngineExpects(const fs::path &engineLibrary)
{
    const std::string dbPath = pathToUtf8(engineLibrary / "Database2" / "m.db");
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        std::cout << "  Information row: could not open " << dbPath << "\n";
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    int rows = -1;
    int firstId = -1;
    int major = -1;
    int minor = -1;
    int patch = -1;
    if (sqlite3_prepare_v2(db,
                           "SELECT count(*), coalesce(min(id), 0), coalesce(max(schemaVersionMajor), -1), "
                           "coalesce(max(schemaVersionMinor), -1), coalesce(max(schemaVersionPatch), -1) "
                           "FROM Information;",
                           -1, &stmt, nullptr)
        == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            rows = sqlite3_column_int(stmt, 0);
            firstId = sqlite3_column_int(stmt, 1);
            major = sqlite3_column_int(stmt, 2);
            minor = sqlite3_column_int(stmt, 3);
            patch = sqlite3_column_int(stmt, 4);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    const bool good = rows == 1 && firstId == 1;
    std::cout << "  schema " << major << "." << minor << "." << patch << ", " << rows
              << " Information row(s), first at id " << firstId << (good ? "  (as Engine writes it)" : "  (WRONG)")
              << "\n";
    return good;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: make_engine_library <stick root> <output dir> [1|2|3]\n";
        return 2;
    }
    const fs::path root = pathFromUtf8(argv[1]);
    const fs::path out = pathFromUtf8(argv[2]);
    bool ok = true;
    const EngineSchemaGeneration generation = argc == 4 ? generationFrom(argv[3], ok) : EngineSchemaGeneration::V3;
    if (!ok) {
        std::cerr << "error: schema generation must be 1, 2 or 3\n";
        return 2;
    }

    try {
        const fs::path pioneer = root / "PIONEER";
        if (!fs::exists(pioneer / "rekordbox" / "export.pdb")) {
            std::cout << "error: no rekordbox export at " << pioneer << "\n";
            std::cout << "RESULT: FAIL\n";
            return 1;
        }
        std::cout << "reading rekordbox export on " << root << "\n";
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(pioneer));
        const std::vector<domain::Track> tracks = application::ScanLibrary(reader).execute();
        std::cout << "  " << tracks.size() << " tracks\n";

        fs::create_directories(out);
        std::cout << "creating Engine Library (schema generation "
                  << (generation == EngineSchemaGeneration::V1 ? "1" : generation == EngineSchemaGeneration::V2 ? "2" : "3")
                  << ") in " << out << "\n";
        PrintingReporter reporter;
        application::CancellationToken cancel;
        const auto rekordbox = infrastructure::engine::readRekordboxImportState({}, pathToUtf8(pioneer));
        const auto result = infrastructure::engine::EngineLibraryCreator::create(
            pathToUtf8(out), tracks, generation, reporter, cancel,
            rekordbox.hasRekordboxLibrary ? std::optional<std::uint64_t>(rekordbox.librarySequence) : std::nullopt);
        if (!result.errorMessage.empty()) {
            std::cout << "error: " << result.errorMessage << "\n";
            std::cout << "RESULT: FAIL\n";
            return 1;
        }
        // Issue #42: made from this export, so imported from it. Read
        // back from what was written, not taken from the flag.
        const auto created = infrastructure::engine::readRekordboxImportState(pathToUtf8(out), pathToUtf8(pioneer));
        std::cout << "  rekordbox import counter " << created.engineCounter << ", export.pdb sequence "
                  << created.librarySequence
                  << (created.playerWillOfferImport() ? ": a player WILL offer to import over this library"
                                                      : ": level, a player will not offer an import")
                  << "\n";
        if (created.playerWillOfferImport()) {
            std::cout << "RESULT: FAIL\n";
            return 1;
        }
        std::cout << "  created " << result.tracksCreated << " track(s), skipped " << result.tracksSkipped
                  << ", carried " << result.cuesCopied << " cue(s)\n";

        // Where this library will actually live, which is what every
        // stored path has to be relative to.
        const fs::path finalLibrary = root / "Engine Library";
        const int rebased = rebasePathsFor(out, finalLibrary);
        if (rebased < 0) {
            std::cout << "error: could not point the track paths at " << finalLibrary << "\n";
            std::cout << "RESULT: FAIL\n";
            return 1;
        }
        if (rebased > 0) {
            std::cout << "  pointed " << rebased << " track path(s) at " << finalLibrary << "\n";
        }

        std::cout << "reading it back\n";
        const bool informationOk = informationRowIsWhereEngineExpects(out);
        const bool filesResolve = everyTrackResolves(out, finalLibrary, root);
        infrastructure::engine::LibdjinteropEngineReader engineReader(pathToUtf8(out));
        const std::vector<domain::Track> readBack = application::ScanLibrary(engineReader).execute();
        size_t cued = 0;
        for (const domain::Track &track : readBack) {
            if (!track.cues.empty()) {
                ++cued;
            }
        }
        std::cout << "  " << readBack.size() << " track(s), " << cued << " with cues\n";

        const bool pass =
            informationOk && filesResolve && readBack.size() == static_cast<size_t>(result.tracksCreated);
        std::cout << "RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
        return pass ? 0 : 1;
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\n";
        std::cout << "RESULT: FAIL\n";
        return 1;
    }
}
