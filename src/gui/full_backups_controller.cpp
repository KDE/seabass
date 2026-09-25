// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "full_backups_controller.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>

#include "gui/backup_changelog_text.hpp"
#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <string>

#include "gui/future_result.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{

// One spelling per archive, so a path from the folder listing and one
// from a browsed library's marker compare equal.
QString canonical(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    return pathToQString(infrastructure::local::canonicalOrAbsolute(pathFromQString(path)));
}

}  // namespace

FullBackupsController::FullBackupsController(QObject *parent) : QObject(parent)
{
    connect(&m_listWatcher, &QFutureWatcher<QVariantList>::finished, this, &FullBackupsController::onListFinished);
    connect(&m_deleteWatcher, &QFutureWatcher<application::DeleteStickBackupResult>::finished, this,
            &FullBackupsController::onDeleteFinished);
}

FullBackupsController::~FullBackupsController()
{
    // Not the listing: it only reads the folder and captures nothing of
    // this object, so it can run out on its own with its result unused.
    // Waiting for it froze the window when the page was left mid-listing
    // on a slow disk. A delete is awaited -- Back is off while one runs,
    // so this only matters when the window closes.
    awaitQuietly(m_deleteWatcher);
}

void FullBackupsController::setBackupDirectory(const QString &directory)
{
    if (m_backupDirectory == directory) {
        return;
    }
    m_backupDirectory = directory;
    emit backupDirectoryChanged();
    refresh();
}

void FullBackupsController::setCurrentArchivePath(const QString &path)
{
    const QString cleaned = canonical(path);
    if (m_currentArchivePath == cleaned) {
        return;
    }
    m_currentArchivePath = cleaned;
    emit currentArchivePathChanged();
    refresh();
}

void FullBackupsController::setOpenArchivePaths(const QStringList &paths)
{
    QStringList cleaned;
    for (const QString &path : paths) {
        if (!path.isEmpty()) {
            cleaned.push_back(canonical(path));
        }
    }
    if (m_openArchivePaths == cleaned) {
        return;
    }
    m_openArchivePaths = cleaned;
    emit openArchivePathsChanged();
}

void FullBackupsController::refresh()
{
    // Nothing to list until there is a folder -- and the page hands over the
    // current archive before the folder, so listing here would read an empty
    // folder and then the real one straight after.
    if (m_backupDirectory.isEmpty()) {
        return;
    }
    if (m_listing) {
        // The folder or the current stick changed mid-listing: list again
        // once this one lands rather than showing a stale answer.
        m_refreshAgain = true;
        return;
    }
    const fs::path directory = pathFromQString(m_backupDirectory);
    const fs::path current = pathFromQString(m_currentArchivePath);
    m_listWatcher.setFuture(QtConcurrent::run([directory, current]() {
        QVariantList backups;
        for (const application::ManagedStickBackup &backup : application::ManageStickBackups::list(directory, current)) {
            const application::StickBackupDescription &d = backup.description;
            QVariantMap map;
            map["archivePath"] = canonical(pathToQString(d.archivePath));
            map["fileName"] = pathToQString(d.archivePath.filename());
            map["error"] = QString::fromStdString(d.error);
            map["label"] = QString::fromStdString(d.stickLabel);
            map["identifier"] = QString::fromStdString(d.stickIdentifier);
            map["status"] = QString::fromUtf8(std::string(infrastructure::stick_backup::toString(d.status)).c_str());
            map["createdAt"] = d.createdAtUnix > 0 ? QDateTime::fromSecsSinceEpoch(d.createdAtUnix).toString(Qt::ISODate)
                                                   : QString();
            map["bytes"] = static_cast<qlonglong>(d.archiveBytes);
            map["entries"] = static_cast<qlonglong>(d.entries);
            map["trackCount"] = backup.trackCount ? static_cast<qlonglong>(*backup.trackCount) : -1;
            map["playlistCount"] = backup.playlistCount ? static_cast<qlonglong>(*backup.playlistCount) : -1;
            map["isCurrentStick"] = backup.isCurrentStick;
            map["sourceReadOnly"] = d.sourceReadOnly;
            map["name"] = QString::fromStdString(d.userName);
            if (!d.error.empty()) {
                // describe() leaves the size unset for an unreadable file;
                // it still takes up space, and that is worth saying.
                map["bytes"] = static_cast<qlonglong>(QFileInfo(pathToQString(d.archivePath)).size());
            }
            backups.push_back(map);
        }
        return backups;
    }));
    m_listing = true;
    emit busyChanged();
}

