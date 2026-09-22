// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Undo Last Save is all or nothing. A save makes one backup record per kind
// of change; if any record of that set is gone (seabass-cli --clean, or the
// space release), the undo must restore nothing and say which are missing,
// rather than restore the rest and report success.

#include <QString>
#include <QStringList>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/changes/restore_backups_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
using seabass::application::CancellationToken;
using seabass::infrastructure::backup::FilesystemBackupStore;
namespace fs = std::filesystem;

namespace
{

std::string read(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void write(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

// Emptied when a case starts, removed when its block ends -- after the
// SaveContext, which keeps Seabass/seabass.log open (Windows will not
// remove an open file). See failed_change_rollback_test.
struct ScratchStick
{
    fs::path path;
    explicit ScratchStick(fs::path p) : path(std::move(p)) { fs::remove_all(path); }
    ~ScratchStick()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec) {
            std::cerr << "warning: could not remove " << path.string() << ": " << ec.message() << "\n";
        }
    }
    ScratchStick(const ScratchStick &) = delete;
    ScratchStick &operator=(const ScratchStick &) = delete;
};

// One save's worth of backups on a stick: export.pdb under one label and an
// analysis file under another, as a Library Health save that repairs one
// issue and deletes an orphan makes. Then both files change, as the save did.
struct SavedStick
{
    fs::path pioneer;
    fs::path pdb;
    fs::path anlz;
    std::vector<UndoableBackup> backups;
    std::string pdbRecord;
    std::string anlzRecord;

    explicit SavedStick(const fs::path &stick)
        : pioneer(stick / "PIONEER"),
          pdb(pioneer / "rekordbox" / "export.pdb"),
          anlz(pioneer / "USBANLZ" / "P001" / "ANLZ0000.EXT")
    {
        write(pdb, "pdb-before");
        write(anlz, "anlz-before");
        const fs::path backupDir = stick / "Seabass" / "backups";
        FilesystemBackupStore store(backupDir.string());
        pdbRecord = store.backup({pdb.string()}, "consistency-repair").id;
        anlzRecord = store.backup({anlz.string()}, "consistency-delete-orphan").id;
        backups = {{QString::fromStdString(backupDir.string()), QString::fromStdString(pdbRecord)},
                   {QString::fromStdString(backupDir.string()), QString::fromStdString(anlzRecord)}};
        write(pdb, "pdb-after");
        write(anlz, "anlz-after");
    }
};

SaveLoopResult runUndo(const SavedStick &saved)
{
    CancellationToken token;
    auto &noProgress = application::NullProgressReporter::instance();
    SaveContext ctx(token, noProgress, {}, QString::fromStdString(saved.pioneer.string()), {});
    std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<RestoreBackupsChange>(saved.backups)};
    return runSaveLoop(changes, ctx);
}

}  // namespace

