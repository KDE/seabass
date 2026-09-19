// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "application/use_cases/manage_stick_backups.hpp"

namespace seabass::gui
{

// Manage Backups: the full stick backups in the backup folder, listed and
// deleted off the GUI thread. Browsing one is MediaController::openBackup;
// this only says which archives are open for browsing, so it never
// deletes one out from under the browsed library.
class FullBackupsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString backupDirectory READ backupDirectory WRITE setBackupDirectory NOTIFY backupDirectoryChanged)
    // The archive the stick being looked at relates to; listed first.
    Q_PROPERTY(QString currentArchivePath READ currentArchivePath WRITE setCurrentArchivePath NOTIFY
                   currentArchivePathChanged)
    // Archives open as browsed libraries right now (see browsedArchiveFor).
    Q_PROPERTY(QStringList openArchivePaths READ openArchivePaths WRITE setOpenArchivePaths NOTIFY
                   openArchivePathsChanged)
    Q_PROPERTY(QVariantList backups READ backups NOTIFY backupsChanged)
    Q_PROPERTY(qlonglong totalBytes READ totalBytes NOTIFY backupsChanged)
    Q_PROPERTY(bool listing READ listing NOTIFY busyChanged)
    Q_PROPERTY(bool deleting READ deleting NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY messagesChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY messagesChanged)

public:
    explicit FullBackupsController(QObject *parent = nullptr);
    ~FullBackupsController() override;

    QString backupDirectory() const { return m_backupDirectory; }
    void setBackupDirectory(const QString &directory);
    QString currentArchivePath() const { return m_currentArchivePath; }
    void setCurrentArchivePath(const QString &path);
    QStringList openArchivePaths() const { return m_openArchivePaths; }
    void setOpenArchivePaths(const QStringList &paths);
    QVariantList backups() const { return m_backups; }
    qlonglong totalBytes() const { return m_totalBytes; }
    bool listing() const { return m_listWatcher.isRunning(); }
    bool deleting() const { return m_deleteWatcher.isRunning(); }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void deleteBackup(const QString &archivePath);
    // The same history file as the stick's own Backup page writes, from
    // the same renderer, so the two cannot disagree about one archive.
    Q_INVOKABLE void openChangelog(const QString &archivePath);
    // The archive a browsed library was extracted from, canonical; empty
    // for any other library root.
    Q_INVOKABLE QString browsedArchiveFor(const QString &libraryRoot) const;
    Q_INVOKABLE bool isOpen(const QString &archivePath) const;

signals:
    void backupDirectoryChanged();
    void currentArchivePathChanged();
    void openArchivePathsChanged();
    void backupsChanged();
    void busyChanged();
    void messagesChanged();
    void backupDeleted(const QString &archivePath);

private:
    void onListFinished();
    void onDeleteFinished();
    void setMessages(const QString &error, const QString &status);

    QString m_backupDirectory;
    QString m_currentArchivePath;
    QStringList m_openArchivePaths;
    QVariantList m_backups;
    qlonglong m_totalBytes = 0;
    QString m_errorMessage;
    QString m_statusMessage;
    QString m_deletingPath;
    bool m_refreshAgain = false;
    QFutureWatcher<QVariantList> m_listWatcher;
    QFutureWatcher<application::DeleteStickBackupResult> m_deleteWatcher;
};

}  // namespace seabass::gui
