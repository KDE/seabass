// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <sqlite3.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/compact_stick_backup.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/restore_path_sanitizer.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"
#include "stick_fixture.hpp"

#include "scratch_path.hpp"

using namespace seabass::application;
using namespace seabass::infrastructure::stick_backup;
using seabass::infrastructure::hashing::Sha256;
using seabass::infrastructure::removeTreeDeepestFirst;
namespace fs = std::filesystem;
using namespace seabass::test_fixture;

namespace
{

// Reads and writes through PosixArchiveFile so the >MAX_PATH cases work on
// Windows: std::fstream takes the path as given and has no way to reach a
// path that needs the \\?\ prefix.
void writeLongPathFile(const fs::path &p, const std::string &content)
{
    PosixArchiveFile f(longPathSafe(p), PosixArchiveFile::OpenMode::ReadWrite);
    f.truncate(0);
    f.append(std::span<const std::byte>(reinterpret_cast<const std::byte *>(content.data()), content.size()));
}

std::string readLongPathFile(const fs::path &p)
{
    PosixArchiveFile f(longPathSafe(p), PosixArchiveFile::OpenMode::ReadOnly);
    std::string out(f.size(), '\0');
    if (f.size() > 0) {
        f.readAt(0, std::span<std::byte>(reinterpret_cast<std::byte *>(out.data()), out.size()));
    }
    return out;
}

std::size_t tempFilesUnder(const fs::path &root)
{
    std::size_t n = 0;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (entry.path().filename().string().find(".seabass-restore-tmp") != std::string::npos) {
            ++n;
        }
    }
    return n;
}

struct Fixture
{
    fs::path root;
    fs::path stick;
    fs::path archive;
    fs::path target;
    BackupStickOptions backup;
    RestoreOptions restore;

