// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/library_catalog_cache.hpp"
#include "clone_stick_controller.hpp"

#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <filesystem>

#include "gui/library_fingerprint_reader.hpp"
#include "gui/stick_backup_paths.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/future_result.hpp"
#include "gui/write_guard.hpp"
#include "infrastructure/engine/engine_restore_check.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using application::BackupOutcomeStatus;
using application::BackupProgress;
using application::CloneProgress;
using application::CloneStick;
using application::CloneStickOptions;
using application::CloneStickOutcome;
using application::CloneStickPreview;
using application::RestoreProgress;
using application::RestoreSummary;

namespace
{

QString backupPhaseName(BackupProgress::Phase phase)
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

QString restorePhaseName(RestoreProgress::Phase phase)
{
    switch (phase) {
    case RestoreProgress::Phase::Analyzing: return QStringLiteral("analyzing");
    case RestoreProgress::Phase::Writing: return QStringLiteral("writing");
    case RestoreProgress::Phase::Removing: return QStringLiteral("removing");
    case RestoreProgress::Phase::Checking: return QStringLiteral("checking");
    }
    return {};
}

QString backupStatusName(BackupOutcomeStatus status)
{
    switch (status) {
    case BackupOutcomeStatus::Complete: return QStringLiteral("complete");
    case BackupOutcomeStatus::NothingToDo: return QStringLiteral("nothing-to-do");
    case BackupOutcomeStatus::Cancelled: return QStringLiteral("cancelled");
    case BackupOutcomeStatus::KeptPartial: return QStringLiteral("kept-partial");
    case BackupOutcomeStatus::Discarded: return QStringLiteral("discarded");
    case BackupOutcomeStatus::ConflictAborted: return QStringLiteral("conflict-aborted");
    case BackupOutcomeStatus::DbTooLarge: return QStringLiteral("db-too-large");
    case BackupOutcomeStatus::DbUnstable: return QStringLiteral("db-unstable");
    case BackupOutcomeStatus::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

QVariantList toVariantList(const std::vector<std::string> &items)
{
    QVariantList list;
    for (const std::string &item : items) {
        list.push_back(QString::fromStdString(item));
    }
    return list;
}

}  // namespace

struct CloneStickController::PreviewResult
{
    CloneStickPreview preview;
    QString blockedBy;
};

struct CloneStickController::RunResult
{
    QString refusal;
    CloneStickOutcome outcome;
};

CloneStickController::CloneStickController(QObject *parent) : QObject(parent)
{
    connect(&m_previewWatcher, &QFutureWatcher<std::shared_ptr<PreviewResult>>::finished, this,
            &CloneStickController::onPreviewFinished);
    connect(&m_runWatcher, &QFutureWatcher<std::shared_ptr<RunResult>>::finished, this,
            &CloneStickController::onRunFinished);
}

CloneStickController::~CloneStickController()
{
    m_cancel.cancel();
    awaitQuietly(m_runWatcher);
    awaitQuietly(m_previewWatcher);
}

void CloneStickController::configure(const QString &sourceLabel, const QString &sourceRekordboxPath,
                                     const QString &sourceEnginePath, const QString &targetMountPoint,
                                     const QString &targetLabel, const QString &backupDirectory)
{
    const QString anyPath = sourceEnginePath.isEmpty() ? sourceRekordboxPath : sourceEnginePath;
    m_sourceLabel = sourceLabel;
    m_sourceRekordboxPath = sourceRekordboxPath;
    m_sourceEnginePath = sourceEnginePath;
    m_sourceRoot = anyPath.isEmpty() ? QString() : QString::fromStdString(fs::path(anyPath.toStdString()).parent_path().string());
    m_targetLabel = targetLabel;
    m_targetRoot = targetMountPoint;
    m_archivePath = archivePathForLabel(backupDirectory, sourceLabel);
    emit configuredChanged();
    refresh();
}

CloneStickOptions CloneStickController::baseOptions() const
{
    CloneStickOptions options;
    options.backup.stickRoot = fs::path(m_sourceRoot.toStdString());
    options.backup.archivePath = fs::path(m_archivePath.toStdString());
    options.backup.stickLabel = m_sourceLabel.toStdString();
    options.targetRoot = fs::path(m_targetRoot.toStdString());
    return options;
}

void CloneStickController::refresh()
{
    if (m_sourceRoot.isEmpty() || m_targetRoot.isEmpty() || m_cloning) {
        return;
    }
    if (m_previewWatcher.isRunning()) {
        m_refreshPending = true;
        return;
    }
    m_previewing = true;
    emit busyChanged();
    const CloneStickOptions options = baseOptions();
    m_previewWatcher.setFuture(QtConcurrent::run([options]() {
        auto result = std::make_shared<PreviewResult>();
        result->blockedBy = QString::fromStdString(infrastructure::system::conflictingDjSoftwareName());
        result->preview = CloneStick::preview(options);
        return result;
    }));
}

void CloneStickController::onPreviewFinished()
{
    QString thrown;
    std::shared_ptr<PreviewResult> result = takeResult(m_previewWatcher, &thrown);
    m_previewing = false;
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("Could not read the source stick or its backup: ") + thrown);
    }
    if (result) {
        const CloneStickPreview &p = result->preview;
        QVariantMap map;
        map["error"] = QString::fromStdString(p.error);
        map["ready"] = p.error.empty();
        map["archiveExists"] = p.backup.archiveExists;
        map["archiveCurrent"] = p.archiveCurrent;
        map["added"] = static_cast<qlonglong>(p.backup.added);
        map["changed"] = static_cast<qlonglong>(p.backup.changed);
        map["removed"] = static_cast<qlonglong>(p.backup.removed);
        map["databaseChanged"] = p.backup.databaseChanged;
        map["bytesToRead"] = static_cast<qlonglong>(p.backup.bytesToRead);
        map["sourceBytes"] = static_cast<qlonglong>(p.sourceBytes);
        map["backupFreeBytes"] = static_cast<qlonglong>(p.backup.freeBytesAtDestination);
        map["enoughBackupSpace"] = p.backup.enoughFreeSpace;
        map["bytesToTarget"] = static_cast<qlonglong>(p.bytesToTarget);
        map["targetFreeBytes"] = static_cast<qlonglong>(p.targetFreeBytes);
        map["enoughTargetSpace"] = p.enoughTargetSpace;
        map["targetHasEngineLibrary"] = p.targetHasEngineLibrary;
        map["restoreKnown"] = p.restore.has_value();
        map["restoreFilesToWrite"] = p.restore ? static_cast<qlonglong>(p.restore->filesToWrite) : -1;
        map["restoreExtras"] = p.restore ? static_cast<qlonglong>(p.restore->extras) : -1;
        m_preview = map;
        m_blockedBy = result->blockedBy;
        setErrorMessage(QString::fromStdString(p.error));
    }
    emit previewChanged();
    emit busyChanged();
    if (m_refreshPending) {
        m_refreshPending = false;
        refresh();
    }
}

