// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "stick_backup_controller.hpp"
#include "gui/sleep_inhibitor.hpp"

#include "domain/library_fingerprint.hpp"
#include "gui/library_fingerprint_reader.hpp"
#include "application/find_stick_archive.hpp"
#include "gui/stick_backup_paths.hpp"
#include "gui/future_result.hpp"

#include <QDateTime>
#include <QDesktopServices>

#include "gui/backup_changelog_text.hpp"
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <filesystem>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/write_guard.hpp"
#include "infrastructure/local/stick_performance_history.hpp"
#include "infrastructure/stick_backup/archive_compactor.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/media/filesystem_health.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"
#include "gui/qt_path.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using application::BackupOutcomeStatus;
using application::BackupPreview;
using application::BackupProgress;
using application::BackupStick;
using application::BackupStickOptions;
using application::BackupStickOutcome;
using application::CompactionOutcome;
using application::CompactStickBackup;
using application::CompactStickBackupOptions;
using application::VerifyOutcome;
using infrastructure::stick_backup::BackupStatus;

namespace
{

QString statusToString(BackupStatus status)
{
    return QString::fromUtf8(std::string(infrastructure::stick_backup::toString(status)).c_str());
}

QString phaseName(BackupProgress::Phase phase)
{
    switch (phase) {
    case BackupProgress::Phase::Scanning: return QStringLiteral("scanning");
    case BackupProgress::Phase::Reading: return QStringLiteral("reading");
    case BackupProgress::Phase::Database: return QStringLiteral("database");
    case BackupProgress::Phase::Writing: return QStringLiteral("writing");
    case BackupProgress::Phase::Verifying: return QStringLiteral("verifying");
    }
    return {};
}


}  // namespace


struct StickBackupController::PreviewResult
{
    BackupPreview preview;
    QString stickIdentifier;
    // Set when this stick's backup was found in the folder under a name
    // other than the one the page assumed. See findStickArchive().
    QString adoptedArchivePath;
    double readMbps = 0.0;  // last measured audio read speed for this stick, 0 = unknown
    QString blockedBy;
    bool stickReadOnly = false;
};

struct StickBackupController::RunResult
{
    QString activity;
    std::shared_ptr<BackupStickOutcome> backup;
    VerifyOutcome verify;
    CompactionOutcome compaction;
    QString refusal;
};

StickBackupController::StickBackupController(QObject *parent) : QObject(parent)
{
    connect(&m_previewWatcher, &QFutureWatcher<std::shared_ptr<PreviewResult>>::finished, this,
            &StickBackupController::onPreviewFinished);
    connect(&m_runWatcher, &QFutureWatcher<std::shared_ptr<RunResult>>::finished, this,
            &StickBackupController::onRunFinished);
}

StickBackupController::~StickBackupController()
{
    // A pending decision that never got made is left to journal recovery
    // (= discard) on the next open; nothing to do here but let it go.
    m_cancel.cancel();
    awaitQuietly(m_runWatcher);
    awaitQuietly(m_previewWatcher);
}

void StickBackupController::configure(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath,
                                      const QString &backupDirectory)
{
    QString anyPath = enginePath.isEmpty() ? rekordboxPath : enginePath;
    m_stickLabel = stickLabel;
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    m_stickRoot = pathToQString(pathFromQString(anyPath).parent_path());
    m_backupDirectory = backupDirectory;
    m_archiveAttempt = 1;
    m_nameCollidedWith.clear();
    m_archivePath = archivePathForLabel(backupDirectory, stickLabel, m_archiveAttempt);
    // The field starts out holding the stick's name rather than empty: it
    // is what the backup would be called anyway, and a filled field says
    // so where a placeholder only hinted at it. refresh() replaces it with
    // the stored name if this stick's backup already has one.
    m_backupName = backupNameFor(QString(), stickLabel);
    m_savedBackupName.clear();
    m_backupNameIsDefault = true;
    m_previewSettled = false;
    emit backupNameChanged();
    emit configuredChanged();
    refresh();
}

BackupStickOptions StickBackupController::baseOptions() const
{
    BackupStickOptions options;
    options.stickRoot = pathFromQString(m_stickRoot);
    options.archivePath = pathFromQString(m_archivePath);
    options.stickIdentifier = m_stickIdentifier.toStdString();
    options.stickLabel = m_stickLabel.toStdString();
    options.sourceReadOnly = m_stickReadOnly;
    // Only when it actually changed. Leaving it unset is what tells
    // BackupStick to keep the name the previous generation had, and an
    // update that merely ran without anyone touching the field must not
    // count as "the user cleared the name".
    //
    // The default the field starts out holding is not a change until a
    // preview has come back and said this stick's backup has no name of
    // its own. Nothing stops a backup being started before then -- busy()
    // covers runs, not previews, so Back Up Now is live the moment the
    // page opens -- and without this the stick's label would be stamped
    // over a name its owner chose, or over every name at all if the
    // preview failed and the stored one was never read.
    if (shouldRecordBackupName(m_backupNameIsDefault, m_previewSettled, m_backupName, m_savedBackupName)) {
        options.userName = m_backupName.toStdString();
    }
    return options;
}

