// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: what an interrupted save left on a stick -- rig check F6
// (tools/rig-interrupted-save.sh, issue #48).
//
//   rig_journal_state <stick root>
//
// For every catalog database on the stick (PIONEER/rekordbox/*.db and
// Engine Library/Database2/*.db, the folders the rig's catalogs() lists),
// whether a rollback journal lies beside it and whether its header is live
// -- the eight bytes infrastructure/sqlite_pending_journal.hpp checks, the
// same function the readers ask before they roll one back -- and whether a
// write-ahead log or its index is there. Then the save's note,
// Seabass/backups/.save-in-progress, and the records it names, each asked
// of the store whether it can be restored.
//
// Reads bytes only. Nothing here opens a database through SQLite: an open
// that may write would roll a hot journal back itself, and the listing
// would then describe a stick this tool had already changed.
//
// The last lines are "hot-journals=N wals=N note-records=N" and
// "RIG RESULT: PASS" (the listing was produced) or "RIG RESULT: FAIL" (the
// stick could not be read), and the exit code matches.

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/interrupted_save.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

// "absent", or its size in bytes.
std::string sidecar(const fs::path &database, const char *suffix)
{
    fs::path side = database;
    side += suffix;
    std::error_code ec;
    if (!fs::is_regular_file(side, ec) || ec) {
        return "absent";
    }
    const std::uintmax_t bytes = fs::file_size(side, ec);
    return ec ? std::string("present (size unreadable)") : std::to_string(bytes) + " bytes";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: rig_journal_state <stick root>\n";
        return 2;
    }
    const fs::path stickRoot = pathFromUtf8(argv[1]);
    int hotJournals = 0;
    int wals = 0;
    int databases = 0;
    bool readable = true;

    for (const char *folder : {"PIONEER/rekordbox", "Engine Library/Database2"}) {
        const fs::path directory = stickRoot / pathFromUtf8(folder);
        std::error_code ec;
        if (!fs::is_directory(directory, ec)) {
            // A stick without one of the two libraries is possible; one
            // whose folder cannot even be asked about is not readable.
            std::cout << folder << ": " << (ec ? "cannot be read: " + ec.message() : std::string("not on this stick"))
                      << "\n";
            readable = readable && !ec;
            continue;
        }
        std::vector<fs::path> found;
        for (const auto &entry : fs::directory_iterator(directory, ec)) {
            std::error_code typeEc;
            if (entry.is_regular_file(typeEc) && entry.path().extension() == ".db") {
                found.push_back(entry.path());
            }
        }
        if (ec) {
            std::cout << folder << ": cannot be listed: " << ec.message() << "\n";
            readable = false;
            continue;
        }
        std::sort(found.begin(), found.end());
        for (const fs::path &database : found) {
            ++databases;
            const std::string journal = sidecar(database, "-journal");
            const bool hot = infrastructure::hasPendingJournal(database);
            const std::string wal = sidecar(database, "-wal");
            hotJournals += hot ? 1 : 0;
            wals += wal != "absent" ? 1 : 0;
            std::cout << folder << "/" << pathToUtf8(database.filename()) << ": journal " << journal
                      << (hot ? ", HOT (live header)" : (journal == "absent" ? "" : ", not live")) << "; wal " << wal
                      << "; shm " << sidecar(database, "-shm") << "\n";
        }
    }
    if (databases == 0) {
        std::cout << "no catalog database found on " << pathToUtf8(stickRoot) << "\n";
        readable = false;
    }

    int noteRecords = 0;
    const std::string backupDir = pathToUtf8(infrastructure::paths::stickBackupsDir(stickRoot));
    std::error_code ec;
    if (!fs::exists(infrastructure::backup::interruptedSaveNote(backupDir), ec)) {
        std::cout << "note: no Seabass/backups/.save-in-progress\n";
    } else {
        const std::vector<std::string> ids = infrastructure::backup::interruptedSaveRecords(backupDir);
        noteRecords = static_cast<int>(ids.size());
        std::cout << "note: Seabass/backups/.save-in-progress names " << ids.size() << " record(s)\n";
        try {
            infrastructure::backup::FilesystemBackupStore store(backupDir);
            for (const std::string &id : ids) {
                std::cout << "  " << id << (store.isRestorable(id) ? " (restorable)" : " (NOT restorable)") << "\n";
            }
        } catch (const std::exception &e) {
            std::cout << "  the records cannot be read: " << e.what() << "\n";
            readable = false;
        }
    }

    std::cout << "hot-journals=" << hotJournals << " wals=" << wals << " note-records=" << noteRecords << "\n";
    std::cout << "RIG RESULT: " << (readable ? "PASS" : "FAIL") << "\n";
    return readable ? 0 : 1;
}
