// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Whether a Denon player will offer to import the rekordbox library over
// the Engine one, and the single number that decides it.
//
// Measured on hardware (2026-09-18, Prime 4 and Prime Go+): with Engine's
// lastRekordBoxLibraryImportReadCounter level with export.pdb's sequence,
// neither player asks; with them apart, both ask, warning that "existing
// playlist and track metadata will be overwritten".

#include "infrastructure/engine/engine_import_state.hpp"

#include <sqlite3.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

#include <djinterop/djinterop.hpp>

#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::engine;

namespace
{

// export.pdb's first six words: the sequence is the last of them. Enough
// of a file for the header read this checks; nothing here parses pages.
void writePdbWithSequence(const fs::path &pioneer, std::uint32_t sequence)
{
    fs::create_directories(pioneer / "rekordbox");
    std::array<std::uint32_t, 6> header{0, 4096, 20, 354, 5, sequence};
    std::ofstream out(pioneer / "rekordbox" / "export.pdb", std::ios::binary);
    out.write(reinterpret_cast<const char *>(header.data()), sizeof(header));
}

void setCounter(const fs::path &library, std::int64_t value)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
    // No id in the statement: a library libdjinterop created numbers this
    // row 2 and an Engine-written one numbers it 1, which is exactly the
    // assumption this test was written to catch.
    const std::string sql =
        "UPDATE Information SET lastRekordBoxLibraryImportReadCounter = " + std::to_string(value);
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

std::set<std::string> entriesOf(const fs::path &dir)
{
    std::set<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        names.insert(seabass::pathToUtf8(entry.path().filename()));
    }
    return names;
}

void exec(sqlite3 *db, const char *sql)
{
    assert(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_engine_import_state_test";
    fs::remove_all(root);
    seabass::testing::sandboxSeabassHome(root / "home");
    const fs::path library = root / "Engine Library";
    const fs::path pioneer = root / "PIONEER";
    fs::create_directories(library);
    { auto db = djinterop::engine::create_database(seabass::pathToUtf8(library)); }

    // 1. The two numbers apart: the player will ask.
    {
        writePdbWithSequence(pioneer, 15217);
        setCounter(library, 14204);
        const auto state = readRekordboxImportState(seabass::pathToUtf8(library), seabass::pathToUtf8(pioneer));
        assert(state.error.empty());
        assert(state.hasEngineLibrary && state.hasRekordboxLibrary);
        assert(state.engineCounter == 14204 && state.librarySequence == 15217);
        assert(state.playerWillOfferImport());
        std::cout << "case 1 (counters apart: the player will offer to import) OK\n";
    }

    // 2. Marking it imported is what makes the question stop.
    {
        std::string error;
        assert(markRekordboxLibraryImported(seabass::pathToUtf8(library), 15217, &error));
        assert(error.empty());
        const auto state = readRekordboxImportState(seabass::pathToUtf8(library), seabass::pathToUtf8(pioneer));
        assert(state.engineCounter == 15217);
        assert(!state.playerWillOfferImport());
        std::cout << "case 2 (level counters: nothing is offered) OK\n";
    }

    // 3. And the library moving on re-arms it, which is the point: this
    //    is a fact about two libraries, not a setting that stays off.
    {
        writePdbWithSequence(pioneer, 15300);
        const auto state = readRekordboxImportState(seabass::pathToUtf8(library), seabass::pathToUtf8(pioneer));
        assert(state.playerWillOfferImport());
        std::cout << "case 3 (a rekordbox library that moved on asks again) OK\n";
    }

    // 4. A stick with only one of the two libraries has nothing to say.
    {
        const auto engineOnly = readRekordboxImportState(seabass::pathToUtf8(library), seabass::pathToUtf8(root / "nowhere"));
        assert(!engineOnly.playerWillOfferImport() && !engineOnly.hasRekordboxLibrary);
        const auto rekordboxOnly = readRekordboxImportState(seabass::pathToUtf8(root / "nowhere"), seabass::pathToUtf8(pioneer));
        assert(!rekordboxOnly.playerWillOfferImport() && !rekordboxOnly.hasEngineLibrary);
        std::cout << "case 4 (one library alone is not a finding) OK\n";
    }

    // 5. Another connection writing m.db (Engine DJ, the CLI, Seabass on
    //    another thread) is an error to report, not a stick without an
    //    Engine library: the read met a lock, not an older schema.
    const fs::path mdb = library / "Database2" / "m.db";
    {
        sqlite3 *writer = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(mdb).c_str(), &writer) == SQLITE_OK);
        exec(writer, "BEGIN EXCLUSIVE");
        const auto state = readRekordboxImportState(seabass::pathToUtf8(library), seabass::pathToUtf8(pioneer));
        exec(writer, "ROLLBACK");
        sqlite3_close(writer);
        if (!state.hasEngineLibrary || state.error.empty()) {
            std::cerr << "hasEngineLibrary=" << state.hasEngineLibrary << " error='" << state.error << "'\n";
        }
        assert(state.hasEngineLibrary && !state.error.empty() && "a locked database is an error, not no library");
        std::cout << "case 5 (a writer holding the database: an error, not no library) OK\n";
    }

    // 6. And a writer mid-commit, its journal live beside m.db, is not a
    //    stick pulled mid-save: nothing copied aside, no "mid-save".
    {
        const fs::path recovered = seabass::infrastructure::paths::localRoot() / "recovered";
        const std::set<std::string> keptBefore = entriesOf(recovered);
        sqlite3 *writer = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(mdb).c_str(), &writer) == SQLITE_OK);
        exec(writer, "PRAGMA journal_mode=DELETE");
        exec(writer, "PRAGMA cache_size=1");
        exec(writer, "PRAGMA cache_spill=1");
        exec(writer, "BEGIN");
        exec(writer, "CREATE TABLE busy_probe(x)");
        exec(writer, "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 300) "
                     "INSERT INTO busy_probe SELECT randomblob(2000) FROM n");
        assert(seabass::infrastructure::hasPendingJournal(mdb) && "the writer's journal is live");
        const auto state = readRekordboxImportState(seabass::pathToUtf8(library), seabass::pathToUtf8(pioneer));
        exec(writer, "ROLLBACK");
        sqlite3_close(writer);
        if (state.error.empty() || state.error.find("mid-save") != std::string::npos) {
            std::cerr << "error='" << state.error << "'\n";
        }
        assert(state.hasEngineLibrary && !state.error.empty());
        assert(state.error.find("mid-save") == std::string::npos && "a live writer is not a pulled stick");
        assert(entriesOf(recovered) == keptBefore && "nothing copied aside");
        const auto after = readRekordboxImportState(seabass::pathToUtf8(library), seabass::pathToUtf8(pioneer));
        assert(after.error.empty() && after.hasEngineLibrary && after.engineCounter == 15217);
        std::cout << "case 6 (a writer mid-commit is not a pulled stick) OK\n";
    }

    fs::remove_all(root);
    std::cout << "engine_import_state_test: all cases passed\n";
    return 0;
}
