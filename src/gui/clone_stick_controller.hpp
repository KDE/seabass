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

#include <memory>

#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/clone_stick.hpp"
#include "gui/edit/direct_write_hold.hpp"

namespace seabass::gui
{

// Wraps application::CloneStick for CloneStickPage.qml -- "Create Backup
// USB Stick from X" onto an empty stick, and "Update this stick from X"
// onto a stick that already holds an older copy of the same library.
// Two stages on one worker: the source's incremental full backup into
// its own archive, then a restore of that archive onto the target.
//
// Untyped `controller` property on the page side, so tst_CloneStickPage
// .qml can stand in a plain JS object for all of this.
class CloneStickController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY configuredChanged)
    Q_PROPERTY(QString sourceRoot READ sourceRoot NOTIFY configuredChanged)
    Q_PROPERTY(QString targetLabel READ targetLabel NOTIFY configuredChanged)
    Q_PROPERTY(QString targetRoot READ targetRoot NOTIFY configuredChanged)
    Q_PROPERTY(QString archivePath READ archivePath NOTIFY configuredChanged)
    // What the run would do (see CloneStickPreview): error, ready,
    // archiveExists, archiveCurrent, added, changed, removed,
    // databaseChanged, bytesToRead, sourceBytes, backupFreeBytes,
    // enoughBackupSpace, bytesToTarget, targetFreeBytes,
    // enoughTargetSpace, targetHasEngineLibrary, restoreKnown,
    // restoreFilesToWrite, restoreExtras.
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
    // "" or the name of the DJ software that blocks a run right now.
    Q_PROPERTY(QString blockedBy READ blockedBy NOTIFY previewChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY busyChanged)
    Q_PROPERTY(bool cloning READ cloning NOTIFY busyChanged)
    // "", "backup" or "restore"; `phase` is the stage's own phase name
    // (scanning / reading / database / writing / verifying, then
    // analyzing / writing / removing / checking).
    Q_PROPERTY(QString stage READ stage NOTIFY progressChanged)
    Q_PROPERTY(QString phase READ phase NOTIFY progressChanged)
    Q_PROPERTY(qlonglong filesDone READ filesDone NOTIFY progressChanged)
    Q_PROPERTY(qlonglong filesTotal READ filesTotal NOTIFY progressChanged)
    Q_PROPERTY(qlonglong bytesDone READ bytesDone NOTIFY progressChanged)
    Q_PROPERTY(qlonglong bytesTotal READ bytesTotal NOTIFY progressChanged)
    Q_PROPERTY(double bytesPerSecond READ bytesPerSecond NOTIFY progressChanged)
    Q_PROPERTY(int etaSeconds READ etaSeconds NOTIFY progressChanged)  // -1: unknown yet
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY progressChanged)
    // The last run's report: status, message, backupStatus, backupSkipped,
    // backupBytesRead, restoreStarted, and -- when the restore step ran --
    // the same keys RestoreStickBackupController's result has.
    Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit CloneStickController(QObject *parent = nullptr);
    ~CloneStickController() override;

    QString sourceLabel() const { return m_sourceLabel; }
    QString sourceRoot() const { return m_sourceRoot; }
    QString targetLabel() const { return m_targetLabel; }
    QString targetRoot() const { return m_targetRoot; }
    QString archivePath() const { return m_archivePath; }
    QVariantMap preview() const { return m_preview; }
    QString blockedBy() const { return m_blockedBy; }
    bool busy() const { return m_previewing || m_cloning; }
    bool previewing() const { return m_previewing; }
    bool cloning() const { return m_cloning; }
    QString stage() const { return m_stage; }
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

    // sourceRekordboxPath / sourceEnginePath are the source stick's
    // PIONEER/ and Engine Library/ folders (either may be empty); the
    // source root is their parent. The archive is the source's own, in
    // backupDirectory (AppSettingsController::stickBackupDirectory).
    // Triggers refresh().
    Q_INVOKABLE void configure(const QString &sourceLabel, const QString &sourceRekordboxPath,
                               const QString &sourceEnginePath, const QString &targetMountPoint,
                               const QString &targetLabel, const QString &backupDirectory);
    // Recomputes the preview (background).
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void start(bool exact);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void retryLockedAction() { m_writeHold.retryLockedAction(); }
    // Drops the last run's report and messages (the page's "Start Over").
    Q_INVOKABLE void clearResult();

signals:
    void configuredChanged();
    void previewChanged();
    void busyChanged();
    void progressChanged();
    void resultChanged();
    // Another instance is editing the source or the target stick's
    // library; nothing was started.
    void lockRefused(const QVariantMap &holder, const QString &libraryId);
    void errorMessageChanged();
    void statusMessageChanged();
    void actionFeedback(const QString &message, bool isError);

private:
    struct PreviewResult;
    struct RunResult;

    application::CloneStickOptions baseOptions() const;
    void onPreviewFinished();
    void onRunFinished();
    void applyProgress(const application::CloneProgress &progress);
    void resetProgress();
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);

    QString m_sourceLabel;
    QString m_sourceRoot;
    QString m_sourceRekordboxPath;
    QString m_sourceEnginePath;
    QString m_targetLabel;
    QString m_targetRoot;
    DirectWriteHold m_writeHold;
    QString m_archivePath;
    QVariantMap m_preview;
    QString m_blockedBy;
    bool m_previewing = false;
    bool m_cloning = false;
    QString m_stage;
    QString m_phase;
    qlonglong m_filesDone = 0;
    qlonglong m_filesTotal = 0;
    qlonglong m_bytesDone = 0;
    qlonglong m_bytesTotal = 0;
    double m_bytesPerSecond = 0.0;
    int m_etaSeconds = -1;
    QElapsedTimer m_progressClock;
    qint64 m_stageStartMs = 0;
    qint64 m_lastProgressMs = 0;
    qlonglong m_lastProgressBytes = 0;
    QString m_currentFile;
    QVariantMap m_result;
    QString m_errorMessage;
    QString m_statusMessage;
    bool m_refreshPending = false;
    application::CancellationToken m_cancel;
    QFutureWatcher<std::shared_ptr<PreviewResult>> m_previewWatcher;
    QFutureWatcher<std::shared_ptr<RunResult>> m_runWatcher;
};

}  // namespace seabass::gui
