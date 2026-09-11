#include "gui/local_file_url.hpp"
#include "gui/seabass_settings.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "app_settings_controller.hpp"

#include <QDir>
#include <QStandardPaths>

namespace seabass::gui
{

QString AppSettingsController::defaultStickBackupDirectory()
{
    return QString::fromStdString(infrastructure::paths::localFullBackupsDir().string());
}

AppSettingsController::AppSettingsController(QObject *parent)
    : QObject(parent), m_settings(openSeabassSettings())
{
    m_useSystemTheme = m_settings.value("useSystemTheme", false).toBool();
    m_preferredFormat = m_settings.value("preferredFormat", "rekordbox").toString();
    m_hideStreamingTracks = m_settings.value("hideStreamingTracks", false).toBool();
    m_keyNotation = m_settings.value("keyNotation", "camelot").toString();
    m_stickBackupDirectory = m_settings.value("stickBackupDirectory", defaultStickBackupDirectory()).toString();
    if (m_stickBackupDirectory.isEmpty()) {
        m_stickBackupDirectory = defaultStickBackupDirectory();
    }
    m_seabassHomeDirectory = m_settings.value("seabassHomeDirectory", defaultSeabassHomeDirectory()).toString();
    if (m_seabassHomeDirectory.isEmpty()) {
        m_seabassHomeDirectory = defaultSeabassHomeDirectory();
    }
    infrastructure::paths::setLocalRootOverride(m_seabassHomeDirectory.toStdString());
    m_lastBrowsePlaylistName = m_settings.value("lastBrowsePlaylistName", "").toString();
#ifdef SEABASS_EXPERIMENTAL_BUILD
    m_experimentalFeaturesEnabled = m_settings.value("experimentalFeaturesEnabled", false).toBool();
#endif
}

void AppSettingsController::setUseSystemTheme(bool value)
{
    if (m_useSystemTheme == value) {
        return;
    }
    m_useSystemTheme = value;
    m_settings.setValue("useSystemTheme", value);
    emit useSystemThemeChanged();
}

void AppSettingsController::setPreferredFormat(const QString &value)
{
    if (m_preferredFormat == value) {
        return;
    }
    m_preferredFormat = value;
    m_settings.setValue("preferredFormat", value);
    emit preferredFormatChanged();
}

void AppSettingsController::setHideStreamingTracks(bool value)
{
    if (m_hideStreamingTracks == value) {
        return;
    }
    m_hideStreamingTracks = value;
    m_settings.setValue("hideStreamingTracks", value);
    emit hideStreamingTracksChanged();
}

void AppSettingsController::setKeyNotation(const QString &value)
{
    if (m_keyNotation == value) {
        return;
    }
    m_keyNotation = value;
    m_settings.setValue("keyNotation", value);
    emit keyNotationChanged();
}

QString AppSettingsController::localPathFromUrl(const QString &pathOrUrl)
{
    return seabass::gui::localPathFromUrl(pathOrUrl);
}

QString AppSettingsController::toLocalFileUrl(const QString &path)
{
    return seabass::gui::toLocalFileUrl(path.toStdString());
}

void AppSettingsController::setStickBackupDirectory(const QString &value)
{
    const QString local = seabass::gui::localPathFromUrl(value);
    QString effective = local.isEmpty() ? defaultStickBackupDirectory() : local;
    if (m_stickBackupDirectory == effective) {
        return;
    }
    m_stickBackupDirectory = effective;
    m_settings.setValue("stickBackupDirectory", effective);
    emit stickBackupDirectoryChanged();
}

QString AppSettingsController::defaultSeabassHomeDirectory()
{
    // Asked of the paths module rather than rebuilt here, so the app and
    // everything Qt-free agree on one answer.
    infrastructure::paths::setLocalRootOverride({});
    return QString::fromStdString(infrastructure::paths::localRoot().string());
}

QString AppSettingsController::anonymizedExportDirectory() const
{
    return QString::fromStdString((std::filesystem::path(m_seabassHomeDirectory.toStdString()) / "testdata").string());
}

void AppSettingsController::setSeabassHomeDirectory(const QString &value)
{
    const QString local = seabass::gui::localPathFromUrl(value);
    const QString effective = local.isEmpty() ? defaultSeabassHomeDirectory() : local;
    if (m_seabassHomeDirectory == effective) {
        return;
    }
    m_seabassHomeDirectory = effective;
    m_settings.setValue("seabassHomeDirectory", effective);
    // Applied immediately: everything Qt-free resolves its paths through
    // localRoot(), so a setting that only took effect after a restart
    // would leave the two halves of the app disagreeing about where the
    // user's data lives.
    infrastructure::paths::setLocalRootOverride(effective.toStdString());
    emit seabassHomeDirectoryChanged();
}

void AppSettingsController::setLastBrowsePlaylistName(const QString &value)
{
    if (m_lastBrowsePlaylistName == value) {
        return;
    }
    m_lastBrowsePlaylistName = value;
    m_settings.setValue("lastBrowsePlaylistName", value);
    emit lastBrowsePlaylistNameChanged();
}

#ifdef SEABASS_EXPERIMENTAL_BUILD
void AppSettingsController::setExperimentalFeaturesEnabled(bool value)
{
    if (m_experimentalFeaturesEnabled == value) {
        return;
    }
    m_experimentalFeaturesEnabled = value;
    m_settings.setValue("experimentalFeaturesEnabled", value);
    emit experimentalFeaturesEnabledChanged();
}
#endif

}  // namespace seabass::gui
