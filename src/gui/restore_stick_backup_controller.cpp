// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/local_file_url.hpp"
#include "gui/library_catalog_cache.hpp"
#include "restore_stick_backup_controller.hpp"

#include <QDateTime>
#include <QDir>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <filesystem>

#include "gui/stick_backup_paths.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/future_result.hpp"
#include "gui/write_guard.hpp"
#include "infrastructure/engine/engine_restore_check.hpp"
#include "infrastructure/media/media_factory.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using application::DetectedStick;
using application::RestoreOptions;
using application::RestorePreview;
using application::RestoreProgress;
using application::RestoreStickBackup;
using application::RestoreSummary;

namespace
{

QVariantMap diskToVariant(const DetectedStick &disk)
{
    QVariantMap map;
    map["label"] = QString::fromStdString(disk.label);
    map["mountPoint"] = QString::fromStdString(disk.mountPoint);
    map["devicePath"] = QString::fromStdString(disk.devicePath);
    map["wholeDiskPath"] = QString::fromStdString(disk.wholeDiskPath);
    map["capacityBytes"] = static_cast<qlonglong>(disk.capacityBytes);
    map["mounted"] = disk.mounted;
    map["hasNoFilesystem"] = disk.hasNoFilesystem;
    map["hasDjLibrary"] = disk.rekordboxPath.has_value() || disk.enginePath.has_value();
    // Exposed so the result page can route "referenced tracks are
    // missing" straight to Library Health for this same stick, rather
    // than only naming the problem -- see RestoreStickBackupPage.qml's
    // onRepairLibraryRequested.
    map["rekordboxPath"] = disk.rekordboxPath ? QString::fromStdString(*disk.rekordboxPath) : QString();
    map["enginePath"] = disk.enginePath ? QString::fromStdString(*disk.enginePath) : QString();
    map["usable"] = disk.mounted && !disk.hasNoFilesystem && !disk.mountPoint.empty();
    QVariantList rootEntries;
    for (const auto &entry : disk.rootEntries) {
        rootEntries.push_back(QString::fromStdString(entry));
    }
    map["rootEntries"] = rootEntries;
    return map;
}

QString phaseName(RestoreProgress::Phase phase)
{
    switch (phase) {
    case RestoreProgress::Phase::Analyzing: return QStringLiteral("analyzing");
    case RestoreProgress::Phase::Writing: return QStringLiteral("writing");
    case RestoreProgress::Phase::Removing: return QStringLiteral("removing");
    case RestoreProgress::Phase::Checking: return QStringLiteral("checking");
    }
    return {};
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

struct RestoreStickBackupController::AnalyzeResult
{
    RestorePreview preview;
    bool targetGiven = false;
};

struct RestoreStickBackupController::RestoreResult
{
    QString refusal;
    RestoreSummary summary;
};

struct RestoreStickBackupController::MountResult
{
    QString devicePath;
    QString mountPoint;
    QString error;
};

RestoreStickBackupController::RestoreStickBackupController(QObject *parent) : QObject(parent)
{
    connect(&m_analyzeWatcher, &QFutureWatcher<std::shared_ptr<AnalyzeResult>>::finished, this,
            &RestoreStickBackupController::onAnalyzeFinished);
    connect(&m_restoreWatcher, &QFutureWatcher<std::shared_ptr<RestoreResult>>::finished, this,
            &RestoreStickBackupController::onRestoreFinished);
    connect(&m_listWatcher, &QFutureWatcher<QVariantList>::finished, this,
            &RestoreStickBackupController::onListFinished);
    connect(&m_mountWatcher, &QFutureWatcher<std::shared_ptr<MountResult>>::finished, this,
            &RestoreStickBackupController::onMountFinished);
    refresh();
}

RestoreStickBackupController::~RestoreStickBackupController()
{
    m_cancel.cancel();
    awaitQuietly(m_restoreWatcher);
    awaitQuietly(m_analyzeWatcher);
    awaitQuietly(m_listWatcher);
    awaitQuietly(m_mountWatcher);
}

void RestoreStickBackupController::refresh()
{
    auto locator = infrastructure::media::createRemovableMediaLocator();
    QVariantList disks;
    for (const auto &disk : locator->detect()) {
        disks.push_back(diskToVariant(disk));
    }
    m_disks = std::move(disks);
    emit disksChanged();
}

void RestoreStickBackupController::setArchivePath(const QString &path)
{
    const QString cleaned = localPathFromUrl(path);
    if (m_archivePath == cleaned) {
        return;
    }
    m_archivePath = cleaned;
    m_archiveInfo.clear();
    m_preview.clear();
    m_result.clear();
    emit archivePathChanged();
    emit archiveInfoChanged();
    emit previewChanged();
    emit resultChanged();
}

void RestoreStickBackupController::setDefaultBackupDirectory(const QString &directory)
{
    if (m_defaultBackupDirectory == directory) {
        return;
    }
    m_defaultBackupDirectory = directory;
    emit defaultBackupDirectoryChanged();
    refreshKnownBackups();
}

QString RestoreStickBackupController::archivePathForLabel(const QString &label) const
{
    return gui::archivePathForLabel(m_defaultBackupDirectory, label);
}

void RestoreStickBackupController::refreshKnownBackups()
{
    if (m_listWatcher.isRunning()) {
        return;
    }
    const fs::path directory(m_defaultBackupDirectory.toStdString());
    m_listWatcher.setFuture(QtConcurrent::run([directory]() {
        QVariantList backups;
        if (directory.empty()) {
            return backups;
        }
        for (const application::StickBackupDescription &d : application::RestoreStickBackup::describeAll(directory)) {
            QVariantMap map;
            map["archivePath"] = QString::fromStdString(d.archivePath.string());
            map["fileName"] = QString::fromStdString(d.archivePath.filename().string());
            map["error"] = QString::fromStdString(d.error);
            map["label"] = QString::fromStdString(d.stickLabel);
            map["identifier"] = QString::fromStdString(d.stickIdentifier);
            map["status"] = QString::fromUtf8(std::string(infrastructure::stick_backup::toString(d.status)).c_str());
            map["createdAt"] = QDateTime::fromSecsSinceEpoch(d.createdAtUnix).toString(Qt::ISODate);
            map["entries"] = static_cast<qlonglong>(d.entries);
            map["bytes"] = static_cast<qlonglong>(d.archiveBytes);
            backups.push_back(map);
        }
        return backups;
    }));
    emit knownBackupsChanged();
}

void RestoreStickBackupController::onListFinished()
{
    QString thrown;
    m_knownBackups = takeResult(m_listWatcher, &thrown);
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("Could not list the backup folder: ") + thrown);
    }
    emit knownBackupsChanged();
}

