// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Every task that writes a stick or makes a backup keeps the system awake.
// The controllers live in the GUI executable, with no library a test could
// link, so this reads their sources: each place such a task is started
// must take a SleepInhibitor hold just before it. What it catches is one of
// the tasks below losing its hold, which nothing at runtime would reveal
// until a laptop suspended in the middle of a restore. A new write path is
// covered only once it is added to the list.

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{

struct Site
{
    const char *file;
    // Text that appears where the task is started, once per such place.
    const char *marker;
    const char *what;
};

const Site kSites[] = {
    {"src/gui/edit/save_loop.cpp", "SaveLoopResult result;", "every staged save"},
    {"src/gui/stick_backup_controller.cpp", "result->activity = QStringLiteral(\"backup\")", "a stick backup"},
    {"src/gui/stick_backup_controller.cpp", "result->activity = QStringLiteral(\"compact\")", "compacting a backup"},
    {"src/gui/stick_backup_controller.cpp", "result->activity = QStringLiteral(\"keep\")", "keeping a partial backup"},
    {"src/gui/stick_backup_controller.cpp", "result->activity = QStringLiteral(\"discard\")", "discarding a partial backup"},
    {"src/gui/restore_stick_backup_controller.cpp", "m_restoreWatcher.setFuture", "a restore"},
    {"src/gui/clone_stick_controller.cpp", "m_runWatcher.setFuture", "a stick copy"},
    {"src/gui/format_usb_controller.cpp", "m_watcher.setFuture", "a format"},
    {"src/gui/backups_controller.cpp", "m_watcher.setFuture", "making or removing backups"},
    {"src/gui/local_cue_controller.cpp", "m_backupWatcher.setFuture", "a Local Cue Backup"},
    {"src/gui/metadata_backup_controller.cpp", "m_saveWatcher.setFuture", "a metadata backup"},
    {"src/gui/cleanup_controller.cpp", "m_pendingWriteWatcher.setFuture", "deleting files from a stick"},
    {"src/gui/engine_library_creator_controller.cpp", "m_watcher.setFuture", "creating an Engine library"},
    {"src/gui/stick_performance_controller.cpp", "m_writeWatcher.setFuture", "the write speed test"},
};

std::string read(const fs::path &path)
{
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

int main()
{
    bool ok = true;
    int checked = 0;
    for (const Site &site : kSites) {
        const fs::path path = fs::path(SEABASS_SOURCE_DIR) / site.file;
        const std::string text = read(path);
        assert(!text.empty() && "a listed source file is missing -- the list is out of date");
        const std::string marker = site.marker;
        std::size_t found = 0;
        for (std::size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1)) {
            ++found;
            // The hold is taken just before the task starts (or, in the save
            // loop, right as it begins).
            const std::size_t from = at > 700 ? at - 700 : 0;
            const std::string around = text.substr(from, (at - from) + 300);
            if (around.find("SleepInhibitor::hold") == std::string::npos) {
                std::cerr << site.file << ": " << site.what
                          << " starts without a SleepInhibitor hold -- the system could suspend mid-write\n";
                ok = false;
            }
            ++checked;
        }
        if (found == 0) {
            std::cerr << site.file << ": no \"" << marker << "\" found for " << site.what
                      << " -- the test's list is out of date\n";
            ok = false;
        }
    }
    if (!ok) {
        return 1;
    }
    std::cout << "sleep_inhibit_wiring_test: " << checked << " write and backup tasks keep the system awake\n";
    return 0;
}