void CloneStickController::resetProgress()
{
    m_stage.clear();
    m_phase.clear();
    m_filesDone = m_filesTotal = m_bytesDone = m_bytesTotal = 0;
    m_bytesPerSecond = 0.0;
    m_etaSeconds = -1;
    m_progressClock.start();
    m_stageStartMs = 0;
    m_lastProgressMs = 0;
    m_lastProgressBytes = 0;
    m_currentFile.clear();
    emit progressChanged();
}

void CloneStickController::applyProgress(const CloneProgress &progress)
{
    const QString stage = progress.stage == CloneProgress::Stage::Backup ? QStringLiteral("backup") : QStringLiteral("restore");
    const qint64 now = m_progressClock.elapsed();
    if (stage != m_stage) {
        // A new stage is a new transfer: its own rate window and ETA.
        m_stage = stage;
        m_stageStartMs = now;
        m_lastProgressMs = now;
        m_lastProgressBytes = 0;
        m_bytesPerSecond = 0.0;
    }
    bool determinate = false;
    if (progress.stage == CloneProgress::Stage::Backup) {
        const BackupProgress &b = progress.backup;
        m_phase = backupPhaseName(b.phase);
        m_filesDone = static_cast<qlonglong>(b.filesDone);
        m_filesTotal = static_cast<qlonglong>(b.filesTotal);
        m_bytesDone = static_cast<qlonglong>(b.bytesDone);
        m_bytesTotal = static_cast<qlonglong>(b.bytesTotal);
        m_currentFile = QString::fromStdString(b.currentFile);
        determinate = b.phase == BackupProgress::Phase::Reading;
    } else {
        const RestoreProgress &r = progress.restore;
        m_phase = restorePhaseName(r.phase);
        m_filesDone = static_cast<qlonglong>(r.filesDone);
        m_filesTotal = static_cast<qlonglong>(r.filesTotal);
        m_bytesDone = static_cast<qlonglong>(r.bytesDone);
        m_bytesTotal = static_cast<qlonglong>(r.bytesTotal);
        m_currentFile = QString::fromStdString(r.currentFile);
        determinate = r.phase == RestoreProgress::Phase::Writing;
    }
    // Same sampling as the backup and restore controllers: a one-second
    // window for the rate, no ETA before five seconds into the stage.
    if (now - m_lastProgressMs >= 1000) {
        const double seconds = static_cast<double>(now - m_lastProgressMs) / 1000.0;
        m_bytesPerSecond = static_cast<double>(m_bytesDone - m_lastProgressBytes) / seconds;
        m_lastProgressMs = now;
        m_lastProgressBytes = m_bytesDone;
    }
    m_etaSeconds = (determinate && m_bytesPerSecond > 0 && m_bytesTotal > m_bytesDone && now - m_stageStartMs > 5000)
                       ? static_cast<int>(static_cast<double>(m_bytesTotal - m_bytesDone) / m_bytesPerSecond)
                       : -1;
    emit progressChanged();
}

