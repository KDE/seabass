// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

#include "scratch_path.hpp"

using namespace seabass::application;
using namespace seabass::infrastructure::stick_backup;
namespace fs = std::filesystem;

namespace
{

std::string pseudoRandom(std::size_t size, std::uint64_t seed)
{
    std::string out(size, '\0');
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (std::size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<char>(x & 0xff);
    }
    return out;
}

void writeFile(const fs::path &p, const std::string &content, std::int64_t mtime)
{
    fs::create_directories(p.parent_path());
    {
        std::ofstream out(p, std::ios::binary);
        out << content;
    }
    fs::last_write_time(p, fromUnixSeconds(mtime));
}

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void execSql(sqlite3 *db, const char *sql)
{
    char *error = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    assert(rc == SQLITE_OK);
    sqlite3_free(error);
}

void createEngineDb(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    sqlite3 *db = nullptr;
    assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    execSql(db, "CREATE TABLE Track(id INTEGER PRIMARY KEY, path TEXT)");
    execSql(db, "INSERT INTO Track(path) VALUES('Contents/a.mp3'),('Contents/Sub/b.mp3')");
    sqlite3_close(db);
}

void touchEngineDb(const fs::path &path)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    execSql(db, "INSERT INTO Track(path) VALUES('Contents/c.mp3')");
    sqlite3_close(db);
}

struct Fixture
{
    fs::path root;
    fs::path stick;
    fs::path archive;
    BackupStickOptions options;

    explicit Fixture(const std::string &name)
        : root(seabass::testing::scratchRoot() / ("seabass_backup_stick_test_" + name)), stick(root / "stick"),
          archive(root / "Seabass Backups" / "STICK.zip")
    {
        fs::remove_all(root);
        writeFile(stick / "Contents" / "a.mp3", pseudoRandom(100'000, 1), 1'700'000'000);
        writeFile(stick / "Contents" / "Sub" / "b.mp3", pseudoRandom(50'000, 2), 1'700'000'001);
        writeFile(stick / "PIONEER" / "rekordbox" / "export.pdb", pseudoRandom(4096, 3), 1'700'000'002);
        createEngineDb(stick / "Engine Library" / "Database2" / "m.db");
        fs::create_directories(stick / "Engine Library" / "Music");
        writeFile(stick / "System Volume Information" / "junk", "os junk", 1);
        writeFile(stick / "Seabass" / "backups" / ".write.lock", "", 1);
        writeFile(stick / "Seabass" / "backups" / "keep-me.txt", "undo store contents", 1'700'000'003);
        writeFile(stick / "Engine Library" / "Database2" / "m.db-shm", std::string(32, '\0'), 1);
#if !defined(_WIN32)
        fs::create_symlink(stick / "Contents" / "a.mp3", stick / "Contents" / "link.mp3");
#endif
        options.stickRoot = stick;
        options.archivePath = archive;
        options.stickIdentifier = "uuid-stick";
        options.stickLabel = "STICK";
    }
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    std::set<std::string> archiveNames() const
    {
        PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadOnly);
        Zip64Reader reader = Zip64Reader::open(file);
        std::set<std::string> names;
        for (const CentralEntry &e : reader.entries()) {
            names.insert(e.name);
        }
        return names;
    }

    BackupManifest manifest() const
    {
        PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadOnly);
        Zip64Reader reader = Zip64Reader::open(file);
        return *BackupManifest::parse(reader.readEntryToString(*reader.findEntry(ManifestEntryName)));
    }

    std::string entryContent(const std::string &name) const
    {
        PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadOnly);
        Zip64Reader reader = Zip64Reader::open(file);
        return reader.readEntryToString(*reader.findEntry(name));
    }

    bool verifies() const
    {
        PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadOnly);
        std::string error;
        bool ok = verifyArchiveTail(file, 0, &error);
        if (!ok) {
            std::cerr << "archive does not verify: " << error << "\n";
        }
        return ok;
    }
};

}  // namespace

