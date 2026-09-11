#include "infrastructure/local/browsed_backup_root.hpp"
#include "application/use_cases/backup_stick.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <set>
#include <system_error>
#include <unordered_map>

#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_recovery.hpp"
#include "infrastructure/stick_backup/archive_stats.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/stick_backup/stat_diff.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;
using namespace infrastructure::stick_backup;
namespace engine = infrastructure::engine;

namespace
{

// Byte counts in a message a person reads: "26.0 GiB", not 27917287424.
std::string humanBytes(std::uint64_t bytes)
{
    static constexpr const char *Units[] = {"bytes", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) {
        out << bytes << ' ' << Units[0];
    } else {
        out << std::fixed << std::setprecision(1) << value << ' ' << Units[unit];
    }
    return out.str();
}


std::int64_t nowUnix()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string entryNameFor(const TreeEntry &entry)
{
    return entry.isDirectory ? entry.relativePath + "/" : entry.relativePath;
}

class FileSource : public EntrySource
{
public:
    explicit FileSource(const fs::path &path) : m_in(path, std::ios::binary) {}
    bool ok() const { return static_cast<bool>(m_in); }
    std::size_t read(std::span<std::byte> out) override
    {
        m_in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<std::size_t>(m_in.gcount());
    }

private:
    std::ifstream m_in;
};

// The archive and its journal, opened and recovered, plus what the
// previous generation says. Owns the files: Zip64Reader keeps a pointer
// into `archive`.
struct OpenedArchive
{
    std::unique_ptr<PosixArchiveFile> archive;
    std::unique_ptr<PosixArchiveFile> journal;
    bool existedBefore = false;
    std::optional<Zip64Reader> reader;
    std::optional<BackupManifest> manifest;
    std::unordered_map<std::string, const ManifestRow *> rowsByPath;
    std::unordered_map<std::string, const CentralEntry *> entriesByName;
    std::string error;