    explicit Fixture(const std::string &name)
        : root(seabass::testing::scratchRoot() / ("seabass_backup_restore_test_" + name)), stick(root / "stick"),
          archive(root / "Seabass Backups" / "STICK.zip"), target(root / "target")
    {
        fs::remove_all(root);
        writeFile(stick / "Contents" / "a.mp3", pseudoRandom(90'000, 1), 1'700'000'000);
        writeFile(stick / "Contents" / "Sub" / "b.mp3", pseudoRandom(40'000, 2), 1'700'000'001);
        writeFile(stick / "Contents" / "Caf\xC3\xA9.mp3", pseudoRandom(3'000, 3), 1'700'000'002);
        writeFile(stick / "PIONEER" / "rekordbox" / "export.pdb", pseudoRandom(4096, 4), 1'700'000'003);
        writeFile(stick / "empty.txt", "", 1'700'000'004);
        createEngineDb(stick / "Engine Library" / "Database2" / "m.db");
        fs::create_directories(stick / "Engine Library" / "Music");
        const char *loopback = std::getenv("SEABASS_LOOPBACK_MOUNT");
        if (loopback != nullptr && *loopback != '\0') {
            target = fs::path(loopback) / ("seabass_restore_" + name);
            fs::remove_all(target);
        }
        fs::create_directories(target);
        backup.stickRoot = stick;
        backup.archivePath = archive;
        backup.stickIdentifier = "uuid";
        backup.stickLabel = "STICK";
        restore.archivePath = archive;
        restore.targetRoot = target;
    }
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove_all(target, ec);
    }
};

}  // namespace

int main()
{
    // ---- Sanitizer ----
    {
        std::string reason;
        bool isDir = false;
        assert(sanitizeEntryName("Contents/Sub/b.mp3", TargetOs::Posix, &reason) == fs::path("Contents") / "Sub" / "b.mp3");
        assert(sanitizeEntryName("dir/", TargetOs::Posix, &reason, &isDir) == fs::path("dir") && isDir);
        for (const char *bad : {"", "/", "../x", "a/../b", "/abs", "C:/x", "c:x", "a//b", "a/./b", "back\\slash", "."}) {
            assert(!sanitizeEntryName(bad, TargetOs::Posix, &reason).has_value());
            assert(!sanitizeEntryName(bad, TargetOs::Windows, &reason).has_value());
        }
        std::string nul("nu\0l", 4);
        assert(!sanitizeEntryName(nul, TargetOs::Posix, &reason).has_value());
        for (const char *windowsOnly : {"aux.mp3", "COM1", "Music/nul", "trailing.", "trailing ", "bad:name", "q?", "a<b", "pipe|x"}) {
            assert(sanitizeEntryName(windowsOnly, TargetOs::Posix, &reason).has_value());
            assert(!sanitizeEntryName(windowsOnly, TargetOs::Windows, &reason).has_value());
        }
        assert(sanitizeEntryName("auxiliary.mp3", TargetOs::Windows, &reason).has_value());
        assert(sanitizeEntryName("Contents/Caf\xC3\xA9.mp3", TargetOs::Windows, &reason).has_value());
        std::cout << "case 1 (entry-name sanitizer: traversal, absolute, drive letters, backslash, NUL, Windows reserved names) OK\n";
    }

    // ---- Round trip: backup, restore, zero changes ----
    {
        Fixture f("roundtrip");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        std::map<std::string, std::string> expected = snapshot(f.stick);

        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.error.empty());
        assert(preview.stickLabel == "STICK" && preview.status == BackupStatus::Complete);
        assert(preview.filesToWrite == 6 && preview.filesUnchanged == 0 && preview.rejected.empty());
        assert(preview.extras == 0 && !preview.targetHasEngineLibrary);

        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(summary.filesWritten == 6 && summary.writeErrors.empty() && summary.rejected.empty());
        assert(snapshot(f.target) == expected);
        for (const TreeEntry &e : walkStickTree(f.stick, CancellationToken::none()).entries) {
            if (!e.isDirectory) {
                std::int64_t restored = toUnixSeconds(fs::last_write_time(f.target / pathFromUtf8(e.relativePath)));
                assert(std::llabs(restored - e.mtimeUnix) <= 2);
            }
        }
        assert(tempFilesUnder(f.target) == 0);

        // The property everything hinges on: a backup taken from the
        // restored tree finds nothing to do.
        BackupStickOptions again = f.backup;
        again.stickRoot = f.target;
        BackupPreview zero = BackupStick::preview(again);
        assert(zero.added == 0 && zero.changed == 0 && zero.removed == 0 && !zero.databaseChanged);
        std::cout << "case 2 (restore is byte-, name- and mtime-exact; re-backup of the restored tree sees zero changes) OK\n";

        RestoreSummary repeat = RestoreStickBackup::execute(f.restore);
        assert(repeat.status == RestoreSummary::Status::Restored);
        assert(repeat.filesWritten == 0 && repeat.filesUnchanged == 6);
        std::cout << "case 3 (restoring again writes nothing) OK\n";
    }

    // ---- A folder name no Windows ANSI code page can hold ----
    // Japanese, Latin-1 and Cyrillic together: whatever the system code
    // page, one of the three is outside it, so any path::string() on the
    // restore's way throws there. Round 8 on Windows lost two tracks to
    // exactly that, written to the stick and then reported as failed.
    {
        Fixture f("non-ansi-folder");
        const fs::path odd = pathFromUtf8("Contents/\xE6\x97\xA5\xE6\x9C\xAC \xC3\xA9 \xD0\x96/c.mp3");
        writeFile(f.stick / odd, pseudoRandom(5'000, 5), 1'700'000'005);
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        for (const std::string &e : summary.writeErrors) {
            std::cerr << "non-ansi-folder: " << e << "\n";
        }
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(summary.writeErrors.empty() && summary.filesWritten == 7);
        assert(fs::exists(f.target / odd));
        std::cout << "case non-ansi-folder (a folder name outside every ANSI code page restores cleanly) OK\n";
    }

    // ---- A database that changed within the mtime window and kept its size is still restored ----
    // SQLite reuses pages, FAT keeps 2 s mtimes: size + mtime cannot tell
    // "one commit later" apart. The manifest's DbSetFingerprint can.
    {
        Fixture f("db-same-second");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        assert(RestoreStickBackup::execute(f.restore).status == RestoreSummary::Status::Restored);
        appendEngineDbRow(f.stick / "Engine Library" / "Database2" / "m.db", "Contents/b.mp3");
        const fs::path stickDb = f.stick / "Engine Library" / "Database2" / "m.db";
        const fs::path targetDb = f.target / "Engine Library" / "Database2" / "m.db";
        assert(fs::file_size(stickDb) == fs::file_size(targetDb));
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.filesToWrite == 1);
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(summary.filesWritten == 1);
        assert(readFile(targetDb) == readFile(stickDb));
        // And once in sync, the fingerprint agrees: nothing to write.
        assert(RestoreStickBackup::preview(f.restore).filesToWrite == 0);
        std::cout << "db-same-second: database rewritten on fingerprint change OK\n";
    }

    // ---- Overlay vs exact ----
    {
        Fixture f("modes");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        assert(RestoreStickBackup::execute(f.restore).status == RestoreSummary::Status::Restored);
        writeFile(f.target / "Contents" / "a.mp3", "tampered", 1'700'500'000);
        fs::remove(f.target / "empty.txt");
        writeFile(f.target / "Contents" / "extra.mp3", pseudoRandom(1000, 9), 1'700'500'001);
        fs::create_directories(f.target / "Extra Dir");

        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.filesToWrite == 2 && preview.extras == 2 && preview.targetHasEngineLibrary);

        RestoreSummary overlay = RestoreStickBackup::execute(f.restore);
        assert(overlay.status == RestoreSummary::Status::Restored);
        assert(overlay.filesWritten == 2 && overlay.extrasRemoved == 0);
        assert(readFile(f.target / "Contents" / "a.mp3") == readFile(f.stick / "Contents" / "a.mp3"));
        assert(fs::exists(f.target / "Contents" / "extra.mp3") && fs::exists(f.target / "Extra Dir"));

        RestoreOptions exact = f.restore;
        exact.exact = true;
        RestoreSummary mirrored = RestoreStickBackup::execute(exact);
        assert(mirrored.status == RestoreSummary::Status::Restored);
        assert(mirrored.filesWritten == 0 && mirrored.extrasRemoved == 2);
        assert(!fs::exists(f.target / "Contents" / "extra.mp3") && !fs::exists(f.target / "Extra Dir"));
        assert(snapshot(f.target) == snapshot(f.stick));
        std::cout << "case 4 (overlay keeps extras and repairs differences; exact removes extras) OK\n";
    }

    // ---- Library check plumbing and problem reporting ----
    {
        Fixture f("check");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        f.restore.libraryCheck = [](const fs::path &) { return std::optional<std::vector<std::string>>{{"Contents/missing.mp3"}}; };
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
        assert(summary.missingTrackPaths && summary.missingTrackPaths->size() == 1);
        std::cout << "case 5 (post-restore library check result surfaces as a problem) OK\n";
    }

    // ---- Adversarial archive: bad names rejected, nothing escapes the target ----
    {
        Fixture f("adversarial");
        std::string good = "harmless", evil = "should never land";
        fs::create_directories(f.archive.parent_path());
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            BackupManifest manifest;
            manifest.stickLabel = "EVIL";
            manifest.createdAtUnix = 1'757'000'000;
            for (auto [name, content] : {std::pair{"ok/good.txt", good}, std::pair{"../evil.txt", evil}, std::pair{"/abs.txt", evil}}) {
                seabass::infrastructure::hashing::Sha256Digest sha;
                CentralEntry e = writer.addFileFromMemory(name, 1'700'000'000, zip::bytesOf(content), &sha);
                manifest.rows.push_back({ManifestRow::Kind::File, name, e.size, e.mtimeUnix, sha, "", e.crc32});
            }
            writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
            file.barrier();
        }
        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.rejected.size() == 2 && preview.filesToWrite == 1);
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
        assert(summary.rejected.size() == 2 && summary.filesWritten == 1);
        assert(readFile(f.target / "ok" / "good.txt") == good);
        assert(!fs::exists(f.target.parent_path() / "evil.txt") && !fs::exists(f.target / "evil.txt") && !fs::exists("/abs.txt"));
        std::cout << "case 6 (traversal and absolute names rejected and reported; the good entry restored) OK\n";
    }

    // ---- Damaged data is never written ----
    {
        Fixture f("damaged");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Reader reader = Zip64Reader::open(file);
            std::size_t victim = *reader.findEntry("Contents/a.mp3");
            std::uint64_t at = reader.dataOffset(victim) + 1000;
            std::vector<std::byte> one(1);
            file.readAt(at, one);
            // No writeAt on purpose -- rewrite the file with one flipped byte.
            std::string bytes = readFile(f.archive);
            bytes[static_cast<std::size_t>(at)] ^= 0x01;
            std::ofstream(f.archive, std::ios::binary) << bytes;
        }
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
        assert(summary.writeErrors.size() == 1 && summary.writeErrors[0].find("Contents/a.mp3") != std::string::npos);
        assert(!fs::exists(f.target / "Contents" / "a.mp3"));
        assert(fs::exists(f.target / "Contents" / "Sub" / "b.mp3"));
        assert(tempFilesUnder(f.target) == 0);
        std::cout << "case 7 (a damaged entry is reported and not written; the rest is) OK\n";
    }

    // ---- Cancel leaves no half-written file ----
    {
        Fixture f("cancel");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        f.restore.onProgress = [&](const RestoreProgress &p) {
            if (p.phase == RestoreProgress::Phase::Writing && p.filesDone == 2) {
                f.restore.cancel.cancel();
            }
        };
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::Cancelled);
        assert(summary.filesWritten == 2);
        assert(tempFilesUnder(f.target) == 0);
        std::cout << "case 8 (cancelled restore: completed files intact, no temp files) OK\n";
    }

