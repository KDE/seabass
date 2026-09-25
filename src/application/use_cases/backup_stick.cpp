// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

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
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_recovery.hpp"
#include "infrastructure/stick_backup/archive_stats.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/file_entry_source.hpp"
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

// One line of the archive's own history, appended as it commits. Called
// from both commit sites -- a full run and a kept partial one -- because
// a cancelled backup that someone chose to keep is a generation too, and
// leaving it out would make the log disagree with the archive.
void recordGeneration(BackupManifest &manifest, const BackupStickOutcome &outcome)
{
    GenerationRow generation;
    generation.createdAtUnix = manifest.createdAtUnix;
    generation.status = manifest.status;
    generation.added = outcome.added;
    generation.changed = outcome.changed;
    generation.removed = outcome.removed;
    generation.bytesRead = outcome.bytesRead;
    generation.userName = manifest.userName;
    manifest.generations.push_back(std::move(generation));
    if (manifest.generations.size() > MaxGenerationRows) {
        manifest.generations.erase(manifest.generations.begin(),
                                    manifest.generations.begin()
                                        + static_cast<std::ptrdiff_t>(manifest.generations.size() - MaxGenerationRows));
    }
}

std::string entryNameFor(const TreeEntry &entry)
{
    return entry.isDirectory ? entry.relativePath + "/" : entry.relativePath;
}


// Stops handing out bytes at `limit`, the way a stick with a bad
// cluster stops. Wraps rather than replaces the real source, so
// everything before the limit is the file's own bytes read the usual
// way -- a stand-in that generated its own data would prove the archive
// stores what the stand-in made up.
class LimitedSource : public EntrySource
{
public:
    LimitedSource(EntrySource &inner, std::optional<std::uint64_t> limit) : m_inner(inner), m_limit(limit) {}
    std::size_t read(std::span<std::byte> out) override
    {
        if (!m_limit) {
            return m_inner.read(out);
        }
        if (m_done >= *m_limit) {
            return 0;
        }
        const std::uint64_t room = *m_limit - m_done;
        if (out.size() > room) {
            out = out.first(static_cast<std::size_t>(room));
        }
        const std::size_t got = m_inner.read(out);
        m_done += got;
        return got;
    }

private:
    EntrySource &m_inner;
    std::optional<std::uint64_t> m_limit;
    std::uint64_t m_done = 0;
};