void StickBackupController::refresh()
{
    // busy() also covers backup/verify/compact -- refresh() used to guard
    // only against a second preview racing its own watcher, so a preview
    // during an actual run was reachable straight from the UI. Harmless
    // now that BackupStick/CompactStickBackup take an archive-level write
    // lock, but still wasted work and a stale-looking read while the
    // write it's about to report on is still in flight.
    if (m_stickRoot.isEmpty() || m_previewWatcher.isRunning() || busy()) {
        return;
    }
    m_previewing = true;
    emit busyChanged();
    BackupStickOptions options = baseOptions();
    QString label = m_stickLabel;
    QString root = m_stickRoot;
    m_previewWatcher.setFuture(QtConcurrent::run([options, label, root]() mutable {
        auto result = std::make_shared<PreviewResult>();
        if (options.stickIdentifier.empty()) {
            auto info = infrastructure::system::readStickHardwareInfo(root.toStdString(), label.toStdString());
            options.stickIdentifier = info.stickIdentifier;
        }
        result->stickIdentifier = QString::fromStdString(options.stickIdentifier);
        // Only when the page's guess is not there: an archive that exists
        // at the expected path is this stick's by construction, and
        // opening every file in the folder to confirm it would cost a
        // read of each on every page open.
        std::error_code archiveEc;
        if (!fs::exists(options.archivePath, archiveEc)) {
            const fs::path found = application::findStickArchive(
                options.archivePath.parent_path(), options.stickIdentifier, label.toStdString());
            if (!found.empty()) {
                options.archivePath = found;
                result->adoptedArchivePath = pathToQString(found);
            }
        }
        result->preview = BackupStick::preview(options);
        // The newest streaming rate USB Stick Performance recorded for
        // this stick on this computer, if it ever measured it; otherwise
        // the ETA stays unknown.
        try {
            infrastructure::local::StickPerformanceHistory history;
            auto records = history.forStick(options.stickIdentifier);
            for (auto it = records.rbegin(); it != records.rend(); ++it) {
                if (it->streamingBytesPerSecond > 0.0) {
                    result->readMbps = it->streamingBytesPerSecond / (1024.0 * 1024.0);
                    break;
                }
            }
        } catch (const std::exception &) {
            // No history is not an error; the ETA just stays unknown.
        }
        result->blockedBy = QString::fromStdString(infrastructure::system::conflictingDjSoftwareName());
        result->stickReadOnly = infrastructure::media::isMountedReadOnly(root.toStdString());
        return result;
    }));
}

