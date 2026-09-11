// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QSettings>

namespace seabass::gui
{

// App-level (not per-stick) preferences, persisted via QSettings under the
// user's standard config location. Main.qml binds Material.theme to
// useSystemTheme; ScanPage/DuplicatesPage/LocalCuePage bind their
// Rekordbox/Engine mode toggle to preferredFormat, so the last-chosen
// format carries over between those pages and across app restarts.
// ScanPage similarly persists lastBrowsePlaylistName, so Browse Library
// reopens on whichever playlist was last selected.
class AppSettingsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool useSystemTheme READ useSystemTheme WRITE setUseSystemTheme NOTIFY useSystemThemeChanged)
    Q_PROPERTY(QString preferredFormat READ preferredFormat WRITE setPreferredFormat NOTIFY preferredFormatChanged)
    Q_PROPERTY(bool hideStreamingTracks READ hideStreamingTracks WRITE setHideStreamingTracks NOTIFY
                   hideStreamingTracksChanged)
    Q_PROPERTY(QString keyNotation READ keyNotation WRITE setKeyNotation NOTIFY keyNotationChanged)
    Q_PROPERTY(QString stickBackupDirectory READ stickBackupDirectory WRITE setStickBackupDirectory NOTIFY
                   stickBackupDirectoryChanged)
    // Where everything Seabass keeps on this computer lives: stick
    // images, anonymized exports, the local cue store, its own
    // bookkeeping. One setting rather than one per feature, because
    // "back up my Seabass data" should have a single answer.
    Q_PROPERTY(QString seabassHomeDirectory READ seabassHomeDirectory WRITE setSeabassHomeDirectory NOTIFY
                   seabassHomeDirectoryChanged)
    Q_PROPERTY(QString lastBrowsePlaylistName READ lastBrowsePlaylistName WRITE setLastBrowsePlaylistName NOTIFY
                   lastBrowsePlaylistNameChanged)
    // Always present (even in a build compiled with SEABASS_EXPERIMENTAL
    // off) so QML can gate the whole Settings section on it.
    Q_PROPERTY(bool experimentalBuildSupported READ experimentalBuildSupported CONSTANT)
#ifdef SEABASS_EXPERIMENTAL_BUILD
    Q_PROPERTY(bool experimentalFeaturesEnabled READ experimentalFeaturesEnabled WRITE setExperimentalFeaturesEnabled
                   NOTIFY experimentalFeaturesEnabledChanged)
#else
    // No WRITE, on purpose: a stable-only build has nothing to toggle,
    // and the getter below is a hardcoded false regardless of anything
    // stored in QSettings from a previous experimental build.
    Q_PROPERTY(bool experimentalFeaturesEnabled READ experimentalFeaturesEnabled CONSTANT)
#endif

public:
    explicit AppSettingsController(QObject *parent = nullptr);

    bool useSystemTheme() const { return m_useSystemTheme; }
    void setUseSystemTheme(bool value);

    QString preferredFormat() const { return m_preferredFormat; }
    void setPreferredFormat(const QString &value);

    // Browse Library's own display preference. Streaming tracks
    // (Engine/TIDAL) are excluded from destructive/consequential
    // operations unconditionally regardless of this setting (see
    // domain::Track::streamingSource); this only controls whether they
    // show up at all. Off by default. Tracks are shown (badged) unless
    // the user opts into hiding them.
    bool hideStreamingTracks() const { return m_hideStreamingTracks; }
    void setHideStreamingTracks(bool value);

    // How KeyBadge.qml renders a musical key everywhere it appears
    // (Browse Library's Key column, Library Statistics' "Tracks per
    // key") -- "camelot" (default, e.g. "6A") or "traditional" (e.g.
    // "F♯m"). Either way the badge's color still comes from the same
    // underlying Camelot wheel position (see Theme.colorForKey()), only
    // the printed label changes.
    QString keyNotation() const { return m_keyNotation; }
    void setKeyNotation(const QString &value);

    // Where full stick backups (one `<label>.zip` per stick) are kept.
    // Defaults to "<home>/Seabass/backups/full" -- a place the user can find,
    // browse with 7-Zip/unzip and copy elsewhere, deliberately not the
    // hidden app-data directory (see docs/stick-backup-plan.md, "Archive
    // location and identity").
    QString stickBackupDirectory() const { return m_stickBackupDirectory; }
    QString seabassHomeDirectory() const { return m_seabassHomeDirectory; }
    void setSeabassHomeDirectory(const QString &value);
    // The default, so the UI can offer "put it back" without knowing how
    // the path is built.
    Q_INVOKABLE static QString defaultSeabassHomeDirectory();
    // For QML dialog results; see gui/local_file_url.hpp. On this
    // controller because every page already has one.
    Q_INVOKABLE static QString localPathFromUrl(const QString &pathOrUrl);
    // The other direction, for a dialog's currentFolder: "file://" + path
    // is malformed on Windows (see gui/local_file_url.hpp).
    Q_INVOKABLE static QString toLocalFileUrl(const QString &path);
    // <seabassHome>/testdata, where anonymized exports are proposed.
    Q_INVOKABLE QString anonymizedExportDirectory() const;
    void setStickBackupDirectory(const QString &value);
    static QString defaultStickBackupDirectory();

    // Name of the playlist Browse Library last had selected (empty means
    // "All tracks"), so returning to Browse -- whether by navigating back
    // in the same session or relaunching the app -- lands back where the
    // user left off instead of always resetting to "All tracks". ScanPage
    // falls back to "All tracks" itself if this name no longer matches any
    // playlist on the stick currently being browsed.
    QString lastBrowsePlaylistName() const { return m_lastBrowsePlaylistName; }
    void setLastBrowsePlaylistName(const QString &value);

    // See docs/experimental-features.md for the convention this backs:
    // new non-trivial features default to hidden behind
    // experimentalFeaturesEnabled until proven, then graduate to
    // stable (an ActionCard just drops its `experimental: true`).
    static constexpr bool experimentalBuildSupported()
    {
#ifdef SEABASS_EXPERIMENTAL_BUILD
        return true;
#else
        return false;
#endif
    }

#ifdef SEABASS_EXPERIMENTAL_BUILD
    bool experimentalFeaturesEnabled() const { return m_experimentalFeaturesEnabled; }
    void setExperimentalFeaturesEnabled(bool value);
#else
    bool experimentalFeaturesEnabled() const { return false; }
#endif

signals:
    void useSystemThemeChanged();
    void preferredFormatChanged();
    void hideStreamingTracksChanged();
    void keyNotationChanged();
    void stickBackupDirectoryChanged();
    void seabassHomeDirectoryChanged();
    void lastBrowsePlaylistNameChanged();
#ifdef SEABASS_EXPERIMENTAL_BUILD
    void experimentalFeaturesEnabledChanged();
#endif

private:
    QSettings m_settings;
    bool m_useSystemTheme = false;
    QString m_preferredFormat = QStringLiteral("rekordbox");
    bool m_hideStreamingTracks = false;
    QString m_keyNotation = QStringLiteral("camelot");
    QString m_stickBackupDirectory;
    QString m_seabassHomeDirectory;
    QString m_lastBrowsePlaylistName;
#ifdef SEABASS_EXPERIMENTAL_BUILD
    bool m_experimentalFeaturesEnabled = false;
#endif
};

}  // namespace seabass::gui
