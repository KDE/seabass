// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/matching_policy.hpp"
#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_silence_probe.hpp"
#endif
#include "gui/app_color_scheme.hpp"
#include "gui/local_file_url.hpp"
#include "gui/seabass_settings.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "app_settings_controller.hpp"
#include "gui/qt_path.hpp"

#include <QDir>
#include <QStandardPaths>

namespace seabass::gui
{

QString AppSettingsController::defaultStickBackupDirectory()
{
    return pathToQString(infrastructure::paths::localFullBackupsDir());
}

AppSettingsController::AppSettingsController(QObject *parent)
    : QObject(parent), m_settings(openSeabassSettings())
{
    m_useSystemTheme = m_settings.value("useSystemTheme", false).toBool();
    m_preferredFormat = m_settings.value("preferredFormat", "rekordbox").toString();
    m_hideStreamingTracks = m_settings.value("hideStreamingTracks", false).toBool();
    m_keyNotation = m_settings.value("keyNotation", "camelot").toString();
    m_exactMatchSeconds = m_settings.value("music/exactMatchSeconds",
                                            static_cast<int>(domain::MatchingPolicy::DefaultExactMatchSeconds))
                              .toInt();
    m_compareAudioSeconds = m_settings.value("music/compareAudioSeconds",
                                              static_cast<int>(domain::MatchingPolicy::DefaultCompareAudioSeconds))
                                .toInt();
    m_ignoreCuesAtStart =
        m_settings.value("music/ignoreCuesAtStart", domain::MatchingPolicy::DefaultIgnoreCuesAtStart).toBool();
    // Before anything can scan. MatchingPolicy clamps, so a hand-edited
    // settings file cannot put a wider window into effect than the page
    // would allow, and the members are read back from it so the page
    // shows what is actually in force rather than what was typed.
    applyMatchingPolicy();
    m_stickBackupDirectory = m_settings.value("stickBackupDirectory", defaultStickBackupDirectory()).toString();
    if (m_stickBackupDirectory.isEmpty()) {
        m_stickBackupDirectory = defaultStickBackupDirectory();
    }
    m_seabassHomeDirectory = m_settings.value("seabassHomeDirectory", defaultSeabassHomeDirectory()).toString();
    if (m_seabassHomeDirectory.isEmpty()) {
        m_seabassHomeDirectory = defaultSeabassHomeDirectory();
    }
    infrastructure::paths::setLocalRootOverride(pathFromQString(m_seabassHomeDirectory));
    m_lastPlaylistName = m_settings.value("lastPlaylistName", "").toString();
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
    // KDE's style follows now, not at the next start: Theme repaints on
    // the signal below, and a style still inking Kelp's near-white over
    // a light system Theme is unreadable. See gui/app_color_scheme.hpp.
    applyAppColorScheme(value);
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

bool AppSettingsController::audioComparisonSupported()
{
#ifdef SEABASS_HAVE_QT_AUDIO
    // Asked once. QAudioDecoder::isSupported() goes to the plugin
    // registry, and this backs a QML binding.
    static const bool available = infrastructure::audio::QtMultimediaSilenceProbe::decodingAvailable();
    return available;
#else
    return false;
#endif
}

int AppSettingsController::exactMatchMaxSeconds() const
{
    return static_cast<int>(domain::MatchingPolicy::MaxExactMatchSeconds);
}

int AppSettingsController::compareAudioMaxSeconds() const
{
    return static_cast<int>(domain::MatchingPolicy::MaxCompareAudioSeconds);
}

void AppSettingsController::applyMatchingPolicy()
{
    domain::MatchingPolicy::set(m_exactMatchSeconds, m_compareAudioSeconds, m_ignoreCuesAtStart);
    // Read back, never assumed. set() clamps both numbers (and raises
    // the wider window to the exact one when it was left below it), so
    // storing what was asked for would leave the page showing a value
    // nothing in the app is using -- a counter that reports confidently
    // and is wrong.
    m_exactMatchSeconds = static_cast<int>(domain::MatchingPolicy::exactMatchSeconds());
    m_compareAudioSeconds = static_cast<int>(domain::MatchingPolicy::compareAudioSeconds());
    m_ignoreCuesAtStart = domain::MatchingPolicy::ignoreCuesAtStart();
}

void AppSettingsController::setExactMatchSeconds(int value)
{
    if (m_exactMatchSeconds == value) {
        return;
    }
    m_exactMatchSeconds = value;
    applyMatchingPolicy();
    m_settings.setValue("music/exactMatchSeconds", m_exactMatchSeconds);
    emit exactMatchSecondsChanged();
    // Raising the exact window can push the wider one up with it (it is
    // never allowed below), so the page has to hear about both.
    m_settings.setValue("music/compareAudioSeconds", m_compareAudioSeconds);
    emit compareAudioSecondsChanged();
}

void AppSettingsController::setCompareAudioSeconds(int value)
{
    if (m_compareAudioSeconds == value) {
        return;
    }
    m_compareAudioSeconds = value;
    applyMatchingPolicy();
    m_settings.setValue("music/compareAudioSeconds", m_compareAudioSeconds);
    emit compareAudioSecondsChanged();
}

void AppSettingsController::setIgnoreCuesAtStart(bool value)
{
    if (m_ignoreCuesAtStart == value) {
        return;
    }
    m_ignoreCuesAtStart = value;
    applyMatchingPolicy();
    m_settings.setValue("music/ignoreCuesAtStart", m_ignoreCuesAtStart);
    emit ignoreCuesAtStartChanged();
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
    return pathToQString(infrastructure::paths::localRoot());
}

QString AppSettingsController::anonymizedExportDirectory() const
{
    return pathToQString(pathFromQString(m_seabassHomeDirectory) / "testdata");
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
    infrastructure::paths::setLocalRootOverride(pathFromQString(effective));
    emit seabassHomeDirectoryChanged();
}

void AppSettingsController::setLastPlaylistName(const QString &value)
{
    if (m_lastPlaylistName == value) {
        return;
    }
    m_lastPlaylistName = value;
    m_settings.setValue("lastPlaylistName", value);
    emit lastPlaylistNameChanged();
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