void StickBackupController::onPreviewFinished()
{
    QString thrown;
    std::shared_ptr<PreviewResult> result = takeResult(m_previewWatcher, &thrown);
    m_previewing = false;
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("Could not read the stick or its backup: ") + thrown);
    }
    if (result) {
        m_stickIdentifier = result->stickIdentifier;
        // The preview found this stick's backup under a name the page did
        // not guess. Take it, or every later action -- update, changelog,
        // rename, replace -- keeps addressing the file that is not there.
        if (!result->adoptedArchivePath.isEmpty() && result->adoptedArchivePath != m_archivePath) {
            m_archivePath = result->adoptedArchivePath;
            emit configuredChanged();
        }
        const BackupPreview &p = result->preview;
        QVariantMap last;
        last["exists"] = p.archiveExists;
        last["error"] = QString::fromStdString(p.error);
        if (p.previousStatus) {
            last["status"] = statusToString(*p.previousStatus);
            last["createdAt"] = QDateTime::fromSecsSinceEpoch(p.previousCreatedAtUnix).toString(Qt::ISODate);
            last["label"] = QString::fromStdString(p.previousLabel);
            last["identifierMismatch"] = p.identifierMismatch;
        }

        // "<label>.zip" is already a different stick's backup. Updating it
        // with this stick would diff the newcomer against it and record
        // every file of the other stick as removed, so the default is to
        // step to the next free name rather than to refuse and stop. The
        // colliding archive is left exactly as it is.
        //
        // The cap is not defensive decoration: each step re-previews, and
        // a directory of same-named archives would otherwise walk forever
        // on a page that looks merely slow.
        static constexpr int MaxNameAttempts = 20;
        if (p.identifierMismatch && m_archiveAttempt < MaxNameAttempts) {
            m_nameCollidedWith = QString::fromStdString(p.previousLabel);
            ++m_archiveAttempt;
            m_archivePath = archivePathForLabel(m_backupDirectory, m_stickLabel, m_archiveAttempt);
            emit configuredChanged();
            refresh();
            return;
        }
        last["name"] = QString::fromStdString(p.previousUserName);
        // Adopt the stored name, unless the field is being edited right
        // now: a preview finishing mid-typing must not overwrite what is
        // in the box. An edit is only in flight when the two differ --
        // except straight after configure(), where they differ because the
        // field holds the default name and nobody has typed anything.
        const QString stored = QString::fromStdString(p.previousUserName);
        if (m_backupNameIsDefault || m_backupName == m_savedBackupName) {
            const QString shown = backupNameFor(stored, m_stickLabel);
            if (m_backupName != shown) {
                m_backupName = shown;
                emit backupNameChanged();
            }
            // Still only a default while this backup has no name of its own.
            m_backupNameIsDefault = stored.trimmed().isEmpty();
        }
        m_savedBackupName = stored;
        // The stored name is known now, so the default may be written.
        m_previewSettled = true;

        // An archive written before the file was named after the backup
        // still sits under the stick's label. Move it once, so the file a
        // person finds in the folder is the one they named -- and only
        // when the destination is free, since renaming over another
        // backup would destroy it.
        if (!stored.isEmpty() && !busy() && !m_backupDirectory.isEmpty()) {
            const QString target = archivePathFor(m_backupDirectory, stored, m_stickLabel, 1);
            // Never back onto the plain label when this page stepped off
            // it on purpose: that file is the other stick's backup, and
            // the step is the only thing keeping the two apart. Reachable
            // since the name defaults to the label, which makes the
            // target and the collided name the same file.
            const bool wouldWalkBackIntoTheCollision =
                m_archiveAttempt > 1 && target == archivePathForLabel(m_backupDirectory, m_stickLabel, 1);
            if (!wouldWalkBackIntoTheCollision && target != m_archivePath && renameArchiveTo(target)) {
                // The archive is called after its name now, so the label
                // collision this page stepped around is no longer the one
                // it is in. Left standing, the page kept offering to
                // replace "the other stick's backup" at a path that had
                // become this stick's own.
                m_archiveAttempt = 1;
                m_nameCollidedWith.clear();
                refresh();
                return;
            }
        }
        last["archiveBytes"] = static_cast<qlonglong>(p.archiveBytes);
        last["entries"] = static_cast<qlonglong>(p.unchanged + p.changed);
        m_lastBackup = last;

        QVariantMap since;
        since["added"] = static_cast<qlonglong>(p.added);
        since["changed"] = static_cast<qlonglong>(p.changed);
        since["removed"] = static_cast<qlonglong>(p.removed);
        since["unchanged"] = static_cast<qlonglong>(p.unchanged);
        since["databaseChanged"] = p.databaseChanged;
        since["bytesToRead"] = static_cast<qlonglong>(p.bytesToRead);
        since["stickBytes"] = static_cast<qlonglong>(p.stickBytes);
        since["entriesOnStick"] = static_cast<qlonglong>(p.entriesOnStick);
        since["freeBytes"] = static_cast<qlonglong>(p.freeBytesAtDestination);
        since["enoughFreeSpace"] = p.enoughFreeSpace;
        since["uniformShiftSeconds"] = static_cast<qlonglong>(p.uniformShiftSeconds);
        since["readMbps"] = result->readMbps;
        since["estimatedSeconds"] =
            result->readMbps > 0 ? static_cast<int>(static_cast<double>(p.bytesToRead) / (result->readMbps * 1024.0 * 1024.0)) : -1;
        since["skippedCount"] = static_cast<qlonglong>(p.skipped.size());
        m_sinceLastBackup = since;

        QVariantMap dead;
        dead["deadBytes"] = static_cast<qlonglong>(p.deadBytes);
        dead["archiveBytes"] = static_cast<qlonglong>(p.archiveBytes);
        dead["ratio"] = p.archiveBytes > 0 ? static_cast<double>(p.deadBytes) / static_cast<double>(p.archiveBytes) : 0.0;
        dead["suggested"] = p.deadBytes > 0
                            && (dead["ratio"].toDouble() >= infrastructure::stick_backup::SuggestCompactionRatio
                                || p.deadBytes >= infrastructure::stick_backup::SuggestCompactionBytes);
        m_deadSpace = dead;
        m_blockedBy = result->blockedBy;
        m_stickReadOnly = result->stickReadOnly;
        if (!p.error.empty()) {
            setErrorMessage(QString::fromStdString(p.error));
        }
    }
    emit previewChanged();
    emit busyChanged();
}