void RestoreStickBackupController::mount(const QString &devicePath)
{
    if (busy() || devicePath.isEmpty()) {
        return;
    }
    m_mounting = true;
    emit busyChanged();
    setErrorMessage({});
    m_mountWatcher.setFuture(QtConcurrent::run([devicePath]() {
        auto result = std::make_shared<MountResult>();
        result->devicePath = devicePath;
        auto mounter = infrastructure::media::createRemovableMediaMounter();
        std::string error;
        if (const std::optional<std::string> mountPoint = mounter->mount(devicePath.toStdString(), error)) {
            result->mountPoint = QString::fromStdString(*mountPoint);
        } else {
            result->error = QString::fromStdString(error);
        }
        return result;
    }));
}

void RestoreStickBackupController::onMountFinished()
{
    QString thrown;
    const std::shared_ptr<MountResult> result = takeResult(m_mountWatcher, &thrown);
    m_mounting = false;
    if (!thrown.isEmpty()) {
        setErrorMessage(thrown);
        emit actionFeedback(thrown, true);
    }
    emit busyChanged();
    if (!result) {
        return;
    }
    if (!result->error.isEmpty()) {
        setErrorMessage(QStringLiteral("Couldn't mount %1: %2").arg(result->devicePath, result->error.trimmed()));
        emit actionFeedback(m_errorMessage, true);
        return;
    }
    refresh();
    // udisksctl's reply may omit the mount point; the fresh disk list
    // knows it either way.
    QString mountPoint = result->mountPoint;
    if (mountPoint.isEmpty()) {
        for (const QVariant &disk : m_disks) {
            const QVariantMap map = disk.toMap();
            if (map["devicePath"].toString() == result->devicePath) {
                mountPoint = map["mountPoint"].toString();
                break;
            }
        }
    }
    emit driveMounted(mountPoint);
}

