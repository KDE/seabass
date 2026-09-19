// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariantMap>

#include <functional>
#include <memory>

#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/compact_stick_backup.hpp"
#include "gui/edit/direct_write_hold.hpp"

namespace seabass::gui
{

// Wraps application::BackupStick / CompactStickBackup for the per-stick
// "Full Stick Backup" page (StickBackupPage.qml). Every operation runs on
// a worker thread; progress arrives through queued calls back onto this
// object. The page never has to know a file path: configure() derives the
// stick root from the paths the hub already has and the archive location
// from the app setting.
//
// Untyped `controller` property on the page side, so tst_StickBackupPage
// .qml can stand in a plain JS object for all of this.
class StickBackupController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString stickLabel READ stickLabel NOTIFY configuredChanged)
    Q_PROPERTY(QString stickRoot READ stickRoot NOTIFY configuredChanged)
    Q_PROPERTY(QString archivePath READ archivePath NOTIFY configuredChanged)
    // What this backup is called. Set before a run and it is written into
    // the new generation; edited on a stick that already has a backup and
    // the next update carries the change. Display and identity are
    // separate on purpose: the archive is still found by its path and its
    // stick, never by this.
    Q_PROPERTY(QString backupName READ backupName WRITE setBackupName NOTIFY backupNameChanged)
    // Non-empty when the plain "<label>.zip" is already another stick's
    // backup: the label of that other stick. The archive path has been
    // moved to the next free name, and this is what to say about it.
    Q_PROPERTY(QString nameCollidedWith READ nameCollidedWith NOTIFY configuredChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool backingUp READ backingUp NOTIFY busyChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY busyChanged)
    Q_PROPERTY(QString activity READ activity NOTIFY busyChanged)  // "", "backup", "verify", "compact", "decide"
    // Progress of the running backup / verify / compaction.
    Q_PROPERTY(QString phase READ phase NOTIFY progressChanged)
    Q_PROPERTY(qlonglong filesDone READ filesDone NOTIFY progressChanged)
    Q_PROPERTY(qlonglong filesTotal READ filesTotal NOTIFY progressChanged)
    Q_PROPERTY(qlonglong bytesDone READ bytesDone NOTIFY progressChanged)
    Q_PROPERTY(qlonglong bytesTotal READ bytesTotal NOTIFY progressChanged)
    Q_PROPERTY(double bytesPerSecond READ bytesPerSecond NOTIFY progressChanged)
    Q_PROPERTY(int etaSeconds READ etaSeconds NOTIFY progressChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY progressChanged)
    // What the page shows when idle (all QVariantMaps so a test can fake
    // them as plain objects): the previous backup, the pending changes,
    // the dead-space report.
    Q_PROPERTY(QVariantMap lastBackup READ lastBackup NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap sinceLastBackup READ sinceLastBackup NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap deadSpace READ deadSpace NOTIFY previewChanged)
    // "" or the name of the DJ software that blocks a backup right now.
    Q_PROPERTY(QString blockedBy READ blockedBy NOTIFY previewChanged)
    // The stick is mounted read-only: this backup is still worth making
    // -- reading is all a backup does -- but what comes off a damaged
    // filesystem is whatever survived, so the page says so before the
    // press and the archive carries the mark afterwards.
    Q_PROPERTY(bool stickReadOnly READ stickReadOnly NOTIFY previewChanged)
    Q_PROPERTY(bool pendingCancelDecision READ pendingCancelDecision NOTIFY pendingCancelDecisionChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit StickBackupController(QObject *parent = nullptr);
    ~StickBackupController() override;

    QString stickLabel() const { return m_stickLabel; }
    QString stickRoot() const { return m_stickRoot; }
    QString archivePath() const { return m_archivePath; }
    QString backupName() const { return m_backupName; }
    QString nameCollidedWith() const { return m_nameCollidedWith; }
    void setBackupName(const QString &name);
    bool renameArchiveTo(const QString &target);
    bool busy() const { return !m_activity.isEmpty(); }
    bool backingUp() const { return m_activity == QStringLiteral("backup"); }
    bool previewing() const { return m_previewing; }
    QString activity() const { return m_activity; }
    QString phase() const { return m_phase; }
    qlonglong filesDone() const { return m_filesDone; }
    qlonglong filesTotal() const { return m_filesTotal; }
    qlonglong bytesDone() const { return m_bytesDone; }
    qlonglong bytesTotal() const { return m_bytesTotal; }
    double bytesPerSecond() const { return m_bytesPerSecond; }
    int etaSeconds() const { return m_etaSeconds; }
    QString currentFile() const { return m_currentFile; }
    QVariantMap lastBackup() const { return m_lastBackup; }
    QVariantMap sinceLastBackup() const { return m_sinceLastBackup; }
    QVariantMap deadSpace() const { return m_deadSpace; }
    QString blockedBy() const { return m_blockedBy; }
    bool stickReadOnly() const { return m_stickReadOnly; }
    bool pendingCancelDecision() const { return m_pending != nullptr; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }

    // rekordboxPath/enginePath are the per-stick paths every hub page
    // already carries (PIONEER/ and Engine Library/); either's parent is
    // the stick root. backupDirectory is AppSettingsController's
    // stickBackupDirectory. Triggers refresh().
    Q_INVOKABLE void configure(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath,
                               const QString &backupDirectory);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void backUp();
    Q_INVOKABLE void cancel();
    // Renders this archive's history to a text file and hands it to the
    // system's editor. Reports through actionFeedback rather than
    // returning: the interesting failures ("no history yet", "cannot be
    // read") are things to say, not values to branch on.
    Q_INVOKABLE void openChangelog();
    // Take the colliding name after all, by deleting the other stick's
    // archive first. Destructive and explicitly asked for: updating it
    // in place would be worse, because the diff would record every file
    // of the other stick as removed and call the result a backup.
    Q_INVOKABLE void replaceCollidingBackup();
    // Re-runs the action lockRefused() stopped, after "Remove Lock".
    Q_INVOKABLE void retryLockedAction() { m_writeHold.retryLockedAction(); }
    Q_INVOKABLE void keepPartial();
    Q_INVOKABLE void discardPartial();
    Q_INVOKABLE void verify();
    // Removes this stick's backup archive and its journal. Offered after
    // a failed verify; takes the stick's edit lock like any other write to
    // the archive.
    Q_INVOKABLE void deleteBackup();
    // Synchronous and cheap (reads the central directory only) -- the
    // numbers the compaction dialog shows before the user commits.
    Q_INVOKABLE QVariantMap compactionPreflight();
    Q_INVOKABLE void compact();
    Q_INVOKABLE void openArchiveFolder();

signals:
    void configuredChanged();
    void backupNameChanged();
    void busyChanged();
    void progressChanged();
    void previewChanged();
    void pendingCancelDecisionChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    // Fires on every outcome. A signal rather than a diffed property: the
    // same message twice in a row is two outcomes, and a property that did
    // not change would announce the second one to nobody.
    void actionFeedback(const QString &message, bool isError);
    // Another instance is editing this stick's library (its archive on
    // disk is part of it); nothing was started. retryLockedAction()
    // re-runs the refused action after "Remove Lock".
    void lockRefused(const QVariantMap &holder, const QString &libraryId);
    // Verify found the backup unreadable or its files not matching what
    // was written. `detail` is one sentence naming what failed. Not raised
    // for a verify the user cancelled.
    void verifyFailed(const QString &detail);

private:
    // Takes this stick's edit lock for a run; emits lockRefused() and
    // returns false when another instance holds it.
    bool enterDirectWrite(std::function<void()> retry);

    struct PreviewResult;
    struct RunResult;

    application::BackupStickOptions baseOptions() const;
    void setActivity(const QString &activity);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);
    void applyProgress(const application::BackupProgress &progress);
    void applySimpleProgress(const QString &phase, qlonglong bytesDone, qlonglong bytesTotal);
    void resetProgress();
    void onPreviewFinished();
    void onRunFinished();
    void finishOutcome(const application::BackupStickOutcome &outcome);

    QString m_stickLabel;
    QString m_stickRoot;
    QString m_archivePath;
    QString m_backupName;
    QString m_nameCollidedWith;
    // How many names were tried: 1 is the plain one. Kept so a refresh
    // does not walk the sequence again from the start each time.
    int m_archiveAttempt = 1;
    QString m_backupDirectory;
    // The name the archive on disk already carries, so an edit can be
    // told apart from a page that simply loaded.
    QString m_savedBackupName;
    DirectWriteHold m_writeHold;
    QString m_stickIdentifier;
    QString m_rekordboxPath;
    QString m_enginePath;
    QString m_activity;
    bool m_previewing = false;
    QString m_phase;
    qlonglong m_filesDone = 0;
    qlonglong m_filesTotal = 0;
    qlonglong m_bytesDone = 0;
    qlonglong m_bytesTotal = 0;
    double m_bytesPerSecond = 0.0;
    int m_etaSeconds = -1;
    QString m_currentFile;
    QElapsedTimer m_progressClock;
    qint64 m_lastProgressMs = 0;
    qlonglong m_lastProgressBytes = 0;
    QVariantMap m_lastBackup;
    QVariantMap m_sinceLastBackup;
    QVariantMap m_deadSpace;
    QString m_blockedBy;
    bool m_stickReadOnly = false;
    QString m_errorMessage;
    QString m_statusMessage;
    application::CancellationToken m_cancel;
    std::unique_ptr<application::PendingBackup> m_pending;
    QFutureWatcher<std::shared_ptr<PreviewResult>> m_previewWatcher;
    QFutureWatcher<std::shared_ptr<RunResult>> m_runWatcher;
};

}  // namespace seabass::gui
