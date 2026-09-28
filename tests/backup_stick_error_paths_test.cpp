// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// BackupStick's refusals: an existing archive that cannot be read, has no
// manifest, or has a damaged one; a lock somebody else holds; a write
// that fails half-way; a fingerprint that cannot be taken; a pending
// backup decided twice. Each asserts the use case's own message, that the
// archive is left as it was, and that the stick's lock is free again
// afterwards: the next operation gets it.

#if !defined(_WIN32)
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"

#include "scratch_path.hpp"

using namespace seabass::application;
using namespace seabass::infrastructure::stick_backup;
using seabass::infrastructure::backup::StickBusyError;
using seabass::infrastructure::backup::StickWriteLock;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &path, const std::string &content, std::int64_t mtime = 1'700'000'000)
{
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }
    fs::last_write_time(path, fromUnixSeconds(mtime));
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// The lock is free when a new holder can take it; StickBusyError says a
// holder is still there.
bool lockIsFree(const fs::path &archive)
{
    try {
        StickWriteLock probe(journal::lockPathFor(archive));
    } catch (const StickBusyError &) {
        return false;
    }
    return true;
}

// Every refusal about an existing archive names the way out.
constexpr const char *WayOut = "Delete it from Manage Backups and back up again";

struct Fixture
{
    fs::path root;
    fs::path stick;
    fs::path archive;
    BackupStickOptions options;

    explicit Fixture(const fs::path &at) : root(at), stick(at / "stick"), archive(at / "Seabass Backups" / "STICK.zip")
    {
        fs::remove_all(root);
        writeFile(stick / "Contents" / "a.mp3", seabass::testing::incompressible(64 * 1024, 1));
        writeFile(stick / "PIONEER" / "rekordbox" / "export.pdb", seabass::testing::incompressible(4096, 2));
        options.stickRoot = stick;
        options.archivePath = archive;
        options.stickIdentifier = "uuid-stick";
        options.stickLabel = "STICK";
        // The margin is for real sticks; a scratch tmpfs need not have
        // 64 MB spare for these cases to mean anything.
        options.freeSpaceMarginBytes = 0;
    }

    // An archive written by the stick backup's own zip writer, holding
    // one file and whatever `manifestName` / `manifestBytes` say.
    void plantArchive(const std::string &manifestName, const std::string &manifestBytes)
    {
        fs::create_directories(archive.parent_path());
        PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadWrite);
        Zip64Writer writer(file, {});
        const std::string content = "a file that is not a manifest";
        writer.addFileFromMemory("Contents/a.mp3", 1'700'000'000,
                                 std::as_bytes(std::span<const char>(content.data(), content.size())));
        writer.finish(manifestBytes, manifestName, 1'700'000'000);
    }

