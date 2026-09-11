// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <sqlite3.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/compact_stick_backup.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/long_paths.hpp"
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

    std::cout << "all cases passed\n";
    return 0;
}
