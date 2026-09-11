#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

namespace seabass::gui
{

// What the first page needs to know about this computer's own backup
// stores, and nothing else: are there any full stick backups to browse,
// and is there anything in the metadata backup.
//
// Its own controller rather than two properties bolted onto existing
// ones, because neither existing owner fits. BackupAdvisorController
// knows the backup directory but is about advising one stick;
// MetadataBackupController answers the second question and costs a
// database creation and a 200-row page to do it. Both questions here are
// the same shape -- "is this feature worth offering yet?" -- and both
// are answered by a probe that opens nothing and creates nothing.
//
// Refreshed on demand rather than watched: the answer only changes when
// the user has been somewhere else in the app doing something about it,
// which is exactly when the first page comes back into view.
class HomeBackupsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Where full stick backups live. Set from the app settings; the
    // counts refresh whenever it changes.
    Q_PROPERTY(QString backupDirectory READ backupDirectory WRITE setBackupDirectory NOTIFY countsChanged)
    Q_PROPERTY(int fullBackupCount READ fullBackupCount NOTIFY countsChanged)
    Q_PROPERTY(int metadataTrackCount READ metadataTrackCount NOTIFY countsChanged)

public:
    explicit HomeBackupsController(QObject *parent = nullptr);

    QString backupDirectory() const { return m_backupDirectory; }
    void setBackupDirectory(const QString &directory);
    int fullBackupCount() const { return m_fullBackupCount; }
    int metadataTrackCount() const { return m_metadataTrackCount; }

    Q_INVOKABLE void refresh();

signals:
    void countsChanged();

private:
    QString m_backupDirectory;
    int m_fullBackupCount = 0;
    int m_metadataTrackCount = 0;
};

}  // namespace seabass::gui
