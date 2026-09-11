// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>

#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "gui/edit/direct_write_hold.hpp"

namespace seabass::gui
{

// Wraps application::RestoreStickBackup for RestoreStickBackupPage.qml --
// the top-level "Restore a Stick Backup" flow (pick a backup file, pick a
// mounted drive, see what will happen, confirm). `disks` is a plain
// QVariantList like FormatUsbController's, for the same testability
// reason; a drive is a valid target only when mounted with a filesystem.
class RestoreStickBackupController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList disks READ disks NOTIFY disksChanged)
    Q_PROPERTY(QString archivePath READ archivePath WRITE setArchivePath NOTIFY archivePathChanged)
    Q_PROPERTY(QString defaultBackupDirectory READ defaultBackupDirectory WRITE setDefaultBackupDirectory NOTIFY
                   defaultBackupDirectoryChanged)
    // Every backup Seabass finds in defaultBackupDirectory, newest first
    // (see RestoreStickBackup::describeAll); each a map with archivePath,
    // fileName, label, identifier, status, createdAt, entries, bytes and
    // error (non-empty when the file could not be read as a backup).
    Q_PROPERTY(QVariantList knownBackups READ knownBackups NOTIFY knownBackupsChanged)
    Q_PROPERTY(bool listingBackups READ listingBackups NOTIFY knownBackupsChanged)
    Q_PROPERTY(QVariantMap archiveInfo READ archiveInfo NOTIFY archiveInfoChanged)
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool restoring READ restoring NOTIFY busyChanged)
    Q_PROPERTY(bool analyzing READ analyzing NOTIFY busyChanged)
    Q_PROPERTY(bool mounting READ mounting NOTIFY busyChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY progressChanged)
    Q_PROPERTY(qlonglong filesDone READ filesDone NOTIFY progressChanged)
    Q_PROPERTY(qlonglong filesTotal READ filesTotal NOTIFY progressChanged)
    Q_PROPERTY(qlonglong bytesDone READ bytesDone NOTIFY progressChanged)
    Q_PROPERTY(qlonglong bytesTotal READ bytesTotal NOTIFY progressChanged)
    Q_PROPERTY(double bytesPerSecond READ bytesPerSecond NOTIFY progressChanged)
    Q_PROPERTY(int etaSeconds READ etaSeconds NOTIFY progressChanged)  // -1: unknown yet
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY progressChanged)
    Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit RestoreStickBackupController(QObject *parent = nullptr);
    ~RestoreStickBackupController() override;

    QVariantList disks() const { return m_disks; }
    QString archivePath() const { return m_archivePath; }
    void setArchivePath(const QString &path);
    QString defaultBackupDirectory() const { return m_defaultBackupDirectory; }
    void setDefaultBackupDirectory(const QString &directory);
    QVariantList knownBackups() const { return m_knownBackups; }
    bool listingBackups() const { return m_listWatcher.isRunning(); }
    QVariantMap archiveInfo() const { return m_archiveInfo; }
    QVariantMap preview() const { return m_preview; }
    bool busy() const { return m_restoring || m_analyzing || m_mounting; }
    bool restoring() const { return m_restoring; }
    bool analyzing() const { return m_analyzing; }
    bool mounting() const { return m_mounting; }
    QString phase() const { return m_phase; }
    qlonglong filesDone() const { return m_filesDone; }
    qlonglong filesTotal() const { return m_filesTotal; }
    qlonglong bytesDone() const { return m_bytesDone; }
    qlonglong bytesTotal() const { return m_bytesTotal; }
    double bytesPerSecond() const { return m_bytesPerSecond; }
    int etaSeconds() const { return m_etaSeconds; }
    QString currentFile() const { return m_currentFile; }
    QVariantMap result() const { return m_result; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }

    // Re-enumerates removable drives (synchronous, cheap).
    Q_INVOKABLE void refresh();
    // Reads the archive's manifest (background) and, when `targetRoot` is
    // a directory, what restoring onto it would do.
    Q_INVOKABLE void analyze(const QString &targetRoot);
    Q_INVOKABLE void restore(const QString &targetRoot, bool exact);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void retryLockedAction() { m_writeHold.retryLockedAction(); }
    // Drops the last restore's report and messages (the page's "Start Over").
    Q_INVOKABLE void clearResult();
    // Full path of the archive a stick with this label would have in the
    // default directory -- the per-stick page's shortcut.
    Q_INVOKABLE QString archivePathForLabel(const QString &label) const;
    // Re-reads defaultBackupDirectory (background); runs by itself whenever
    // that directory changes.
    Q_INVOKABLE void refreshKnownBackups();
    // Mounts a formatted-but-unmounted drive so it can become the target
    // (a stick fresh out of Format USB Stick is not remounted by the
    // formatter). Background; refreshes disks and emits driveMounted on
    // success.
    Q_INVOKABLE void mount(const QString &devicePath);

signals:
    void disksChanged();
    void archivePathChanged();
    void defaultBackupDirectoryChanged();
    void knownBackupsChanged();
    void driveMounted(const QString &mountPoint);
    void archiveInfoChanged();
    // Another instance is editing the target stick's library, or the
    // library the archive came from; nothing was started.
    void lockRefused(const QVariantMap &holder, const QString &libraryId);
    void previewChanged();
    void busyChanged();
    void progressChanged();
    void resultChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    void actionFeedback(const QString &message, bool isError);

private:
    struct AnalyzeResult;
    struct RestoreResult;
    struct MountResult;

    void onAnalyzeFinished();
    void onRestoreFinished();
    void onListFinished();
    void onMountFinished();
    void applyProgress(const application::RestoreProgress &progress);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);

    QVariantList m_disks;
    QString m_archivePath;
    QString m_defaultBackupDirectory;
    QVariantMap m_archiveInfo;
    QVariantMap m_preview;
    QVariantMap m_result;
    QVariantList m_knownBackups;
    bool m_restoring = false;
    QString m_restoreTarget;  // where the last restore() wrote, for cache invalidation
    DirectWriteHold m_writeHold;
    bool m_analyzing = false;
    bool m_mounting = false;
    QString m_phase;
    qlonglong m_filesDone = 0;
    qlonglong m_filesTotal = 0;
    qlonglong m_bytesDone = 0;
    qlonglong m_bytesTotal = 0;
    double m_bytesPerSecond = 0.0;
    int m_etaSeconds = -1;
    QElapsedTimer m_progressClock;
    qint64 m_lastProgressMs = 0;
    qlonglong m_lastProgressBytes = 0;
    QString m_currentFile;
    QString m_errorMessage;
    QString m_statusMessage;
    // An analyze() requested while one is running; re-run when it ends
    // rather than dropped (the archive or drive changed under it).
    std::optional<QString> m_pendingAnalyzeTarget;
    application::CancellationToken m_cancel;
    QFutureWatcher<std::shared_ptr<AnalyzeResult>> m_analyzeWatcher;
    QFutureWatcher<std::shared_ptr<RestoreResult>> m_restoreWatcher;
    QFutureWatcher<QVariantList> m_listWatcher;
    QFutureWatcher<std::shared_ptr<MountResult>> m_mountWatcher;
};

}  // namespace seabass::gui
