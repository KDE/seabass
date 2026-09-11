// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>

#include <djinterop/djinterop.hpp>

#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::backup;
using namespace seabass::infrastructure::engine;
namespace fs = std::filesystem;

// Exercises the exact backup-then-write sequence
// src/gui/cleanup_controller.cpp's runApplyTask() uses for a real
// multi-group duplicate cleanup, directly against the real ports/
// adapters it composes (FilesystemBackupStore, LibdjinteropEngine
// CleanupWriter, a real Engine database) -- without depending on Qt,
// so this can run as a plain ctest. See this project's "Destructive-
// feature test & verification plan" (BRAINSTORM.md's own "Clean up...
// allow cancelling... incl rollback" note): the property this proves is
// that however far a cleanup batch gets before being interrupted (app
// crash, force-quit, closed mid-batch), restoring the *one* backup
// taken before the batch started always cleanly reverts every group
// processed so far -- not just the most recent one. Engine's cleanup
// writes all go through the same single m.db file, so (per
// makeContext()'s own comment in cleanup_controller.cpp) that one file
// is backed up exactly once, deduped, the first time any group needs
// it -- this test is what actually proves that single backup is enough
// to safely undo an arbitrarily-interrupted batch.
int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_duplicate_cleanup_interruption_test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path engineRoot = root / "Engine Library";
    fs::path backupDir = root / "Seabass" / "backups";

    int64_t survivor1Id, doomed1Id, survivor2Id, doomed2Id;
    {
        auto db = djinterop::engine::create_database(engineRoot.string());

        djinterop::track_snapshot snapshot;
        snapshot.title = "Group 1 Survivor";
        snapshot.relative_path = "g1_survivor.mp3";
        survivor1Id = db.create_track(snapshot).id();
        snapshot.title = "Group 1 Doomed";
        snapshot.relative_path = "g1_doomed.mp3";
        doomed1Id = db.create_track(snapshot).id();

        snapshot.title = "Group 2 Survivor";
        snapshot.relative_path = "g2_survivor.mp3";
        survivor2Id = db.create_track(snapshot).id();
        snapshot.title = "Group 2 Doomed";
        snapshot.relative_path = "g2_doomed.mp3";
        doomed2Id = db.create_track(snapshot).id();

        auto pl = db.create_root_playlist("My Set");
        pl.add_track_back(*db.track_by_id(survivor1Id));
        pl.add_track_back(*db.track_by_id(doomed1Id));
        pl.add_track_back(*db.track_by_id(survivor2Id));
        pl.add_track_back(*db.track_by_id(doomed2Id));
    }

    std::string engineDbFile = (engineRoot / "Database2" / "m.db").string();
    FilesystemBackupStore backupStore(backupDir.string());

    // Mirrors runApplyTask()'s backupIfNeeded(): back up the (single,
    // shared) database file exactly once, before any group's write.
    auto backupRecord = backupStore.backup({engineDbFile}, "duplicate-file-cleanup");
    assert(fs::exists(engineDbFile));

    // Process group 1 only -- group 2 is never touched, simulating the
    // app being killed/closed right after group 1 completes.
    {
        LibdjinteropEngineCleanupWriter writer(engineRoot.string());
        writer.removeTrackReplacingWith(std::to_string(doomed1Id), std::to_string(survivor1Id));
    }

    // Confirm the "interrupted" state: group 1 processed, group 2
    // completely untouched.
    {
        auto db = djinterop::engine::load_database(engineRoot.string());
        assert(!db.track_by_id(doomed1Id).has_value());
        assert(db.track_by_id(survivor1Id).has_value());
        assert(db.track_by_id(survivor2Id).has_value());
        assert(db.track_by_id(doomed2Id).has_value());  // group 2's doomed track: still there, never processed
        std::cout << "case 1 (interrupted after group 1: group 1 done, group 2 untouched) OK\n";
    }

    // The actual rollback: restore the one upfront backup.
    bool restored = backupStore.restore(backupRecord.id);
    assert(restored);

    // Everything is back exactly as it was before the batch started --
    // group 1's removal is undone too, not just group 2 left alone.
    {
        auto db = djinterop::engine::load_database(engineRoot.string());
        assert(db.track_by_id(doomed1Id).has_value());
        assert(db.track_by_id(doomed1Id)->title() == std::optional<std::string>("Group 1 Doomed"));
        assert(db.track_by_id(survivor1Id).has_value());
        assert(db.track_by_id(survivor2Id).has_value());
        assert(db.track_by_id(doomed2Id).has_value());

        auto playlists = db.root_playlists();
        assert(playlists.size() == 1);
        assert(playlists[0].tracks().size() == 4);  // membership fully restored too
        std::cout << "case 2 (restoring the one upfront backup fully reverts group 1 as well) OK\n";
    }

    // BackupStore::restore()'s own contract: the pre-restore state is
    // itself backed up first, so the restore can be undone too -- a
    // real safety net, not just documented intent.
    {
        auto records = backupStore.list();
        bool hasPreRestore = false;
        for (const auto &r : records) {
            if (r.label == "pre-restore") {
                hasPreRestore = true;
            }
        }
        assert(hasPreRestore);
        std::cout << "case 3 (restore() itself backs up the pre-restore state first) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
