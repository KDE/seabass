// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A file a command-line run did not foresee, covered before it changes
// (infrastructure/backup/cover_file_late.hpp), as copy-beatgrids does it:
//
// 1. One that is there goes into the run's backup record.
// 2. One that is not is noted absent in it, so restoring the record takes
//    away what the run made.
// 3. One whether there or not cannot be told is a refusal: it throws, and
//    the record is as it was.
// 4. One that is not there with no record yet is a refusal too.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "infrastructure/backup/cover_file_late.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using infrastructure::backup::coverFileLate;
using infrastructure::backup::FilesystemBackupStore;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

application::BackupRecord recordOf(FilesystemBackupStore &store, const std::string &id)
{
    for (const auto &record : store.list()) {
        if (record.id == id) {
            return record;
        }
    }
    return {};
}

bool contains(const std::vector<std::string> &list, const std::string &file)
{
    return std::find(list.begin(), list.end(), file) != list.end();
}

}  // namespace

int main()
{
    const fs::path root = testing::scratchRoot() / "cover_file_late_test";
    fs::remove_all(root);
    fs::create_directories(root / "stick");
    FilesystemBackupStore store(pathToUtf8(root / "backups"));
    const std::string first = pathToUtf8(root / "stick" / "m.db");
    std::ofstream(pathFromUtf8(first)) << "db";
    const std::string recordId = store.backup({first}, "copy-beatgrids").id;

    // 1. There: into the record.
    const std::string there = pathToUtf8(root / "stick" / "there.rgb");
    std::ofstream(pathFromUtf8(there)) << "player's";
    const auto copied = coverFileLate(store, recordId, there, "copy-beatgrids");
    check(copied && copied->id == recordId, "a file that is there goes into the run's record");
    check(contains(recordOf(store, recordId).filePaths, there), "and the record holds it");

    // 2. Not there: noted absent; restoring takes it away again.
    const std::string fresh = pathToUtf8(root / "stick" / "new.rgb");
    check(!coverFileLate(store, recordId, fresh, "copy-beatgrids"), "a new file copies nothing");
    check(contains(recordOf(store, recordId).absentPaths, fresh), "it is noted absent in the record");
    std::ofstream(pathFromUtf8(fresh)) << "made by the run";
    std::ofstream(pathFromUtf8(there), std::ios::trunc) << "Seabass's";
    check(store.restore(recordId), "the record restores");
    check(!fs::exists(pathFromUtf8(fresh)), "the file the run made is gone again");
    {
        std::ifstream in(pathFromUtf8(there));
        std::string text;
        std::getline(in, text);
        check(text == "player's", "the file that was there is as it was");
    }

    // 3. Cannot be looked at: refused, the record as it was.
    const fs::path locked = root / "stick" / "locked";
    fs::create_directories(locked);
    const std::string hidden = pathToUtf8(locked / "inside.rgb");
    fs::permissions(locked, fs::perms::none, fs::perm_options::replace);
    std::error_code ec;
    (void)fs::exists(pathFromUtf8(hidden), ec);
    if (!ec) {
        // Root looks through any permission: nothing to test here.
        std::cout << "cover_file_late_test: exists() does not fail here (running as root?), case 3 not run\n";
        ++failures;
    } else {
        const auto before = recordOf(store, recordId);
        bool threw = false;
        try {
            coverFileLate(store, recordId, hidden, "copy-beatgrids");
        } catch (const std::runtime_error &e) {
            threw = std::string(e.what()).find("cannot be told") != std::string::npos;
        }
        check(threw, "a file that cannot be looked at is refused, saying why");
        const auto after = recordOf(store, recordId);
        check(after.filePaths == before.filePaths && after.absentPaths == before.absentPaths,
              "and the record is as it was");
    }
    fs::permissions(locked, fs::perms::owner_all, fs::perm_options::replace);

    // 4. Not there, no record: refused.
    bool threw = false;
    try {
        coverFileLate(store, std::string(), pathToUtf8(root / "stick" / "other.rgb"), "copy-beatgrids");
    } catch (const std::runtime_error &) {
        threw = true;
    }
    check(threw, "a new file with no record to note it in is refused");

    fs::remove_all(root);
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "cover_file_late_test: all checks passed\n";
    return 0;
}
