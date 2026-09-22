// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <cassert>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/file_clock.hpp"

#include "scratch_path.hpp"

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

using namespace seabass::infrastructure::backup;
using seabass::application::BackupOrigin;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_filesystem_backup_store_test";
    fs::remove_all(root);
    fs::create_directories(root);

    fs::path backupsDir = root / "Seabass" / "backups";
    fs::path targetFile = root / "m.db";
    writeFile(targetFile, "original contents");

    // Basic backup + restore round trip.
    {
        FilesystemBackupStore store(backupsDir.string());
        auto record = store.backup({targetFile.string()}, "sync");
        assert(!record.id.empty());

        auto records = store.list();
        assert(records.size() == 1);
        assert(records[0].id == record.id);
        assert(records[0].filePaths.size() == 1);
        assert(records[0].filePaths[0] == fs::absolute(targetFile).string());

        writeFile(targetFile, "corrupted by something later");
        assert(readFile(targetFile) == "corrupted by something later");

        bool restored = store.restore(record.id);
        assert(restored);
        assert(readFile(targetFile) == "original contents");

        // restore() itself backs up what it overwrote first -- the
        // "corrupted" version should now be recoverable too.
        auto afterRestore = store.list();
        assert(afterRestore.size() == 2);
        std::cout << "case 1 (backup + restore round trip, restore backs up what it overwrites) OK\n";
    }

    // Restore puts the file's modification time back, not the moment of the
    // restore: an undo that dates every file "now" makes the stick look
    // freshly edited to the metadata merge rule and the backup advice.
    {
        // Converted with file_clock.hpp, as the store does, so this test builds
        // without stick_tree_walker.cpp just like the store itself.
        const auto fromUnixSeconds = [](std::int64_t seconds) {
            return seabass::infrastructure::toFileClock(
                std::chrono::system_clock::time_point{std::chrono::seconds(seconds)});
        };
        const auto toUnixSeconds = [](fs::file_time_type time) {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       seabass::infrastructure::toSystemClock(time).time_since_epoch())
                .count();
        };
        fs::path dated = root / "dated" / "export.pdb";
        writeFile(dated, "as exported");
        fs::last_write_time(dated, fromUnixSeconds(1'483'254'692));  // 2017-01-01, like a real export
        FilesystemBackupStore store((root / "dated" / "Seabass" / "backups").string());
        auto record = store.backup({dated.string()}, "add-cue");
        writeFile(dated, "after the save");
        assert(toUnixSeconds(fs::last_write_time(dated)) > 1'483'254'692);
        assert(store.restore(record.id));
        assert(readFile(dated) == "as exported");
        assert(toUnixSeconds(fs::last_write_time(dated)) == 1'483'254'692);
        std::cout << "case 1c (restore puts the modification time back too) OK\n";
    }

    // One record for many files: files added later join the same backup
    // (same directory, same manifest), and a restore brings all of them
    // back. Same-named files from different directories stay apart.
    {
        fs::remove_all(backupsDir);
        fs::path a1 = root / "a" / "ANLZ0000.EXT";
        fs::path b1 = root / "b" / "ANLZ0000.EXT";
        fs::create_directories(a1.parent_path());
        fs::create_directories(b1.parent_path());
        writeFile(a1, "a original");
        writeFile(b1, "b original");
        FilesystemBackupStore store(backupsDir.string());
        auto record = store.backup({a1.string()}, "junk-cue-cleanup");
        auto grown = store.addToArchive(record.id, {b1.string()});
        assert(grown.id == record.id);
        assert(grown.sizeBytes > record.sizeBytes);
        auto records = store.list();
        assert(records.size() == 1);
        assert(records[0].filePaths.size() == 2);
        writeFile(a1, "a changed");
        writeFile(b1, "b changed");
        assert(store.restore(record.id));
        assert(readFile(a1) == "a original");
        assert(readFile(b1) == "b original");
        bool threw = false;
        try {
            store.addToArchive("no-such-backup", {a1.string()});
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 1b (addToArchive grows one record; restore brings every file back) OK\n";
    }

    // A manifest with no version line is not a shape this build wrote.
    // It used to be read as "version 1"; there is no version 1 any more,
    // so it is refused rather than guessed at.
    {
        fs::remove_all(backupsDir);
        FilesystemBackupStore store(backupsDir.string());
        auto record = store.backup({targetFile.string()}, "sync");

        fs::path manifestPath = fs::path(record.path) / ".manifest";
        std::string original = readFile(manifestPath);
        size_t firstNewline = original.find('\n');
        writeFile(manifestPath, original.substr(firstNewline + 1));

        writeFile(targetFile, "should stay untouched");
        assert(!store.restore(record.id));
        assert(readFile(targetFile) == "should stay untouched");
        std::cout << "case 2 (a manifest with no version line is refused) OK\n";
    }

    // A manifest claiming a future format version this build doesn't
    // understand is refused, not misinterpreted.
    {
        fs::remove_all(backupsDir);
        FilesystemBackupStore store(backupsDir.string());
        auto record = store.backup({targetFile.string()}, "sync");

        fs::path manifestPath = fs::path(record.path) / ".manifest";
        std::string original = readFile(manifestPath);
        size_t firstNewline = original.find('\n');
        std::string rest = original.substr(firstNewline + 1);
        writeFile(manifestPath, "MANIFEST-VERSION\t999\n" + rest);

        writeFile(targetFile, "should stay untouched");
        assert(!store.restore(record.id));
        assert(readFile(targetFile) == "should stay untouched");
        std::cout << "case 3 (unrecognized manifest version refuses to restore) OK\n";
    }

    // Deleting a single backup removes just that one.
    {
        fs::remove_all(backupsDir);
        FilesystemBackupStore store(backupsDir.string());
        auto record1 = store.backup({targetFile.string()}, "sync");
        auto record2 = store.backup({targetFile.string()}, "sync");
        assert(store.list().size() == 2);

        bool removed = store.remove(record1.id);
        assert(removed);
        auto remaining = store.list();
        assert(remaining.size() == 1);
        assert(remaining[0].id == record2.id);
        assert(!store.remove(record1.id));  // already gone
        std::cout << "case 4 (deleting one backup leaves the other intact) OK\n";
    }

    // prune(): removes only the oldest backups beyond keepCount, leaves
    // the newest keepCount intact, and returns exactly the bytes freed.
    {
        fs::remove_all(backupsDir);
        FilesystemBackupStore store(backupsDir.string());
        // Same label for every call, matching case 4's own convention:
        // ids are timestamp-*and*-label-prefixed, so distinct labels
        // created within the same second wouldn't sort chronologically
        // (lexical order would go by label text, not creation order) --
        // the same label instead forces the "-1"/"-2"/... disambiguating
        // suffix (filesystem_backup_store.cpp's own backup() comment),
        // which *does* sort chronologically.
        auto r1 = store.backup({targetFile.string()}, "sync");
        auto r2 = store.backup({targetFile.string()}, "sync");
        auto r3 = store.backup({targetFile.string()}, "sync");
        auto r4 = store.backup({targetFile.string()}, "sync");
        auto r5 = store.backup({targetFile.string()}, "sync");
        assert(store.list().size() == 5);

        auto beforeRecords = store.list();
        std::uint64_t expectedFreed = 0;
        for (const auto &r : beforeRecords) {
            if (r.id == r1.id || r.id == r2.id) {
                expectedFreed += r.sizeBytes;
            }
        }

        auto pruned = store.prune(3);
        assert(pruned.bytesFreed == expectedFreed);
        assert(pruned.bytesFreed > 0);
        assert(pruned.removed == 2);
        assert(pruned.failed == 0);

        auto remaining = store.list();
        assert(remaining.size() == 3);
        for (const auto &r : remaining) {
            assert(r.id != r1.id);  // oldest two: pruned
            assert(r.id != r2.id);
        }
        bool has3 = false, has4 = false, has5 = false;
        for (const auto &r : remaining) {
            if (r.id == r3.id) has3 = true;
            if (r.id == r4.id) has4 = true;
            if (r.id == r5.id) has5 = true;
        }
        assert(has3 && has4 && has5);  // newest three: kept
        std::cout << "case 5 (prune: removes only the oldest backups beyond keepCount) OK\n";
    }

    // prune(): asking to keep at least as many as exist is a genuine
    // no-op -- nothing removed, 0 bytes freed, not an error and not an
    // off-by-one that removes one anyway.
    {
        fs::remove_all(backupsDir);
        FilesystemBackupStore store(backupsDir.string());
        store.backup({targetFile.string()}, "one");
        store.backup({targetFile.string()}, "two");
        assert(store.list().size() == 2);

        auto keepAll = store.prune(2);
        assert(keepAll.bytesFreed == 0);
        assert(keepAll.removed == 0);
        assert(keepAll.failed == 0);  // nothing to do is not a failure
        assert(store.list().size() == 2);

        auto keepMore = store.prune(10);
        assert(keepMore.bytesFreed == 0);  // keepCount well beyond what exists
        assert(keepMore.removed == 0);
        assert(keepMore.failed == 0);
        assert(store.list().size() == 2);
        std::cout << "case 6 (prune: keepCount >= existing count is a true no-op) OK\n";
    }

    // prune(0): the explicit "keep nothing" edge case removes every
    // backup, not just every-but-one -- worth pinning down since off-
    // by-one bugs love this exact boundary.
    {
        fs::remove_all(backupsDir);
        FilesystemBackupStore store(backupsDir.string());
        store.backup({targetFile.string()}, "one");
        store.backup({targetFile.string()}, "two");
        assert(store.list().size() == 2);

        auto pruned = store.prune(0);
        assert(pruned.bytesFreed > 0);
        assert(pruned.removed == 2);
        assert(pruned.failed == 0);
        assert(store.list().empty());
        std::cout << "case 7 (prune(0): removes every backup, the true empty-keep edge case) OK\n";
    }

    // prune(): a backup it selected but could not remove is reported,
    // not passed over in silence.
    //
    // The byte count on its own cannot tell "there was nothing old
    // enough to remove" from "two backups are still there because the
    // filesystem refused" -- both used to come back as zero, and the
    // second is the one worth saying, because the space the user asked
    // for is still gone. A read-only backups folder is the cheapest way
    // to make a real removal fail; on a stick it is a sharing violation
    // or a name the filesystem will not resolve for unlink.
    //
    // POSIX only, and skipped for a run as root: Windows does not
    // refuse a delete for a read-only parent directory the way this
    // needs, and root ignores the permission bits entirely -- either
    // would report this green while proving nothing.
#if !defined(_WIN32)
    if (::geteuid() != 0) {
        fs::remove_all(backupsDir);
        FilesystemBackupStore store(backupsDir.string());
        store.backup({targetFile.string()}, "one");
        store.backup({targetFile.string()}, "two");
        store.backup({targetFile.string()}, "three");
        assert(store.list().size() == 3);

        // The two oldest are what prune(1) will go for. Made
        // unremovable from the inside -- a read-only record directory,
        // so its own contents cannot be unlinked -- rather than by
        // taking write permission off the folder above, which would let
        // remove_all() empty each record and only then fail, leaving
        // the backup destroyed anyway and nothing to assert about.
        auto before = store.list();  // oldest first
        assert(before.size() == 3);
        const fs::perms originalPerms = fs::status(before[0].path).permissions();
        fs::permissions(before[0].path, fs::perms::owner_read | fs::perms::owner_exec);
        fs::permissions(before[1].path, fs::perms::owner_read | fs::perms::owner_exec);

        auto pruned = store.prune(1);
        fs::permissions(before[0].path, originalPerms);
        fs::permissions(before[1].path, originalPerms);

        assert(pruned.failed == 2);
        assert(pruned.removed == 0);
        assert(pruned.bytesFreed == 0);   // nothing was freed, so nothing is claimed
        assert(store.list().size() == 3);  // and all three really are still there
        std::cout << "case 8 (prune: a removal the filesystem refused is reported, not counted as freed) OK\n";
    }
#endif

    fs::remove_all(root);
    // A stick does not come back at the same mount point after a reboot,
    // and on Windows it gets whatever drive letter is free. Paths on the
    // stick are therefore recorded relative to it, so the same backup
    // restores onto the same stick wherever it turns up.
    {
        fs::path stickA = root / "mount-a";
        fs::path pioneer = stickA / "PIONEER" / "rekordbox";
        fs::path exportPdb = pioneer / "export.pdb";
        writeFile(exportPdb, "original library");

        FilesystemBackupStore store((stickA / "Seabass" / "backups").string());
        auto record = store.backup({exportPdb.string()}, "sync");

        // Nothing absolute may have been written down.
        std::string manifest = readFile(stickA / "Seabass" / "backups" / record.id / ".manifest");
        assert(manifest.find("MANIFEST-VERSION\t4") != std::string::npos);
        assert(manifest.find(stickA.string()) == std::string::npos);
        assert(manifest.find("PIONEER/rekordbox/export.pdb") != std::string::npos);

        // The same stick, now mounted somewhere else entirely.
        fs::path stickB = root / "mount-b";
        fs::rename(stickA, stickB);
        writeFile(stickB / "PIONEER" / "rekordbox" / "export.pdb", "changed since");

        FilesystemBackupStore moved((stickB / "Seabass" / "backups").string());
        assert(moved.restore(record.id));
        assert(readFile(stickB / "PIONEER" / "rekordbox" / "export.pdb") == "original library");
        // ...and nothing was resurrected at the old mount point.
        assert(!fs::exists(stickA));

        // list() still reports where the file actually is now.
        auto listed = moved.list();
        bool found = false;
        for (const auto &r : listed) {
            for (const auto &fp : r.filePaths) {
                if (fp == fs::absolute(stickB / "PIONEER" / "rekordbox" / "export.pdb").string()) {
                    found = true;
                }
            }
        }
        assert(found);
        std::cout << "case 9 (stick paths are relative, so a moved stick still restores) OK\n";
    }

    // A file that is genuinely not on the stick keeps its absolute path:
    // making it relative would produce "../../.." nonsense.
    {
        fs::path stick = root / "offstick" / "mount";
        fs::path elsewhere = root / "offstick" / "not-the-stick" / "cues.db";
        writeFile(elsewhere, "local cue store");
        fs::create_directories(stick);

        FilesystemBackupStore store((stick / "Seabass" / "backups").string());
        auto record = store.backup({elsewhere.string()}, "local-restore");
        std::string manifest = readFile(stick / "Seabass" / "backups" / record.id / ".manifest");
        assert(manifest.find(fs::absolute(elsewhere).string()) != std::string::npos);

        writeFile(elsewhere, "clobbered");
        assert(store.restore(record.id));
        assert(readFile(elsewhere) == "local cue store");
        std::cout << "case 10 (a file off the stick stays absolute) OK\n";
    }

    // ---- archive-backed records --------------------------------------
    // One record, one deflated archive: what a save's backup becomes.
    {
        fs::path stick = root / "arch";
        fs::path a = stick / "PIONEER" / "USBANLZ" / "P001" / "ANLZ0000.EXT";
        fs::path b = stick / "PIONEER" / "USBANLZ" / "P002" / "ANLZ0000.EXT";
        const std::string aBody(200000, 'a');
        const std::string bBody = "cue at 0:00, and again, and again, and again";
        writeFile(a, aBody);
        writeFile(b, bBody);

        FilesystemBackupStore store((stick / "Seabass" / "backups").string());
        auto record = store.backup({a.string(), b.string()}, "stray-cues");
        assert(record.filePaths.size() == 2);
        assert(fs::exists(fs::path(record.path) / "backup.zip"));
        // Both files are called ANLZ0000.EXT. The loose layout has to
        // rename the second one; the archive tells them apart by path.
        assert(record.filePaths[0] != record.filePaths[1]);

        writeFile(a, "clobbered");
        writeFile(b, "clobbered too");
        assert(store.restore(record.id));
        assert(readFile(a) == aBody);
        assert(readFile(b) == bBody);
        std::cout << "case 12 (an archive record restores both files that share a basename) OK\n";

        // It listed like any other record, and the restore made its own
        // pre-restore backup as the loose path does.
        auto records = store.list();
        bool sawPreRestore = false;
        for (const auto &r : records) {
            if (r.label == "pre-restore") {
                sawPreRestore = true;
            }
        }
        assert(sawPreRestore);
        std::cout << "case 13 (restoring an archive still backs up what it overwrites) OK\n";
    }

    // A damaged archive must refuse rather than half-restore: writing a
    // prefix over a live file is worse than doing nothing.
    {
        fs::path stick = root / "arch-damaged";
        fs::path a = stick / "PIONEER" / "export.pdb";
        writeFile(a, "the original");
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());
        auto record = store.backup({a.string()}, "sync");

        fs::path archive = fs::path(record.path) / "backup.zip";
        fs::resize_file(archive, fs::file_size(archive) / 2);

        writeFile(a, "current contents");
        assert(!store.restore(record.id));
        assert(readFile(a) == "current contents");
        std::cout << "case 14 (a truncated archive refuses to restore) OK\n";
    }

    // A record that cannot give back every file restores none of them.
    // Undo Last Save takes true for "all of it is back"; returning true once
    // any one file was written left a stick half in each state while the
    // undo reported success.
    {
        fs::path stick = root / "arch-one-bad-entry";
        fs::path a = stick / "PIONEER" / "export.pdb";
        fs::path b = stick / "PIONEER" / "USBANLZ" / "ANLZ0000.EXT";
        std::string payload;
        for (int i = 0; i < 4096; ++i) {
            payload.push_back(static_cast<char>((i * 7919) % 251));
        }
        writeFile(a, payload);
        writeFile(b, "analysis before");
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());
        auto record = store.backup({a.string(), b.string()}, "sync");

        // Damage the first entry's data, inside the archive, past its header.
        fs::path archive = fs::path(record.path) / "backup.zip";
        std::fstream zip(archive, std::ios::in | std::ios::out | std::ios::binary);
        char header[30];
        zip.read(header, 30);
        const auto u16 = [&](int at) {
            return static_cast<unsigned char>(header[at]) | (static_cast<unsigned char>(header[at + 1]) << 8);
        };
        const std::streamoff damageAt = 30 + u16(26) + u16(28) + 64;
        char byte = 0;
        zip.seekg(damageAt);
        zip.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5a);
        zip.seekp(damageAt);
        zip.write(&byte, 1);
        zip.close();

        writeFile(a, "pdb now");
        writeFile(b, "analysis now");
        assert(!store.restore(record.id));
        assert(readFile(a) == "pdb now");
        assert(readFile(b) == "analysis now" && "the intact entry is not restored on its own");
        std::cout << "case 14b (one damaged entry: the record restores nothing and says so) OK\n";
    }

    // --- who owns a backup, and therefore who may delete it -----------
    //
    // Automatic records are Seabass's own safety copies and Seabass may
    // release them under space pressure. Anything the user asked for is
    // the user's. Getting this wrong deletes data that cannot be got
    // back, so each rule is checked rather than assumed.
    {
        fs::path stick = root / "origins";
        fs::path a = stick / "PIONEER" / "export.pdb";
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());

        writeFile(a, "one");
        auto auto1 = store.backup({a.string()}, "sync");
        writeFile(a, "two");
        auto mine = store.backup({a.string()}, "before-gig", BackupOrigin::UserRequested);
        writeFile(a, "three");
        auto auto2 = store.backup({a.string()}, "sync");

        auto records = store.list();
        assert(records.size() == 3);
        std::map<std::string, BackupOrigin> byId;
        for (const auto &r : records) {
            byId[r.id] = r.origin;
        }
        assert(byId[auto1.id] == BackupOrigin::Automatic);
        assert(byId[mine.id] == BackupOrigin::UserRequested);
        assert(byId[auto2.id] == BackupOrigin::Automatic);
        std::cout << "case 16 (origin survives a round trip through the store) OK\n";

        // keepCount counts automatic records only. Were the user's
        // counted, three of their own backups would push out every
        // safety copy Seabass still needs.
        auto pruned = store.prune(1);
        assert(pruned.bytesFreed > 0);
        assert(pruned.removed == 1);
        assert(pruned.failed == 0);
        assert(fs::exists(mine.path));   // never Seabass's to delete
        assert(fs::exists(auto2.path));  // newest automatic, the keepCount survivor
        assert(!fs::exists(auto1.path));
        std::cout << "case 17 (prune deletes automatic backups and leaves the user's) OK\n";
    }

    // The GUI's Clean Up deletes one backup at a time so it can be
    // cancelled between two, choosing them with pruneCandidates(). It once
    // chose the oldest of every backup instead, and deleted one the user
    // had made (found on Windows, 2026-09-14). So: the same choice as
    // prune(), with the user's backup the oldest of all and in between.
    {
        fs::path stick = root / "candidates";
        fs::path a = stick / "PIONEER" / "export.pdb";
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());

        writeFile(a, "zero");
        auto mineOldest = store.backup({a.string()}, "first-gig", BackupOrigin::UserRequested);
        writeFile(a, "one");
        auto auto1 = store.backup({a.string()}, "sync");
        writeFile(a, "two");
        auto mineBetween = store.backup({a.string()}, "before-gig", BackupOrigin::UserRequested);
        writeFile(a, "three");
        auto auto2 = store.backup({a.string()}, "sync");

        auto keepOne = store.pruneCandidates(1);
        assert(keepOne.size() == 1);
        assert(keepOne[0].id == auto1.id);

        auto keepNone = store.pruneCandidates(0);
        assert(keepNone.size() == 2);
        assert(keepNone[0].id == auto1.id);  // oldest first
        assert(keepNone[1].id == auto2.id);
        for (const auto &r : keepNone) {
            assert(r.id != mineOldest.id && r.id != mineBetween.id);
        }

        // keepCount counts automatic backups only: two of them, keep two,
        // nothing to remove even though there are four backups in all.
        assert(store.pruneCandidates(2).empty());
        assert(store.pruneCandidates(10).empty());
        // Choosing removes nothing.
        assert(store.list().size() == 4);
        std::cout << "case 17b (pruneCandidates never chooses a user's backup) OK\n";
    }

    // The newest automatic record is what Undo Last Save needs, so the
    // pressure release never takes it however much space is asked for.
    {
        fs::path stick = root / "release";
        fs::path a = stick / "PIONEER" / "export.pdb";
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());

        writeFile(a, std::string(4096, 'x'));
        auto oldest = store.backup({a.string()}, "sync");
        writeFile(a, std::string(4096, 'y'));
        auto middle = store.backup({a.string()}, "sync");
        writeFile(a, std::string(4096, 'z'));
        auto newest = store.backup({a.string()}, "sync");

        std::uint64_t freed = store.releaseAutomaticBackups(1);
        assert(freed > 0);
        assert(!fs::exists(oldest.path));  // oldest first
        assert(fs::exists(middle.path));   // asked for 1 byte, stopped once it had it
        assert(fs::exists(newest.path));
        std::cout << "case 18 (the release takes the oldest first and stops when satisfied) OK\n";

        // Far more than the records hold: it must still refuse the last one.
        store.releaseAutomaticBackups(1ull << 40);
        assert(fs::exists(newest.path));
        assert(!fs::exists(middle.path));
        std::cout << "case 19 (the newest automatic backup is never released) OK\n";

        // Nothing asked for, nothing deleted.
        auto before = store.list().size();
        assert(store.releaseAutomaticBackups(0) == 0);
        assert(store.list().size() == before);
        std::cout << "case 20 (asking for no bytes deletes nothing) OK\n";
    }

    // A save makes one record per kind of change, and Undo Last Save needs
    // all of them. The release right after that save is told which they
    // are and takes neither, even though only one of them is the newest.
    {
        fs::path stick = root / "release-spare";
        fs::path a = stick / "PIONEER" / "export.pdb";
        fs::path b = stick / "PIONEER" / "USBANLZ" / "P001" / "ANLZ0000.EXT";
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());

        writeFile(a, std::string(4096, 'o'));
        writeFile(b, std::string(4096, 'o'));
        // Ids are a timestamp and the label, so records made within one
        // second sort by label. "auto-sync" sorts before the save's two,
        // so it is the oldest whether or not the clock ticked in between.
        auto older = store.backup({a.string()}, "auto-sync");
        writeFile(a, std::string(4096, 'p'));
        auto saveRepair = store.backup({a.string()}, "consistency-repair");
        auto saveOrphans = store.backup({b.string()}, "consistency-delete-orphan");
        const std::string newestOfSave = std::max(saveRepair.id, saveOrphans.id);
        const std::string otherOfSave = std::min(saveRepair.id, saveOrphans.id);

        store.releaseAutomaticBackups(1ull << 40, {saveRepair.id, saveOrphans.id});
        assert(!fs::exists(older.path));  // not this save's, so it may go
        assert(fs::exists(saveRepair.path));
        assert(fs::exists(saveOrphans.path));

        // Without being told, only the newest record survives -- which is
        // exactly how a save used to lose half its undo.
        store.releaseAutomaticBackups(1ull << 40);
        assert(fs::exists((stick / "Seabass" / "backups" / newestOfSave)));
        assert(!fs::exists((stick / "Seabass" / "backups" / otherOfSave)));
        std::cout << "case 20b (the release spares every record of the save that just finished) OK\n";
    }

    // restore() takes a copy of what it is about to overwrite. The user
    // asked for the restore, so that copy is theirs.
    {
        fs::path stick = root / "prerestore";
        fs::path a = stick / "PIONEER" / "export.pdb";
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());

        writeFile(a, "original");
        auto record = store.backup({a.string()}, "sync");
        writeFile(a, "changed");
        assert(store.restore(record.id));

        bool sawUserOwned = false;
        for (const auto &r : store.list()) {
            if (r.label == "pre-restore") {
                assert(r.origin == BackupOrigin::UserRequested);
                sawUserOwned = true;
            }
        }
        assert(sawUserOwned);
        // And the pressure release must not be able to take it.
        store.releaseAutomaticBackups(1ull << 40);
        bool stillThere = false;
        for (const auto &r : store.list()) {
            stillThere = stillThere || r.label == "pre-restore";
        }
        assert(stillThere);
        std::cout << "case 22 (a restore's undo copy belongs to the user) OK\n";
    }

    // The manifest's own header lines are bookkeeping, not content. The
    // parser turns any line it does not recognise into a backed-up file
    // entry, so a header it forgets about comes back as a file to restore.
    {
        fs::path stick = root / "marker";
        fs::path a = stick / "PIONEER" / "export.pdb";
        FilesystemBackupStore store((stick / "Seabass" / "backups").string());
        writeFile(a, "payload");
        auto record = store.backup({a.string()}, "sync");

        bool seen = false;
        for (const auto &listed : store.list()) {
            if (listed.id != record.id) {
                continue;
            }
            seen = true;
            assert(listed.filePaths.size() == 1);
            assert(listed.filePaths.front().find("export.pdb") != std::string::npos);
            assert(listed.origin == BackupOrigin::Automatic);
            // The record is one deflated archive, so its size is the
            // archive's -- not the payload's, and not zero.
            assert(listed.sizeBytes > 0);
            assert(fs::exists(fs::path(listed.path) / "backup.zip"));
        }
        assert(seen);
        std::cout << "case 23 (manifest headers are not read back as files to restore) OK\n";
    }

    // Only the store's own records are records. A folder someone put
    // under Seabass/backups/ used to be listed as an automatic backup
    // that sorted first, and prune() removed it.
    {
        fs::path foreignDir = root / "Seabass2" / "backups";
        fs::create_directories(foreignDir / "0-my-own-folder");
        writeFile(foreignDir / "0-my-own-folder" / "precious.txt", "mine");
        FilesystemBackupStore store(foreignDir.string());
        fs::path victim = root / "victim.db";
        writeFile(victim, "v1");
        store.backup({victim.string()}, "sync");
        store.backup({victim.string()}, "sync");
        assert(store.list().size() == 2);
        store.prune(1);
        assert(fs::exists(foreignDir / "0-my-own-folder" / "precious.txt"));
        assert(store.list().size() == 1);
        // A directory holding only backup.zip is a backup that died before
        // its manifest, not a record: not listed, so neither offered to
        // restore from nor counted as automatic by prune. Earlier builds
        // left such directories on sticks, and unlisted they were space
        // nobody could reclaim, so prune sweeps them once their own
        // timestamp id is a day old; a younger one waits (a backup may
        // still be writing it), and a folder that is not the store's --
        // no timestamp, or no archive -- is never touched.
        const std::string young = store.backup({victim.string()}, "sync").id;
        fs::remove(foreignDir / young / ".manifest");
        fs::create_directories(foreignDir / "20200101T000000-add-cue");
        writeFile(foreignDir / "20200101T000000-add-cue" / "backup.zip", "not really a zip, but the store's own file name");
        fs::create_directories(foreignDir / "1-not-ours");
        writeFile(foreignDir / "1-not-ours" / "backup.zip", "not really a zip, but the store's own file name");
        assert(store.list().size() == 1 && "none of the three manifest-less directories is a record");
        store.prune(1);
        assert(!fs::exists(foreignDir / "20200101T000000-add-cue") && "an old dead record is swept");
        assert(fs::exists(foreignDir / young) && "a young one is left for a backup that may still be writing it");
        assert(fs::exists(foreignDir / "1-not-ours") && "a folder without the store's timestamp id is not touched");
        assert(fs::exists(foreignDir / "0-my-own-folder" / "precious.txt") && "nor a folder without an archive");
        std::cout << "case: a directory without a manifest is not a record and survives prune OK\n";
    }

    // A database restored next to a stale -wal or -journal would have
    // those frames replayed over it on the next open. Restore removes
    // the sidecars the archive does not itself contain.
    {
        fs::path walDir = root / "Seabass3" / "backups";
        FilesystemBackupStore store(walDir.string());
        fs::path db = root / "wal" / "exportLibrary.db";
        writeFile(db, "generation 1");
        auto record = store.backup({db.string()}, "sync");
        writeFile(db, "generation 2");
        writeFile(root / "wal" / "exportLibrary.db-wal", "frames from generation 2");
        writeFile(root / "wal" / "exportLibrary.db-shm", std::string(32, '\0'));
        assert(store.restore(record.id));
        assert(readFile(db) == "generation 1");
        assert(!fs::exists(root / "wal" / "exportLibrary.db-wal"));
        assert(!fs::exists(root / "wal" / "exportLibrary.db-shm"));
        std::cout << "case: restoring a database removes stale sidecars beside it OK\n";
    }

    // And a sidecar it could NOT remove fails the restore rather than
    // reporting one. The sweep in #35 filed every -wal/-shm removal
    // under "a generated name, a failure is cleanup noise", which is
    // true everywhere except here: the case above says why, and this is
    // what happens when that removal is refused. On Windows the refusal
    // is ordinary -- something still has the -wal open.
    //
    // Arranged with a non-empty directory in the sidecar's place, which
    // no filesystem will unlink, and which needs no permission games
    // that would have stopped the database write first and proved
    // nothing.
    {
        fs::path walDir = root / "Seabass4" / "backups";
        FilesystemBackupStore store(walDir.string());
        fs::path db = root / "stuck-wal" / "exportLibrary.db";
        writeFile(db, "generation 1");
        auto record = store.backup({db.string()}, "sync");
        writeFile(db, "generation 2");
        const fs::path stuck = root / "stuck-wal" / "exportLibrary.db-wal";
        fs::create_directories(stuck);
        writeFile(stuck / "keeps-it-alive", "not going anywhere");

        assert(!store.restore(record.id) && "a sidecar that survives means the restore did not hold");
        assert(fs::exists(stuck) && "and it really is still there");
        std::cout << "case: a sidecar that cannot be removed fails the restore OK\n";
    }

