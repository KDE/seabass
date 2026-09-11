// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>

#include <cstdint>
#include <string>
#include <vector>

#include "application/ports/backup_store.hpp"
#include "application/ports/cancellation_token.hpp"
#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

// Read-only Qt list model over the backups BackupsController last listed.
class BackupListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by BackupsController; not constructible from QML")

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        LabelRole,
        DescriptionRole,
        SizeHumanRole,
        SizeBytesRole,
        FileNamesRole,
    };

    explicit BackupListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRecords(std::vector<application::BackupRecord> records);

private:
    std::vector<application::BackupRecord> m_records;
};

// What a background task should do before re-listing the directory -- see
// BackupsController::startTask().
enum class BackupsAction { Load, Clean, SetDescription, Restore, Delete };

// Result of a background task -- see BackupsAction and
// BackupsController::startTask(). Built entirely on a worker thread, with
// no access to the controller: every operation ends by re-listing the
// directory, so the controller always has a fresh, consistent view once
// the task returns.
struct BackupsTaskResult
{
    std::vector<application::BackupRecord> records;
    QString totalSizeHuman;
    QString backupDir;
    QString errorMessage;  // empty on success
    QString statusMessage;
    // The write summary ("3 of 7 backups deleted"), shown by the page
    // after every mutating action except a description edit.
    bool showSummary = false;
    int written = 0;
    int total = 0;
    QString unit;
    QString verb;
    bool cancelled = false;
};

// Wraps FilesystemBackupStore for QML: lists the backups made under a
// stick's Seabass/backups directory (shared across rekordbox/Engine on
// that stick, see backupDirFor() in cli/main.cpp), prunes old ones,
// restores/deletes individual ones, and edits their descriptions. Every
// operation -- even a plain list -- is disk I/O against a directory that
// can hold many, possibly large, backup copies, so (like every other
// write-capable controller) it runs on a background thread via
// QtConcurrent rather than ever blocking the UI thread.
//
// Every mutating action is a direct write on the stick's library (its
// backup archive is part of the library, see docs/edit-mode-and-cancel.md):
// it takes the library's edit lock for its duration (lockRefused() when
// another instance holds it) and ends with a writeFinished() summary.
// Clean Up deletes one backup at a time and can be cancelled between
// two; a single restore or delete is one step and cannot.
class BackupsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::BackupListModel *backups READ backupsModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY busyChanged)
    Q_PROPERTY(bool writeCancellable READ writeCancellable NOTIFY busyChanged)
    Q_PROPERTY(int writeCurrent READ writeCurrent NOTIFY writeProgressChanged)
    Q_PROPERTY(int writeTotal READ writeTotal NOTIFY writeProgressChanged)
    Q_PROPERTY(QString libraryId READ libraryId NOTIFY backupsChanged)
    Q_PROPERTY(QString totalSizeHuman READ totalSizeHuman NOTIFY backupsChanged)
    Q_PROPERTY(QString backupDir READ backupDir NOTIFY backupsChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit BackupsController(QObject *parent = nullptr);

    BackupListModel *backupsModel() { return &m_model; }
    bool busy() const { return m_busy; }
    bool writing() const { return m_busy && m_writing; }
    bool writeCancellable() const { return writing() && m_cancellable && !m_cancel.cancelled(); }
    int writeCurrent() const { return m_writeCurrent; }
    int writeTotal() const { return m_writeTotal; }
    QString libraryId() const { return m_libraryId; }
    QString totalSizeHuman() const { return m_totalSizeHuman; }
    QString backupDir() const { return m_backupDir; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }

    // rekordboxPath/enginePath: pass whichever are non-empty (at least one
    // must be) -- the backup directory is shared per stick, not per format.
    // stickLabel only names the holder in the lock cookie other instances
    // see; it may be empty.
    Q_INVOKABLE void load(const QString &rekordboxPath, const QString &enginePath, const QString &stickLabel = {});
    Q_INVOKABLE void clean(int keepCount);
    Q_INVOKABLE void setDescription(const QString &id, const QString &description);

    // Overwrites the backup's original files with this backup's copies
    // (the current files are themselves backed up first -- see
    // BackupStore::restore()).
    Q_INVOKABLE void restoreBackup(const QString &id);

    // Permanently deletes a single backup.
    Q_INVOKABLE void deleteBackup(const QString &id);

    // Stops a running Clean Up after the backup being deleted right now.
    Q_INVOKABLE void cancelWrite();

signals:
    void backupsChanged();
    void busyChanged();
    void writeProgressChanged();
    // {written, total, unit, verb, cancelled, error}, the
    // OperationSummaryDialog shape.
    void writeFinished(const QVariantMap &summary);
    // Another instance is editing this library; nothing was done.
    void lockRefused(const QVariantMap &holder);
    void errorMessageChanged();
    void statusMessageChanged();

private:
    void startTask(BackupsAction action, int keepCount, const QString &id, const QString &description);
    void onTaskFinished();
    void setBusy(bool busy);
    void setWriteProgress(int current, int total);
    std::shared_ptr<QtProgressReporter> makeReporter();
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);

    BackupListModel m_model;
    QFutureWatcher<BackupsTaskResult> m_watcher;
    QString m_rekordboxPath;
    QString m_enginePath;
    QString m_stickLabel;
    QString m_libraryId;
    bool m_busy = false;
    bool m_writing = false;
    bool m_cancellable = false;
    bool m_holdsDirectWrite = false;
    application::CancellationToken m_cancel;
    int m_writeCurrent = 0;
    int m_writeTotal = 0;
    QString m_totalSizeHuman;
    QString m_backupDir;
    QString m_errorMessage;
    QString m_statusMessage;
};

}  // namespace seabass::gui
