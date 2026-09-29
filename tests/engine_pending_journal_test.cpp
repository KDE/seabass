// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The Engine reader meets a journal a pulled stick left, and keeps a copy
// before anything rolls it back (#48).
//
// Found by rig check F6 on 2026-09-28: a Sync save killed after its first
// track left Engine's m.db with a hot journal, the next session read the
// stick and undid the save as it should -- but no copy of the database and
// its journal was ever kept. LibdjinteropEngineReader::readAll() asked
// djinterop::engine::database_exists() first, which opens m.db read-write,
// and SQLite rolled the journal back right there, silently, before
// recoverEnginePendingJournals() had a chance to copy it. The rollback is
// the one step that changes the stick, and the rule is that it is not
// taken without a way back.
//
// Run over a copy of the fixture's Engine library, left the way a crash
// mid-transaction leaves it: changed pages already in m.db, the originals
// in a journal with a live header.

#include <sqlite3.h>

#include <cassert>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "application/ports/progress_reporter.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;

namespace
{

struct QuietReporter : seabass::application::ProgressReporter
{
    std::vector<std::string> warnings;
    void start(const std::string &, size_t) override {}
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &message) override { warnings.push_back(message); }
};

void exec(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::cerr << "sqlite: " << (error ? error : "?") << " in: " << sql << "\n";
        sqlite3_free(error);
        assert(false);
    }
}

