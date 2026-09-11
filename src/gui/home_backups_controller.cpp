#include "home_backups_controller.hpp"

#include <filesystem>

#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/local/metadata_store.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

HomeBackupsController::HomeBackupsController(QObject *parent) : QObject(parent)
{
    // Not refreshed here: backupDirectory is still empty, so the full
    // backup count would be a guaranteed zero that the page would then
    // have to ask for again the moment it set the directory.
}

void HomeBackupsController::setBackupDirectory(const QString &directory)
{
    if (m_backupDirectory == directory) {
        return;
    }
    m_backupDirectory = directory;
    refresh();
}

void HomeBackupsController::refresh()
{
    const int backups = m_backupDirectory.isEmpty()
        ? 0
        : application::RestoreStickBackup::countArchives(fs::path(m_backupDirectory.toStdString()));
    const int tracks = infrastructure::local::MetadataStore::storedTrackCountIfPresent();
    if (backups == m_fullBackupCount && tracks == m_metadataTrackCount) {
        return;
    }
    m_fullBackupCount = backups;
    m_metadataTrackCount = tracks;
    emit countsChanged();
}

}  // namespace seabass::gui
