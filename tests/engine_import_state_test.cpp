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

#include <djinterop/djinterop.hpp>

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
    assert(sqlite3_open((library / "Database2" / "m.db").string().c_str(), &db) == SQLITE_OK);
    // No id in the statement: a library libdjinterop created numbers this
    // row 2 and an Engine-written one numbers it 1, which is exactly the
    // assumption this test was written to catch.
    const std::string sql =
        "UPDATE Information SET lastRekordBoxLibraryImportReadCounter = " + std::to_string(value);
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_engine_import_state_test";
    fs::remove_all(root);
    const fs::path library = root / "Engine Library";
    const fs::path pioneer = root / "PIONEER";
    fs::create_directories(library);
    { auto db = djinterop::engine::create_database(library.string()); }

    // 1. The two numbers apart: the player will ask.
    {
        writePdbWithSequence(pioneer, 15217);
        setCounter(library, 14204);
        const auto state = readRekordboxImportState(library.string(), pioneer.string());
        assert(state.error.empty());
        assert(state.hasEngineLibrary && state.hasRekordboxLibrary);
        assert(state.engineCounter == 14204 && state.librarySequence == 15217);
        assert(state.playerWillOfferImport());
        std::cout << "case 1 (counters apart: the player will offer to import) OK\n";
    }

    // 2. Marking it imported is what makes the question stop.
    {
        std::string error;
        assert(markRekordboxLibraryImported(library.string(), 15217, &error));
        assert(error.empty());
        const auto state = readRekordboxImportState(library.string(), pioneer.string());
        assert(state.engineCounter == 15217);
        assert(!state.playerWillOfferImport());
        std::cout << "case 2 (level counters: nothing is offered) OK\n";
    }

    // 3. And the library moving on re-arms it, which is the point: this
    //    is a fact about two libraries, not a setting that stays off.
    {
        writePdbWithSequence(pioneer, 15300);
        const auto state = readRekordboxImportState(library.string(), pioneer.string());
        assert(state.playerWillOfferImport());
        std::cout << "case 3 (a rekordbox library that moved on asks again) OK\n";
    }

    // 4. A stick with only one of the two libraries has nothing to say.
    {
        const auto engineOnly = readRekordboxImportState(library.string(), (root / "nowhere").string());
        assert(!engineOnly.playerWillOfferImport() && !engineOnly.hasRekordboxLibrary);
        const auto rekordboxOnly = readRekordboxImportState((root / "nowhere").string(), pioneer.string());
        assert(!rekordboxOnly.playerWillOfferImport() && !rekordboxOnly.hasEngineLibrary);
        std::cout << "case 4 (one library alone is not a finding) OK\n";
    }

    fs::remove_all(root);
    std::cout << "engine_import_state_test: all cases passed\n";
    return 0;
}