    // `recover`: apply a leftover journal, truncating the archive back to
    // its pre-update length, before reading. Only execute() does that.
    //
    // preview() and verify() pass false, because recovery is a write and
    // a running backup has a live journal by design -- a preview that
    // recovered would truncate an archive out from under the run that is
    // still appending to it. The same mistake on the restore side cost a
    // real 12 GB backup; see restore_stick_backup.cpp's Opened::open()
    // and tests/backup_archive_concurrent_reader_test.cpp. A preview
    // reading an unrecovered archive is at worst slightly stale, and the
    // run that follows recovers properly anyway.
    bool open(const BackupStickOptions &options, bool recover)
    {
        std::error_code ec;
        fs::create_directories(options.archivePath.parent_path(), ec);
        existedBefore = fs::exists(options.archivePath, ec) && fs::file_size(options.archivePath, ec) > 0;
        try {
            // Still opened read-write even when not recovering: a first
            // backup's preview runs before the archive exists, and
            // O_RDONLY will not create it. Nothing is written unless the
            // caller recovers or runs an update -- it is recovery, not
            // the open, that truncates.
            archive = std::make_unique<PosixArchiveFile>(options.archivePath, PosixArchiveFile::OpenMode::ReadWrite);
            if (recover) {
                journal = std::make_unique<PosixArchiveFile>(BackupStick::journalPathFor(options.archivePath),
                                                             PosixArchiveFile::OpenMode::ReadWrite);
                recoverOnOpen(*archive, *journal);
            }
        } catch (const std::exception &e) {
            error = std::string("could not open the backup archive: ") + e.what();
            return false;
        }
        if (archive->size() == 0) {
            existedBefore = false;
            return true;
        }
        std::string openError;
        reader = Zip64Reader::tryOpen(*archive, &openError);
        if (!reader) {
            error = "the existing backup archive is unreadable: " + openError;
            return false;
        }
        std::optional<std::size_t> manifestIndex = reader->findEntry(ManifestEntryName);
        if (!manifestIndex) {
            error = "the existing backup archive has no manifest";
            return false;
        }
        std::string manifestError;
        manifest = BackupManifest::parse(reader->readEntryToString(*manifestIndex), &manifestError);
        if (!manifest) {
            error = "the existing backup archive's manifest is damaged: " + manifestError;
            return false;
        }
        for (const ManifestRow &row : manifest->rows) {
            rowsByPath.emplace(row.path, &row);
        }
        for (const CentralEntry &entry : reader->entries()) {
            entriesByName.emplace(entry.name, &entry);
        }
        return true;
    }
};

// The plan for one run: the stat diff, adjusted for SQLite database sets,
// which are captured or carried as a unit.
struct RunPlan
{
    TreeWalk walk;
    DiffResult diff;
    std::vector<const TreeEntry *> filesToRead;      // added + changed, minus DB-set members, sorted
    std::vector<const TreeEntry *> directoriesToAdd;
    std::vector<const TreeEntry *> carried;          // unchanged, including carried DB sets
    std::vector<std::string> dbSetsToCapture;        // main-file relative paths
    std::set<std::string> dbSetMemberPaths;          // every member of every set to capture
    std::uint64_t bytesToRead = 0;
    bool cancelled = false;
};

RunPlan planRun(const BackupStickOptions &options, const OpenedArchive &opened)
{
    RunPlan plan;
    plan.walk = walkStickTree(options.stickRoot, options.cancel);
    if (plan.walk.cancelled) {
        plan.cancelled = true;
        return plan;
    }
    plan.diff = diffTreeAgainstManifest(plan.walk, opened.manifest ? &*opened.manifest : nullptr);

    // Classify every SQLite database set: carried only when the header
    // fingerprint and every member's stat are unchanged, otherwise the
    // whole set is re-read together.
    std::set<std::string> unchangedPaths;
    for (const TreeEntry *entry : plan.diff.unchanged) {
        unchangedPaths.insert(entry->relativePath);
    }
    std::set<std::string> removedPaths(plan.diff.removed.begin(), plan.diff.removed.end());
    for (const TreeEntry &entry : plan.walk.entries) {
        if (entry.isDirectory || !engine::isSqliteDatabaseFile(entry.relativePath)) {
            continue;
        }
        fs::path mainDb = options.stickRoot / pathFromUtf8(entry.relativePath);
        std::optional<DbSetFingerprint> fingerprint = fingerprintDbSet(mainDb);
        if (!fingerprint) {
            continue;  // not SQLite after all: an ordinary file
        }
        std::vector<std::string> members;
        for (const fs::path &member : dbSetMembers(mainDb)) {
            members.push_back(entry.relativePath + member.filename().string().substr(mainDb.filename().string().size()));
        }
        bool carried = true;
        auto row = opened.rowsByPath.find(entry.relativePath);
        if (row == opened.rowsByPath.end() || row->second->extra != fingerprint->toHex()) {
            carried = false;
        }
        for (const std::string &member : members) {
            if (unchangedPaths.count(member) == 0) {
                carried = false;
            }
        }
        for (const char *suffix : {"-wal", "-journal"}) {
            if (removedPaths.count(entry.relativePath + suffix) != 0) {
                carried = false;
            }
        }
        if (!carried) {
            plan.dbSetsToCapture.push_back(entry.relativePath);
            plan.dbSetMemberPaths.insert(members.begin(), members.end());
        }
    }

    for (const TreeEntry *entry : plan.diff.unchanged) {
        if (plan.dbSetMemberPaths.count(entry->relativePath) == 0) {
            plan.carried.push_back(entry);
        }
    }
    for (const std::vector<const TreeEntry *> *group : {&plan.diff.added, &plan.diff.changed}) {
        for (const TreeEntry *entry : *group) {
            if (entry->isDirectory) {
                plan.directoriesToAdd.push_back(entry);
            } else if (plan.dbSetMemberPaths.count(entry->relativePath) == 0) {
                plan.filesToRead.push_back(entry);
            }
        }
    }
    auto byPath = [](const TreeEntry *a, const TreeEntry *b) { return a->relativePath < b->relativePath; };
    std::sort(plan.filesToRead.begin(), plan.filesToRead.end(), byPath);
    std::sort(plan.directoriesToAdd.begin(), plan.directoriesToAdd.end(), byPath);

    for (const TreeEntry *entry : plan.filesToRead) {
        plan.bytesToRead += entry->size;
    }
    for (const TreeEntry &entry : plan.walk.entries) {
        if (plan.dbSetMemberPaths.count(entry.relativePath) != 0) {
            plan.bytesToRead += entry.size;
        }
    }
    return plan;
}

std::uint64_t freeBytesAt(const fs::path &archivePath)
{
    std::error_code ec;
    fs::space_info info = fs::space(archivePath.parent_path(), ec);
    return ec ? 0 : info.available;
}

ManifestRow rowForEntry(const TreeEntry &entry, const ArchiveUpdater::AppendedEntry *appended)
{
    ManifestRow row;
    row.kind = entry.isDirectory ? ManifestRow::Kind::Directory : ManifestRow::Kind::File;
    row.path = entry.relativePath;
    row.mtimeUnix = entry.mtimeUnix;
    if (appended != nullptr) {
        row.size = appended->entry.size;
        row.sha256 = appended->sha256;
        row.crc32 = appended->entry.crc32;
    }
    return row;
}

}  // namespace