int main()
{
    // ---- Preview, first backup, no-op, incremental ----
    {
        Fixture f("main");
        BackupPreview preview = BackupStick::preview(f.options);
        assert(preview.error.empty());
        assert(!preview.archiveExists);
        // A preview only reads. It used to open the archive read-write,
        // which created it: every stick previewed but never backed up left
        // an empty .zip in the backup folder.
        assert(!fs::exists(f.archive) && "a first backup's preview creates no archive file");
        assert(preview.added == preview.entriesOnStick && preview.entriesOnStick > 0);
        assert(preview.databaseChanged);
        assert(preview.enoughFreeSpace);
        assert(preview.bytesToRead == preview.stickBytes);
        bool symlinkSkipped = false;
        for (const std::string &s : preview.skipped) {
            symlinkSkipped = symlinkSkipped || s.find("link.mp3") != std::string::npos;
        }
#if !defined(_WIN32)
        assert(symlinkSkipped);
#endif
        std::cout << "case 1 (preview of a first backup) OK\n";

        BackupStickOutcome first = BackupStick::execute(f.options);
        assert(first.status == BackupOutcomeStatus::Complete);
        assert(first.databaseCaptured);
        assert(!first.pending);
        assert(f.verifies());
        std::set<std::string> names = f.archiveNames();
        assert(names.count("Contents/a.mp3") && names.count("Contents/Sub/b.mp3") && names.count("PIONEER/rekordbox/export.pdb"));
        assert(names.count("Engine Library/Database2/m.db") && names.count("Engine Library/Music/"));
        assert(names.count("Seabass/backups/keep-me.txt"));
        assert(!names.count("Seabass/backups/.write.lock"));
        assert(!names.count("System Volume Information/junk") && !names.count("System Volume Information/"));
        assert(!names.count("Engine Library/Database2/m.db-shm"));
        assert(!names.count("Contents/link.mp3"));
        BackupManifest m = f.manifest();
        assert(m.status == BackupStatus::Complete && m.stickIdentifier == "uuid-stick" && m.stickLabel == "STICK");
        const ManifestRow *dbRow = m.findRow("Engine Library/Database2/m.db");
        assert(dbRow != nullptr && !dbRow->extra.empty());
        assert(f.entryContent("Contents/a.mp3") == readFile(f.stick / "Contents" / "a.mp3"));
        assert(!fs::exists(BackupStick::journalPathFor(f.archive)) || fs::file_size(BackupStick::journalPathFor(f.archive)) == 0);
        std::cout << "case 2 (first backup: exclusions honoured, DB fingerprint recorded, archive verifies) OK\n";

        BackupStickOutcome again = BackupStick::execute(f.options);
        assert(again.status == BackupOutcomeStatus::NothingToDo);
        assert(again.bytesRead == 0);
        BackupPreview quiet = BackupStick::preview(f.options);
        assert(quiet.archiveExists && quiet.added == 0 && quiet.changed == 0 && quiet.removed == 0 && !quiet.databaseChanged);
        assert(quiet.previousStatus == BackupStatus::Complete && quiet.deadBytes == 0);
        std::cout << "case 3 (unchanged stick: nothing to do, DB skip check holds) OK\n";

        writeFile(f.stick / "Contents" / "a.mp3", pseudoRandom(120'000, 11), 1'700'100'000);  // replaced
        writeFile(f.stick / "Contents" / "c.mp3", pseudoRandom(30'000, 12), 1'700'100'001);   // added
        fs::remove(f.stick / "Contents" / "Sub" / "b.mp3");                                   // removed
        touchEngineDb(f.stick / "Engine Library" / "Database2" / "m.db");                     // DB changed
        BackupPreview delta = BackupStick::preview(f.options);
        assert(delta.added == 1 && delta.changed == 1 && delta.removed == 1 && delta.databaseChanged);
        std::uint64_t dbSize = fs::file_size(f.stick / "Engine Library" / "Database2" / "m.db");
        assert(delta.bytesToRead == 120'000 + 30'000 + dbSize);

        BackupStickOutcome second = BackupStick::execute(f.options);
        assert(second.status == BackupOutcomeStatus::Complete);
        assert(second.bytesRead == 120'000 + 30'000 + 2 * dbSize);  // DB read twice: copy, then re-hash
        assert(second.deadBytes > 0);
        assert(f.verifies());
        names = f.archiveNames();
        assert(names.count("Contents/c.mp3") && !names.count("Contents/Sub/b.mp3") && names.count("Contents/Sub/"));
        assert(f.entryContent("Contents/a.mp3") == readFile(f.stick / "Contents" / "a.mp3"));
        assert(f.entryContent("Engine Library/Database2/m.db") == readFile(f.stick / "Engine Library" / "Database2" / "m.db"));
        std::cout << "case 4 (incremental: added/changed/removed/DB, only changed bytes read, dead space appears) OK\n";

        BackupStickOptions other = f.options;
        other.stickIdentifier = "uuid-someone-else";
        assert(BackupStick::preview(other).identifierMismatch);
        // And execute() refuses outright: an update from another stick
        // would diff it against this archive and record every file of
        // the original as removed.
        {
            auto namesBefore = f.archiveNames();
            BackupStickOutcome refusedOther = BackupStick::execute(other);
            assert(refusedOther.status == BackupOutcomeStatus::Failed
                   && refusedOther.message.find("different stick") != std::string::npos);
            assert(f.archiveNames() == namesBefore);
        }
        assert(backupStatusFromString("partial-skipped") == BackupStatus::PartialSkipped);
        assert(toString(BackupStatus::PartialSkipped) == "partial-skipped");
        writeFile(f.stick / "Contents" / "d.mp3", pseudoRandom(10'000, 13), 1'700'100'002);  // something to write
        BackupStickOptions tight = f.options;
        tight.freeSpaceMarginBytes = std::uint64_t{1} << 62;
        BackupStickOutcome refused = BackupStick::execute(tight);
        assert(refused.status == BackupOutcomeStatus::Failed && refused.message.find("free space") != std::string::npos);
        assert(!f.archiveNames().count("Contents/d.mp3"));
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.archiveNames().count("Contents/d.mp3"));
        std::cout << "case 5 (identifier mismatch flagged; free-space preflight refuses before writing) OK\n";

        // A journal left behind with orphan bytes: recovery rolls back, then
        // the unchanged stick is a no-op again.
        std::uint64_t sizeBefore = fs::file_size(f.archive);
        {
            PosixArchiveFile file(f.archive, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Reader reader = Zip64Reader::open(file);
            PosixArchiveFile journalFile(BackupStick::journalPathFor(f.archive), PosixArchiveFile::OpenMode::ReadWrite);
            journal::write(journalFile, JournalRecord{sizeBefore, reader.layout().endOfCentralDirectoryOffset});
            std::string orphan = pseudoRandom(5000, 99);
            file.append(zip::bytesOf(orphan));
        }
        assert(fs::file_size(f.archive) == sizeBefore + 5000);
        BackupStickOutcome recovered = BackupStick::execute(f.options);
        assert(recovered.status == BackupOutcomeStatus::NothingToDo);
        assert(fs::file_size(f.archive) == sizeBefore);
        assert(f.verifies());
        std::cout << "case 6 (orphan bytes + journal rolled back on the next run) OK\n";
    }

    // ---- Engine DJ appears mid-run ----
    {
        Fixture f("conflict");
        int probes = 0;
        f.options.probeInterval = std::chrono::milliseconds(0);
        f.options.conflictingProcessProbe = [&] { return ++probes >= 3; };  // after two files
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::ConflictAborted);
        assert(!outcome.databaseCaptured);
        assert(f.verifies());
        BackupManifest m = f.manifest();
        assert(m.status == BackupStatus::PartialConflict);
        std::size_t files = 0;
        for (const ManifestRow &row : m.rows) {
            files += row.kind == ManifestRow::Kind::File ? 1 : 0;
        }
        assert(files == 2);
        assert(!f.archiveNames().count("Engine Library/Database2/m.db"));

        f.options.conflictingProcessProbe = [] { return false; };
        BackupStickOutcome finished = BackupStick::execute(f.options);
        assert(finished.status == BackupOutcomeStatus::Complete && finished.databaseCaptured);
        assert(f.manifest().status == BackupStatus::Complete);
        assert(f.archiveNames().count("Engine Library/Database2/m.db"));
        std::cout << "case 7 (conflicting process mid-run: completed files kept, DB skipped, next run finishes) OK\n";
    }

    // ---- Cancel, keep for later, resume ----
    {
        Fixture f("cancel-keep");
        f.options.onProgress = [&](const BackupProgress &p) {
            if (p.phase == BackupProgress::Phase::Reading && p.filesDone == 1) {
                f.options.cancel.cancel();
            }
        };
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Cancelled);
        assert(outcome.pending != nullptr && !outcome.pending->decided());
        assert(fs::file_size(BackupStick::journalPathFor(f.archive)) > 0);
        BackupStickOutcome kept = outcome.pending->keep();
        assert(kept.status == BackupOutcomeStatus::KeptPartial);
        assert(outcome.pending->decided());
        assert(f.verifies());
        assert(f.manifest().status == BackupStatus::PartialCancelled);
        assert(fs::file_size(BackupStick::journalPathFor(f.archive)) == 0);
        std::size_t filesKept = 0;
        for (const ManifestRow &row : f.manifest().rows) {
            filesKept += row.kind == ManifestRow::Kind::File ? 1 : 0;
        }
        assert(filesKept == 1);

        BackupStickOptions resume = f.options;
        resume.cancel = CancellationToken::none();
        resume.onProgress = {};
        BackupStickOutcome resumed = BackupStick::execute(resume);
        assert(resumed.status == BackupOutcomeStatus::Complete);
        assert(resumed.carried >= 1);
        assert(f.manifest().status == BackupStatus::Complete);
        // A resumed archive was never actually verified here, which is
        // how a hollow one (right length, no data) could have gone
        // unnoticed by the suite -- see
        // tests/backup_archive_concurrent_reader_test.cpp.
        VerifyOutcome resumedVerify = BackupStick::verify(f.archive);
        assert(resumedVerify.error.empty());
        assert(resumedVerify.ok);
        assert(f.verifies());
        std::cout << "case 8 (cancel after one file, keep, resume to complete) OK\n";
    }

    // ---- Cancel a first backup, discard: nothing left behind ----
    {
        Fixture f("cancel-discard");
        f.options.onProgress = [&](const BackupProgress &p) {
            if (p.phase == BackupProgress::Phase::Reading && p.filesDone == 1) {
                f.options.cancel.cancel();
            }
        };
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Cancelled && outcome.pending);
        BackupStickOutcome discarded = outcome.pending->discard();
        assert(discarded.status == BackupOutcomeStatus::Discarded);
        assert(!fs::exists(f.archive));
        assert(!fs::exists(BackupStick::journalPathFor(f.archive)));
        // Nor a lock file: it named a backup that never came to exist.
        fs::path lockFile = f.archive;
        lockFile += ".lock";
        assert(!fs::exists(lockFile));
        // And nothing is held: the path locks again.
        seabass::infrastructure::backup::StickWriteLock again(lockFile.string());
        std::cout << "case 9 (cancel a first backup and discard: archive, journal and lock file removed) OK\n";
    }

    // ---- Cancel an update, discard: previous backup byte-exact ----
    {
        Fixture f("cancel-discard-update");
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        std::string before = readFile(f.archive);
        writeFile(f.stick / "Contents" / "new1.mp3", pseudoRandom(20'000, 21), 1'700'200'000);
        writeFile(f.stick / "Contents" / "new2.mp3", pseudoRandom(20'000, 22), 1'700'200'001);
        f.options.onProgress = [&](const BackupProgress &p) {
            if (p.phase == BackupProgress::Phase::Reading && p.filesDone == 1) {
                f.options.cancel.cancel();
            }
        };
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Cancelled && outcome.pending);
        assert(readFile(f.archive).size() > before.size());
        assert(outcome.pending->discard().status == BackupOutcomeStatus::Discarded);
        assert(readFile(f.archive) == before);
        assert(f.verifies());
        std::cout << "case 10 (cancel an update and discard: previous archive byte-exact) OK\n";
    }

    // ---- an emergency copy off a read-only stick keeps its mark ----
    {
        Fixture f("emergency");
        f.options.sourceReadOnly = true;
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.manifest().sourceReadOnly);
        // Sticky: an update from a healthy stick still carries entries
        // read off the damaged one, so the archive stays marked.
        writeFile(f.stick / "Contents" / "later.mp3", pseudoRandom(5'000, 77), 1'700'300'000);
        f.options.sourceReadOnly = false;
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.manifest().sourceReadOnly);
        assert(f.verifies());
        std::cout << "case 11 (a backup off a read-only stick is marked, and stays marked) OK\n";
    }

    // ---- the name someone gave a backup survives every later update ----
    {
        Fixture f("named");
        f.options.userName = std::string("before the Berlin gig");
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.manifest().userName == "before the Berlin gig");

        // The case this exists for. An update that says nothing about the
        // name must keep it: options.userName is how the GUI says "the
        // field was edited", and every other caller leaves it unset. If
        // this ever reads as empty, running a routine update silently
        // throws away what the person called their backup.
        writeFile(f.stick / "Contents" / "later.mp3", pseudoRandom(5'000, 78), 1'700'400'000);
        f.options.userName.reset();
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.manifest().userName == "before the Berlin gig");

        // Renaming from an update does take effect.
        writeFile(f.stick / "Contents" / "later2.mp3", pseudoRandom(5'000, 79), 1'700'500'000);
        f.options.userName = std::string("after the Berlin gig");
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.manifest().userName == "after the Berlin gig");

        // And an empty name that was actually supplied clears it, which is
        // the whole reason this option is optional rather than a string.
        writeFile(f.stick / "Contents" / "later3.mp3", pseudoRandom(5'000, 80), 1'700'600'000);
        f.options.userName = std::string();
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.manifest().userName.empty());
        assert(f.verifies());
        std::cout << "case 12 (a backup's name survives updates, and can be changed by one) OK\n";
    }

    // ---- the archive keeps a log of what each update did ----
    {
        Fixture f("changelog");
        f.options.userName = std::string("first");
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(f.manifest().generations.size() == 1);
        assert(f.manifest().generations[0].added > 0);
        assert(f.manifest().generations[0].userName == "first");

        // A second run appends rather than replacing: the header only ever
        // describes the newest generation, and without the carry-forward
        // every update would start the log again.
        writeFile(f.stick / "Contents" / "later.mp3", pseudoRandom(5'000, 90), 1'700'700'000);
        f.options.userName.reset();
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        const BackupManifest after = f.manifest();
        assert(after.generations.size() == 2);
        assert(after.generations[1].added == 1);
        assert(after.generations[1].bytesRead > 0);
        // Ordered oldest first, and each row keeps the name in force at
        // the time, so renaming later does not rewrite history.
        assert(after.generations[0].createdAtUnix <= after.generations[1].createdAtUnix);
        assert(after.generations[1].userName == "first");

        // A run that changed nothing writes no generation: NothingToDo
        // never commits, and a log full of "did nothing" would bury the
        // rows that matter.
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::NothingToDo);
        assert(f.manifest().generations.size() == 2);
        assert(f.verifies());
        std::cout << "case 13 (an archive logs every generation, oldest first) OK\n";
    }

    // A destination folder nothing can write to. The very first thing
    // execute() does is take the archive's write lock, which creates
    // the lock file next to the archive -- and creating it is what
    // fails here, not "somebody else holds it". Only StickBusyError was
    // caught, so this came out of the use case as an exception; from
    // the GUI it is thrown inside a QtConcurrent task, where nothing
    // catches it either.
    //
    // RestoreStickBackup already does the right thing for the same
    // situation and says why in its own comment. A backup that cannot
    // be started has to be a sentence on screen, not a crash: the whole
    // point of it is the user being careful with their data.
    //
    // POSIX only, and not as root: neither Windows nor root refuses the
    // write this arranges, so it would report green while proving
    // nothing.
