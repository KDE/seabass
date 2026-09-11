#include "stick_backup_controller.hpp"

#include "domain/library_fingerprint.hpp"
#include "gui/library_fingerprint_reader.hpp"
#include "gui/stick_backup_paths.hpp"
#include "gui/future_result.hpp"

#include <QDateTime>
#include <QDesktopServices>
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
#include "infrastructure/system/rekordbox_process_detector.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

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
    double readMbps = 0.0;  // last measured audio read speed for this stick, 0 = unknown
    QString blockedBy;
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
    m_stickRoot = QString::fromStdString(fs::path(anyPath.toStdString()).parent_path().string());
    m_archivePath = archivePathForLabel(backupDirectory, stickLabel);
    emit configuredChanged();
    refresh();
}

BackupStickOptions StickBackupController::baseOptions() const
{
    BackupStickOptions options;
    options.stickRoot = fs::path(m_stickRoot.toStdString());
    options.archivePath = fs::path(m_archivePath.toStdString());
    options.stickIdentifier = m_stickIdentifier.toStdString();
    options.stickLabel = m_stickLabel.toStdString();
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
        emit actionFeedback(QStringLiteral("Still busy -- try again once the current operation finishes."), true);
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
    m_runWatcher.setFuture(QtConcurrent::run([options, rekordboxPath, enginePath]() mutable {
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
        if (const auto fingerprint = readLibraryFingerprint(rekordboxPath, enginePath)) {
            options.libraryFingerprint = fingerprint->serialize();
        }
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
    setActivity(QStringLiteral("decide"));
    application::PendingBackup *pending = m_pending.get();
    m_runWatcher.setFuture(QtConcurrent::run([pending]() {
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
    setActivity(QStringLiteral("decide"));
    application::PendingBackup *pending = m_pending.get();
    m_runWatcher.setFuture(QtConcurrent::run([pending]() {
        auto result = std::make_shared<RunResult>();
        result->activity = QStringLiteral("discard");
        result->backup = std::make_shared<BackupStickOutcome>(pending->discard());
        return result;
    }));
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
    fs::path archive(m_archivePath.toStdString());
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
    application::CompactionPreflight pre = CompactStickBackup::preflight(fs::path(m_archivePath.toStdString()));
    QVariantMap map;
    map["error"] = QString::fromStdString(pre.error);
    map["archiveBytes"] = static_cast<qlonglong>(pre.archiveBytes);
    map["liveBytes"] = static_cast<qlonglong>(pre.liveBytes);
    map["deadBytes"] = static_cast<qlonglong>(pre.deadBytes);
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
    options.archivePath = fs::path(m_archivePath.toStdString());
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
    m_runWatcher.setFuture(QtConcurrent::run([options]() {
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
        } else if (!v.ok) {
            setErrorMessage(QStringLiteral("Verification found %1 damaged entr%2 -- this backup should not be trusted; run a new backup.")
                                .arg(v.failures.size())
                                .arg(v.failures.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
            emit actionFeedback(m_errorMessage, true);
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

void StickBackupController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