std::vector<seabass::domain::Track> readAll(const fs::path &library)
{
    QuietReporter reporter;
    seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(library));
    reader.setProgressReporter(reporter);
    return reader.readAll();
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

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_engine_pending_journal";
    seabass::testing::sandboxSeabassHome(scratch / "home");
    const fs::path source = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "engine";
    std::error_code ec;
    fs::remove_all(scratch / "live", ec);
    fs::remove_all(scratch / "crashed", ec);
    fs::create_directories(scratch / "live");
    fs::create_directories(scratch / "crashed");
    const fs::path live = scratch / "live" / "Engine Library";
    const fs::path crashed = scratch / "crashed" / "Engine Library";
    fs::copy(source, live, fs::copy_options::recursive);
    fs::copy(source, crashed, fs::copy_options::recursive);

    const std::size_t before = readAll(live).size();
    assert(before > 0);

    // A transaction that renames every track, with its changed pages
    // spilled into m.db before it commits -- cache_spill, not cache_size,
    // is what forces that -- copied with its journal and then rolled back
    // in the live copy. See sqlite_pending_journal_test for the recipe.
    {
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(live / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        exec(db, "PRAGMA journal_mode=DELETE");
        exec(db, "PRAGMA cache_size=1");
        exec(db, "PRAGMA cache_spill=1");
        exec(db, "BEGIN");
        exec(db, "UPDATE Track SET title = 'INTERRUPTED', path = hex(randomblob(400))");
        fs::copy_file(live / "Database2" / "m.db", crashed / "Database2" / "m.db",
                      fs::copy_options::overwrite_existing);
        fs::copy_file(live / "Database2" / "m.db-journal", crashed / "Database2" / "m.db-journal",
                      fs::copy_options::overwrite_existing);
        exec(db, "ROLLBACK");
        sqlite3_close(db);
    }
    const fs::path crashedDb = crashed / "Database2" / "m.db";
    assert(seabass::infrastructure::hasPendingJournal(crashedDb) && "the copied journal is live");

    const fs::path recovered = seabass::infrastructure::paths::localRoot() / "recovered";
    if (fs::exists(recovered) && !fs::is_directory(recovered)) {
        fs::remove(recovered);  // case 4's stand-in, left by a run that stopped there
    }
    const std::set<std::string> keptBefore = entriesOf(recovered);

    // 1. Read as it was before the transaction.
    const auto tracks = readAll(crashed);
    assert(tracks.size() == before && "every track is read");
    for (const auto &track : tracks) {
        assert(track.title != "INTERRUPTED" && "the unfinished transaction was rolled back");
    }
    assert(!seabass::infrastructure::hasPendingJournal(crashedDb));
    std::cout << "case 1 (read as before the unfinished save: " << tracks.size() << " tracks) OK\n";

    // 2. And a copy of how it was found, kept first: the database and its
    //    live journal, in a folder of their own under recovered/.
    std::vector<std::string> kept;
    for (const std::string &name : entriesOf(recovered)) {
        if (keptBefore.count(name) == 0) {
            kept.push_back(name);
        }
    }
    if (kept.size() != 1) {
        std::cerr << kept.size() << " new folder(s) in " << seabass::pathToUtf8(recovered)
                  << ", expected exactly one: the roll back kept no copy first\n";
    }
    assert(kept.size() == 1 && "one copy kept before the roll back");
    const fs::path copy = recovered / seabass::pathFromUtf8(kept.front());
    assert(fs::exists(copy / "m.db") && fs::exists(copy / "m.db-journal"));
    assert(seabass::infrastructure::hasPendingJournal(copy / "m.db") && "the copy is the state as it was found");
    std::cout << "case 2 (a copy kept first: " << seabass::pathToUtf8(copy) << ") OK\n";

    // 3. The artwork stage opens m.db on its own, in a later pass, and a
    //    stick pulled mid-save can have left a journal by then: it is
    //    recovered the same way, a copy kept first, before that read.
    {
        const fs::path again = scratch / "crashed-again" / "Engine Library";
        fs::remove_all(again.parent_path(), ec);
        fs::create_directories(again.parent_path());
        fs::copy(source, again, fs::copy_options::recursive);
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(live / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        exec(db, "PRAGMA journal_mode=DELETE");
        exec(db, "PRAGMA cache_size=1");
        exec(db, "PRAGMA cache_spill=1");
        exec(db, "BEGIN");
        exec(db, "UPDATE Track SET title = 'INTERRUPTED', path = hex(randomblob(400))");
        fs::copy_file(live / "Database2" / "m.db", again / "Database2" / "m.db", fs::copy_options::overwrite_existing);
        fs::copy_file(live / "Database2" / "m.db-journal", again / "Database2" / "m.db-journal",
                      fs::copy_options::overwrite_existing);
        exec(db, "ROLLBACK");
        sqlite3_close(db);
        const fs::path againDb = again / "Database2" / "m.db";
        assert(seabass::infrastructure::hasPendingJournal(againDb));
        // Folders are named to the second, so case 1's could be reused.
        fs::remove_all(recovered, ec);

        QuietReporter reporter;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(again));
        reader.setProgressReporter(reporter);
        std::vector<seabass::domain::Track> none;
        reader.fillArtwork(none);
        assert(!seabass::infrastructure::hasPendingJournal(againDb) && "recovered before the artwork read");
        assert(entriesOf(recovered).size() == 1 && "and a copy kept first");
        std::cout << "case 3 (the artwork stage recovers a pending journal before it reads) OK\n";
    }

    // 4. A journal that cannot be put back fails the artwork stage alone:
    //    it warns, and the rest of the catalog's last stage still runs.
    {
        const fs::path stuck = scratch / "stuck" / "Engine Library";
        fs::remove_all(stuck.parent_path(), ec);
        fs::create_directories(stuck.parent_path());
        fs::copy(source, stuck, fs::copy_options::recursive);
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(live / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        exec(db, "PRAGMA journal_mode=DELETE");
        exec(db, "PRAGMA cache_size=1");
        exec(db, "PRAGMA cache_spill=1");
        exec(db, "BEGIN");
        exec(db, "UPDATE Track SET title = 'INTERRUPTED', path = hex(randomblob(400))");
        fs::copy_file(live / "Database2" / "m.db", stuck / "Database2" / "m.db", fs::copy_options::overwrite_existing);
        fs::copy_file(live / "Database2" / "m.db-journal", stuck / "Database2" / "m.db-journal",
                      fs::copy_options::overwrite_existing);
        exec(db, "ROLLBACK");
        sqlite3_close(db);
        // No copy can be kept, so nothing is rolled back.
        fs::remove_all(recovered, ec);
        std::ofstream(recovered) << "a file where the folder should be";
        QuietReporter reporter;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(stuck));
        reader.setProgressReporter(reporter);
        std::vector<seabass::domain::Track> none;
        reader.fillArtwork(none);
        assert(reporter.warnings.size() == 1);
        assert(seabass::infrastructure::hasPendingJournal(stuck / "Database2" / "m.db") && "left as it was");
        fs::remove(recovered, ec);
        std::cout << "case 4 (a journal that cannot be put back fails the artwork stage alone) OK\n";
    }

    fs::remove_all(scratch, ec);
    std::cout << "engine_pending_journal_test: all passed\n";
    return 0;
}
