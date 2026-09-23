// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Counting the tracks an Engine player will have to analyse (issue #38).
//
// The interesting cases are the ones where a wrong answer looks like a
// right one: a library with no isAnalyzed column reads as "nothing to
// report" and so does a library where everything is analysed, and those
// are not the same state. Each is asserted separately, and the committed
// fixture supplies the real distribution rather than a synthetic one.

#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "infrastructure/engine/engine_analysis_state.hpp"

#include "scratch_path.hpp"

using seabass::infrastructure::engine::auditAnalysisState;
using seabass::infrastructure::engine::AnalysisStateAudit;
namespace fs = std::filesystem;

namespace
{

// A minimal Engine library at <root>/Database2/m.db, with whatever Track
// table the caller describes. Built by hand rather than through
// libdjinterop because the point of two of these cases is a schema
// libdjinterop will not produce.
void makeLibrary(const fs::path &root, const std::string &createTrack, const std::string &rows)
{
    fs::create_directories(root / "Database2");
    sqlite3 *db = nullptr;
    assert(sqlite3_open((root / "Database2" / "m.db").string().c_str(), &db) == SQLITE_OK);
    auto run = [&](const std::string &sql) {
        char *err = nullptr;
        const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::cerr << "setup SQL failed: " << (err ? err : "?") << "\n";
        }
        sqlite3_free(err);
        assert(rc == SQLITE_OK);
    };
    run(createTrack);
    if (!rows.empty()) {
        run(rows);
    }
    sqlite3_close(db);
}

void theFixtureReportsItsRealDistribution(const fs::path &fixtureEngineRoot)
{
    const AnalysisStateAudit audit = auditAnalysisState(fixtureEngineRoot.string());
    assert(audit.error.empty());
    assert(audit.hasColumn);
    // The committed fixture came off a Denon-written stick, and these are
    // its actual numbers. Asserted exactly, not as a ratio: if the
    // fixture is ever regenerated these must be updated deliberately,
    // which is the moment to check the new stick really is in this state.
    std::cout << "  fixture: " << audit.notAnalyzed << " of " << audit.tracksChecked << " not analysed\n";
    assert(audit.tracksChecked == 1564);
    assert(audit.notAnalyzed == 1214);
    assert(audit.worthReporting());
}

void aFullyAnalysedLibraryReportsNothing(const fs::path &scratch)
{
    const fs::path root = scratch / "analysed";
    makeLibrary(root, "CREATE TABLE Track (id INTEGER PRIMARY KEY, isAnalyzed BOOLEAN);",
                "INSERT INTO Track (isAnalyzed) VALUES (1), (1), (1);");
    const AnalysisStateAudit audit = auditAnalysisState(root.string());
    assert(audit.error.empty());
    assert(audit.hasColumn);
    assert(audit.tracksChecked == 3);
    assert(audit.notAnalyzed == 0);
    // Nothing to say, and it must say nothing for the RIGHT reason: the
    // column was there and every track was analysed.
    assert(!audit.worthReporting());
    std::cout << "  a fully analysed library: 0 of 3, nothing to report\n";
}

void anOldSchemaIsNotZero(const fs::path &scratch)
{
    const fs::path root = scratch / "engine1";
    // Engine 1.x: a Track table with no isAnalyzed column at all.
    makeLibrary(root, "CREATE TABLE Track (id INTEGER PRIMARY KEY, title TEXT);",
                "INSERT INTO Track (title) VALUES ('a'), ('b');");
    const AnalysisStateAudit audit = auditAnalysisState(root.string());
    assert(audit.error.empty());
    // The distinction this case exists for: "too old to record the
    // state" is not "everything is analysed", and both would otherwise
    // report zero and say nothing.
    assert(!audit.hasColumn);
    assert(audit.notAnalyzed == 0);
    assert(!audit.worthReporting());
    // Present, just too old to say. The page's wording turns on this.
    assert(audit.libraryPresent);
    std::cout << "  an Engine 1.x library: present but no column, reported as unknown rather than as zero\n";
}

void aNullFlagCountsAsNotAnalysed(const fs::path &scratch)
{
    const fs::path root = scratch / "nulls";
    makeLibrary(root, "CREATE TABLE Track (id INTEGER PRIMARY KEY, isAnalyzed BOOLEAN);",
                "INSERT INTO Track (isAnalyzed) VALUES (1), (NULL), (0);");
    const AnalysisStateAudit audit = auditAnalysisState(root.string());
    assert(audit.tracksChecked == 3);
    // NULL is no recorded result, which is the same thing to the player
    // as a zero. Counting only `= 0` would silently under-report.
    assert(audit.notAnalyzed == 2);
    assert(audit.worthReporting());
    std::cout << "  a NULL flag counts as not analysed: 2 of 3\n";
}

void anAbsentLibraryIsNotAnError(const fs::path &scratch)
{
    const AnalysisStateAudit missing = auditAnalysisState((scratch / "not-there").string());
    assert(missing.error.empty() && !missing.worthReporting() && missing.tracksChecked == 0);
    const AnalysisStateAudit empty = auditAnalysisState("");
    assert(empty.error.empty() && !empty.worthReporting());
    // Distinct from an Engine 1.x library, which is also "nothing to
    // report" and is a completely different sentence on screen. Without
    // this the page told a stick with no Engine library that its Engine
    // library does not record analysis state -- a claim about something
    // that is not there. Caught by looking at a screenshot of the page,
    // not by any assertion, which is why this one exists now.
    assert(!missing.libraryPresent);
    assert(!empty.libraryPresent);
    std::cout << "  an absent library and an empty path both report nothing, without an error, and are "
                 "distinguishable from an old one\n";
}

void anUnreadableDatabaseIsAnError(const fs::path &scratch)
{
    const fs::path root = scratch / "corrupt";
    fs::create_directories(root / "Database2");
    // Present, the right name, and not a database.
    std::ofstream(root / "Database2" / "m.db", std::ios::binary) << std::string(4096, '\x7f');
    const AnalysisStateAudit audit = auditAnalysisState(root.string());
    // An error, NOT a quiet zero: a library this cannot read is a
    // question unanswered, and reporting "nothing to analyse" for it
    // would be a reassurance nobody earned.
    assert(!audit.error.empty());
    assert(!audit.worthReporting());
    std::cout << "  an unreadable database: reported as an error, not as zero\n";
}

}  // namespace

int main(int argc, char **argv)
{
    const fs::path fixture = argc > 1 ? fs::path(argv[1]) : fs::path("tests/fixtures/anonymized_library");
    const fs::path fixtureEngine = fixture / "engine";
    if (!fs::is_regular_file(fixtureEngine / "Database2" / "m.db")) {
        std::cerr << "fixture Engine library not found under " << fixtureEngine << "\n";
        return 1;
    }

    const fs::path scratch = seabass::testing::scratchRoot() / "engine-analysis-state";
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    theFixtureReportsItsRealDistribution(fixtureEngine);
    aFullyAnalysedLibraryReportsNothing(scratch);
    anOldSchemaIsNotZero(scratch);
    aNullFlagCountsAsNotAnalysed(scratch);
    anAbsentLibraryIsNotAnError(scratch);
    anUnreadableDatabaseIsAnError(scratch);

    fs::remove_all(scratch);
    std::cout << "engine_analysis_state_test: ok\n";
    return 0;
}