    // ---- A compacted archive restores identically ----
    {
        Fixture f("compacted");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        writeFile(f.stick / "Contents" / "a.mp3", pseudoRandom(95'000, 5), 1'700'100'000);
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        CompactStickBackupOptions compact;
        compact.archivePath = f.archive;
        assert(CompactStickBackup::execute(compact).status == CompactionOutcome::Status::Compacted);
        assert(RestoreStickBackup::execute(f.restore).status == RestoreSummary::Status::Restored);
        assert(snapshot(f.target) == snapshot(f.stick));
        std::cout << "case 9 (restore from a compacted archive) OK\n";
    }

    // ---- A destination past Windows' MAX_PATH ----
    {
        // Five 40-character segments put the destination near 300
        // characters, past Windows' 260-character MAX_PATH, while every
        // individual component stays well under NAME_MAX so the same tree
        // is legal on Linux -- there this case simply exercises long names.
        //
        // Built as an archive rather than by backing up a real tree,
        // because walkStickTree cannot descend past MAX_PATH on Windows in
        // the first place, and this case is about the restore side.
        //
        // The stale temp file is what an interrupted restore leaves behind.
        // writeEntry used to remove it through the unprefixed path, which
        // on Windows fails past MAX_PATH *and reports success* in its
        // error_code, and then opened the survivor without truncating: the
        // entry was appended after the stale bytes, and because the CRC,
        // the SHA-256 and the byte count are all taken from the archive
        // stream rather than from the file on disk, every check passed and
        // the restore reported Restored. The assertion that catches it is
        // the byte comparison, not the status.
        //
        // Deliberately not a Fixture: Fixture's constructor and destructor
        // both call fs::remove_all, which never returns on a tree of this
        // shape, so a single interrupted run would leave a directory that
        // hangs every later run of this whole file. This case owns its
        // root and clears it deepest-first at both ends instead.
        const fs::path root = seabass::testing::scratchRoot() / "seabass_backup_restore_test_longpath";
        const fs::path archive = root / "Seabass Backups" / "STICK.zip";
        const fs::path target = fs::absolute(root / "target");
        removeTreeDeepestFirst(root);
        fs::create_directories(target);

        std::string deep = "Contents";
        for (char c = 'a'; c < 'f'; ++c) {
            deep += "/" + std::string(40, c);
        }
        const std::string entryName = deep + "/track.mp3";
        const std::string content = pseudoRandom(4096, 11);

        fs::create_directories(archive.parent_path());
        {
            PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            BackupManifest manifest;
            manifest.stickLabel = "LONG";
            manifest.createdAtUnix = 1'757'000'000;
            seabass::infrastructure::hashing::Sha256Digest sha;
            CentralEntry e = writer.addFileFromMemory(entryName, 1'700'000'000, zip::bytesOf(content), &sha);
            manifest.rows.push_back({ManifestRow::Kind::File, entryName, e.size, e.mtimeUnix, sha, "", e.crc32});
            writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
            file.barrier();
        }

        const fs::path destination = target / fs::path(entryName);
        assert(destination.native().size() > 260);
        std::error_code ec;
        fs::create_directories(longPathSafe(destination.parent_path()), ec);
        assert(!ec);

        fs::path temp = destination;
        temp += std::string(".seabass-restore-tmp");
        const std::string stale(777, 'X');
        writeLongPathFile(temp, stale);
        assert(readLongPathFile(temp) == stale);

        RestoreOptions restore;
        restore.archivePath = archive;
        restore.targetRoot = target;
        RestoreSummary summary = RestoreStickBackup::execute(restore);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(summary.filesWritten == 1 && summary.writeErrors.empty());
        assert(readLongPathFile(destination) == content);
        assert(!fs::exists(longPathSafe(temp)));
        std::cout << "case 10 (destination past MAX_PATH: a leftover temp is replaced, not appended to) OK\n";
        removeTreeDeepestFirst(root);
    }

    // ---- A bulk-corrupted archive is refused outright, not ground through ----
    //
    // Reproduces a real failure: a 28 GiB "Full Stick Backup" whose central
    // directory parsed perfectly (names, sizes, mtimes are metadata,
    // written once, up front) while all but the last few entries' local
    // headers were, on disk, zero -- something outside this archive's own
    // write path zeroed roughly the first 28 of its 30 GiB sometime after
    // it was written; nothing in Zip64Writer/Reader, PosixArchiveFile, or
    // ArchiveUpdater's own verification was ever able to reproduce that
    // corruption directly, so this test reproduces its *symptom* (a
    // majority of entries whose local header is zero) rather than its
    // still-unknown cause. Before this fix, RestoreStickBackup could not
    // tell that apart from ordinary bad luck on a couple of files: it
    // created a directory for every one of thousands of planned entries,
    // then attempted and failed every one of them individually, and only
    // said so at the very end, past the point of taking the target stick
    // back -- see countUnreadableEntries's own comment.
    {
        Fixture f("bulk-corrupt");
        std::string good = "still readable";
        fs::create_directories(f.archive.parent_path());
        std::vector<std::string> corruptNames;
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            BackupManifest manifest;
            manifest.stickLabel = "BULK";
            manifest.createdAtUnix = 1'757'100'000;
            for (int i = 0; i < 6; ++i) {
                std::string name = "Contents/track" + std::to_string(i) + ".mp3";
                std::string content = i == 5 ? good : pseudoRandom(2'000, static_cast<std::uint64_t>(i));
                seabass::infrastructure::hashing::Sha256Digest sha;
                CentralEntry e = writer.addFileFromMemory(name, 1'700'000'000 + i, zip::bytesOf(content), &sha);
                manifest.rows.push_back({ManifestRow::Kind::File, name, e.size, e.mtimeUnix, sha, "", e.crc32});
                if (i != 5) {
                    corruptNames.push_back(name);
                }
            }
            writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
            file.barrier();
        }
        // Zero every local header but the last entry's, in place -- exactly
        // the shape the real archive was found in: a structurally valid
        // central directory, and no local header behind most of it.
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Reader reader = Zip64Reader::open(file);
            std::string bytes = readFile(f.archive);
            for (const std::string &name : corruptNames) {
                std::uint64_t at = reader.entries()[*reader.findEntry(name)].localHeaderOffset;
                for (std::uint64_t b = at; b < at + 16; ++b) {
                    bytes[static_cast<std::size_t>(b)] = '\0';
                }
            }
            std::ofstream(f.archive, std::ios::binary) << bytes;
        }

        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.error.empty());
        assert(preview.unreadableEntries == corruptNames.size());  // 5 of 6: majority

        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::Failed);
        assert(summary.message.find("damaged") != std::string::npos);
        // 5 corrupted files of 7 archive entries -- the 6 files plus the
        // manifest itself, which countUnreadableEntries also scans (it was
        // never corrupted, so it does not add to the count, but it does
        // count towards the total).
        assert(summary.message.find("5 of 7") != std::string::npos);
        // Refused before touching the target at all -- not "created
        // thousands of directories, then failed every file individually".
        assert(summary.directoriesCreated == 0 && summary.filesWritten == 0 && summary.writeErrors.empty());
        assert(!fs::exists(f.target / "Contents"));
        std::cout << "case 11 (bulk-corrupted archive: refused up front, target untouched) OK\n";
    }