void StickBackupController::setActivity(const QString &activity)
{
    if (m_activity == activity) {
        return;
    }
    m_activity = activity;
    emit busyChanged();
}

void StickBackupController::resetProgress()
{
    m_phase.clear();
    m_filesDone = m_filesTotal = m_bytesDone = m_bytesTotal = 0;
    m_bytesPerSecond = 0.0;
    m_etaSeconds = -1;
    m_currentFile.clear();
    m_progressClock.start();
    m_lastProgressMs = 0;
    m_lastProgressBytes = 0;
    emit progressChanged();
}

void StickBackupController::applySimpleProgress(const QString &phase, qlonglong bytesDone, qlonglong bytesTotal)
{
    m_phase = phase;
    m_bytesDone = bytesDone;
    m_bytesTotal = bytesTotal;
    qint64 now = m_progressClock.elapsed();
    if (now - m_lastProgressMs >= 1000) {
        double seconds = static_cast<double>(now - m_lastProgressMs) / 1000.0;
        m_bytesPerSecond = static_cast<double>(bytesDone - m_lastProgressBytes) / seconds;
        m_lastProgressMs = now;
        m_lastProgressBytes = bytesDone;
    }
    m_etaSeconds = (m_bytesPerSecond > 0 && bytesTotal > bytesDone && now > 5000)
                       ? static_cast<int>(static_cast<double>(bytesTotal - bytesDone) / m_bytesPerSecond)
                       : -1;
    emit progressChanged();
}

void StickBackupController::applyProgress(const BackupProgress &progress)
{
    m_filesDone = static_cast<qlonglong>(progress.filesDone);
    m_filesTotal = static_cast<qlonglong>(progress.filesTotal);
    m_currentFile = QString::fromStdString(progress.currentFile);
    applySimpleProgress(phaseName(progress.phase), static_cast<qlonglong>(progress.bytesDone),
                        static_cast<qlonglong>(progress.bytesTotal));
}

bool StickBackupController::enterDirectWrite(std::function<void()> retry)
{
    const QString libraryId = EditSessionRegistry::instance()->libraryIdForPath(m_stickRoot);
    if (auto refusal = m_writeHold.acquire({libraryId}, m_stickLabel, std::move(retry))) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder, m_writeHold.refusedLibraryId());

        }
        return false;
    }
    return true;
}

void StickBackupController::backUp()
{
    if (busy() || m_pending || m_stickRoot.isEmpty()) {
        emit actionFeedback(QStringLiteral("Still busy. Try again once the current operation finishes."), true);
        return;
    }
    if (!enterDirectWrite([this] { backUp(); })) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    setActivity(QStringLiteral("backup"));
    resetProgress();
    m_cancel = application::CancellationToken();
    BackupStickOptions options = baseOptions();
    options.cancel = m_cancel;
    options.conflictingProcessProbe = [] { return infrastructure::system::isConflictingDjSoftwareRunning(); };
    QPointer<StickBackupController> self(this);
    auto lastPost = std::make_shared<std::chrono::steady_clock::time_point>();
    options.onProgress = [self, lastPost](const BackupProgress &progress) {
        auto now = std::chrono::steady_clock::now();
        bool phaseEdge = progress.bytesDone == 0 || progress.bytesDone == progress.bytesTotal;
        if (!phaseEdge && now - *lastPost < std::chrono::milliseconds(100)) {
            return;
        }
        *lastPost = now;
        BackupProgress copy = progress;
        QMetaObject::invokeMethod(
            self, [self, copy] {
                if (self) {
                    self->applyProgress(copy);
                }
            },
            Qt::QueuedConnection);
    };
    const QString rekordboxPath = m_rekordboxPath;
    const QString enginePath = m_enginePath;
    // Awake for the whole backup: see SleepInhibitor.
    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Backing up a USB stick"));
    m_runWatcher.setFuture(QtConcurrent::run([keepAwake, options, rekordboxPath, enginePath]() mutable {
        auto result = std::make_shared<RunResult>();
        result->activity = QStringLiteral("backup");
        result->refusal = refuseIfDjSoftwareRunning();
        if (!result->refusal.isEmpty()) {
            return result;
        }
        // The library's content identity goes into the manifest header so
        // the stick list can later tell "this backup is of that library"
        // regardless of which stick (or format) it ends up on. Read-only,
        // and a failure here just leaves the previous fingerprint in place.
        //
        // Handed to the backup to call rather than read here: it takes it
        // once the copy is done, so the header describes what the archive
        // holds. Read FRESH, around the catalog cache, because the cache
        // answers for the catalogs as it last saw them and the archive
        // holds them as they are -- the two disagreeing is what put 143
        // tracks in a header over catalogs holding 156.
        options.readLibraryFingerprint = [rekordboxPath, enginePath]() -> std::string {
            const auto fingerprint = readLibraryFingerprintUncached(rekordboxPath, enginePath);
            return fingerprint ? fingerprint->serialize() : std::string();
        };
        result->backup = std::make_shared<BackupStickOutcome>(BackupStick::execute(options));
        return result;
    }));
}