// ---- PendingBackup ----

struct PendingBackup::Impl
{
    BackupStickOptions options;
    // Held from just before OpenedArchive::open() until keep()/discard()
    // actually runs -- which, for a cancelled-and-pending backup, can be
    // much later than execute() returning. Keeps a restore, a clone or
    // another backup from opening this same archive for writing while
    // this one is still in flight or awaiting a decision.
    std::unique_ptr<infrastructure::backup::StickWriteLock> lock;
    OpenedArchive opened;
    std::unique_ptr<ArchiveUpdater> updater;
    BackupManifest manifest;  // rows for carried + completed entries
    BackupStickOutcome partial;  // counts so far
    bool firstBackup = false;
    bool decided = false;
};

PendingBackup::PendingBackup(std::unique_ptr<Impl> impl) : m_impl(std::move(impl))
{
}

PendingBackup::~PendingBackup() = default;

bool PendingBackup::decided() const
{
    return m_impl->decided;
}

BackupStickOutcome PendingBackup::keep()
{
    BackupStickOutcome outcome = std::move(m_impl->partial);
    m_impl->partial = BackupStickOutcome{};
    if (m_impl->decided) {
        outcome.status = BackupOutcomeStatus::Failed;
        outcome.message = "already decided";
        return outcome;
    }
    m_impl->decided = true;
    m_impl->manifest.status = BackupStatus::PartialCancelled;
    m_impl->manifest.createdAtUnix = nowUnix();
    try {
        m_impl->updater->commit(m_impl->manifest);
    } catch (const std::exception &e) {
        // Written, or explicitly left for recovery -- either way this
        // archive is done being touched by this session. Release now:
        // a caller keeping the PendingBackup around just to check
        // decided() must not still be blocking a resume.
        m_impl->lock.reset();
        outcome.status = BackupOutcomeStatus::Failed;
        outcome.message = e.what();
        return outcome;
    }
    m_impl->lock.reset();
    outcome.status = BackupOutcomeStatus::KeptPartial;
    outcome.archiveBytes = m_impl->opened.archive->size();
    if (std::optional<Zip64Reader> reader = Zip64Reader::tryOpen(*m_impl->opened.archive)) {
        outcome.deadBytes = deadSpace(*reader).deadBytes;
    }
    return outcome;
}

BackupStickOutcome PendingBackup::discard()
{
    BackupStickOutcome outcome;
    if (m_impl->decided) {
        outcome.message = "already decided";
        return outcome;
    }
    m_impl->decided = true;
    try {
        m_impl->updater->abort();
    } catch (const std::exception &e) {
        // See the matching comment in keep(): abort() has now run,
        // regardless of outcome, so this archive is done being touched
        // by this session.
        m_impl->lock.reset();
        outcome.message = e.what();
        return outcome;
    }
    m_impl->lock.reset();
    outcome.status = BackupOutcomeStatus::Discarded;
    outcome.archiveBytes = m_impl->opened.archive->size();
    if (m_impl->firstBackup) {
        m_impl->updater.reset();
        m_impl->opened.reader.reset();
        m_impl->opened.archive.reset();
        m_impl->opened.journal.reset();
        std::error_code ec;
        fs::remove(m_impl->options.archivePath, ec);
        fs::remove(BackupStick::journalPathFor(m_impl->options.archivePath), ec);
        outcome.archiveBytes = 0;
    }
    return outcome;
}

// ---- BackupStick ----

fs::path BackupStick::journalPathFor(const fs::path &archivePath)
{
    return journal::journalPathFor(archivePath);
}