void RestoreStickBackupController::analyze(const QString &targetRoot)
{
    if (m_archivePath.isEmpty() || m_restoring) {
        return;
    }
    if (m_analyzeWatcher.isRunning()) {
        m_pendingAnalyzeTarget = targetRoot;
        return;
    }
    m_analyzing = true;
    emit busyChanged();
    RestoreOptions options;
    options.archivePath = fs::path(m_archivePath.toStdString());
    options.targetRoot = fs::path(targetRoot.toStdString());
    bool targetGiven = !targetRoot.isEmpty();
    m_analyzeWatcher.setFuture(QtConcurrent::run([options, targetGiven]() {
        auto result = std::make_shared<AnalyzeResult>();
        result->targetGiven = targetGiven;
        result->preview = RestoreStickBackup::preview(options);
        return result;
    }));
}

void RestoreStickBackupController::onAnalyzeFinished()
{
    QString thrown;
    std::shared_ptr<AnalyzeResult> result = takeResult(m_analyzeWatcher, &thrown);
    m_analyzing = false;
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("Could not read the backup: ") + thrown);
    }
    if (result) {
        const RestorePreview &p = result->preview;
        QVariantMap info;
        info["error"] = QString::fromStdString(p.error);
        info["label"] = QString::fromStdString(p.stickLabel);
        info["identifier"] = QString::fromStdString(p.stickIdentifier);
        info["status"] = QString::fromUtf8(std::string(infrastructure::stick_backup::toString(p.status)).c_str());
        info["createdAt"] = QDateTime::fromSecsSinceEpoch(p.createdAtUnix).toString(Qt::ISODate);
        info["entries"] = static_cast<qlonglong>(p.entries);
        info["bytes"] = static_cast<qlonglong>(p.bytes);
        info["rejectedCount"] = static_cast<qlonglong>(p.rejected.size());
        info["unreadableEntries"] = static_cast<qlonglong>(p.unreadableEntries);
        m_archiveInfo = info;
        QVariantMap preview;
        if (result->targetGiven && p.error.empty()) {
            preview["filesToWrite"] = static_cast<qlonglong>(p.filesToWrite);
            preview["filesUnchanged"] = static_cast<qlonglong>(p.filesUnchanged);
            preview["bytesToWrite"] = static_cast<qlonglong>(p.bytesToWrite);
            preview["extras"] = static_cast<qlonglong>(p.extras);
            preview["targetHasEngineLibrary"] = p.targetHasEngineLibrary;
            preview["freeBytes"] = static_cast<qlonglong>(p.freeBytesAtTarget);
            preview["enoughFreeSpace"] = p.enoughFreeSpace;
        }
        m_preview = preview;
        if (!p.error.empty()) {
            setErrorMessage(QString::fromStdString(p.error));
        } else {
            setErrorMessage({});
        }
    }
    emit archiveInfoChanged();
    emit previewChanged();
    emit busyChanged();
    if (m_pendingAnalyzeTarget) {
        const QString target = *m_pendingAnalyzeTarget;
        m_pendingAnalyzeTarget.reset();
        analyze(target);
    }
}

void RestoreStickBackupController::applyProgress(const RestoreProgress &progress)
{
    m_phase = phaseName(progress.phase);
    m_filesDone = static_cast<qlonglong>(progress.filesDone);
    m_filesTotal = static_cast<qlonglong>(progress.filesTotal);
    m_bytesDone = static_cast<qlonglong>(progress.bytesDone);
    m_bytesTotal = static_cast<qlonglong>(progress.bytesTotal);
    m_currentFile = QString::fromStdString(progress.currentFile);
    // Same sampling as StickBackupController: a one-second window for the
    // rate, no ETA before five seconds in (too jumpy until then).
    const qint64 now = m_progressClock.elapsed();
    if (now - m_lastProgressMs >= 1000) {
        const double seconds = static_cast<double>(now - m_lastProgressMs) / 1000.0;
        m_bytesPerSecond = static_cast<double>(m_bytesDone - m_lastProgressBytes) / seconds;
        m_lastProgressMs = now;
        m_lastProgressBytes = m_bytesDone;
    }
    m_etaSeconds = (progress.phase == RestoreProgress::Phase::Writing && m_bytesPerSecond > 0 && m_bytesTotal > m_bytesDone
                    && now > 5000)
                       ? static_cast<int>(static_cast<double>(m_bytesTotal - m_bytesDone) / m_bytesPerSecond)
                       : -1;
    emit progressChanged();
}