void StickBackupController::cancel()
{
    m_cancel.cancel();
}

void StickBackupController::keepPartial()
{
    if (!m_pending || busy()) {
        return;
    }
    if (!enterDirectWrite([this] { keepPartial(); })) {
        return;
    }
    // Every other action clears the banner on the way in; these two did
    // not, so a red error from before the cancel sat above "Discarded the
    // interrupted backup." and read as though the discard had failed.
    setErrorMessage({});
    setActivity(QStringLiteral("decide"));
    application::PendingBackup *pending = m_pending.get();
    // Awake while the archive is committed or rolled back: see SleepInhibitor.
    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Finishing a stick backup"));
    m_runWatcher.setFuture(QtConcurrent::run([keepAwake, pending]() {
        auto result = std::make_shared<RunResult>();
        result->activity = QStringLiteral("keep");
        result->backup = std::make_shared<BackupStickOutcome>(pending->keep());
        return result;
    }));
}

void StickBackupController::discardPartial()
{
    if (!m_pending || busy()) {
        return;
    }
    if (!enterDirectWrite([this] { discardPartial(); })) {
        return;
    }
    // Every other action clears the banner on the way in; these two did
    // not, so a red error from before the cancel sat above "Discarded the
    // interrupted backup." and read as though the discard had failed.
    setErrorMessage({});
    setActivity(QStringLiteral("decide"));
    application::PendingBackup *pending = m_pending.get();
    // Awake while the archive is committed or rolled back: see SleepInhibitor.
    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Discarding a partial stick backup"));
    m_runWatcher.setFuture(QtConcurrent::run([keepAwake, pending]() {
        auto result = std::make_shared<RunResult>();
        result->activity = QStringLiteral("discard");
        result->backup = std::make_shared<BackupStickOutcome>(pending->discard());
        return result;
    }));
}

void StickBackupController::deleteBackup()
{
    if (busy() || m_pending || m_archivePath.isEmpty()) {
        return;
    }
    // The archive is part of this stick's library as far as other
    // instances are concerned; deleting it takes the same lock a backup
    // writing to it does.
    if (!enterDirectWrite([this] { deleteBackup(); })) {
        return;
    }
    const fs::path archive = pathFromQString(m_archivePath);
    std::error_code archiveError;
    fs::remove(archive, archiveError);
    // The journal goes too: left behind, the next backup would try to
    // recover an archive that is no longer there.
    std::error_code journalError;
    fs::remove(BackupStick::journalPathFor(archive), journalError);
    m_writeHold.release();
    if (archiveError) {
        setErrorMessage(QStringLiteral("Could not delete the backup: ") + QString::fromStdString(archiveError.message()));
        emit actionFeedback(m_errorMessage, true);
    } else {
        setErrorMessage({});
        setStatusMessage(QStringLiteral("Deleted the damaged backup. The next backup copies the whole stick again."));
        emit actionFeedback(m_statusMessage, false);
    }
    refresh();
}

void StickBackupController::verify()
{
    if (busy() || m_pending) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    setActivity(QStringLiteral("verify"));
    resetProgress();
    m_cancel = application::CancellationToken();
    fs::path archive = pathFromQString(m_archivePath);
    application::CancellationToken cancel = m_cancel;
    QPointer<StickBackupController> self(this);
    auto lastPost = std::make_shared<std::chrono::steady_clock::time_point>();
    m_runWatcher.setFuture(QtConcurrent::run([archive, cancel, self, lastPost]() {
        auto result = std::make_shared<RunResult>();
        result->activity = QStringLiteral("verify");
        result->verify = BackupStick::verify(archive, cancel, [self, lastPost](std::uint64_t done, std::uint64_t total) {
            auto now = std::chrono::steady_clock::now();
            if (done != total && now - *lastPost < std::chrono::milliseconds(100)) {
                return;
            }
            *lastPost = now;
            QMetaObject::invokeMethod(
                self,
                [self, done, total] {
                    if (self) {
                        self->applySimpleProgress(QStringLiteral("verifying"), static_cast<qlonglong>(done),
                                                  static_cast<qlonglong>(total));
                    }
                },
                Qt::QueuedConnection);
        });
        return result;
    }));
}

