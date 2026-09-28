// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// SaveContext's backup records, on a scratch stick: one archive per label
// per save, reused by every later file under that label whichever path
// adds it (backupOnce or backupAllNow); the records a failed backupAllNow
// had made taken away again while an earlier one is kept; the clean-up
// after a save that changed nothing, including when a record will not
// go; the space release that spares this save's own records; and the
// finish hooks' three ways to fail.
//
// Every one of these is a path a real save takes; none of them needs a
// catalog, so the stick here is a folder with a few files in it.

#include <QString>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/interrupted_save.hpp"
#include "infrastructure/backup/stick_space.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using seabass::application::CancellationToken;
using seabass::application::NullProgressReporter;
using seabass::gui::BackupTarget;
using seabass::gui::SaveContext;
using seabass::infrastructure::backup::FilesystemBackupStore;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &path, const std::string &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    assert(out.good());
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A stick with a PIONEER folder holding what a save backs up: two
// catalog-like files and a database with a write-ahead log beside it.
fs::path freshStick(const fs::path &scratch, const std::string &name)
{
    const fs::path stick = scratch / seabass::pathFromUtf8(name);
    fs::remove_all(stick);
    writeFile(stick / "PIONEER" / "rekordbox" / "export.pdb", "export.pdb bytes");
    writeFile(stick / "PIONEER" / "rekordbox" / "exportExt.pdb", "exportExt.pdb bytes");
    writeFile(stick / "PIONEER" / "rekordbox" / "exportLibrary.db", "database main file");
    writeFile(stick / "PIONEER" / "rekordbox" / "exportLibrary.db-wal", "committed pages not yet folded");
    return stick;
}

fs::path backupsOf(const fs::path &stick)
{
    return stick / "Seabass" / "backups";
}

std::vector<std::string> recordIds(const fs::path &stick)
{
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(backupsOf(stick), ec)) {
        if (entry.is_directory(ec)) {
            ids.push_back(seabass::pathToUtf8(entry.path().filename()));
        }
    }
    return ids;
}

std::string stickLog(const fs::path &stick)
{
    return readFile(stick / "Seabass" / "seabass.log");
}

std::set<std::string> filenamesIn(const seabass::application::BackupRecord &record)
{
    std::set<std::string> names;
    for (const std::string &path : record.filePaths) {
        names.insert(seabass::pathToUtf8(seabass::pathFromUtf8(path).filename()));
    }
    return names;
}

seabass::application::BackupRecord onlyRecord(const fs::path &stick)
{
    FilesystemBackupStore store(seabass::pathToUtf8(backupsOf(stick)));
    const auto records = store.list();
    assert(records.size() == 1);
    return records.front();
}