void CloneStickController::start(bool exact)
{
    if (busy() || m_sourceRoot.isEmpty() || m_targetRoot.isEmpty()) {
        emit actionFeedback(QStringLiteral("Still busy. Try again once the current operation finishes."), true);
        return;
    }
    // The source's archive on disk is updated and the target is
    // overwritten: both libraries' locks, or neither.
    auto *registry = EditSessionRegistry::instance();
    if (auto refusal = m_writeHold.acquire({registry->libraryIdForPath(m_sourceRoot), registry->libraryIdForPath(m_targetRoot)},
                                          m_sourceLabel, [this, exact] { start(exact); })) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder, m_writeHold.refusedLibraryId());

        }
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_result.clear();
    emit resultChanged();
    m_cloning = true;
    emit busyChanged();
    resetProgress();
    m_cancel = application::CancellationToken();
    CloneStickOptions options = baseOptions();
    options.exact = exact;
    options.cancel = m_cancel;
    options.backup.conflictingProcessProbe = [] { return infrastructure::system::isConflictingDjSoftwareRunning(); };
    options.libraryCheck = infrastructure::engine::checkRestoredEngineLibrary;
    QPointer<CloneStickController> self(this);
    auto lastPost = std::make_shared<std::chrono::steady_clock::time_point>();
    options.onProgress = [self, lastPost](const CloneProgress &progress) {
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t done = progress.stage == CloneProgress::Stage::Backup ? progress.backup.bytesDone : progress.restore.bytesDone;
        const std::uint64_t total = progress.stage == CloneProgress::Stage::Backup ? progress.backup.bytesTotal : progress.restore.bytesTotal;
        const bool edge = done == 0 || done == total;
        if (!edge && now - *lastPost < std::chrono::milliseconds(100)) {
            return;
        }
        *lastPost = now;
        CloneProgress copy = progress;
        QMetaObject::invokeMethod(
            self, [self, copy] {
                if (self) {
                    self->applyProgress(copy);
                }
            },
            Qt::QueuedConnection);
    };
    const QString sourceLabel = m_sourceLabel;
    const QString sourceRoot = m_sourceRoot;
    const QString rekordboxPath = m_sourceRekordboxPath;
    const QString enginePath = m_sourceEnginePath;
    m_runWatcher.setFuture(QtConcurrent::run([options, sourceLabel, sourceRoot, rekordboxPath, enginePath]() mutable {
        auto result = std::make_shared<RunResult>();
        result->refusal = refuseIfDjSoftwareRunning();
        if (!result->refusal.isEmpty()) {
            return result;
        }
        options.backup.stickIdentifier =
            infrastructure::system::readStickHardwareInfo(sourceRoot.toStdString(), sourceLabel.toStdString()).stickIdentifier;
        if (const auto fingerprint = readLibraryFingerprint(rekordboxPath, enginePath)) {
            options.backup.libraryFingerprint = fingerprint->serialize();
        }
        result->outcome = CloneStick::execute(options);
        return result;
    }));
}

