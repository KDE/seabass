#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infrastructure/backup/filesystem_backup_store.hpp"

#include "scratch_path.hpp"

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

        auto freed = store.prune(3);
        assert(freed == expectedFreed);
        assert(freed > 0);

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

        std::uint64_t prunedCount1 = store.prune(2);
        assert(prunedCount1 == 0);
        assert(store.list().size() == 2);

        std::uint64_t prunedCount2 = store.prune(10);
        assert(prunedCount2 == 0);  // keepCount well beyond what exists
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

        auto freed = store.prune(0);
        assert(freed > 0);
        assert(store.list().empty());
        std::cout << "case 7 (prune(0): removes every backup, the true empty-keep edge case) OK\n";
    }

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
        std::uint64_t freed = store.prune(1);
        assert(freed > 0);
        assert(fs::exists(mine.path));   // never Seabass's to delete
        assert(fs::exists(auto2.path));  // newest automatic, the keepCount survivor
        assert(!fs::exists(auto1.path));
        std::cout << "case 17 (prune deletes automatic backups and leaves the user's) OK\n";
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

    std::cout << "all cases passed\n";
    return 0;
}