bool runningAsRoot()
{
#if defined(_WIN32)
    return false;
#else
    return ::geteuid() == 0;
#endif
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_save_context_archive_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    auto &noProgress = NullProgressReporter::instance();

    // Case 1: one label, one archive, however the files arrive. A file
    // backed up by backupOnce() and more files under the same label from
    // backupAllNow() all land in the one record, the file already in it
    // is not added twice, and a database brings its -wal along.
    {
        const fs::path stick = freshStick(scratch, "one-archive");
        const fs::path rb = stick / "PIONEER" / "rekordbox";
        std::vector<QString> statuses;
        SaveContext ctx(CancellationToken(), noProgress, [&statuses](const QString &text) { statuses.push_back(text); },
                        seabass::gui::pathToQString(stick / "PIONEER"), QString());
        ctx.status(QStringLiteral("Backing up"));
        assert(statuses.size() == 1 && statuses.front() == "Backing up");
        assert(&ctx.backupStore() == static_cast<seabass::application::BackupStore *>(&ctx.archiveStore()));

        assert(ctx.backupOnce(seabass::pathToUtf8(rb / "export.pdb"), "Cue edit"));
        assert(!ctx.backupOnce(seabass::pathToUtf8(rb / "export.pdb"), "Cue edit"));  // already in the record
        const std::string firstId = ctx.backupIdOf(seabass::pathToUtf8(rb / "export.pdb"));
        assert(!firstId.empty());

        ctx.backupAllNow({{seabass::pathToUtf8(rb / "export.pdb"), "Cue edit"},
                          {seabass::pathToUtf8(rb / "exportExt.pdb"), "Cue edit"},
                          {seabass::pathToUtf8(rb / "exportLibrary.db"), "Cue edit"},
                          {std::string(), "Cue edit"}});
        assert(ctx.backupIdOf(seabass::pathToUtf8(rb / "exportExt.pdb")) == firstId);
        assert(ctx.backupIdOf(seabass::pathToUtf8(rb / "exportLibrary.db")) == firstId);
        assert(ctx.backupIdOf(seabass::pathToUtf8(rb / "exportLibrary.db-wal")) == firstId);

        // backupOnce() after that, for a database it has not seen: its
        // log goes in first, under the same label, into the same archive.
        writeFile(rb / "second.db", "another database");
        writeFile(rb / "second.db-wal", "its log");
        assert(ctx.backupOnce(seabass::pathToUtf8(rb / "second.db"), "Cue edit"));
        assert(ctx.backupIdOf(seabass::pathToUtf8(rb / "second.db-wal")) == firstId);

        assert(recordIds(stick) == std::vector<std::string>{firstId});
        const auto record = onlyRecord(stick);
        assert(record.label == "Cue-edit");  // as list() reads it back from the id
        assert(filenamesIn(record)
               == (std::set<std::string>{"export.pdb", "exportExt.pdb", "exportLibrary.db", "exportLibrary.db-wal",
                                         "second.db", "second.db-wal"}));
        assert(record.filePaths.size() == 6);  // each once

        // The stick's note of this save names the one record, so an
        // interrupted save can still be undone.
        const auto noted = seabass::infrastructure::backup::interruptedSaveRecords(seabass::pathToUtf8(backupsOf(stick)));
        assert(noted == std::vector<std::string>{firstId});
        const auto backups = ctx.takeBackups();
        assert(backups.size() == 1 && backups.front().id.toStdString() == firstId);
        std::cout << "case 1 (one archive per label, reused by backupOnce and backupAllNow alike) OK\n";
    }

    // Case 2: a backupAllNow() that fails part-way takes back the record
    // it made for a new label, and keeps the one an earlier call made --
    // here the earlier record's directory has gone missing, so adding to
    // it is what fails.
    {
        const fs::path stick = freshStick(scratch, "failed-backup");
        const fs::path rb = stick / "PIONEER" / "rekordbox";
        SaveContext ctx(CancellationToken(), noProgress, {}, seabass::gui::pathToQString(stick / "PIONEER"), QString());
        assert(ctx.backupOnce(seabass::pathToUtf8(rb / "export.pdb"), "B earlier"));
        const std::string earlier = ctx.backupIdOf(seabass::pathToUtf8(rb / "export.pdb"));
        fs::remove_all(backupsOf(stick) / seabass::pathFromUtf8(earlier));

        std::string message;
        try {
            // "A new" comes first, so its record exists by the time "B
            // earlier" fails.
            ctx.backupAllNow({{seabass::pathToUtf8(rb / "exportExt.pdb"), "A new"},
                              {seabass::pathToUtf8(rb / "exportLibrary.db"), "B earlier"}});
        } catch (const std::runtime_error &e) {
            message = e.what();
        }
        assert(message == "no backup with id " + earlier + " to add to");
        assert(recordIds(stick).empty());  // A's record went again; B's was already gone
        const std::string log = stickLog(stick);
        assert(log.find("backup failed: removed 1 of 1 record(s) this save had already made") != std::string::npos);
        // The file A would have covered is not claimed as backed up.
        assert(ctx.backupIdOf(seabass::pathToUtf8(rb / "exportExt.pdb")).empty());
        // The earlier record keeps its place in the save's own list: the
        // save still knows about it, and says nothing it did not do.
        const auto backups = ctx.takeBackups();
        assert(backups.size() == 1 && backups.front().id.toStdString() == earlier);
        std::cout << "case 2 (a failed backupAllNow removes only the records it made) OK\n";
    }

    // Case 3: a save that changed nothing throws its records away, and
    // the note with them.
    {
        const fs::path stick = freshStick(scratch, "discard");
        const fs::path rb = stick / "PIONEER" / "rekordbox";
        SaveContext ctx(CancellationToken(), noProgress, {}, seabass::gui::pathToQString(stick / "PIONEER"), QString());
        ctx.backupAllNow({{seabass::pathToUtf8(rb / "export.pdb"), "Cues"},
                          {seabass::pathToUtf8(rb / "exportExt.pdb"), "Settings"}});
        assert(recordIds(stick).size() == 2);
        ctx.discardBackupsTakenThisSave();
        assert(recordIds(stick).empty());
        assert(!fs::exists(seabass::infrastructure::backup::interruptedSaveNote(seabass::pathToUtf8(backupsOf(stick)))));
        assert(stickLog(stick).find("so 2 backup record(s) this save had taken were removed") != std::string::npos);
        assert(ctx.takeBackups().empty());
        ctx.discardBackupsTakenThisSave();  // nothing left: nothing happens
        std::cout << "case 3 (a save that changed nothing leaves no record) OK\n";
    }

    // Case 4: ...and when a record will not go, that is said, by id,
    // rather than counted as removed.
#if !defined(_WIN32)
    if (runningAsRoot()) {
        std::cout << "case 4 skipped: running as root, so a read-only folder does not refuse anything\n";
    } else {
        const fs::path stick = freshStick(scratch, "discard-refused");
        const fs::path rb = stick / "PIONEER" / "rekordbox";
        SaveContext ctx(CancellationToken(), noProgress, {}, seabass::gui::pathToQString(stick / "PIONEER"), QString());
        assert(ctx.backupOnce(seabass::pathToUtf8(rb / "export.pdb"), "Cues"));
        const std::string id = ctx.backupIdOf(seabass::pathToUtf8(rb / "export.pdb"));
        // The record's own folder read-only: nothing inside it can be
        // deleted, so the record stays whole rather than half-emptied.
        const fs::path recordDir = backupsOf(stick) / seabass::pathFromUtf8(id);
        fs::permissions(recordDir, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
        ctx.discardBackupsTakenThisSave();
        fs::permissions(recordDir, fs::perms::owner_all, fs::perm_options::replace);
        assert(recordIds(stick) == std::vector<std::string>{id});
        assert(onlyRecord(stick).filePaths.size() == 1);  // still a whole record
        const std::string log = stickLog(stick);
        assert(log.find("1 backup record(s) this save had taken could NOT be removed and are still on the stick: "
                        + id)
               != std::string::npos);
        assert(log.find("were removed") == std::string::npos);
        std::cout << "case 4 (a record that will not go is named, not counted as removed) OK\n";
    }
#endif

    // Case 5: a note the stick will not take is said in the log, and the
    // backup is still made.
    {
        const fs::path stick = freshStick(scratch, "no-note");
        const fs::path rb = stick / "PIONEER" / "rekordbox";
        const fs::path note = seabass::infrastructure::backup::interruptedSaveNote(seabass::pathToUtf8(backupsOf(stick)));
        fs::create_directories(note / "in the way");  // a folder where the note has to go
        SaveContext ctx(CancellationToken(), noProgress, {}, seabass::gui::pathToQString(stick / "PIONEER"), QString());
        assert(ctx.backupOnce(seabass::pathToUtf8(rb / "export.pdb"), "Cues"));
        assert(recordIds(stick).size() == 2);  // the record, and the folder in the note's place
        assert(stickLog(stick).find("could not note the save in progress in") != std::string::npos);
        std::cout << "case 5 (a note that cannot be written is logged, the backup stands) OK\n";
    }

    // Case 6: releasing space when the stick is tight takes an earlier
    // save's automatic record and spares this save's; a stick that could
    // not be measured, or has room, releases nothing.
    {
        const fs::path stick = freshStick(scratch, "tight");
        const fs::path rb = stick / "PIONEER" / "rekordbox";
        std::string older;
        {
            SaveContext earlier(CancellationToken(), noProgress, {}, seabass::gui::pathToQString(stick / "PIONEER"),
                                QString());
            assert(earlier.backupOnce(seabass::pathToUtf8(rb / "export.pdb"), "Earlier save"));
            older = earlier.backupIdOf(seabass::pathToUtf8(rb / "export.pdb"));
        }
        SaveContext ctx(CancellationToken(), noProgress, {}, seabass::gui::pathToQString(stick / "PIONEER"), QString());
        assert(ctx.backupOnce(seabass::pathToUtf8(rb / "exportExt.pdb"), "This save"));
        const std::string mine = ctx.backupIdOf(seabass::pathToUtf8(rb / "exportExt.pdb"));
        assert(recordIds(stick).size() == 2);

        seabass::infrastructure::backup::StickSpace unmeasured;
        assert(ctx.releaseAutomaticBackupsIfTight(unmeasured) == 0);
        seabass::infrastructure::backup::StickSpace roomy;
        roomy.capacityBytes = 64ull << 30;
        roomy.freeBytes = 32ull << 30;
        assert(ctx.releaseAutomaticBackupsIfTight(roomy) == 0);
        assert(recordIds(stick).size() == 2);

        seabass::infrastructure::backup::StickSpace tight;
        tight.capacityBytes = 64ull << 30;
        tight.freeBytes = 0;
        assert(ctx.releaseAutomaticBackupsIfTight(tight) > 0);
        assert(recordIds(stick) == std::vector<std::string>{mine});
        assert(mine != older);
        std::cout << "case 6 (a tight stick releases an earlier save's record, never this save's) OK\n";
    }

    // Case 7: finish hooks. Every hook runs even after one throws; a
    // tidy-up that failed (either kind) is a warning, anything else an
    // error; the first of each is the one reported; and running them a
    // second time runs nothing.
    {
        const fs::path stick = freshStick(scratch, "hooks");
        SaveContext ctx(CancellationToken(), noProgress, {}, seabass::gui::pathToQString(stick / "PIONEER"), QString());
        std::vector<std::string> ran;
        ctx.onFinish([&ran](bool ok) {
            ran.push_back(ok ? "first ok" : "first failed");
            throw seabass::gui::SaveTidyUpFailed("the log beside the database did not fold");
        });
        ctx.onFinish([&ran](bool) {
            ran.push_back("second");
            throw seabass::infrastructure::onelibrary::OneLibraryLogNotFolded("OneLibrary kept its log");
        });
        ctx.onFinish([&ran](bool) {
            ran.push_back("third");
            throw std::runtime_error("the commit did not land");
        });
        ctx.onFinish([&ran](bool) {
            ran.push_back("fourth");
            throw std::runtime_error("nor did this one");
        });
        const auto outcome = ctx.runFinishHooks(true);
        assert((ran == std::vector<std::string>{"first ok", "second", "third", "fourth"}));
        assert(outcome.warning && *outcome.warning == "the log beside the database did not fold");
        assert(outcome.error && *outcome.error == "the commit did not land");
        const std::string log = stickLog(stick);
        assert(log.find("save: the log beside the database did not fold") != std::string::npos);
        assert(log.find("save: OneLibrary kept its log") != std::string::npos);

        const auto again = ctx.runFinishHooks(false);
        assert(ran.size() == 4);
        assert(!again.error && !again.warning);
        std::cout << "case 7 (finish hooks: all run, warnings apart from errors, once only) OK\n";
    }

    std::cout << "save_context_archive_test: all cases passed\n";
    return 0;
}