QVariantMap StickBackupController::compactionPreflight()
{
    application::CompactionPreflight pre = CompactStickBackup::preflight(pathFromQString(m_archivePath));
    QVariantMap map;
    map["error"] = QString::fromStdString(pre.error);
    map["archiveBytes"] = static_cast<qlonglong>(pre.archiveBytes);
    map["liveBytes"] = static_cast<qlonglong>(pre.liveBytes);
    map["deadBytes"] = static_cast<qlonglong>(pre.deadBytes);
    map["reclaimableBytes"] = static_cast<qlonglong>(pre.reclaimableBytes);
    map["compactedBytes"] = static_cast<qlonglong>(pre.compactedBytes);
    map["ratio"] = pre.deadRatio;
    map["suggested"] = pre.suggested;
    map["requiredFreeBytes"] = static_cast<qlonglong>(pre.requiredFreeBytes);
    map["availableFreeBytes"] = static_cast<qlonglong>(pre.availableFreeBytes);
    map["enoughFreeSpace"] = pre.enoughFreeSpace;
    return map;
}

void StickBackupController::compact()
{
    if (busy() || m_pending) {
        return;
    }
    if (!enterDirectWrite([this] { compact(); })) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    setActivity(QStringLiteral("compact"));
    resetProgress();
    m_cancel = application::CancellationToken();
    CompactStickBackupOptions options;
    options.archivePath = pathFromQString(m_archivePath);
    options.cancel = m_cancel;
    QPointer<StickBackupController> self(this);
    auto lastPost = std::make_shared<std::chrono::steady_clock::time_point>();
    options.onProgress = [self, lastPost](std::uint64_t done, std::uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        if (done != total && now - *lastPost < std::chrono::milliseconds(100)) {
            return;
        }
        *lastPost = now;
        QMetaObject::invokeMethod(
            self,
            [self, done, total] {
                if (self) {
                    self->applySimpleProgress(QStringLiteral("compacting"), static_cast<qlonglong>(done),
                                              static_cast<qlonglong>(total));
                }
            },
            Qt::QueuedConnection);
    };
    // Awake while the archive is rewritten: see SleepInhibitor.
    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Compacting a stick backup"));
    m_runWatcher.setFuture(QtConcurrent::run([keepAwake, options]() {
        auto result = std::make_shared<RunResult>();
        result->activity = QStringLiteral("compact");
        result->compaction = CompactStickBackup::execute(options);
        return result;
    }));
}