int main()
{
    // 1. Every record there: both files come back.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_restore_backups_whole");
        const SavedStick saved(scratch.path);
        const SaveLoopResult result = runUndo(saved);
        assert(result.error.isEmpty());
        assert(result.appliedIds == QStringList{"undo:last-save"});
        assert(read(saved.pdb) == "pdb-before");
        assert(read(saved.anlz) == "anlz-before");
        std::cout << "case 1 (an undo with every backup there restores all of it) OK\n";
    }

    // 2. One record gone: nothing restored, not even the file whose backup
    //    is still there, and the error names the one that is missing.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_restore_backups_missing");
        const SavedStick saved(scratch.path);
        FilesystemBackupStore store((scratch.path / "Seabass" / "backups").string());
        assert(store.remove(saved.pdbRecord));

        const SaveLoopResult result = runUndo(saved);
        assert(result.appliedIds.isEmpty());
        assert(result.failedId == "undo:last-save");
        assert(result.error.contains("no longer on the stick"));
        assert(result.error.contains(QString::fromStdString(saved.pdbRecord)));
        assert(!result.error.contains(QString::fromStdString(saved.anlzRecord)));
        assert(read(saved.pdb) == "pdb-after");
        assert(read(saved.anlz) == "anlz-after");  // its backup is there, and it still stays as it is
        std::cout << "case 2 (a missing backup stops the whole undo and is named) OK\n";
    }

    // 3. The files a Clean Up save put in Delete Orphaned Files come off the
    //    list with the undo that brings their rows back; an entry another
    //    save made stays.
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_restore_backups_orphans");
        const SavedStick saved(scratch.path);
        using seabass::infrastructure::cleanup::PendingDeletion;
        using seabass::infrastructure::cleanup::PendingDeletionManifest;
        PendingDeletionManifest manifest(seabass::infrastructure::paths::stickPendingDeletions(scratch.path).string());
        PendingDeletion ofThisSave;
        ofThisSave.format = "rekordbox";
        ofThisSave.filePath = (scratch.path / "Contents" / "copy.mp3").string();
        ofThisSave.backupId = saved.pdbRecord;
        manifest.append(ofThisSave);
        PendingDeletion ofAnotherSave = ofThisSave;
        ofAnotherSave.filePath = (scratch.path / "Contents" / "older.mp3").string();
        ofAnotherSave.backupId = "20260101T000000-duplicate-file-cleanup";
        manifest.append(ofAnotherSave);

        const SaveLoopResult result = runUndo(saved);
        assert(result.error.isEmpty());
        const auto left = manifest.list();
        assert(left.size() == 1);
        assert(left.front().filePath == ofAnotherSave.filePath);
        std::cout << "case 3 (undo takes its save's files back off Delete Orphaned Files) OK\n";
    }

    // 4. The list cannot be rewritten: the undo still stands, and says so.
    //    Refusing here would have the save loop roll the restore back out
    //    -- the library taken away again over a bookkeeping file, on the
    //    full or failing stick where getting it back mattered most.
#if !defined(_WIN32)
    {
        const ScratchStick scratch(seabass::testing::scratchRoot() / "seabass_restore_backups_locked_list");
        const SavedStick saved(scratch.path);
        using seabass::infrastructure::cleanup::PendingDeletion;
        using seabass::infrastructure::cleanup::PendingDeletionManifest;
        const fs::path manifestPath = seabass::infrastructure::paths::stickPendingDeletions(scratch.path);
        PendingDeletionManifest manifest(manifestPath.string());
        PendingDeletion ofThisSave;
        ofThisSave.format = "rekordbox";
        ofThisSave.filePath = (scratch.path / "Contents" / "copy.mp3").string();
        ofThisSave.backupId = saved.pdbRecord;
        manifest.append(ofThisSave);

        // The file and the folder, so neither a truncating write nor a
        // temp-and-rename can get through. See the manifest's own test.
        fs::permissions(manifestPath, fs::perms::owner_read, fs::perm_options::replace);
        fs::permissions(manifestPath.parent_path(), fs::perms::owner_read | fs::perms::owner_exec,
                        fs::perm_options::replace);
        const SaveLoopResult result = runUndo(saved);
        fs::permissions(manifestPath.parent_path(), fs::perms::owner_all, fs::perm_options::replace);
        fs::permissions(manifestPath, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);

        if (::geteuid() != 0) {
            assert(result.error.isEmpty() && "a list that could not be rewritten must not undo the undo");
            assert(result.appliedIds == QStringList{"undo:last-save"});
            assert(read(saved.pdb) == "pdb-before" && "the library is back");
            assert(read(saved.anlz) == "anlz-before");
            assert(!result.warning.isEmpty() && "and the person is told what did not happen");
            assert(result.warning.contains(QStringLiteral("waiting to be deleted")));
            assert(manifest.list().size() == 1 && "the entry is still there, to be dropped by a later pass");
        }
        std::cout << "case 4 (a list that cannot be rewritten warns, and keeps the restore) OK\n";
    }
#endif

    std::cout << "restore_backups_change_test: all cases passed\n";
    return 0;
}