    // ---- A single corrupted entry among many is still just one problem ----
    //
    // The other side of case 11's threshold: below a majority, this is the
    // ordinary "one bad file" case (already covered content-wise by case
    // 7's flipped byte), confirming unreadableEntries reports it without
    // execute() refusing the whole restore over it.
    {
        Fixture f("one-corrupt");
        fs::create_directories(f.archive.parent_path());
        std::vector<std::string> names;
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            BackupManifest manifest;
            manifest.stickLabel = "ONE";
            manifest.createdAtUnix = 1'757'200'000;
            for (int i = 0; i < 6; ++i) {
                std::string name = "Contents/track" + std::to_string(i) + ".mp3";
                seabass::infrastructure::hashing::Sha256Digest sha;
                CentralEntry e = writer.addFileFromMemory(name, 1'700'000'000 + i, zip::bytesOf(pseudoRandom(2'000, static_cast<std::uint64_t>(i))), &sha);
                manifest.rows.push_back({ManifestRow::Kind::File, name, e.size, e.mtimeUnix, sha, "", e.crc32});
                names.push_back(name);
            }
            writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
            file.barrier();
        }
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Reader reader = Zip64Reader::open(file);
            std::string bytes = readFile(f.archive);
            std::uint64_t at = reader.entries()[*reader.findEntry(names[0])].localHeaderOffset;
            for (std::uint64_t b = at; b < at + 16; ++b) {
                bytes[static_cast<std::size_t>(b)] = '\0';
            }
            std::ofstream(f.archive, std::ios::binary) << bytes;
        }

        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.unreadableEntries == 1);

        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
        assert(summary.filesWritten == 5);
        assert(summary.writeErrors.size() == 1 && summary.writeErrors[0].find(names[0]) != std::string::npos);
        for (std::size_t i = 1; i < names.size(); ++i) {
            assert(fs::exists(f.target / pathFromUtf8(names[i])));
        }
        std::cout << "case 12 (one corrupted entry among six: reported, the rest still restores) OK\n";
    }

    // ---- an emergency copy says so all the way to the restore ----
    {
        Fixture f("emergency-copy");
        f.backup.sourceReadOnly = true;
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        // Both the list and the preview carry it: the list badges it, the
        // preview's confirmation leads with it, and neither can read the
        // flag off anything but the archive.
        assert(RestoreStickBackup::describe(f.archive).sourceReadOnly);
        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.error.empty());
        assert(preview.sourceReadOnly);
        std::cout << "case: an emergency copy is still marked when it is offered for restore OK\n";
    }

    // ---- Restoring a salvage backup ----------------------------------
    //
    // A backup taken off a stick that had already gone read-only holds
    // part of some files: what was readable before the device stopped.
    // Restoring one onto a FRESH stick should put that part back, since
    // part of a track beats none of it. Restoring it over a copy that is
    // whole must not.
    //
    // Four megabytes of a nine megabyte track written silently over the
    // nine is the outcome this whole feature exists to avoid, and a
    // restore reporting "Restored" afterwards is how it would happen.
    {
        Fixture f("salvage");
        BackupStickOptions salvage = f.backup;
        salvage.sourceReadOnly = true;
        salvage.readLimitForTesting = [](const std::string &path) -> std::optional<std::uint64_t> {
            if (path == "Contents/a.mp3") {
                return std::uint64_t{30'000};
            }
            return std::nullopt;
        };
        const BackupStickOutcome taken = BackupStick::execute(salvage);
        assert(taken.salvaged.size() == 1);
        const std::string wholeFile = readFile(f.stick / "Contents" / "a.mp3");

        // Onto an empty target: the readable part goes back, and the
        // restore says which file it is and how much of it there is.
        {
            RestoreSummary summary = RestoreStickBackup::execute(f.restore);
            assert(summary.status == RestoreSummary::Status::RestoredWithProblems
                   && "a restore that put back part of a file is not a clean Restored");
            assert(summary.partial.size() == 1);
            assert(summary.partial[0].path == "Contents/a.mp3");
            assert(summary.partial[0].bytesAvailable == 30'000);
            assert(summary.partial[0].originalSize == 90'000);
            assert(summary.partial[0].written && "on a fresh stick, part of the track beats none of it");
            assert(readFile(f.target / "Contents" / "a.mp3") == wholeFile.substr(0, 30'000));
            // The files beside it are whole and say nothing.
            assert(readFile(f.target / "Contents" / "Sub" / "b.mp3")
                   == readFile(f.stick / "Contents" / "Sub" / "b.mp3"));
        }

        // Now the dangerous one. The target already holds the whole
        // track -- the copy the DJ still has -- and the backup holds a
        // third of it.
        {
            writeFile(f.target / "Contents" / "a.mp3", wholeFile, 1'700'000'000);
            RestoreSummary summary = RestoreStickBackup::execute(f.restore);
            assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
            assert(summary.partial.size() == 1);
            assert(!summary.partial[0].written && "the whole copy is left alone");
            assert(readFile(f.target / "Contents" / "a.mp3") == wholeFile
                   && "and it really is untouched, byte for byte");
        }
        std::cout << "case salvage-restore (part of a file goes back, but never over a whole copy) OK\n";
    }

    // The same for a database set, which is restored whole or not at
    // all. Over a whole copy of the main file, neither the part nor the
    // set's other members are written: a sidecar from the backup beside
    // the target's own main file is a database made of two moments.
    {
        Fixture f("salvage-db");
        const fs::path stickDb = f.stick / "Engine Library" / "Database2" / "m.db";
        writeFile(fs::path(stickDb.string() + "-journal"), pseudoRandom(512, 9), 1'700'000'100);
        const std::string wholeDb = readFile(stickDb);
        BackupStickOptions salvage = f.backup;
        salvage.sourceReadOnly = true;
        salvage.readLimitForTesting = [](const std::string &path) -> std::optional<std::uint64_t> {
            if (path == "Engine Library/Database2/m.db") {
                return std::uint64_t{4096};
            }
            return std::nullopt;
        };
        const BackupStickOutcome taken = BackupStick::execute(salvage);
        assert(taken.salvaged.size() == 1 && taken.salvaged[0].path == "Engine Library/Database2/m.db");

        const fs::path targetDb = f.target / "Engine Library" / "Database2" / "m.db";
        const fs::path targetJournal = fs::path(targetDb.string() + "-journal");
        {
            RestoreSummary summary = RestoreStickBackup::execute(f.restore);
            assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
            assert(summary.partial.size() == 1 && summary.partial[0].written);
            assert(readFile(targetDb) == wholeDb.substr(0, 4096));
        }
        {
            writeFile(targetDb, wholeDb, 1'700'000'200);
            writeFile(targetJournal, "the target's own journal", 1'700'000'201);
            RestoreSummary summary = RestoreStickBackup::execute(f.restore);
            assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
            assert(summary.partial.size() == 1 && !summary.partial[0].written);
            assert(readFile(targetDb) == wholeDb && "the whole database is left alone");
            assert(readFile(targetJournal) == "the target's own journal" && "and so is the rest of its set");
        }
        // Exact mode must not then remove the kept set's own sidecar as
        // an extra: the backup has no m.db-wal, the target's is part of
        // the database being kept.
        {
            const fs::path targetWal = fs::path(targetDb.string() + "-wal");
            writeFile(targetWal, "the target's own wal", 1'700'000'202);
            RestoreOptions exact = f.restore;
            exact.exact = true;
            RestoreSummary summary = RestoreStickBackup::execute(exact);
            assert(summary.partial.size() == 1 && !summary.partial[0].written);
            assert(readFile(targetDb) == wholeDb);
            assert(fs::exists(targetWal) && readFile(targetWal) == "the target's own wal"
                   && "an exact restore leaves the kept database's -wal where it is");
        }
        std::cout << "case salvage-restore-db (a partial database never goes over a whole one, nor do its siblings) OK\n";
    }

    // The other way round: the backup holds the main database WHOLE and
    // only part of its journal. Holding the set back is still right, a
    // whole m.db from the backup beside the drive's own journal is two
    // moments of one database. Saying nothing about it was not: the held
    // main file is not partial, so it went in no list, and a restore
    // that never wrote the database reported filesUnchanged and
    // "Restored".
    {
        Fixture f("salvage-db-sidecar");
        const fs::path stickDb = f.stick / "Engine Library" / "Database2" / "m.db";
        const fs::path stickJournal = fs::path(stickDb.string() + "-journal");
        const std::string wholeJournal = pseudoRandom(8192, 11);
        writeFile(stickJournal, wholeJournal, 1'700'000'100);
        const std::string wholeDb = readFile(stickDb);
        BackupStickOptions salvage = f.backup;
        salvage.sourceReadOnly = true;
        salvage.readLimitForTesting = [](const std::string &path) -> std::optional<std::uint64_t> {
            if (path == "Engine Library/Database2/m.db-journal") {
                return std::uint64_t{4096};
            }
            return std::nullopt;
        };
        const BackupStickOutcome taken = BackupStick::execute(salvage);
        assert(taken.salvaged.size() == 1 && taken.salvaged[0].path == "Engine Library/Database2/m.db-journal"
               && "the main database was read whole; only its journal is a part");

        const fs::path targetDb = f.target / "Engine Library" / "Database2" / "m.db";
        const fs::path targetJournal = fs::path(targetDb.string() + "-journal");
        writeFile(targetDb, std::string(wholeDb.size(), 'x'), 1'700'000'200);
        writeFile(targetJournal, wholeJournal, 1'700'000'201);
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
        assert(readFile(targetDb) == std::string(wholeDb.size(), 'x') && "the set is held: the drive's database stays");
        const bool named = std::any_of(summary.warnings.begin(), summary.warnings.end(), [](const std::string &w) {
            return w.find("Engine Library/Database2/m.db:") != std::string::npos && w.find("not written") != std::string::npos;
        });
        assert(named && "a whole file held back must be named, not left to filesUnchanged");
        std::cout << "case salvage-restore-db-sidecar (a whole database held back by a partial sidecar says so) OK\n";
    }

    std::cout << "all cases passed\n";

    // ---- A write-protected backup restores ----
    // A reference copy, a read-only share, a file marked read-only on
    // Windows: restoring only reads the archive, so none of that may stop
    // it. Preview and restore both opened it read-write before.
    {
        Fixture f("read-only-archive");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const auto sizeBefore = fs::file_size(f.archive);
        const auto timeBefore = fs::last_write_time(f.archive);
        const auto protect = [](const fs::path &p, bool readOnly) {
            std::error_code ec;
            if (!fs::exists(p, ec)) return;
            fs::permissions(p, readOnly ? (fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read)
                                        : (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read),
                            fs::perm_options::replace, ec);
        };
        protect(f.archive, true);
        protect(BackupStick::journalPathFor(f.archive), true);
        // The folder too, like a read-only share: the restore cannot create
        // its lock file there, and must still read the archive. The backup
        // left one behind, and an existing file stays openable in a
        // write-protected folder -- so it goes first, or the restore would
        // lock it and never take the path this case is about.
        const fs::path folder = f.archive.parent_path();
        std::error_code folderEc;
        fs::remove(seabass::infrastructure::stick_backup::journal::lockPathFor(f.archive), folderEc);
        assert(!fs::exists(seabass::infrastructure::stick_backup::journal::lockPathFor(f.archive)));
        fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec
                                    | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, folderEc);
        // Windows maps this to the read-only attribute, which does not stop
        // files being created in a folder: say so rather than print a pass
        // for a path that did not run.
        bool folderEnforced = true;
        {
            const fs::path probe = folder / "probe.tmp";
            std::ofstream out(probe);
            if (out.is_open()) {
                folderEnforced = false;
                out.close();
                fs::remove(probe, folderEc);
            }
        }
        bool enforced = true;
        try {
            PosixArchiveFile probe(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            enforced = false;  // running as root: permissions do not bind
        } catch (const std::exception &) {
        }
        if (!enforced) {
            std::cout << "case read-only-archive SKIPPED (write protection not enforced for this user)\n";
        } else {
            RestorePreview preview = RestoreStickBackup::preview(f.restore);
            if (!preview.error.empty()) {
                std::cerr << "preview of a read-only archive: " << preview.error << "\n";
            }
            assert(preview.error.empty() && "a read-only backup previews");
            assert(preview.filesToWrite > 0);
            RestoreSummary summary = RestoreStickBackup::execute(f.restore);
            if (summary.status != RestoreSummary::Status::Restored) {
                std::cerr << "restore of a read-only archive: " << summary.message << "\n";
            }
            assert(summary.status == RestoreSummary::Status::Restored && "a read-only backup restores");
            assert(RestoreStickBackup::preview(f.restore).filesToWrite == 0);
            VerifyOutcome verified = BackupStick::verify(f.archive);
            assert(verified.error.empty() && verified.ok && "a read-only backup verifies");
            assert(fs::file_size(f.archive) == sizeBefore && fs::last_write_time(f.archive) == timeBefore);
            std::cout << (folderEnforced
                              ? "case read-only-archive (previews, restores and verifies with neither the file nor its folder writable) OK\n"
                              : "case read-only-archive (previews, restores and verifies a read-only file; folder protection not enforced here) OK\n");
        }
        fs::permissions(folder, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                    | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, folderEc);
        protect(f.archive, false);
        protect(BackupStick::journalPathFor(f.archive), false);
    }

    // ---- An unfinished update: preview reads past it, restore rolls it back ----
    // What a crash or a still-running backup leaves: a journal recording
    // the last good length, and bytes past it that do not form a valid
    // archive tail. Preview used to "recover" -- truncate -- without the
    // archive's lock, cutting a running backup short. It must not truncate,
    // and it must not refuse either: the restore is the one step that can
    // roll the update back, so a preview that refused left the backup
    // unrestorable from the app.
    {
        Fixture f("unfinished-update");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const std::uint64_t goodLength = fs::file_size(f.archive);
        std::uint64_t eocd = 0;
        {
            PosixArchiveFile archive(f.archive, PosixArchiveFile::OpenMode::ReadOnly);
            eocd = Zip64Reader::open(archive).layout().endOfCentralDirectoryOffset;
        }
        {
            const fs::path journalPath = BackupStick::journalPathFor(f.archive);
            std::error_code ec;
            fs::remove(journalPath, ec);
            PosixArchiveFile journalFile(journalPath, PosixArchiveFile::OpenMode::ReadWrite);
            journal::write(journalFile, JournalRecord{goodLength, eocd});
        }
        {
            std::ofstream out(f.archive, std::ios::binary | std::ios::app);
            out << std::string(4096, 'x');
        }
        const std::uint64_t unfinishedLength = fs::file_size(f.archive);
        assert(unfinishedLength == goodLength + 4096);

        const StickBackupDescription listed = RestoreStickBackup::describe(f.archive);
        if (!listed.error.empty()) {
            std::cerr << "listing an unfinished update: " << listed.error << "\n";
        }
        assert(listed.error.empty() && "the backup list offers it rather than calling it unreadable");
        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        if (!preview.error.empty()) {
            std::cerr << "preview of an unfinished update: " << preview.error << "\n";
        }
        assert(preview.error.empty() && "an unfinished update still previews, as its last complete generation");
        assert(preview.rollsBackUnfinishedUpdate && "and says the restore will roll it back");
        assert(preview.filesToWrite > 0);
        assert(fs::file_size(f.archive) == unfinishedLength && "preview must not truncate the archive");

        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        if (summary.status != RestoreSummary::Status::Restored) {
            std::cerr << "restore after an unfinished update: " << summary.message << "\n";
        }
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(fs::file_size(f.archive) == goodLength && "the restore rolled the unfinished update back");
        RestorePreview after = RestoreStickBackup::preview(f.restore);
        assert(after.error.empty() && !after.rollsBackUnfinishedUpdate && after.filesToWrite == 0);
        std::cout << "case unfinished-update (preview reads past it untouched, restore rolls back) OK\n";
    }


    // ---- A corrupt leftover journal is cleared by the restore ----
    // Nothing was appended, so there is nothing to roll back -- but left in
    // place, every preview after it re-checks the archive's tail.
    {
        Fixture f("stale-journal");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const fs::path journalPath = BackupStick::journalPathFor(f.archive);
        const std::uint64_t length = fs::file_size(f.archive);
        {
            std::ofstream out(journalPath, std::ios::binary | std::ios::trunc);
            out << "not a journal record";
        }
        assert(RestoreStickBackup::preview(f.restore).error.empty());
        assert(fs::file_size(journalPath) > 0 && "preview leaves the journal alone");
        assert(RestoreStickBackup::execute(f.restore).status == RestoreSummary::Status::Restored);
        assert(fs::file_size(journalPath) == 0 && "the restore, holding the lock, cleared it");
        assert(fs::file_size(f.archive) == length && "and the archive itself was not touched");
        std::cout << "case stale-journal (a corrupt leftover journal is cleared, the archive untouched) OK\n";
    }


    // ---- A finished update's leftover journal is cleared too ----
    // A valid record whose update did in fact complete (the journal just
    // outlived it): checked thoroughly, not rolled back, then cleared.
    {
        Fixture f("finished-update-journal");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const fs::path journalPath = BackupStick::journalPathFor(f.archive);
        const std::uint64_t length = fs::file_size(f.archive);
        {
            std::error_code ec;
            fs::remove(journalPath, ec);
            PosixArchiveFile journalFile(journalPath, PosixArchiveFile::OpenMode::ReadWrite);
            journal::write(journalFile, JournalRecord{0, 0});  // a first backup that finished
        }
        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.error.empty() && !preview.rollsBackUnfinishedUpdate);
        assert(RestoreStickBackup::execute(f.restore).status == RestoreSummary::Status::Restored);
        assert(fs::file_size(journalPath) == 0 && "the finished update's journal was cleared");
        assert(fs::file_size(f.archive) == length && "and nothing was rolled back");
        std::cout << "case finished-update-journal (a finished update's journal is cleared, nothing rolled back) OK\n";
    }

    // ---- An update that opens but fails verification is still unfinished ----
    // The commit writes the new central directory, then verifies what it
    // appended; a failure there leaves an archive that opens from its end
    // beside a valid journal. Listing and preview must show the generation
    // the restore will actually roll back to, not the failed one.
    {
        Fixture f("failed-verification");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const std::uint64_t firstLength = fs::file_size(f.archive);
        std::uint64_t firstEocd = 0;
        {
            PosixArchiveFile archive(f.archive, PosixArchiveFile::OpenMode::ReadOnly);
            firstEocd = Zip64Reader::open(archive).layout().endOfCentralDirectoryOffset;
        }
        writeFile(f.stick / "Contents" / "new.mp3", pseudoRandom(60'000, 9), 1'700'000'100);
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        assert(fs::file_size(f.archive) > firstLength);
        {
            const fs::path journalPath = BackupStick::journalPathFor(f.archive);
            std::error_code ec;
            fs::remove(journalPath, ec);
            PosixArchiveFile journalFile(journalPath, PosixArchiveFile::OpenMode::ReadWrite);
            journal::write(journalFile, JournalRecord{firstLength, firstEocd});
        }
        {
            // One flipped byte inside the appended data: the archive still
            // opens from its end, but the appended entries do not verify.
            std::fstream io(f.archive, std::ios::binary | std::ios::in | std::ios::out);
            io.seekg(static_cast<std::streamoff>(firstLength + 200));
            char byte = 0;
            io.read(&byte, 1);
            io.seekp(static_cast<std::streamoff>(firstLength + 200));
            byte = static_cast<char>(byte ^ 0x5a);
            io.write(&byte, 1);
        }
        {
            PosixArchiveFile archive(f.archive, PosixArchiveFile::OpenMode::ReadOnly);
            assert(Zip64Reader::tryOpen(archive).has_value() && "the test needs an archive that opens from its end");
            assert(!verifyArchiveTail(archive, firstLength) && "and whose appended data does not verify");
        }
        const std::uint64_t failedLength = fs::file_size(f.archive);
        assert(RestoreStickBackup::describe(f.archive).error.empty());
        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.error.empty());
        assert(preview.rollsBackUnfinishedUpdate && "preview says the failed update will be rolled back");
        assert(fs::file_size(f.archive) == failedLength && "without touching the archive");
        RestoreSummary summary = RestoreStickBackup::execute(f.restore);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(fs::file_size(f.archive) == firstLength && "the restore rolled back to the generation preview showed");
        std::cout << "case failed-verification (an update that opens but does not verify is shown and restored as rolled back) OK\n";
    }


    // ---- An unfinished update in a read-only archive file restores its settled generation ----
    // A zip with the read-only attribute in an ordinary folder: the lock is
    // taken, but the archive cannot be opened to roll back. The restore
    // reads the generation preview showed instead of failing.
    {
        Fixture f("read-only-file-unfinished");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const std::uint64_t goodLength = fs::file_size(f.archive);
        std::uint64_t eocd = 0;
        {
            PosixArchiveFile archive(f.archive, PosixArchiveFile::OpenMode::ReadOnly);
            eocd = Zip64Reader::open(archive).layout().endOfCentralDirectoryOffset;
        }
        const fs::path journalPath = BackupStick::journalPathFor(f.archive);
        {
            std::error_code ec;
            fs::remove(journalPath, ec);
            PosixArchiveFile journalFile(journalPath, PosixArchiveFile::OpenMode::ReadWrite);
            journal::write(journalFile, JournalRecord{goodLength, eocd});
        }
        {
            std::ofstream out(f.archive, std::ios::binary | std::ios::app);
            out << std::string(4096, 'x');
        }
        const std::uint64_t tornLength = fs::file_size(f.archive);
        std::error_code ec;
        fs::permissions(f.archive, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                        fs::perm_options::replace, ec);
        bool enforced = true;
        try {
            PosixArchiveFile probe(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            enforced = false;
        } catch (const std::exception &) {
        }
        if (!enforced) {
            std::cout << "case read-only-file-unfinished SKIPPED (write protection not enforced for this user)\n";
        } else {
            const StickBackupDescription listed = RestoreStickBackup::describe(f.archive);
            assert(listed.error.empty() && listed.archiveBytes == goodLength && "listed at its settled size");
            RestorePreview preview = RestoreStickBackup::preview(f.restore);
            assert(preview.error.empty() && preview.rollsBackUnfinishedUpdate);
            RestoreSummary summary = RestoreStickBackup::execute(f.restore);
            if (summary.status != RestoreSummary::Status::Restored) {
                std::cerr << "restore of a read-only unfinished archive: " << summary.message << "\n";
            }
            assert(summary.status == RestoreSummary::Status::Restored && "restores the settled generation");
            assert(fs::file_size(f.archive) == tornLength && "rolled nothing back: it could not");
            assert(RestoreStickBackup::preview(f.restore).filesToWrite == 0);
            std::cout << "case read-only-file-unfinished (a write-protected archive restores its settled generation) OK\n";
        }
        fs::permissions(f.archive, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                        fs::perm_options::replace, ec);
    }

    // ---- A lock that cannot be taken in a writable folder is reported ----
    // Only a folder nobody can write to may restore without the lock. A lock
    // file that cannot be opened (another account's, say) in a folder that is
    // writable must stop the restore, not let it run unlocked.
    {
        Fixture f("unopenable-lock");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const fs::path lockPath = seabass::infrastructure::stick_backup::journal::lockPathFor(f.archive);
        {
            std::ofstream touch(lockPath, std::ios::app);
        }
        std::error_code ec;
        fs::permissions(lockPath, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                        fs::perm_options::replace, ec);
        bool enforced = true;
        {
            std::ofstream probe(lockPath, std::ios::app);
            enforced = !probe.is_open();
        }
        if (!enforced) {
            std::cout << "case unopenable-lock SKIPPED (write protection not enforced for this user)\n";
        } else {
            RestoreSummary summary = RestoreStickBackup::execute(f.restore);
            assert(summary.status == RestoreSummary::Status::Failed);
            assert(summary.message.find("could not lock the backup") != std::string::npos);
            std::cout << "case unopenable-lock (a lock that cannot be opened in a writable folder stops the restore) OK\n";
        }
        fs::permissions(lockPath, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                        fs::perm_options::replace, ec);
    }


    // ---- Exact mode removes a letter-case variant only where it is a file of its own ----
    // On exFAT/FAT32 "Contents/ARTBAT/x.mp3" and "Contents/Artbat/x.mp3" are
    // one file: the restore writes the backup's copy into it, and a
    // case-sensitive extras list then deleted it as an extra. On a
    // case-sensitive filesystem (ext4, a local folder) they are two files,
    // and the variant is stale: keeping it made an exact restore not exact.
    // The filesystem decides, so the test expects whichever this one does.
    {
        Fixture f("case-variant-extra");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        writeFile(f.target / "case-probe", "", 1'600'000'000);
        const bool foldsCase = fs::exists(f.target / "CASE-PROBE");
        fs::remove(f.target / "case-probe");
        writeFile(f.target / "CONTENTS" / "SUB" / "B.MP3", "the old library's spelling", 1'600'000'000);
        writeFile(f.target / "Seabass" / "backups" / ".write.lock", "", 1'600'000'000);
        writeFile(f.target / "stray.txt", "really extra", 1'600'000'000);
        RestorePreview preview = RestoreStickBackup::preview(f.restore);
        assert(preview.error.empty());
        if (foldsCase) {
            assert(preview.extras == 1 && "only the real stray counts: not the case variant, not the lock's folders");
        } else {
            assert(preview.extras == 4 && "the stray, and the variant with its two folders: files of their own here");
        }
        RestoreOptions exact = f.restore;
        exact.exact = true;
        RestoreSummary summary = RestoreStickBackup::execute(exact);
        assert(summary.status == RestoreSummary::Status::Restored);
        assert(fs::exists(f.target / "CONTENTS" / "SUB" / "B.MP3") == foldsCase
               && "kept where it is the restored file, removed where it is a stale file of its own");
        assert(!fs::exists(f.target / "stray.txt") && "a real extra is still removed");
        for (const std::string &warning : summary.warnings) {
            assert(warning.find("Seabass") == std::string::npos && "no failed removal of the lock's folders");
        }
        std::cout << "case case-variant-extra (a letter-case variant is removed only where the filesystem keeps it apart) OK\n";
    }


    // ---- Exact mode asks the filesystem before removing an extra ----
    // Folding names can only approximate what a filesystem treats as one name
    // (exFAT folds scripts path keys do not; macOS folds NFC and NFD). So an
    // extra is not removed when it is the very same file as one the backup
    // holds. A hard link is the same situation on any filesystem: two names,
    // one file.
    {
        Fixture f("same-file-extra");
        assert(BackupStick::execute(f.backup).status == BackupOutcomeStatus::Complete);
        const fs::path original = f.stick / "Contents" / "a.mp3";
        const fs::path onTarget = f.target / "Contents" / "a.mp3";
        fs::create_directories(onTarget.parent_path());
        fs::copy_file(original, onTarget, fs::copy_options::overwrite_existing);
        fs::last_write_time(onTarget, fs::last_write_time(original));
        const fs::path alias = f.target / "Contents" / "alias-of-a.mp3";
        std::error_code linkError;
        fs::create_hard_link(onTarget, alias, linkError);
        // "CONTENTS" here is meant to be a file of its own, distinct from
        // the real "Contents" folder above, colliding only in name -- the
        // whole point being that the restore must tell the two apart. On
        // a case-folding filesystem they are not distinct at all: "CONTENTS"
        // and "Contents" are the same directory entry, so there is no
        // separate file left to assert about. Confirmed directly on
        // Windows/NTFS: fs::exists(.../"CONTENTS") saw the real "Contents"
        // folder created a few lines up and never went false, in a scenario
        // this test cannot construct here rather than a real bug.
        const bool foldsCase = fs::exists(f.target / "cOnTeNtS");
        if (linkError) {
            std::cout << "case same-file-extra SKIPPED (no hard links here: " << linkError.message() << ")\n";
        } else if (foldsCase) {
            std::cout << "case same-file-extra SKIPPED (this filesystem folds case, so a file named like "
                         "the Contents folder is that folder)\n";
        } else {
            writeFile(f.target / "CONTENTS", "a file named like a backup folder", 1'600'000'000);
            RestoreOptions exact = f.restore;
            exact.exact = true;
            RestoreSummary summary = RestoreStickBackup::execute(exact);
            assert(summary.status == RestoreSummary::Status::Restored);
            assert(fs::exists(onTarget) && fs::exists(alias) && "a second name for a backup file is not removed");
            assert(fs::equivalent(onTarget, alias));
            assert(!fs::exists(f.target / "CONTENTS") && "a file named like a backup folder is an extra, not a match");
            std::cout << "case same-file-extra (an extra that is the same file as a backup file is kept) OK\n";
        }
    }


    // ---- An exact restore that could not write everything removes nothing ----
    // An extra can be the stick's own copy of a file whose backup copy failed
    // to write, under a spelling of its name the restore does not recognise.
    // Removing extras then would leave the stick with neither copy.
    {
        Fixture f("exact-with-write-error");
        fs::create_directories(f.archive.parent_path());
        std::vector<std::string> names;
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            BackupManifest manifest;
            manifest.stickLabel = "ONE";
            manifest.createdAtUnix = 1'757'200'000;
            for (int i = 0; i < 6; ++i) {
                std::string name = "Contents/track" + std::to_string(i) + ".mp3";
                seabass::infrastructure::hashing::Sha256Digest sha;
                CentralEntry e = writer.addFileFromMemory(name, 1'700'000'000 + i, zip::bytesOf(pseudoRandom(2'000, static_cast<std::uint64_t>(i))), &sha);
                manifest.rows.push_back({ManifestRow::Kind::File, name, e.size, e.mtimeUnix, sha, "", e.crc32});
                names.push_back(name);
            }
            writer.finish(manifest.serialize(), std::string(ManifestEntryName), manifest.createdAtUnix);
            file.barrier();
        }
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Reader reader = Zip64Reader::open(file);
            std::string bytes = readFile(f.archive);
            std::uint64_t at = reader.entries()[*reader.findEntry(names[0])].localHeaderOffset;
            for (std::uint64_t b = at; b < at + 16; ++b) {
                bytes[static_cast<std::size_t>(b)] = '\0';
            }
            std::ofstream(f.archive, std::ios::binary) << bytes;
        }
        // The stick's own copy of the track that will fail, under a name the
        // restore does not match, and an ordinary stray.
        writeFile(f.target / "Contents" / "track0-older-spelling.mp3", pseudoRandom(1'500, 99), 1'600'000'000);
        writeFile(f.target / "stray.txt", "extra", 1'600'000'000);

        RestoreOptions exact = f.restore;
        exact.exact = true;
        RestoreSummary summary = RestoreStickBackup::execute(exact);
        assert(summary.status == RestoreSummary::Status::RestoredWithProblems);
        assert(summary.writeErrors.size() == 1);
        assert(summary.extrasRemoved == 0);
        assert(fs::exists(f.target / "Contents" / "track0-older-spelling.mp3") && "the stick's own copy survives");
        assert(fs::exists(f.target / "stray.txt") && "no extra is removed after a failed write");
        bool warned = false;
        for (const std::string &warning : summary.warnings) {
            warned = warned || warning.find("left in place") != std::string::npos;
        }
        assert(warned && "the restore says why the extras stayed");
        for (std::size_t i = 1; i < names.size(); ++i) {
            assert(fs::exists(f.target / pathFromUtf8(names[i])));
        }
        std::cout << "case exact-with-write-error (a restore that could not write everything removes nothing) OK\n";
    }

    return 0;
}