    // The same refusal from all three entry points, and the archive and
    // the lock both as they were before.
    void expectRefusedEverywhere(const std::string &expected)
    {
        const std::string before = readFile(archive);
        const BackupPreview preview = BackupStick::preview(options);
        const VerifyOutcome verified = BackupStick::verify(archive);
        const BackupStickOutcome first = BackupStick::execute(options);
        const bool freeAfterFirst = lockIsFree(archive);
        const BackupStickOutcome second = BackupStick::execute(options);
        if (!contains(first.message, expected)) {
            std::cerr << "expected \"" << expected << "\", got \"" << first.message << "\"\n";
        }
        assert(contains(preview.error, expected) && contains(preview.error, WayOut));
        assert(contains(verified.error, expected) && !verified.ok);
        assert(first.status == BackupOutcomeStatus::Failed);
        assert(contains(first.message, expected) && contains(first.message, WayOut));
        assert(freeAfterFirst && "a refused run released the stick's lock");
        assert(second.message == first.message && "the next run got the lock and was refused for the same reason");
        assert(readFile(archive) == before && "the archive is untouched");
    }
};

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_backup_stick_error_paths_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // Bytes that are no archive at all.
    {
        Fixture f(root / "unreadable");
        writeFile(f.archive, std::string(4096, 'z'));
        f.expectRefusedEverywhere("the existing backup archive is unreadable: ");
        std::cout << "case 1 (an archive that cannot be read is refused by preview, verify and execute) OK\n";
    }

    // A readable archive with no stick-backup manifest in it.
    {
        Fixture f(root / "no-manifest");
        f.plantArchive("backup-manifest.json", "{}");
        f.expectRefusedEverywhere("the existing backup archive has no manifest");
        std::cout << "case 2 (an archive without a manifest is refused everywhere) OK\n";
    }

    // A manifest entry whose contents are not a manifest.
    {
        Fixture f(root / "damaged-manifest");
        f.plantArchive(std::string(ManifestEntryName), "not a manifest\n");
        f.expectRefusedEverywhere("the existing backup archive's manifest is damaged: manifest has no #sha256 trailer");
        std::cout << "case 3 (an archive whose manifest is damaged is refused everywhere) OK\n";
    }

    // Verify with nothing to verify.
    {
        Fixture f(root / "nothing-to-verify");
        const VerifyOutcome outcome = BackupStick::verify(f.archive);
        assert(outcome.error == "there is no backup to verify" && !outcome.ok);
        assert(!fs::exists(f.archive) && "verify creates no archive");
        std::cout << "case 4 (verify of an archive that is not there says so) OK\n";
    }

    // A verify cancelled before its first entry.
    {
        Fixture f(root / "verify-cancelled");
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        CancellationToken cancel;
        cancel.cancel();
        const VerifyOutcome outcome = BackupStick::verify(f.archive, cancel);
        assert(outcome.error == "cancelled" && !outcome.ok && outcome.entriesChecked == 0);
        assert(BackupStick::verify(f.archive).ok && "the archive itself is fine");
        std::cout << "case 5 (a cancelled verify says cancelled, checks nothing) OK\n";
    }

    // Somebody else holds the lock: refused by name, nothing written, and
    // the run after the holder lets go succeeds.
    {
        Fixture f(root / "busy");
        fs::create_directories(f.archive.parent_path());
        {
            StickWriteLock holder(journal::lockPathFor(f.archive));
            const BackupStickOutcome outcome = BackupStick::execute(f.options);
            assert(outcome.status == BackupOutcomeStatus::Failed);
            assert(contains(outcome.message, "Another Seabass operation is already writing to this stick"));
            assert(!fs::exists(f.archive) && "a refused run starts no archive");
        }
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        std::cout << "case 6 (a held lock refuses the run; released, the next one completes) OK\n";
    }

    // A pending backup is decided once. The second keep() or discard() is
    // refused and changes nothing.
    {
        Fixture f(root / "decided-twice");
        f.options.onProgress = [&](const BackupProgress &p) {
            if (p.phase == BackupProgress::Phase::Reading && p.filesDone == 1) {
                f.options.cancel.cancel();
            }
        };
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Cancelled && outcome.pending);
        assert(!lockIsFree(f.archive) && "an undecided backup still holds the stick");
        assert(outcome.pending->keep().status == BackupOutcomeStatus::KeptPartial);
        assert(lockIsFree(f.archive) && "keep() released it");
        const std::string afterKeep = readFile(f.archive);
        const BackupStickOutcome keptAgain = outcome.pending->keep();
        assert(keptAgain.status == BackupOutcomeStatus::Failed && keptAgain.message == "already decided");
        const BackupStickOutcome discarded = outcome.pending->discard();
        assert(discarded.status == BackupOutcomeStatus::Failed && discarded.message == "already decided");
        assert(readFile(f.archive) == afterKeep && "neither second decision touched the archive");
        std::cout << "case 7 (a pending backup decided twice refuses the second decision) OK\n";
    }

    // The library fingerprint: a reader that finds nothing, and one that
    // throws. Neither costs the backup; both are said.
    {
        Fixture f(root / "fingerprint-empty");
        f.options.readLibraryFingerprint = [] { return std::string(); };
        const BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Complete);
        bool said = false;
        for (const std::string &w : outcome.warnings) {
            said = said || contains(w, "the library's catalogs could not be read after the copy");
        }
        assert(said);
        std::cout << "case 8 (a fingerprint that reads nothing is a warning, the backup completes) OK\n";
    }
    {
        Fixture f(root / "fingerprint-throws");
        f.options.readLibraryFingerprint = []() -> std::string { throw std::runtime_error("catalog parse failed"); };
        const BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Complete);
        bool said = false;
        for (const std::string &w : outcome.warnings) {
            said = said || contains(w, "the library fingerprint could not be taken after the copy (catalog parse failed)");
        }
        assert(said);
        std::cout << "case 9 (a fingerprint reader that throws is a warning naming why) OK\n";
    }

    // Something that is not a file where the archive goes: execute cannot
    // open it for writing and says so; the directory is left alone.
    {
        Fixture f(root / "archive-is-a-directory");
        fs::create_directories(f.archive / "inside");
        const BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Failed);
        assert(contains(outcome.message, "could not open the backup archive: "));
        assert(fs::is_directory(f.archive / "inside"));
        assert(lockIsFree(f.archive));
        std::cout << "case 10 (a directory in the archive's place is refused by execute) OK\n";
    }

    // A stick backup being browsed is not a stick: neither previewed nor
    // backed up, and no archive is started for it.
    {
        Fixture f(root / "browsed");
        const fs::path otherArchive = f.root / "elsewhere.zip";
        assert(seabass::infrastructure::local::writeBrowsedBackupMarker(f.stick, otherArchive, f.stick));
        assert(seabass::infrastructure::local::isBrowsedBackupRoot(f.stick) && "the precondition: the marker binds");
        const std::string refusal = seabass::infrastructure::local::browsedBackupRefusal("STICK");
        assert(BackupStick::preview(f.options).error == refusal);
        const BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Failed && outcome.message == refusal);
        assert(!fs::exists(f.archive));
        std::cout << "case 11 (a browsed backup is neither previewed nor backed up) OK\n";
    }

