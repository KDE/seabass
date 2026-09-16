// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: every save left an automatic backup, and each one can be
// restored -- rig check W9.
//
//   rig_save_backups <stick root> [--expect-at-least N]
//
// Lists the stick's automatic backup records the way the app does, and
// asks the store whether each is restorable: a record whose archive is
// missing, truncated or of an unrecognised manifest version is not undo
// the DJ can take, however tidy it looks in a listing.
//
// Reads only. The rig runs it after the edit checks, which make one
// record per save; --expect-at-least says how many that was.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "application/ports/backup_store.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

namespace fs = std::filesystem;
using namespace seabass;

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4) {
        std::cerr << "usage: rig_save_backups <stick root> [--expect-at-least N]\n";
        return 2;
    }
    const fs::path stickRoot = argv[1];
    int expectAtLeast = 0;
    if (argc == 4) {
        if (std::string(argv[2]) != "--expect-at-least") {
            std::cerr << "usage: rig_save_backups <stick root> [--expect-at-least N]\n";
            return 2;
        }
        expectAtLeast = std::atoi(argv[3]);
    }
    bool pass = true;

    try {
        const fs::path directory = infrastructure::paths::stickBackupsDir(stickRoot);
        std::cout << "automatic backups in " << directory.string() << "\n";
        infrastructure::backup::FilesystemBackupStore store(directory.string());
        const std::vector<application::BackupRecord> records = store.list();

        std::size_t restorable = 0;
        std::uint64_t bytes = 0;
        for (const application::BackupRecord &record : records) {
            const bool ok = store.isRestorable(record.id);
            restorable += ok ? 1 : 0;
            bytes += record.sizeBytes;
            std::cout << "  " << (ok ? "restorable " : "NOT RESTORABLE ") << record.id << "  " << record.label << ", "
                      << record.filePaths.size() << " file(s), " << record.sizeBytes << " bytes\n";
        }
        std::cout << records.size() << " record(s), " << restorable << " restorable, " << bytes << " bytes\n";

        pass = pass && !records.empty() && restorable == records.size();
        if (expectAtLeast > 0) {
            const bool enough = static_cast<int>(records.size()) >= expectAtLeast;
            std::cout << "expected at least " << expectAtLeast << " record(s) from this round's saves -> "
                      << (enough ? "yes" : "NO") << "\n";
            pass = pass && enough;
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