VerifyOutcome BackupStick::verify(const fs::path &archivePath, CancellationToken cancel,
                                  const std::function<void(std::uint64_t, std::uint64_t)> &onProgress)
{
    VerifyOutcome outcome;
    BackupStickOptions options;
    options.archivePath = archivePath;
    OpenedArchive opened;
    if (!opened.open(options, false)) {
        outcome.error = opened.error;
        return outcome;
    }
    if (!opened.reader) {
        outcome.error = "there is no backup to verify";
        return outcome;
    }
    std::string structural;
    if (!verifyArchiveTail(*opened.archive, 0, &structural)) {
        outcome.error = structural;
        return outcome;
    }
    outcome.status = opened.manifest->status;
    std::uint64_t total = 0;
    for (const CentralEntry &entry : opened.reader->entries()) {
        total += entry.size;
    }
    std::uint64_t done = 0;
    for (std::size_t i = 0; i < opened.reader->entries().size(); ++i) {
        const CentralEntry &entry = opened.reader->entries()[i];
        if (entry.isDirectory || entry.name == ManifestEntryName) {
            continue;
        }
        if (cancel.cancelled()) {
            outcome.error = "cancelled";
            return outcome;
        }
        auto row = opened.rowsByPath.find(entry.name);
        infrastructure::hashing::Sha256 hasher;
        opened.reader->readEntry(i, [&](std::span<const std::byte> piece) {
            hasher.update(piece);
            done += piece.size();
            if (onProgress) {
                onProgress(done, total);
            }
        });
        if (row == opened.rowsByPath.end() || hasher.finish() != row->second->sha256) {
            outcome.failures.push_back(entry.name);
        }
        ++outcome.entriesChecked;
        outcome.bytesChecked += entry.size;
    }
    outcome.ok = outcome.failures.empty();
    return outcome;
}

namespace
{

// A stick backup being browsed is a catalogs-only cache of an archive.
// Backing it up -- or cloning from it, which is a backup underneath --
// would diff that cache against the archive's manifest, call every
// analysis and audio file "removed", and rewrite the user's full backup
// down to the catalogs. Refused here, where the write happens, rather
// than at whichever button led here: the stick list withholds its own
// cards, but a *different* row's Clone card resolves the same archive by
// label, and there is always another button.
}  // namespace

BackupPreview BackupStick::preview(const BackupStickOptions &options, ProgressReporter &reporter)
{
    BackupPreview preview;
    if (infrastructure::local::isBrowsedBackupRoot(options.stickRoot)) {
        preview.error = infrastructure::local::browsedBackupRefusal(options.stickLabel);
        return preview;
    }
    OpenedArchive opened;
    if (!opened.open(options, false)) {
        preview.error = opened.error;
        return preview;
    }
    preview.archiveExists = opened.existedBefore;
    if (opened.manifest) {
        preview.previousStatus = opened.manifest->status;
        preview.previousCreatedAtUnix = opened.manifest->createdAtUnix;
        preview.previousIdentifier = opened.manifest->stickIdentifier;
        preview.previousLabel = opened.manifest->stickLabel;
        preview.identifierMismatch = !options.stickIdentifier.empty() && !opened.manifest->stickIdentifier.empty()
                                     && options.stickIdentifier != opened.manifest->stickIdentifier;
        preview.archiveBytes = opened.archive->size();
        preview.deadBytes = deadSpace(*opened.reader).deadBytes;
    }

    reporter.start("Scanning stick", 0);
    RunPlan plan = planRun(options, opened);
    reporter.finish();
    preview.entriesOnStick = plan.walk.entries.size();
    preview.stickBytes = plan.walk.totalFileBytes;
    preview.added = plan.diff.added.size();
    preview.changed = plan.diff.changed.size();
    preview.removed = plan.diff.removed.size();
    preview.unchanged = plan.diff.unchanged.size();
    preview.databaseChanged = !plan.dbSetsToCapture.empty();
    preview.bytesToRead = plan.bytesToRead;
    preview.uniformShiftSeconds = plan.diff.uniformShiftSeconds;
    preview.skipped = plan.walk.skipped;
    preview.freeBytesAtDestination = freeBytesAt(options.archivePath);
    preview.enoughFreeSpace = preview.freeBytesAtDestination >= plan.bytesToRead + options.freeSpaceMarginBytes;
    return preview;
}