#if !defined(_WIN32)
    // A file on the stick that cannot be opened is skipped with a warning
    // naming it, and the backup does not call itself complete. Not as
    // root, which opens it anyway.
    if (::geteuid() == 0) {
        std::cout << "skipped case 12: running as root, which reads a file with no permissions\n";
    } else {
        Fixture f(root / "unopenable-file");
        const fs::path locked = f.stick / "Contents" / "a.mp3";
        fs::permissions(locked, fs::perms::none, fs::perm_options::replace);
        const BackupStickOutcome outcome = BackupStick::execute(f.options);
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        assert(outcome.status == BackupOutcomeStatus::Complete);
        bool named = false;
        for (const std::string &w : outcome.warnings) {
            named = named || w == "Contents/a.mp3: could not open, skipped";
        }
        assert(named);
        const VerifyOutcome verified = BackupStick::verify(f.archive);
        assert(verified.ok && verified.status == BackupStatus::PartialSkipped
               && "a backup with a file missing is not recorded as complete");
        std::cout << "case 12 (a file that cannot be opened is skipped by name, the record is partial) OK\n";
    }
#endif

#if !defined(_WIN32)
    // A write that fails half-way through a file. Provoked with
    // RLIMIT_FSIZE and SIGXFSZ ignored, which fails the write with EFBIG
    // the way a full stick does, and binds root too. The run is refused
    // with the reason, the lock is released although the failure was an
    // exception inside the run, and the next run recovers the journal and
    // completes.
    {
        Fixture f(root / "write-fails");
        struct rlimit unlimited{};
        assert(getrlimit(RLIMIT_FSIZE, &unlimited) == 0);
        const auto xfszBefore = signal(SIGXFSZ, SIG_IGN);
        struct rlimit limited = unlimited;
        limited.rlim_cur = 16 * 1024;  // below the 64 KiB incompressible file
        // Nothing printed while the limit is on: a cout past it fails
        // and sets badbit on stdout for the rest of the run.
        assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
        const BackupStickOutcome failed = BackupStick::execute(f.options);
        assert(setrlimit(RLIMIT_FSIZE, &unlimited) == 0);
        signal(SIGXFSZ, xfszBefore);
        std::cout << "  the backup failed as arranged: " << failed.message << '\n';
        assert(failed.status == BackupOutcomeStatus::Failed);
        assert(contains(failed.message, "write failed: "));
        assert(lockIsFree(f.archive) && "the failed run released the stick's lock");
        const BackupStickOutcome next = BackupStick::execute(f.options);
        assert(next.status == BackupOutcomeStatus::Complete);
        const VerifyOutcome verified = BackupStick::verify(f.archive);
        assert(verified.ok && verified.error.empty());
        std::cout << "case 13 (a write that fails half-way is refused, the lock is free, the next run completes) OK\n";
    }
#endif

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