#ifndef _WIN32
    // A backup that fails part-way leaves nothing broken behind. The
    // release rig's full-stick check found the opposite: a save refused
    // for lack of space left <backups>/<ts>-add-cue/ holding a truncated
    // backup.zip and no manifest, and Manage Backups listed it as an
    // empty record. Provoked here without a full disk: a write past
    // RLIMIT_FSIZE fails with EFBIG once SIGXFSZ is ignored, which is the
    // same mid-write failure a full stick gives, deterministic, and it
    // binds root as much as anyone.
    {
        struct rlimit unlimited{};
        assert(getrlimit(RLIMIT_FSIZE, &unlimited) == 0);
        const auto xfszBefore = signal(SIGXFSZ, SIG_IGN);
        // Nothing is printed while the limit is on: with stdout sent to a
        // file, a cout past the limit fails too and sets badbit for good,
        // and every later line of this test would silently vanish.
        std::string arranged;
        const auto limitWritesTo = [&](rlim_t bytes) {
            struct rlimit limit = unlimited;
            limit.rlim_cur = bytes;
            assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        };
        const auto liftTheLimit = [&]() { assert(setrlimit(RLIMIT_FSIZE, &unlimited) == 0); };
        // Incompressible, so the archive really is as big as the file and
        // the limit is reached.
        fs::path failRoot = root / "fails";
        fs::path failBackups = failRoot / "Seabass" / "backups";
        fs::path first = failRoot / "m.db";
        const std::string firstContents = seabass::testing::incompressible(64 * 1024, 1);
        writeFile(first, firstContents);
        FilesystemBackupStore store(failBackups.string());
        auto good = store.backup({first.string()}, "sync");
        assert(store.list().size() == 1);

        // A first backup that fails: no record is left behind, and the
        // record made before it is untouched.
        limitWritesTo(4096);
        bool threw = false;
        try {
            store.backup({first.string()}, "add-cue");
        } catch (const std::exception &e) {
            threw = true;
            arranged = e.what();
        }
        liftTheLimit();
        std::cout << "  the backup failed as arranged: " << arranged << '\n';
        assert(threw && "a backup whose archive cannot be written reports the failure");
        std::vector<std::string> left;
        for (const auto &entry : fs::directory_iterator(failBackups)) {
            left.push_back(entry.path().filename().string());
        }
        assert(left.size() == 1 && left[0] == good.id && "the failed backup's directory is gone, the earlier record stays");
        assert(store.list().size() == 1 && store.list()[0].id == good.id);
        std::cout << "case: a backup that fails part-way leaves no record behind OK\n";

        // An append that fails: the record stays exactly what it was --
        // archive, manifest -- and still restores. Without the cut-back
        // the new entry's bytes trail the old end-of-central-directory
        // and the reader refuses the whole archive, first file included.
        fs::path second = failRoot / "export.pdb";
        writeFile(second, seabass::testing::incompressible(64 * 1024, 2));
        const fs::path archive = fs::path(good.path) / "backup.zip";
        const fs::path manifest = fs::path(good.path) / ".manifest";
        const auto archiveBefore = fs::file_size(archive);
        const std::string manifestBefore = readFile(manifest);
        limitWritesTo(archiveBefore + 512);
        threw = false;
        try {
            store.addToArchive(good.id, {second.string()});
        } catch (const std::exception &e) {
            threw = true;
            arranged = e.what();
        }
        liftTheLimit();
        signal(SIGXFSZ, xfszBefore);
        std::cout << "  the append failed as arranged: " << arranged << '\n';
        assert(threw && "an append whose archive cannot be written reports the failure");
        assert(fs::file_size(archive) == archiveBefore && "the archive is cut back to where it was");
        assert(readFile(manifest) == manifestBefore && "the manifest names what the archive holds");
        assert(store.list().size() == 1 && store.list()[0].filePaths.size() == 1);
        writeFile(first, "changed after the failed append");
        assert(store.restore(good.id) && "the record still restores");
        assert(readFile(first) == firstContents);
        std::cout << "case: an append that fails part-way leaves the record as it was OK\n";

        // An archive the reader refuses (trailing bytes from a torn append
        // that never got cut back) is not appended to: appending would
        // write a central directory naming only the new entries under a
        // manifest that still names the old ones. The save is refused
        // instead, and the manifest stays as it was.
        {
            std::ofstream tail(archive, std::ios::app | std::ios::binary);
            tail << "trailing bytes from a torn append";
        }
        const std::string manifestBeforeRefusal = readFile(manifest);
        threw = false;
        try {
            store.addToArchive(good.id, {second.string()});
        } catch (const std::exception &e) {
            threw = true;
            std::cout << "  the append was refused: " << e.what() << '\n';
        }
        assert(threw && "an unreadable archive is not appended to");
        assert(readFile(manifest) == manifestBeforeRefusal);
        std::cout << "case: an unreadable archive is not appended to OK\n";

        // Issue #27: restore() says why it failed, and a write that fails
        // is not reported as a backup that could not be read. Undo on a
        // nearly full stick used to make its pre-restore copy, run out of
        // space putting the files back, and tell the user the backup was
        // unreadable while the archive was intact. The space a restore
        // needs -- what comes back plus what is there now -- is known
        // before anything is written, so a stick without it is refused
        // up front; the arithmetic is checked here, the refusal needs a
        // full stick (the release rig's F4).
        {
            fs::path spaceRoot = root / "space";
            fs::path big = spaceRoot / "m.db";
            fs::path small = spaceRoot / "export.pdb";
            const std::string bigContents = seabass::testing::incompressible(64 * 1024, 3);
            writeFile(big, bigContents);
            writeFile(small, std::string(1000, 'p'));
            FilesystemBackupStore spaceStore((spaceRoot / "Seabass" / "backups").string());
            auto record = spaceStore.backup({big.string(), small.string()}, "sync");
            writeFile(big, "ten bytes!");
            writeFile(small, std::string(20, 'q'));
            // The peak's upper bound: the copy of what is there now (30
            // bytes), one temporary the size of the largest entry, the
            // growth of both files, and the margin.
            assert(*spaceStore.restoreSpaceNeeded(record.id)
                       == (10 + 20) + 64 * 1024 + ((64 * 1024 - 10) + (1000 - 20)) + 256 * 1024 + 2 * 64 * 1024
                   && "what the restore can need at its peak");

            // The files that come back are bigger than the write limit,
            // the copy of what is there now is not: the pre-restore record
            // is made, the write back fails, and the reason says so and
            // names the file. What was there stays whole.
            const auto xfszHere = signal(SIGXFSZ, SIG_IGN);
            limitWritesTo(8 * 1024);
            const bool restored = spaceStore.restore(record.id);
            liftTheLimit();
            signal(SIGXFSZ, xfszHere);
            assert(!restored);
            std::cout << "  restore refused as arranged: " << spaceStore.lastRestoreError() << '\n';
            assert(spaceStore.lastRestoreError().find("could not write") != std::string::npos);
            assert(spaceStore.lastRestoreError().find("m.db") != std::string::npos);
            assert(readFile(big) == "ten bytes!" && "a failed write leaves the file as it was");
            assert(spaceStore.list().size() == 1
                   && "the pre-restore copy is removed again: nothing was overwritten, so it protects nothing");
            assert(spaceStore.restore(record.id) && readFile(big) == bigContents && "with room again it restores");

            // A damaged archive reads as damaged, not as a write problem --
            // and is refused before any pre-restore copy is made.
            const std::size_t recordsBefore = spaceStore.list().size();
            {
                std::ofstream tail(fs::path(record.path) / "backup.zip", std::ios::app | std::ios::binary);
                tail << "trailing bytes";
            }
            assert(!spaceStore.restore(record.id));
            assert(spaceStore.lastRestoreError().find("damaged") != std::string::npos);
            assert(spaceStore.list().size() == recordsBefore && "a refused restore makes no pre-restore copy");
            std::cout << "case: a restore says whether the backup or the stick was the problem OK\n";
        }
    }
#endif

    std::cout << "all cases passed\n";
    return 0;
}