void CloneStickController::cancel()
{
    m_cancel.cancel();
}

void CloneStickController::clearResult()
{
    if (m_cloning) {
        return;
    }
    m_result.clear();
    emit resultChanged();
    setErrorMessage({});
    setStatusMessage({});
}

void CloneStickController::onRunFinished()
{
    QString thrown;
    std::shared_ptr<RunResult> result = takeResult(m_runWatcher, &thrown);
    // The target stick's catalogs were replaced wholesale.
    LibraryCatalogCache::instance().invalidateEveryCatalogOn(m_targetRoot.toStdString());
    m_writeHold.release();
    m_cloning = false;
    emit busyChanged();
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
    const CloneStickOutcome &o = result->outcome;
    QVariantMap map;
    map["status"] = QString::fromUtf8(std::string(application::toString(o.status)).c_str());
    map["message"] = QString::fromStdString(o.message);
    map["backupStatus"] = backupStatusName(o.backupStatus);
    map["backupSkipped"] = o.backupStatus == BackupOutcomeStatus::NothingToDo;
    map["backupBytesRead"] = static_cast<qlonglong>(o.backupBytesRead);
    map["restoreStarted"] = o.restoreStarted;
    if (o.restoreStarted) {
        const RestoreSummary &s = o.restore;
        map["filesWritten"] = static_cast<qlonglong>(s.filesWritten);
        map["filesUnchanged"] = static_cast<qlonglong>(s.filesUnchanged);
        map["directoriesCreated"] = static_cast<qlonglong>(s.directoriesCreated);
        map["extrasRemoved"] = static_cast<qlonglong>(s.extrasRemoved);
        map["bytesWritten"] = static_cast<qlonglong>(s.bytesWritten);
        QVariantList rejected;
        for (const auto &[name, reason] : s.rejected) {
            rejected.push_back(QString::fromStdString(name + ": " + reason));
        }
        map["rejected"] = rejected;
        map["writeErrors"] = toVariantList(s.writeErrors);
        map["warnings"] = toVariantList(s.warnings);
        map["missingTracks"] = s.missingTrackPaths ? toVariantList(*s.missingTrackPaths) : QVariantList{};
        map["databaseChecked"] = s.missingTrackPaths.has_value();
    }
    m_result = map;
    emit resultChanged();

    const QString target = m_targetLabel.isEmpty() ? QStringLiteral("the target") : m_targetLabel;
    switch (o.status) {
    case CloneStickOutcome::Status::Cloned:
        setStatusMessage(QStringLiteral("%1 now holds %2's library: %3 files written, %4 already up to date.")
                             .arg(target, m_sourceLabel)
                             .arg(o.restore.filesWritten)
                             .arg(o.restore.filesUnchanged));
        emit actionFeedback(m_statusMessage, false);
        break;
    case CloneStickOutcome::Status::ClonedWithProblems:
        setStatusMessage(QStringLiteral("%1 was written, but with problems; see the report below.").arg(target));
        emit actionFeedback(m_statusMessage, true);
        break;
    case CloneStickOutcome::Status::Cancelled:
        setStatusMessage(QString::fromStdString(o.message));
        break;
    case CloneStickOutcome::Status::Refused:
    case CloneStickOutcome::Status::BackupIncomplete:
    case CloneStickOutcome::Status::RestoreFailed:
        setErrorMessage(QString::fromStdString(o.message));
        emit actionFeedback(m_errorMessage, true);
        break;
    }
    refresh();
}

void CloneStickController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void CloneStickController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