void StickBackupController::openArchiveFolder()
{
    QString folder = QFileInfo(m_archivePath).absolutePath();
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void StickBackupController::replaceCollidingBackup()
{
    if (m_nameCollidedWith.isEmpty() || busy()) {
        return;
    }
    // Delete the other stick's archive, then start again from the plain
    // name. Deleting and writing afresh rather than updating in place is
    // the point: an update would keep that stick's manifest and call
    // every one of its files removed, producing something that looks
    // like a backup of neither stick.
    const QString colliding = archivePathForLabel(m_backupDirectory, m_stickLabel, 1);
    std::error_code ec;
    fs::remove(pathFromQString(colliding), ec);
    if (ec) {
        setErrorMessage(QStringLiteral("Could not remove ") + colliding + QStringLiteral(": ")
                        + QString::fromStdString(ec.message()));
        emit actionFeedback(m_errorMessage, true);
        return;
    }
    fs::remove(pathFromQString(colliding + QStringLiteral(".journal")), ec);

    m_nameCollidedWith.clear();
    m_archiveAttempt = 1;
    m_archivePath = colliding;
    emit configuredChanged();
    refresh();
}

void StickBackupController::openChangelog()
{
    QString error;
    const QString path = writeChangelogFile(pathFromQString(m_archivePath), &error);
    if (path.isEmpty()) {
        emit actionFeedback(error, true);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void StickBackupController::finishOutcome(const BackupStickOutcome &outcome)
{
    QString bytes = QString::number(static_cast<double>(outcome.bytesRead) / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MiB");
    switch (outcome.status) {
    case BackupOutcomeStatus::Complete:
        setStatusMessage(QStringLiteral("Backup complete and verified: %1 added, %2 changed, %3 removed (%4 read from the stick).")
                             .arg(outcome.added)
                             .arg(outcome.changed)
                             .arg(outcome.removed)
                             .arg(bytes));
        emit actionFeedback(m_statusMessage, false);
        break;
    case BackupOutcomeStatus::NothingToDo:
        setStatusMessage(QStringLiteral("Nothing changed since the last backup."));
        emit actionFeedback(m_statusMessage, false);
        break;
    case BackupOutcomeStatus::Cancelled:
        setStatusMessage(QString::fromStdString(outcome.message));
        break;
    case BackupOutcomeStatus::KeptPartial:
        setStatusMessage(QStringLiteral("Kept what was copied so far; the next backup continues from here."));
        emit actionFeedback(m_statusMessage, false);
        break;
    case BackupOutcomeStatus::Discarded:
        setStatusMessage(QStringLiteral("Discarded the interrupted backup."));
        emit actionFeedback(m_statusMessage, false);
        break;
    case BackupOutcomeStatus::ConflictAborted:
    case BackupOutcomeStatus::DbTooLarge:
    case BackupOutcomeStatus::DbUnstable: {
        QString detail = QString::fromStdString(outcome.message);
        for (const std::string &warning : outcome.warnings) {
            if (warning.find("not backed up") != std::string::npos) {
                detail += (detail.isEmpty() ? QString() : QStringLiteral(" ")) + QString::fromStdString(warning);
            }
        }
        setStatusMessage(QStringLiteral("Backup is incomplete: ") + detail);
        emit actionFeedback(m_statusMessage, true);
        break;
    }
    case BackupOutcomeStatus::Failed:
        setErrorMessage(QString::fromStdString(outcome.message));
        emit actionFeedback(m_errorMessage, true);
        break;
    }

    // A salvage run says what it could not get, whatever else happened
    // above. This is the one number the person needs off a dying stick,
    // and it must not be buried under "Backed up 1 431 files": those
    // files are not all there.
    if (!outcome.salvaged.empty()) {
        QString said = QStringLiteral("%1 file(s) could not be read in full off this stick: ")
                           .arg(outcome.salvaged.size());
        for (std::size_t i = 0; i < outcome.salvaged.size() && i < 3; ++i) {
            const auto &entry = outcome.salvaged[i];
            said += (i == 0 ? QString() : QStringLiteral(", ")) + QString::fromStdString(entry.path)
                + QStringLiteral(" (") + humanBytes(entry.bytesSalvaged) + QStringLiteral(" of ")
                + humanBytes(entry.expectedSize) + QStringLiteral(")");
        }
        if (outcome.salvaged.size() > 3) {
            said += QStringLiteral(" and %1 more").arg(outcome.salvaged.size() - 3);
        }
        said += QStringLiteral(". The backup lists every one of them in SEABASS-SALVAGE.txt.");
        setStatusMessage(said);
        emit actionFeedback(said, true);
    }
}

void StickBackupController::onRunFinished()
{
    QString thrown;
    std::shared_ptr<RunResult> result = takeResult(m_runWatcher, &thrown);
    m_writeHold.release();
    setActivity({});
    if (!thrown.isEmpty()) {
        setErrorMessage(thrown);
        emit actionFeedback(thrown, true);
        refresh();
        return;
    }
    if (!result) {
        return;
    }
    if (!result->refusal.isEmpty()) {
        setErrorMessage(result->refusal);
        emit actionFeedback(result->refusal, true);
        refresh();
        return;
    }
    if (result->activity == QStringLiteral("backup") && result->backup) {
        if (result->backup->status == BackupOutcomeStatus::Cancelled && result->backup->pending) {
            m_pending = std::move(result->backup->pending);
            emit pendingCancelDecisionChanged();
        }
        finishOutcome(*result->backup);
    } else if ((result->activity == QStringLiteral("keep") || result->activity == QStringLiteral("discard")) && result->backup) {
        m_pending.reset();
        emit pendingCancelDecisionChanged();
        finishOutcome(*result->backup);
    } else if (result->activity == QStringLiteral("verify")) {
        const VerifyOutcome &v = result->verify;
        if (!v.error.empty()) {
            setErrorMessage(QStringLiteral("Verification failed: ") + QString::fromStdString(v.error));
            emit actionFeedback(m_errorMessage, true);
            // No decision dialog here. This branch is every way verify can
            // stop without having compared the backup's contents: the
            // archive would not open, there is no backup, its tail could
            // not be read, or the user cancelled. Several of those are the
            // drive or the moment, not the backup -- a disk that went away,
            // a permission, a transient read error -- and offering Delete
            // as the default there could remove an archive that is intact.
            // The message says what failed; running Verify again is the
            // answer to all of them.
        } else if (!v.ok) {
            // Only here is the backup itself known to be damaged: every
            // entry was read back, and these did not match what was
            // written.
            setErrorMessage(QStringLiteral("Verification found %1 damaged entr%2. This backup should not be trusted; run a new backup.")
                                .arg(v.failures.size())
                                .arg(v.failures.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
            emit actionFeedback(m_errorMessage, true);
            emit verifyFailed(QStringLiteral("%1 file%2 in the backup did not match what was written.")
                                  .arg(v.failures.size())
                                  .arg(v.failures.size() == 1 ? QString() : QStringLiteral("s")));
        } else {
            setStatusMessage(QStringLiteral("Verified: %1 files, %2 MiB, every checksum matches.")
                                 .arg(v.entriesChecked)
                                 .arg(QString::number(static_cast<double>(v.bytesChecked) / (1024.0 * 1024.0), 'f', 1)));
            emit actionFeedback(m_statusMessage, false);
        }
    } else if (result->activity == QStringLiteral("compact")) {
        const CompactionOutcome &c = result->compaction;
        switch (c.status) {
        case CompactionOutcome::Status::Compacted:
            setStatusMessage(QStringLiteral("Compacted: reclaimed %1 MiB.")
                                 .arg(QString::number(static_cast<double>(c.bytesBefore - c.bytesAfter) / (1024.0 * 1024.0), 'f', 1)));
            emit actionFeedback(m_statusMessage, false);
            break;
        case CompactionOutcome::Status::NothingToReclaim:
            setStatusMessage(QStringLiteral("Nothing to reclaim."));
            emit actionFeedback(m_statusMessage, false);
            break;
        case CompactionOutcome::Status::Cancelled:
            setStatusMessage(QStringLiteral("Compaction cancelled; the backup is unchanged."));
            break;
        case CompactionOutcome::Status::NotEnoughSpace:
        case CompactionOutcome::Status::Failed:
            setErrorMessage(QString::fromStdString(c.message));
            emit actionFeedback(m_errorMessage, true);
            break;
        }
    }
    refresh();
}

void StickBackupController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

// Move an archive to the filename its name implies, with the siblings
// that are derived from its path.
//
// The archive is identity: .journal and .lock are its path plus a
// suffix, and .seabass-backup-source stores the absolute path of a
// browsed one. So this refuses rather than half-moves -- a rename that
// took the archive and left the journal would produce an archive whose
// recovery record belongs to a file that no longer exists.
bool StickBackupController::renameArchiveTo(const QString &target)
{
    if (target.isEmpty() || target == m_archivePath) {
        return true;
    }
    const fs::path from = pathFromQString(m_archivePath);
    const fs::path to = pathFromQString(target);
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        // Nothing written yet: the next backup simply creates it under
        // the new name.
        m_archivePath = target;
        emit configuredChanged();
        return true;
    }
    if (fs::exists(to, ec)) {
        // Someone else's archive already has this name. The collision
        // path handles choosing another; silently overwriting here would
        // destroy a backup.
        return false;
    }
    fs::rename(from, to, ec);
    if (ec) {
        setErrorMessage(QStringLiteral("Could not rename the backup file: ") + QString::fromStdString(ec.message()));
        emit actionFeedback(m_errorMessage, true);
        return false;
    }
    // The journal only exists after an interrupted run; missing is fine.
    std::error_code journalEc;
    fs::rename(pathFromQString(m_archivePath + QStringLiteral(".journal")),
                pathFromQString(target + QStringLiteral(".journal")), journalEc);
    m_archivePath = target;
    emit configuredChanged();
    return true;
}

void StickBackupController::setBackupName(const QString &name)
{
    // Trimmed on the way in: a name typed with a trailing space would
    // otherwise look unchanged on screen while counting as an edit, and
    // would be written that way into the manifest.
    const QString trimmed = name.trimmed();
    if (m_backupName == trimmed) {
        // Nothing was typed. QML sends editingFinished on plain focus
        // loss too, so clicking into the box and out again must leave the
        // default a default -- otherwise the stored name a preview is
        // still fetching would never be allowed to replace it.
        return;
    }
    // Typed, so it is the person's name now and no longer the default --
    // including an empty field, which is a name deliberately cleared.
    m_backupNameIsDefault = false;
    m_backupName = trimmed;
    emit backupNameChanged();

    // The file on disk is called after the name, so changing the name
    // moves it. Not while a run is in flight: the archive is open.
    if (!busy() && !m_backupDirectory.isEmpty()) {
        const QString target = archivePathFor(m_backupDirectory, m_backupName, m_stickLabel, 1);
        if (renameArchiveTo(target)) {
            m_archiveAttempt = 1;
            m_nameCollidedWith.clear();
            refresh();
        }
    }
}

void StickBackupController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