// The salvage log's text: every file this archive holds only part of,
// plus everything this run could not read at all, in plain words.
//
// Built from the MANIFEST rather than from this run's counters, so it
// describes the archive as it now is rather than as this run left it. A
// backup updated later by a healthy run still holds the entries an
// earlier salvage saved, and their rows are carried forward, so they
// belong in the log even though this run never touched them.
//
// The run's own warnings are appended because they say what is NOT in
// the archive at all -- a file whose open failed leaves no row behind to
// find later.
std::string salvageLogText(const BackupManifest &manifest, const std::vector<std::string> &warnings)
{
    std::string out = "This backup was taken off a stick that could not be read in full.\n"
                      "Some of the files below are here only in part, and some are not here at all.\n\n";
    std::size_t partial = 0;
    for (const ManifestRow &row : manifest.rows) {
        if (row.salvagedFromSize == 0) {
            continue;
        }
        ++partial;
        out += "PARTIAL  " + row.path + "\n";
        out += "         " + humanBytes(row.size) + " of " + humanBytes(row.salvagedFromSize) + " was readable\n";
    }
    if (partial == 0) {
        out += "No file in this backup is truncated.\n";
    }
    if (!warnings.empty()) {
        out += "\nWhat this run could not do:\n";
        for (const std::string &warning : warnings) {
            out += "  " + warning + "\n";
        }
    }
    out += "\nThe authoritative record is SEABASS-MANIFEST.tsv: each row's eighth\n"
           "field is the size the file had on the stick, where the archive holds\n"
           "less than that. This file is the readable companion to it.\n";
    return out;
}

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
        // A first guess only, and one that cannot be trusted: both calls
        // take an error_code and neither answer distinguishes "not there"
        // from "I could not look". It decides whether to open at all; the
        // authoritative answer is taken from the opened file below.
        existedBefore = fs::exists(options.archivePath, ec) && fs::file_size(options.archivePath, ec) > 0;
        if (!recover && !existedBefore) {
            // Nothing to read yet (a first backup's preview, or verify of
            // an archive that is not there). Opening read-write here used
            // to create the file, and every preview of a stick that was
            // never backed up left an empty .zip in the backup folder.
            return true;
        }
        try {
            if (recover) {
                fs::create_directories(options.archivePath.parent_path(), ec);
                archive = std::make_unique<PosixArchiveFile>(options.archivePath, PosixArchiveFile::OpenMode::ReadWrite);
                journal = std::make_unique<PosixArchiveFile>(BackupStick::journalPathFor(options.archivePath),
                                                             PosixArchiveFile::OpenMode::ReadWrite);
                recoverOnOpen(*archive, *journal);
            } else {
                // Reading only, so read-only: a backup that is write
                // protected (a reference copy, a read-only share) can
                // still be previewed against and verified.
                archive = std::make_unique<PosixArchiveFile>(options.archivePath, PosixArchiveFile::OpenMode::ReadOnly);
            }
        } catch (const std::exception &e) {
            error = std::string("could not open the backup archive: ") + e.what();
            return false;
        }
        // Set from the open file, not merely cleared by it. This line
        // only ever cleared the flag, so a transient failure of the
        // exists() above left existedBefore false for an archive that is
        // plainly there -- and firstBackup is !existedBefore, and
        // discard() on a first backup fs::remove()s the archive. That is
        // a pre-existing, possibly multi-gigabyte backup deleted by a
        // run that only meant to roll itself back.
        existedBefore = archive->size() > 0;
        if (!existedBefore) {
            return true;
        }
        // All three of these refusals leave the person stuck unless the
        // way out is named. Refusing is right -- an archive that cannot be
        // read is one that cannot be safely appended to, and appending
        // anyway would destroy whatever is still in there -- but "it is
        // broken" on its own reads as "you can never back this stick up
        // again". Deleting the archive from Manage Backups is the answer
        // to each, and that page handles unreadable ones specifically.
        static constexpr const char *WayOut =
            ". Delete it from Manage Backups and back up again to start a fresh archive; "
            "this backup's contents cannot be recovered.";

        std::string openError;
        reader = Zip64Reader::tryOpen(*archive, &openError);
        if (!reader) {
            error = "the existing backup archive is unreadable: " + openError + WayOut;
            return false;
        }
        std::optional<std::size_t> manifestIndex = reader->findEntry(ManifestEntryName);
        if (!manifestIndex) {
            error = std::string("the existing backup archive has no manifest") + WayOut;
            return false;
        }
        std::string manifestError;
        manifest = BackupManifest::parse(reader->readEntryToString(*manifestIndex), &manifestError);
        if (!manifest) {
            error = "the existing backup archive's manifest is damaged: " + manifestError + WayOut;
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
        fs::path mainDb = options.stickRoot / seabass::pathFromUtf8(entry.relativePath);
        std::optional<DbSetFingerprint> fingerprint = fingerprintDbSet(mainDb);
        if (!fingerprint) {
            continue;  // not SQLite after all: an ordinary file
        }
        std::vector<std::string> members;
        for (const fs::path &member : dbSetMembers(mainDb)) {
            members.push_back(entry.relativePath + pathToGenericUtf8(member.filename()).substr(pathToGenericUtf8(mainDb.filename()).size()));
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
    // The backup folder need not exist yet: the first backup creates it,
    // and a preview no longer does. Ask the nearest folder that exists --
    // the volume the backup will land on.
    std::error_code ec;
    fs::path folder = archivePath.parent_path();
    while (!folder.empty() && !fs::exists(folder, ec)) {
        const fs::path up = folder.parent_path();
        if (up == folder) {
            break;
        }
        folder = up;
    }
    fs::space_info info = fs::space(folder, ec);
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
    // `outcome`, not m_impl->partial: partial was moved from and cleared
    // four lines up, so logging it would record a generation of all
    // zeroes for a run that copied real files.
    recordGeneration(m_impl->manifest, outcome);
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
    outcome.status = BackupOutcomeStatus::Discarded;
    outcome.archiveBytes = m_impl->opened.archive->size();
    if (m_impl->firstBackup) {
        // Removed while the lock is still held, so nothing can open the
        // archive between its removal and the release -- and then the lock
        // file too, which would otherwise sit beside a backup that never
        // came to exist.
        m_impl->updater.reset();
        m_impl->opened.reader.reset();
        m_impl->opened.archive.reset();
        m_impl->opened.journal.reset();
        std::error_code ec;
        fs::remove(m_impl->options.archivePath, ec);
        fs::remove(BackupStick::journalPathFor(m_impl->options.archivePath), ec);
        outcome.archiveBytes = 0;
        if (m_impl->lock) {
            m_impl->lock->releaseAndRemoveFile();
        }
    }
    m_impl->lock.reset();
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
        if (entry.isDirectory || isArchiveMetadataEntry(entry.name)) {
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
        preview.previousUserName = opened.manifest->userName;
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
            journal::lockPathFor(options.archivePath));
    } catch (const infrastructure::backup::StickBusyError &e) {
        outcome.message = e.what();
        return outcome;
    } catch (const std::exception &e) {
        // Not "somebody else holds it" but "it could not be created":
        // a destination folder nothing can write to, a lock file
        // another account owns. Unlike a restore, which may read from a
        // folder it cannot write and so runs unlocked there, a backup
        // has to write the archive -- so this is the end of it, and it
        // has to be a sentence rather than an exception thrown out of
        // the QtConcurrent task the GUI runs this in.
        outcome.message = std::string("could not lock the backup: ") + e.what();
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
    // Starts as whatever the previous generation recorded. A run that
    // completes replaces it further down, once the files are captured; a
    // run that is cancelled keeps this, because a partial archive does
    // not hold the library its header would otherwise claim.
    manifest.libraryFingerprint = opened.manifest ? opened.manifest->libraryFingerprint : std::string();
    // Sticky unless this run says otherwise: updating a backup must not
    // silently drop the name it was given when it was created, and every
    // caller that does not care about names leaves this unset. An empty
    // value that was actually supplied still clears it, which is why this
    // is an optional and not a string.
    manifest.userName = options.userName ? *options.userName
                                          : (opened.manifest ? opened.manifest->userName : std::string());
    // The archive's own history, carried forward and appended to at
    // commit. Without this line every update would start the log again.
    manifest.generations = opened.manifest ? opened.manifest->generations : std::vector<GenerationRow>{};
    // Sticky across generations: an update carries entries captured in
    // the run that read the damaged stick, so the archive goes on holding
    // data of unknown quality even when the stick is healthy again. The
    // mark comes off by making a new backup, not by updating this one.
    manifest.sourceReadOnly = options.sourceReadOnly || (opened.manifest && opened.manifest->sourceReadOnly);
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
        fs::path fullPath = options.stickRoot / seabass::pathFromUtf8(file->relativePath);
        infrastructure::stick_backup::FileEntrySource source(fullPath);
        // A stick that stops giving bytes part-way through a file. The
        // hook is unset in every real run, so this is the plain source.
        std::optional<std::uint64_t> limit;
        if (options.readLimitForTesting) {
            limit = options.readLimitForTesting(file->relativePath);
        }
        LimitedSource limited(source, limit);
        EntrySource &bytes = limit ? static_cast<EntrySource &>(limited) : static_cast<EntrySource &>(source);
        if (!source.ok()) {
            outcome.warnings.push_back(file->relativePath + ": could not open, skipped");
            continue;
        }
        std::uint64_t bytesBefore = progress.bytesDone;
        std::optional<ArchiveUpdater::AppendedEntry> appended;
        try {
            appended = updater.appendFile(file->relativePath, file->mtimeUnix, bytes, options.cancel,
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
        // A stat that FAILED and a stat that came back different are
        // different findings, and lumping them together threw away the
        // bytes salvage exists to keep.
        //
        // On a read-only stick the file cannot have changed -- the
        // kernel has already refused writes to it, which is the same
        // argument the salvage branch below makes -- so a failing
        // file_size() there is the device being unwell, not the file
        // moving. And a failing stat is exactly what a damaged stick
        // hands back for the file whose read just stopped. Treating it
        // as "changed underneath" dropped the readable bytes, wrote no
        // salvagedFromSize row, put nothing in the salvage log, and told
        // the user it was "left for the next run" on a stick where there
        // is no next run.
        const bool statFailed = static_cast<bool>(ec);
        const bool valuesDiffer = !statFailed && (sizeNow != file->size || mtimeNow != file->mtimeUnix);
        const bool changedUnderneath = valuesDiffer || (statFailed && !options.sourceReadOnly);
        const bool shortRead = appended->entry.size != file->size;
        if (changedUnderneath) {
            updater.forgetLastEntries(1);
            outcome.warnings.push_back(file->relativePath + ": changed while it was being read, left for the next run");
        } else if (shortRead && options.sourceReadOnly) {
            // A salvage run keeps what it got. The file did not change --
            // it cannot, the kernel has already refused writes to this
            // stick -- so fewer bytes than the stick claims means the
            // read stopped, and those bytes are the only copy anybody is
            // going to get. Throwing them away to keep the archive tidy
            // is the wrong trade when what is at stake is somebody's own
            // work.
            //
            // The row says both numbers, so nothing downstream can
            // present four megabytes of a nine megabyte track as a whole
            // one. See ManifestRow::salvagedFromSize.
            ManifestRow row = rowForEntry(*file, &*appended);
            row.salvagedFromSize = file->size;
            manifest.rows.push_back(row);
            outcome.salvaged.push_back({file->relativePath, appended->entry.size, file->size,
                                        source.readFailed() ? "the stick refused to read past this point"
                                                            : "the file ended sooner than the stick said it would"});
            outcome.warnings.push_back(file->relativePath + ": only " + humanBytes(appended->entry.size) + " of "
                                        + humanBytes(file->size) + " could be read");
        } else if (shortRead) {
            // A healthy stick that reads short is a real fault, and
            // stays one. Named for what it is rather than as a change:
            // nothing about this file moved.
            updater.forgetLastEntries(1);
            outcome.warnings.push_back(file->relativePath + ": could not be read in full, left for the next run");
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
                capture = captureDbSet(
                    options.stickRoot, mainDb, updater, 3,
                    [&](std::uint64_t bytes) {
                        progress.bytesDone = bytesBefore + bytes;
                        report(BackupProgress::Phase::Database);
                    },
                    options.sourceReadOnly, options.readLimitForTesting);
            } catch (const std::exception &e) {
                outcome.message = std::string("write failed: ") + e.what();
                return outcome;
            }
            outcome.bytesRead += capture.bytesRead;
            progress.bytesDone = bytesBefore + capture.bytesRead;
            // Salvaged is stored exactly like Captured -- the entries are
            // there and the rows describe them truthfully -- and then
            // said out loud, because what is in the archive may not be a
            // database that opens. Refusing it instead, which is what
            // every other status does, would throw away the one thing on
            // the stick worth most.
            if (capture.status == DbSetCapture::Status::Salvaged) {
                // A warning for the set, and a SalvagedFile only for a
                // member that really is truncated. A set whose members
                // are all here in full is not missing anything -- what
                // is wrong with it is that they may not agree -- and
                // listing it would have printed "12 MiB of 12 MiB" and
                // counted it as read only in part, which it is not.
                outcome.warnings.push_back(mainDb + ": " + capture.detail);
                for (std::size_t i = 0; i < capture.entries.size(); ++i) {
                    if (capture.memberSalvagedFromSizes[i] != 0) {
                        outcome.salvaged.push_back({capture.memberRelativePaths[i], capture.entries[i].entry.size,
                                                    capture.memberSalvagedFromSizes[i],
                                                    "the stick refused to read past this point"});
                    }
                }
            }
            if (capture.status == DbSetCapture::Status::Captured
                || capture.status == DbSetCapture::Status::Salvaged) {
                for (std::size_t i = 0; i < capture.entries.size(); ++i) {
                    ManifestRow row;
                    row.kind = ManifestRow::Kind::File;
                    row.path = capture.memberRelativePaths[i];
                    row.mtimeUnix = capture.memberMtimes[i];
                    row.size = capture.entries[i].entry.size;
                    row.sha256 = capture.entries[i].sha256;
                    row.crc32 = capture.entries[i].entry.crc32;
                    row.salvagedFromSize = capture.memberSalvagedFromSizes[i];
                    if (i == 0) {
                        row.extra = capture.fingerprint.toHex();
                    }
                    manifest.rows.push_back(row);
                }
            } else {
                outcome.databaseCaptured = false;
                outcome.warnings.push_back(mainDb + " not backed up: " + capture.detail);
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
    // Read here, with every file already captured, and only for a backup
    // that holds the whole library. Taken any earlier it would describe
    // the catalogs as some caller last saw them rather than as this
    // archive stores them, which is the gap that let a header claim 143
    // tracks over catalogs holding 156. A partial keeps the previous
    // generation's, set when the manifest was built: a half-copied stick
    // has no library identity of its own to record.
    //
    // Failure is not fatal and not silent. The fingerprint is an
    // advisory number -- it decides what the advisor says, never what is
    // written -- so a backup that copied every byte must not be thrown
    // away because a catalog would not parse. It leaves the previous
    // value and says so in the outcome.
    if (status == BackupStatus::Complete && options.readLibraryFingerprint) {
        try {
            if (const std::string fresh = options.readLibraryFingerprint(); !fresh.empty()) {
                manifest.libraryFingerprint = fresh;
            } else {
                outcome.warnings.push_back(
                    "the library's catalogs could not be read after the copy, so this backup keeps the previous "
                    "fingerprint and the stick list may not recognise it");
            }
        } catch (const std::exception &e) {
            outcome.warnings.push_back(std::string("the library fingerprint could not be taken after the copy (")
                                       + e.what() + "), so this backup keeps the previous one");
        }
    }
    manifest.createdAtUnix = nowUnix();
    recordGeneration(manifest, outcome);
    // A salvage backup carries its own account of what is missing, so it
    // can be read on a machine that has never heard of Seabass -- which
    // is the machine somebody reaches for when a stick has died.
    //
    // Written whenever the archive holds a truncated file or this run
    // could not read something, and rewritten every commit rather than
    // carried, so it can never describe a state the archive has left.
    // The old copy becomes dead space, which compaction reclaims; it is
    // a few hundred bytes.
    // Only when there is really something to say. A backup of a healthy
    // stick that skipped a symlink has a warning and has lost nothing,
    // and a file headed "some of these are here only in part" inside an
    // archive where none of them are is worse than no file at all: it is
    // there when somebody is frightened, and it says the wrong thing.
    const bool holdsATruncatedFile = std::any_of(manifest.rows.begin(), manifest.rows.end(),
                                                  [](const ManifestRow &row) { return row.salvagedFromSize != 0; });
    const bool anythingToSalvageLog = holdsATruncatedFile || (options.sourceReadOnly && !outcome.warnings.empty());
    if (anythingToSalvageLog) {
        const std::string log = salvageLogText(manifest, outcome.warnings);
        updater.appendFromMemory(std::string(SalvageLogEntryName), manifest.createdAtUnix,
                                 std::as_bytes(std::span<const char>(log.data(), log.size())));
    }
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
