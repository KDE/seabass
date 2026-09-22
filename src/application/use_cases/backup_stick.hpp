// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::application
{

// Rich progress for a GUI (byte counts overflow the int-based
// ProgressReporter for real sticks). Optional; the ProgressReporter
// still receives per-phase start/tick/finish for the CLI.
struct BackupProgress
{
    enum class Phase
    {
        Scanning,   // stat-only walk of the stick
        Reading,    // streaming changed/added files
        Database,   // capturing the Engine database set(s)
        Writing,    // manifest + central directory
        Verifying,  // reopen-and-verify
    };
    Phase phase = Phase::Scanning;
    std::size_t filesDone = 0;
    std::size_t filesTotal = 0;
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::string currentFile;
};

struct BackupStickOptions
{
    std::filesystem::path stickRoot;
    std::filesystem::path archivePath;  // the journal lives at archivePath + ".journal"
    std::string stickIdentifier;
    std::string stickLabel;
    // How to read the library's content identity, as
    // domain::LibraryFingerprint::serialize(). Called by the backup ITSELF
    // once every file has been captured, and only for a backup that
    // completed; an empty result keeps the previous backup's.
    //
    // A callback rather than a plain string, and called late rather than
    // supplied early, because the header has to describe the library this
    // archive HOLDS. A caller that reads the catalogs at some earlier
    // moment is describing whatever it saw then, and nothing afterwards
    // ever checks the two against each other. That is not hypothetical:
    // TESTRIG_ABC.zip records 143 tracks in its header while the catalogs
    // stored beside it in the same archive fingerprint as 156, so a stick
    // restored from that archive compares as a DIFFERENT library from the
    // archive it came out of. Both sides say "v1", so nothing notices.
    //
    // Callers must read fresh here. Reading through a cache reintroduces
    // exactly the gap this closes: the cache answers for the catalogs as
    // they were, the archive holds them as they are.
    std::function<std::string()> readLibraryFingerprint;
    // The stick is mounted read-only, i.e. damaged: the run still reads
    // everything it can, and the archive is marked as an emergency copy.
    bool sourceReadOnly = false;
    // How far into a file the stick will let this run read, per
    // stick-relative path; nullopt (and an unset hook) means "all of
    // it", which is every real backup.
    //
    // This exists because the salvage path cannot otherwise be
    // exercised. A read that stops part-way WITHOUT the file changing is
    // a device refusing, and no test and no rig can arrange one on a
    // healthy filesystem: truncating the file changes its size, which is
    // a different branch and the one that says "changed while it was
    // being read". The rig's own fault injection
    // (tools/rig_file_failure.cpp) can make an OPEN fail, which is a
    // third branch again, and its header explains why even that took a
    // directory to arrange.
    //
    // So: the one thing a damaged stick does that nothing here can
    // imitate is handed in. It is read only by the loop that copies
    // files, and an unset hook costs nothing.
    std::function<std::optional<std::uint64_t>(const std::string &relativePath)> readLimitForTesting;
    // What the person called this backup, stored in the manifest header.
    //
    // Deliberately optional rather than a plain string, because "leave the
    // name alone" and "clear the name" are different instructions and an
    // empty string cannot say both. nullopt keeps whatever the previous
    // generation had, which is what every caller that does not care about
    // names wants; a value replaces it, including an empty one.
    std::optional<std::string> userName;
    CancellationToken cancel = CancellationToken::none();
    // Polled between files/chunks, at most every `probeInterval`: true
    // means Engine DJ / rekordbox appeared and the database must not be
    // read. Empty = never checked.
    std::function<bool()> conflictingProcessProbe;
    std::chrono::milliseconds probeInterval{2500};
    std::function<void(const BackupProgress &)> onProgress;
    std::size_t chunkSize = 1u << 20;
    std::uint64_t freeSpaceMarginBytes = 64u << 20;
};

// What a backup run would do, computed from a stat-only walk -- the
// "since last backup" line in the UI. Never reads file contents.
struct BackupPreview
{
    bool archiveExists = false;
    std::optional<infrastructure::stick_backup::BackupStatus> previousStatus;
    std::int64_t previousCreatedAtUnix = 0;
    std::string previousIdentifier;
    std::string previousLabel;
    // The name the existing backup carries, so a page can show it and
    // offer to change it without opening the archive a second time.
    std::string previousUserName;
    bool identifierMismatch = false;  // same archive name, different stick

    std::size_t entriesOnStick = 0;
    std::uint64_t stickBytes = 0;
    std::size_t added = 0;
    std::size_t changed = 0;
    std::size_t removed = 0;
    std::size_t unchanged = 0;
    bool databaseChanged = false;
    std::uint64_t bytesToRead = 0;
    std::int64_t uniformShiftSeconds = 0;

    std::uint64_t archiveBytes = 0;
    std::uint64_t deadBytes = 0;
    std::uint64_t freeBytesAtDestination = 0;
    bool enoughFreeSpace = true;

    std::vector<std::string> skipped;
    std::string error;  // non-empty: could not preview (unreadable archive, stick gone, ...)
};

enum class BackupOutcomeStatus
{
    Complete,
    NothingToDo,      // stick matches the archive; nothing written
    Cancelled,        // stopped; `pending` (if set) awaits keep()/discard()
    KeptPartial,      // PendingBackup::keep() committed the partial run
    Discarded,        // PendingBackup::discard() rolled it back
    ConflictAborted,  // Engine DJ / rekordbox appeared; files committed, DB set skipped
    DbTooLarge,       // committed without the DB set (>= 1 GiB)
    DbUnstable,       // committed without the DB set (kept changing)
    Failed,
};

class PendingBackup;

// One file the run could not read in full off a damaged stick. What was
// readable is in the archive; this says how much of it that was, and
// why the rest is not there.
struct SalvagedFile
{
    std::string path;                 // stick-relative
    std::uint64_t bytesSalvaged = 0;  // what reached the archive
    std::uint64_t expectedSize = 0;   // what the stick said the file was
    std::string reason;
};

struct BackupStickOutcome
{
    BackupOutcomeStatus status = BackupOutcomeStatus::Failed;
    std::string message;
    std::size_t added = 0;
    std::size_t changed = 0;
    std::size_t removed = 0;
    std::size_t carried = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t archiveBytes = 0;
    std::uint64_t deadBytes = 0;
    bool databaseCaptured = false;
    std::vector<std::string> warnings;
    // Files kept in part rather than dropped, which only a salvage run
    // (sourceReadOnly) produces. Empty on every healthy-stick backup.
    std::vector<SalvagedFile> salvaged;
    std::unique_ptr<PendingBackup> pending;
};

// A cancelled run, frozen in the "orphan bytes + journal" state. Exactly
// one of keep()/discard() must be called; if neither is (the app dies),
// the journal makes the next open discard -- the safe default.
class PendingBackup
{
public:
    ~PendingBackup();
    PendingBackup(const PendingBackup &) = delete;
    PendingBackup &operator=(const PendingBackup &) = delete;

    BackupStickOutcome keep();
    BackupStickOutcome discard();
    bool decided() const;

private:
    friend class BackupStick;
    struct Impl;
    explicit PendingBackup(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

struct VerifyOutcome
{
    bool ok = false;
    std::string error;  // the archive could not be opened or is structurally broken
    std::size_t entriesChecked = 0;
    std::uint64_t bytesChecked = 0;
    std::vector<std::string> failures;  // entries whose bytes do not match the manifest
    infrastructure::stick_backup::BackupStatus status = infrastructure::stick_backup::BackupStatus::Complete;
};

class BackupStick
{
public:
    static std::filesystem::path journalPathFor(const std::filesystem::path &archivePath);

    // The user-facing "Verify backup": structure, every entry's CRC and
    // SHA-256 against the manifest. Reads the whole archive; never touches
    // the stick.
    static VerifyOutcome verify(const std::filesystem::path &archivePath,
                                CancellationToken cancel = CancellationToken::none(),
                                const std::function<void(std::uint64_t bytesDone, std::uint64_t bytesTotal)> &onProgress = {});

    static BackupPreview preview(const BackupStickOptions &options,
                                 ProgressReporter &reporter = NullProgressReporter::instance());

    static BackupStickOutcome execute(const BackupStickOptions &options,
                                      ProgressReporter &reporter = NullProgressReporter::instance());
};

}  // namespace seabass::application
