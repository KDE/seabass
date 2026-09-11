#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::application
{

struct RestoreProgress
{
    enum class Phase
    {
        Analyzing,
        Writing,
        Removing,  // exact mode: files not in the backup
        Checking,  // opening the restored database
    };
    Phase phase = Phase::Analyzing;
    std::size_t filesDone = 0;
    std::size_t filesTotal = 0;
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::string currentFile;
};

// Runs after the files are in place and reports every track the restored
// Engine database references that does not exist -- the domain-level
// "did it work". Injected because the concrete implementation lives with
// the Engine reader (libdjinterop); see infrastructure/engine/
// engine_restore_check.hpp. Returns nullopt when there is no database to
// open.
using RestoredLibraryCheck = std::function<std::optional<std::vector<std::string>>(const std::filesystem::path &targetRoot)>;

struct RestoreOptions
{
    std::filesystem::path archivePath;
    std::filesystem::path targetRoot;  // a mounted, formatted volume (or any directory)
    // Overlay (default): files on the target that are not in the backup
    // stay. Exact: they are removed after the restore.
    bool exact = false;
    CancellationToken cancel = CancellationToken::none();
    std::function<void(const RestoreProgress &)> onProgress;
    RestoredLibraryCheck libraryCheck;
    std::size_t chunkSize = 1u << 20;
    std::uint64_t freeSpaceMarginBytes = 64u << 20;
};

struct RestorePreview
{
    std::string error;
    std::string stickLabel;
    std::string stickIdentifier;
    infrastructure::stick_backup::BackupStatus status = infrastructure::stick_backup::BackupStatus::Complete;
    std::int64_t createdAtUnix = 0;
    std::size_t entries = 0;   // files + directories in the backup
    std::uint64_t bytes = 0;   // total file bytes in the backup
    std::size_t filesToWrite = 0;   // missing on the target or different size/mtime
    std::size_t filesUnchanged = 0; // present with the same size and mtime: skipped
    std::uint64_t bytesToWrite = 0;
    std::size_t extras = 0;    // on the target, not in the backup (removed only in exact mode)
    std::vector<std::pair<std::string, std::string>> rejected;  // entry name, reason
    bool targetHasEngineLibrary = false;
    std::uint64_t freeBytesAtTarget = 0;
    bool enoughFreeSpace = true;
    // How many of the archive's entries do not have their bytes where the
    // central directory says they are. The central directory itself can
    // still parse fine -- names, sizes and mtimes are metadata, written
    // once, up front -- so this is the only warning available before an
    // actual restore attempt starts finding out file by file. Zero on a
    // healthy archive; see RestoreSummary::Status::Failed for what a large
    // count here does to execute().
    std::size_t unreadableEntries = 0;
};

// What a backup file is, without planning a restore from it: enough to
// list a folder of backups and let a person pick one.
struct StickBackupDescription
{
    std::filesystem::path archivePath;
    std::string error;  // non-empty: unreadable, the other fields are unset
    std::string stickLabel;
    std::string stickIdentifier;
    infrastructure::stick_backup::BackupStatus status = infrastructure::stick_backup::BackupStatus::Complete;
    std::int64_t createdAtUnix = 0;
    std::size_t entries = 0;      // files + directories in the backup
    std::uint64_t archiveBytes = 0;  // size of the .zip on disk
    std::string libraryFingerprint;  // domain::LibraryFingerprint::serialize(), empty for older backups
    // Archive-relative path of each captured database's main file and the
    // DbSetFingerprint hex it had: the exact "has the library changed
    // since" test against the same database on a stick.
    std::vector<std::pair<std::string, std::string>> databaseFingerprints;
};

struct RestoreSummary
{
    enum class Status
    {
        Restored,
        RestoredWithProblems,  // rejected entries, missing tracks, or write errors -- see the lists
        Cancelled,
        Failed,                // nothing usable happened (archive unreadable, target unwritable)
    };
    Status status = Status::Failed;
    std::string message;
    std::size_t filesWritten = 0;
    std::size_t filesUnchanged = 0;
    std::size_t directoriesCreated = 0;
    std::size_t extrasRemoved = 0;
    std::uint64_t bytesWritten = 0;
    std::vector<std::pair<std::string, std::string>> rejected;
    std::vector<std::string> writeErrors;
    std::optional<std::vector<std::string>> missingTrackPaths;  // nullopt: no database / no check
    std::vector<std::string> warnings;
};

// Streams entries out of the archive onto the target -- one seek and one
// read per file, never an "extract everything first" step. Each file is
// written to a temporary name next to its destination, flushed, then
// renamed over it, and its mtime restored so the next incremental backup
// sees zero changes. Database sets are written together and last.
class RestoreStickBackup
{
public:
    // Reads only the manifest. Every `.zip` directly inside `directory`
    // is described, newest first, unreadable ones last.
    static StickBackupDescription describe(const std::filesystem::path &archivePath);
    static std::vector<StickBackupDescription> describeAll(const std::filesystem::path &directory);

    // How many archives `directory` holds, without opening any of them.
    //
    // describeAll() reads every manifest, which is right when the answer
    // is a list to show and far too much when it is only "is there
    // anything here?" -- a question a button asks to decide whether to
    // grey itself out, on the UI thread, every time the first page comes
    // back into view. Same rule for what counts as an archive: a regular
    // file directly inside the directory, named .zip.
    static int countArchives(const std::filesystem::path &directory);
    static RestorePreview preview(const RestoreOptions &options);
    static RestoreSummary execute(const RestoreOptions &options, ProgressReporter &reporter = NullProgressReporter::instance());
};

}  // namespace seabass::application
