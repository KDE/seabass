// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The reads a save makes of the library it writes see what it has written
// (#110). Reads of a stick that only read go through a shared read copy of
// a WAL-mode m.db (infrastructure::StickReadSnapshots); a save's own
// lookups must not, or a change counts the playlist it has just made as
// not there, and a later change in the same save misses an earlier one's
// rows.
//
// On a copy of the anonymized fixture with m.db in WAL mode, a read copy
// made first and the app's "Seabass is writing" state on, as during any
// save: one save creates a playlist (its own check counts it, and must
// find 1) and then asks to create it again (which must find the first
// one's row and skip). Once with a small save, which writes m.db in
// place, and once with a large one, which writes a scratch copy.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "gui/edit/changes/engine_playlist_changes.hpp"
#include "infrastructure/database_read_copy.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_save_reads_live_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "engine_save_reads_live_home");
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "engine_save_reads_live_stick");
    testing::levelImportCounter(stick);
    const std::string library = stick.engineUtf8();
    assert(testing::sqlExec(stick.db, "PRAGMA journal_mode=WAL;"));
    assert(infrastructure::isWalModeSqlite(pathFromUtf8(stick.db)));

    int failures = 0;
    for (const auto &[hint, path] : {std::pair{1, std::string("In Place/List")}, std::pair{1000, std::string("Scratch/List")}}) {
        // A read copy as Browse leaves one, then a save as the app runs it.
        (void)infrastructure::engine::listEnginePlaylists(library);
        infrastructure::StickReadSnapshots::instance().setWriting(true);
        auto create = std::make_shared<CreateEnginePlaylistChange>(stick.enginePath(), path, hint);
        auto again = std::make_shared<CreateEnginePlaylistChange>(stick.enginePath(), path, hint);
        const SaveLoopResult saved = testing::saveChanges({create, again}, stick.pioneerPath(), stick.enginePath());
        infrastructure::StickReadSnapshots::instance().setWriting(false);
        const std::string what = hint == 1 ? "in place" : "through a scratch copy";
        if (!saved.error.isEmpty()) {
            std::cerr << "FAIL (" << what << "): the save failed: " << saved.error.toStdString() << "\n";
            ++failures;
            continue;
        }
        if (!saved.appliedIds.contains(create->id()) || saved.skippedIds != QStringList{again->id()}) {
            std::cerr << "FAIL (" << what << "): the second create did not see the first one's playlist ("
                      << saved.appliedIds.size() << " applied, " << saved.skippedIds.size() << " skipped)\n";
            ++failures;
            continue;
        }
        if (infrastructure::engine::enginePlaylistCountAtPath(library, path) != 1) {
            std::cerr << "FAIL (" << what << "): the stick does not have the playlist once\n";
            ++failures;
            continue;
        }
        std::cout << "a save " << what << " reads what it wrote: OK\n";
    }
    return failures == 0 ? 0 : 1;
}