#if !defined(_WIN32)
    if (::geteuid() != 0) {
        Fixture f("unwritable-destination");
        const fs::path readOnly = f.root / "locked-away";
        fs::create_directories(readOnly);
        const fs::perms originalPerms = fs::status(readOnly).permissions();
        fs::permissions(readOnly, fs::perms::owner_read | fs::perms::owner_exec);

        BackupStickOptions options = f.options;
        options.archivePath = (readOnly / "STICK.zip").string();

        BackupStickOutcome outcome = BackupStick::execute(options);
        fs::permissions(readOnly, originalPerms);

        assert(outcome.status == BackupOutcomeStatus::Failed);
        assert(!outcome.message.empty());
        assert(!fs::exists(readOnly / "STICK.zip"));
        std::cout << "case 14 (a destination that cannot be written is a refusal, not an exception) OK\n";
    }
#endif

    // ---- Salvage: an emergency copy keeps what it could read ----------
    //
    // A stick that has gone read-only after an unclean unplug is a
    // salvage job, and the run should behave like one. Today a file that
    // reads short is discarded whole, which on a healthy stick is right
    // (the file is presumably being written) and on a damaged one is
    // backwards: those bytes are the only copy anybody is going to get,
    // and what is at stake is somebody's own cue points and years of
    // edits.
    //
    // The read that stops part-way comes from options.readLimitForTesting,
    // because nothing else can arrange one: truncating the file changes
    // its size, which is the other branch. Everything before the limit is
    // the file's own bytes, read normally.
    {
        Fixture f("salvage");
        const fs::path damaged = f.stick / "Contents" / "a.mp3";
        const std::string whole = readFile(damaged);
        assert(whole.size() == 100'000);

        BackupStickOptions options = f.options;
        options.sourceReadOnly = true;
        options.readLimitForTesting = [](const std::string &path) -> std::optional<std::uint64_t> {
            if (path == "Contents/a.mp3") {
                return std::uint64_t{40'000};
            }
            return std::nullopt;
        };

        BackupStickOutcome outcome = BackupStick::execute(options);
        assert(outcome.status != BackupOutcomeStatus::Failed);

        // Kept, not dropped, and named.
        assert(outcome.salvaged.size() == 1);
        assert(outcome.salvaged[0].path == "Contents/a.mp3");
        assert(outcome.salvaged[0].bytesSalvaged == 40'000);
        assert(outcome.salvaged[0].expectedSize == 100'000);
        assert(!outcome.salvaged[0].reason.empty());

        // In the archive, and it really is the first 40 kB of the file
        // rather than 40 kB of something.
        const auto names = f.archiveNames();
        assert(names.count("Contents/a.mp3") && "the readable part is in the backup");
        const std::string stored = f.entryContent("Contents/a.mp3");
        assert(stored.size() == 40'000);
        assert(stored == whole.substr(0, 40'000));

        // And the manifest says how much is missing, so nothing
        // downstream can present it as a whole file.
        const BackupManifest manifest = f.manifest();
        const ManifestRow *row = manifest.findRow("Contents/a.mp3");
        assert(row != nullptr);
        assert(row->size == 40'000);
        assert(row->salvagedFromSize == 100'000);
        // Its neighbours are untouched: a salvage run is not an excuse
        // to mark the whole archive as damaged.
        const ManifestRow *intact = manifest.findRow("Contents/Sub/b.mp3");
        assert(intact != nullptr && intact->salvagedFromSize == 0);

        // A record with a hole in it must not present itself as complete.
        assert(manifest.status == BackupStatus::PartialSkipped);
        assert(manifest.sourceReadOnly);
        assert(f.verifies() && "a salvaged archive is still a valid archive");
        std::cout << "case 15 (a salvage run keeps the part it could read, and says how much is missing) OK\n";
    }

    // The same short read on a HEALTHY stick is a real fault and stays
    // one: the entry is dropped and the run says so. A stick that is not
    // read-only can be written, so a file that reads short is most
    // likely being written right now, and half of it is not worth
    // keeping when the next run can have all of it.
    {
        Fixture f("salvage-healthy");
        BackupStickOptions options = f.options;  // sourceReadOnly stays false
        options.readLimitForTesting = [](const std::string &path) -> std::optional<std::uint64_t> {
            if (path == "Contents/a.mp3") {
                return std::uint64_t{40'000};
            }
            return std::nullopt;
        };

        BackupStickOutcome outcome = BackupStick::execute(options);
        assert(outcome.salvaged.empty() && "nothing is salvaged off a stick that is not in trouble");
        assert(!f.archiveNames().count("Contents/a.mp3") && "the short read is not kept");
        bool said = false;
        for (const std::string &warning : outcome.warnings) {
            if (warning.find("Contents/a.mp3") != std::string::npos
                && warning.find("could not be read in full") != std::string::npos) {
                said = true;
            }
        }
        assert(said && "and it is reported as the fault it is");
        assert(f.manifest().status == BackupStatus::PartialSkipped);
        std::cout << "case 16 (a short read off a healthy stick is still a fault, not a salvage) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