BackupStickOutcome BackupStick::execute(const BackupStickOptions &options, ProgressReporter &reporter)
{
    BackupStickOutcome outcome;
    if (infrastructure::local::isBrowsedBackupRoot(options.stickRoot)) {
        outcome.message = infrastructure::local::browsedBackupRefusal(options.stickLabel);
        return outcome;
    }
    auto impl = std::make_unique<PendingBackup::Impl>();
    impl->options = options;
    try {
        impl->lock = std::make_unique<infrastructure::backup::StickWriteLock>(
            journal::lockPathFor(options.archivePath).string());
    } catch (const infrastructure::backup::StickBusyError &e) {
        outcome.message = e.what();
        return outcome;
    }
    OpenedArchive &opened = impl->opened;
    if (!opened.open(options, true)) {
        outcome.message = opened.error;
        return outcome;
    }
    impl->firstBackup = !opened.existedBefore;
    // The archive is chosen by label, and two sticks can share one. An
    // update from a different stick would diff the newcomer against the
    // archive and record every file of the original as removed, which is
    // the original's backup gone. Refuse; preview flags the same thing.
    if (opened.manifest && !options.stickIdentifier.empty() && !opened.manifest->stickIdentifier.empty()
        && options.stickIdentifier != opened.manifest->stickIdentifier) {
        outcome.message = "this backup belongs to a different stick (" + opened.manifest->stickLabel
                          + ", id " + opened.manifest->stickIdentifier + "); back this one up under another name";
        return outcome;
    }

    BackupProgress progress;
    auto report = [&](BackupProgress::Phase phase) {
        progress.phase = phase;
        if (options.onProgress) {
            options.onProgress(progress);
        }
    };
    report(BackupProgress::Phase::Scanning);
    reporter.start("Scanning stick", 0);
    RunPlan plan = planRun(options, opened);
    reporter.finish();
    if (plan.cancelled) {
        outcome.status = BackupOutcomeStatus::Cancelled;
        outcome.message = "cancelled while scanning the stick; nothing was written";
        return outcome;
    }
    outcome.warnings = plan.walk.skipped;
    // Deliberate skips (symlinks, excluded folders) are warnings the
    // walk explains; anything added after this line is a file the
    // backup meant to capture and could not.
    const std::size_t walkWarnings = outcome.warnings.size();
    outcome.added = plan.diff.added.size();
    outcome.changed = plan.diff.changed.size();
    outcome.removed = plan.diff.removed.size();
    outcome.carried = plan.carried.size();

    const bool previousComplete = opened.manifest && opened.manifest->status == BackupStatus::Complete;
    if (opened.existedBefore && previousComplete && plan.filesToRead.empty() && plan.directoriesToAdd.empty()
        && plan.dbSetsToCapture.empty() && plan.diff.removed.empty()) {
        outcome.status = BackupOutcomeStatus::NothingToDo;
        outcome.archiveBytes = opened.archive->size();
        outcome.deadBytes = deadSpace(*opened.reader).deadBytes;
        outcome.databaseCaptured = true;
        return outcome;
    }

    // Refused before a single byte is written, never half-way through:
    // a backup that runs for twenty minutes and then fails on space is
    // worse than one that never starts, and an archive abandoned part
    // way is exactly the "looks like a backup, isn't one" outcome this
    // whole feature exists to avoid. Sizes are spelled out the way the
    // UI shows them so the message says what to actually free up.
    std::uint64_t freeBytes = freeBytesAt(options.archivePath);
    const std::uint64_t needed = plan.bytesToRead + options.freeSpaceMarginBytes;
    if (freeBytes < needed) {
        outcome.message = "not enough free space to back this stick up: needs " + humanBytes(needed) + ", only "
                          + humanBytes(freeBytes) + " free where the backup goes. Free up "
                          + humanBytes(needed - freeBytes) + " and try again.";
        return outcome;
    }

    // Carried entries and their manifest rows come straight from the
    // previous generation.
    std::vector<CentralEntry> carriedEntries;
    BackupManifest &manifest = impl->manifest;
    manifest.stickIdentifier = options.stickIdentifier;
    manifest.stickLabel = options.stickLabel;
    manifest.libraryFingerprint = options.libraryFingerprint.empty() && opened.manifest
                                      ? opened.manifest->libraryFingerprint
                                      : options.libraryFingerprint;
    for (const TreeEntry *entry : plan.carried) {
        auto found = opened.entriesByName.find(entryNameFor(*entry));
        auto row = opened.rowsByPath.find(entry->relativePath);
        if (found == opened.entriesByName.end() || row == opened.rowsByPath.end()) {
            outcome.message = "internal error: carried entry missing from the archive: " + entry->relativePath;
            return outcome;
        }
        carriedEntries.push_back(*found->second);
        manifest.rows.push_back(*row->second);
    }
    std::uint64_t priorEocd = opened.reader ? opened.reader->layout().endOfCentralDirectoryOffset : 0;
    impl->updater = std::make_unique<ArchiveUpdater>(*opened.archive, *opened.journal, std::move(carriedEntries), priorEocd,
                                                     options.chunkSize);
    ArchiveUpdater &updater = *impl->updater;
    try {
        updater.begin();
    } catch (const std::exception &e) {
        outcome.message = e.what();
        return outcome;
    }

    // Process probe, rate limited.
    auto lastProbe = std::chrono::steady_clock::now() - options.probeInterval;
    bool conflict = false;
    auto probe = [&]() {
        if (conflict || !options.conflictingProcessProbe) {
            return conflict;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - lastProbe >= options.probeInterval) {
            lastProbe = now;
            conflict = options.conflictingProcessProbe();
        }
        return conflict;
    };

    // BackupStickOutcome owns the PendingBackup, so it is move-only; the
    // counts are copied field by field into the pending handle and the
    // returned outcome.
    auto copyCounts = [](const BackupStickOutcome &from, BackupStickOutcome &to) {
        to.added = from.added;
        to.changed = from.changed;
        to.removed = from.removed;
        to.carried = from.carried;
        to.bytesRead = from.bytesRead;
        to.warnings = from.warnings;
    };
    auto makePending = [&](const BackupStickOutcome &soFar) {
        copyCounts(soFar, impl->partial);
        impl->partial.status = BackupOutcomeStatus::Cancelled;
        BackupStickOutcome result;
        copyCounts(soFar, result);
        result.status = BackupOutcomeStatus::Cancelled;
        result.message = "cancelled; the files copied so far can be kept for a later run or discarded";
        result.pending = std::unique_ptr<PendingBackup>(new PendingBackup(std::move(impl)));
        return result;
    };

    progress.filesTotal = plan.filesToRead.size() + plan.dbSetMemberPaths.size();
    progress.bytesTotal = plan.bytesToRead;
    report(BackupProgress::Phase::Reading);
    reporter.start("Reading stick", plan.filesToRead.size());

    for (const TreeEntry *dir : plan.directoriesToAdd) {
        updater.appendDirectory(dir->relativePath, dir->mtimeUnix);
        manifest.rows.push_back(rowForEntry(*dir, nullptr));
    }

    std::size_t filesDone = 0;
    for (const TreeEntry *file : plan.filesToRead) {
        if (options.cancel.cancelled()) {
            return makePending(outcome);
        }
        if (probe()) {
            break;
        }
        progress.currentFile = file->relativePath;
        fs::path fullPath = options.stickRoot / pathFromUtf8(file->relativePath);
        FileSource source(fullPath);
        if (!source.ok()) {
            outcome.warnings.push_back(file->relativePath + ": could not open, skipped");
            continue;
        }
        std::uint64_t bytesBefore = progress.bytesDone;
        std::optional<ArchiveUpdater::AppendedEntry> appended;
        try {
            appended = updater.appendFile(file->relativePath, file->mtimeUnix, source, options.cancel,
                                          [&](std::uint64_t bytes) {
                                              progress.bytesDone = bytesBefore + bytes;
                                              report(BackupProgress::Phase::Reading);
                                          });
        } catch (const std::exception &e) {
            outcome.message = std::string("write failed: ") + e.what();
            return outcome;  // journal stays; next open rolls back
        }
        if (!appended) {
            outcome.bytesRead = progress.bytesDone;
            return makePending(outcome);
        }
        // Re-stat: a file that changed underneath the read is not the file
        // the manifest would describe. Leave it for the next run.
        std::error_code ec;
        std::uint64_t sizeNow = fs::file_size(fullPath, ec);
        std::int64_t mtimeNow = ec ? 0 : toUnixSeconds(fs::last_write_time(fullPath, ec));
        if (ec || sizeNow != file->size || mtimeNow != file->mtimeUnix || appended->entry.size != file->size) {
            updater.forgetLastEntries(1);
            outcome.warnings.push_back(file->relativePath + ": changed while it was being read, left for the next run");
        } else {
            manifest.rows.push_back(rowForEntry(*file, &*appended));
        }
        progress.bytesDone = bytesBefore + appended->entry.size;
        progress.filesDone = ++filesDone;
        reporter.tick(filesDone);
        report(BackupProgress::Phase::Reading);
    }
    reporter.finish();
    outcome.bytesRead = progress.bytesDone;

    // Database sets last, each only if nothing conflicting is running.
    BackupStatus status = BackupStatus::Complete;
    outcome.databaseCaptured = true;
    if (!plan.dbSetsToCapture.empty()) {
        report(BackupProgress::Phase::Database);
        reporter.start("Capturing database", plan.dbSetsToCapture.size());
        std::size_t setsDone = 0;
        for (const std::string &mainDb : plan.dbSetsToCapture) {
            if (options.cancel.cancelled()) {
                return makePending(outcome);
            }
            if (probe()) {
                break;
            }
            progress.currentFile = mainDb;
            std::uint64_t bytesBefore = progress.bytesDone;
            DbSetCapture capture;
            try {
                capture = captureDbSet(options.stickRoot, mainDb, updater, 3, [&](std::uint64_t bytes) {
                    progress.bytesDone = bytesBefore + bytes;
                    report(BackupProgress::Phase::Database);
                });
            } catch (const std::exception &e) {
                outcome.message = std::string("write failed: ") + e.what();
                return outcome;
            }
            outcome.bytesRead += capture.bytesRead;
            progress.bytesDone = bytesBefore + capture.bytesRead;
            if (capture.status == DbSetCapture::Status::Captured) {
                for (std::size_t i = 0; i < capture.entries.size(); ++i) {
                    ManifestRow row;
                    row.kind = ManifestRow::Kind::File;
                    row.path = capture.memberRelativePaths[i];
                    row.mtimeUnix = capture.memberMtimes[i];
                    row.size = capture.entries[i].entry.size;
                    row.sha256 = capture.entries[i].sha256;
                    row.crc32 = capture.entries[i].entry.crc32;
                    if (i == 0) {
                        row.extra = capture.fingerprint.toHex();
                    }
                    manifest.rows.push_back(row);
                }
            } else {
                outcome.databaseCaptured = false;
                outcome.warnings.push_back(mainDb + ": not backed up -- " + capture.detail);
                if (capture.status == DbSetCapture::Status::TooLarge) {
                    if (status == BackupStatus::Complete) {
                        status = BackupStatus::PartialDbTooLarge;
                    }
                    outcome.status = BackupOutcomeStatus::DbTooLarge;
                } else {
                    status = BackupStatus::PartialConflict;
                    outcome.status = BackupOutcomeStatus::DbUnstable;
                }
            }
            progress.filesDone += capture.entries.size();
            reporter.tick(++setsDone);
        }
        reporter.finish();
    }
    if (conflict) {
        status = BackupStatus::PartialConflict;
        outcome.databaseCaptured = plan.dbSetsToCapture.empty() ? outcome.databaseCaptured : false;
        outcome.status = BackupOutcomeStatus::ConflictAborted;
        outcome.message = "Engine DJ or rekordbox started during the backup; what was copied is kept, the database was not read";
    }

    // A file that could not be read, or changed while it was, is not in
    // this backup, and if it was in the previous one it was not carried
    // either. A record with holes must not present itself as complete:
    // the restore page would call it VERIFIED and the advisor would
    // stop asking for a new one.
    if (status == BackupStatus::Complete && outcome.warnings.size() > walkWarnings) {
        status = BackupStatus::PartialSkipped;
    }
    manifest.status = status;
    manifest.createdAtUnix = nowUnix();
    report(BackupProgress::Phase::Writing);
    reporter.start("Writing index", 0);
    try {
        report(BackupProgress::Phase::Verifying);
        updater.commit(manifest);
    } catch (const std::exception &e) {
        reporter.finish();
        outcome.status = BackupOutcomeStatus::Failed;
        outcome.message = std::string("the backup did not verify after writing and was left for recovery: ") + e.what();
        return outcome;
    }
    reporter.finish();

    if (outcome.status == BackupOutcomeStatus::Failed) {  // nothing above set a partial status
        outcome.status = BackupOutcomeStatus::Complete;
    }
    outcome.archiveBytes = opened.archive->size();
    if (std::optional<Zip64Reader> reader = Zip64Reader::tryOpen(*opened.archive)) {
        outcome.deadBytes = deadSpace(*reader).deadBytes;
    }
    return outcome;
}

}  // namespace seabass::application