void RestoreStickBackupController::restore(const QString &targetRoot, bool exact)
{
    if (busy() || m_archivePath.isEmpty() || targetRoot.isEmpty()) {
        emit actionFeedback(QStringLiteral("Still busy. Try again once the current operation finishes."), true);
        return;
    }
    // Two libraries are involved: the target stick's (overwritten) and,
    // when the archive is of a library another instance may be editing
    // right now, that one's.
    const QString targetId = EditSessionRegistry::instance()->libraryIdForPath(targetRoot);
    const QString archiveId = m_archiveInfo.value("identifier").toString();
    if (auto refusal = m_writeHold.acquire({targetId, archiveId}, QString(),
                                          [this, targetRoot, exact] { restore(targetRoot, exact); })) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder, m_writeHold.refusedLibraryId());

        }
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_result.clear();
    emit resultChanged();
    m_restoring = true;
    emit busyChanged();
    m_phase.clear();
    m_filesDone = m_filesTotal = m_bytesDone = m_bytesTotal = 0;
    m_bytesPerSecond = 0.0;
    m_etaSeconds = -1;
    m_progressClock.start();
    m_lastProgressMs = 0;
    m_lastProgressBytes = 0;
    m_currentFile.clear();
    emit progressChanged();
    m_cancel = application::CancellationToken();
    RestoreOptions options;
    options.archivePath = fs::path(m_archivePath.toStdString());
    options.targetRoot = fs::path(targetRoot.toStdString());
    m_restoreTarget = targetRoot;
    options.exact = exact;
    options.cancel = m_cancel;
    options.libraryCheck = infrastructure::engine::checkRestoredEngineLibrary;
    QPointer<RestoreStickBackupController> self(this);
    auto lastPost = std::make_shared<std::chrono::steady_clock::time_point>();
    options.onProgress = [self, lastPost](const RestoreProgress &progress) {
        auto now = std::chrono::steady_clock::now();
        bool edge = progress.bytesDone == 0 || progress.bytesDone == progress.bytesTotal;
        if (!edge && now - *lastPost < std::chrono::milliseconds(100)) {
            return;
        }
        *lastPost = now;
        RestoreProgress copy = progress;
        QMetaObject::invokeMethod(
            self, [self, copy] {
                if (self) {
                    self->applyProgress(copy);
                }
            },
            Qt::QueuedConnection);
    };
    m_restoreWatcher.setFuture(QtConcurrent::run([options]() {
        auto result = std::make_shared<RestoreResult>();
        result->refusal = refuseIfDjSoftwareRunning();
        if (!result->refusal.isEmpty()) {
            return result;
        }
        result->summary = RestoreStickBackup::execute(options);
        return result;
    }));
}

void RestoreStickBackupController::cancel()
{
    m_cancel.cancel();
}

void RestoreStickBackupController::clearResult()
{
    if (m_restoring) {
        return;
    }
    m_result.clear();
    emit resultChanged();
    setErrorMessage({});
    setStatusMessage({});
}

void RestoreStickBackupController::onRestoreFinished()
{
    QString thrown;
    std::shared_ptr<RestoreResult> result = takeResult(m_restoreWatcher, &thrown);
    // A restore replaces the catalogs wholesale, so nothing that read them
    // before may be trusted afterwards.
    LibraryCatalogCache::instance().invalidateEveryCatalogOn(m_restoreTarget.toStdString());
    m_writeHold.release();
    m_restoring = false;
    if (!thrown.isEmpty()) {
        setErrorMessage(thrown);
        emit actionFeedback(thrown, true);
    }
    emit busyChanged();
    if (!result) {
        return;
    }
    if (!result->refusal.isEmpty()) {
        setErrorMessage(result->refusal);
        emit actionFeedback(result->refusal, true);
        return;
    }
    const RestoreSummary &s = result->summary;
    QVariantMap map;
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
    m_result = map;
    emit resultChanged();

    switch (s.status) {
    case RestoreSummary::Status::Restored:
        setStatusMessage(QStringLiteral("Restored %1 files (%2 already up to date).").arg(s.filesWritten).arg(s.filesUnchanged));
        emit actionFeedback(m_statusMessage, false);
        break;
    case RestoreSummary::Status::RestoredWithProblems:
        setStatusMessage(QStringLiteral("Restored %1 files, but with problems; see the report below.").arg(s.filesWritten));
        emit actionFeedback(m_statusMessage, true);
        break;
    case RestoreSummary::Status::Cancelled:
        setStatusMessage(QString::fromStdString(s.message));
        break;
    case RestoreSummary::Status::Failed:
        setErrorMessage(QString::fromStdString(s.message));
        emit actionFeedback(m_errorMessage, true);
        break;
    }
}

void RestoreStickBackupController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void RestoreStickBackupController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