void FullBackupsController::onListFinished()
{
    QString thrown;
    m_backups = takeResult(m_listWatcher, &thrown);
    m_totalBytes = 0;
    for (const QVariant &backup : std::as_const(m_backups)) {
        m_totalBytes += backup.toMap().value(QStringLiteral("bytes")).toLongLong();
    }
    if (!thrown.isEmpty()) {
        setMessages(QStringLiteral("Could not list the backup folder: ") + thrown, m_statusMessage);
    }
    m_listing = false;
    emit backupsChanged();
    emit busyChanged();
    if (m_refreshAgain) {
        m_refreshAgain = false;
        refresh();
    }
}

void FullBackupsController::openChangelog(const QString &archivePath)
{
    if (archivePath.isEmpty()) {
        return;
    }
    QString error;
    const QString path = writeChangelogFile(pathFromQString(canonical(archivePath)), &error);
    if (path.isEmpty()) {
        // setMessages is this page's only way to speak; an error here is
        // never fatal, so it reads as a note rather than a failure state.
        setMessages(error, {});
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void FullBackupsController::deleteBackup(const QString &archivePath)
{
    if (m_deleteWatcher.isRunning() || archivePath.isEmpty()) {
        return;
    }
    const QString path = canonical(archivePath);
    if (isOpen(path)) {
        setMessages(QStringLiteral("%1 is open for browsing. Close it on the Home page first.")
                        .arg(QFileInfo(path).fileName()),
                    {});
        return;
    }
    setMessages({}, {});
    m_deletingPath = path;
    const fs::path archive = pathFromQString(path);
    m_deleteWatcher.setFuture(QtConcurrent::run([archive]() { return application::ManageStickBackups::remove(archive); }));
    emit busyChanged();
}

void FullBackupsController::onDeleteFinished()
{
    QString thrown;
    const application::DeleteStickBackupResult result = takeResult(m_deleteWatcher, &thrown);
    const QString path = m_deletingPath;
    m_deletingPath.clear();
    if (!thrown.isEmpty()) {
        setMessages(QStringLiteral("Could not delete the backup: ") + thrown, {});
    } else if (result.status == application::DeleteStickBackupResult::Status::Deleted) {
        setMessages({}, QStringLiteral("Deleted %1.").arg(QFileInfo(path).fileName()));
        emit backupDeleted(path);
    } else {
        setMessages(QString::fromStdString(result.message), {});
    }
    emit busyChanged();
    refresh();
}

QString FullBackupsController::browsedArchiveFor(const QString &libraryRoot) const
{
    if (libraryRoot.isEmpty()) {
        return {};
    }
    const auto archive = infrastructure::local::browsedBackupArchive(pathFromQString(libraryRoot));
    return archive ? canonical(QString::fromStdString(archive->string())) : QString();
}

bool FullBackupsController::isOpen(const QString &archivePath) const
{
    return !archivePath.isEmpty() && m_openArchivePaths.contains(canonical(archivePath));
}

void FullBackupsController::setMessages(const QString &error, const QString &status)
{
    if (m_errorMessage == error && m_statusMessage == status) {
        return;
    }
    m_errorMessage = error;
    m_statusMessage = status;
    emit messagesChanged();
}

}  // namespace seabass::gui
