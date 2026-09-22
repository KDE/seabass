// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "library_consistency_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "application/track_file_presence.hpp"
#include "domain/clustered_cue.hpp"
#include "domain/junk_cue.hpp"
#include "domain/track_scope.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/local_file_url.hpp"
#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cleanup_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/delete_orphan_change.hpp"
#include "gui/edit/changes/remove_junk_cue_change.hpp"
#include "gui/edit/changes/repair_artwork_change.hpp"

#include "gui/future_result.hpp"
#include "infrastructure/media/filesystem_health.hpp"
#include "gui/artwork_rescue_sources.hpp"
#include "gui/edit/changes/fill_sample_rate_change.hpp"
#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif
#include "gui/app_settings_controller.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/edit/changes/repair_issue_change.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using domain::LibraryConsistencyIssue;

LibraryConsistencyIssueListModel::LibraryConsistencyIssueListModel(QObject *parent) : QAbstractListModel(parent)
{
    // countChanged follows the model's own structural signals rather than
    // being emitted by hand at each mutation site, so a mutation added
    // later cannot forget it -- which is how `count` came to be wrong in
    // the first place, only in QML rather than here.
    connect(this, &QAbstractItemModel::modelReset, this, &LibraryConsistencyIssueListModel::countChanged);
    connect(this, &QAbstractItemModel::rowsInserted, this, &LibraryConsistencyIssueListModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &LibraryConsistencyIssueListModel::countChanged);
}

int LibraryConsistencyIssueListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_issues.size());
}

namespace
{

QVariantMap brokenTrackToMap(const domain::Track &track)
{
    QVariantMap m;
    // Same "side" key every other trackToMap()-style helper in this
    // codebase uses (sync_controller.cpp, duplicates_controller.cpp,
    // cleanup_controller.cpp) -- TrackWaveformCard reads track.side, not
    // track.format, for both its Play wiring and WaveformView's format
    // hint. Unused by this file's own existing callers (the missing-file
    // detail view passes format separately), added now for the new
    // TrackWaveformCard usage in the memory-cue section below.
    m["side"] = QString::fromStdString(track.format);
    m["sourceId"] = QString::fromStdString(track.sourceId);
    m["title"] = QString::fromStdString(track.title);
    m["artist"] = QString::fromStdString(track.artist);
    m["filePath"] = QString::fromStdString(track.filePath);
    m["artworkPath"] = toLocalFileUrl(track.artworkPath);
    m["durationMs"] = track.durationSeconds * 1000.0;
    QVariantList cues;
    for (const auto &c : track.cues) {
        QVariantMap cueMap;
        cueMap["kind"] = c.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("hot") : QStringLiteral("memory");
        cueMap["hotCueNumber"] = c.hotCueNumber;
        cueMap["positionMs"] = c.positionMs;
        cueMap["color"] = QString::fromStdString(c.color);
        cues << cueMap;
    }
    m["cues"] = cues;
    // Which playlists this track is in, so the detail view can say where
    // the hole is. "Track X is gone" is not the question a DJ has; "which
    // set am I about to play with a gap in it" is, and the answer was
    // being read off the same track list already (tallyPlaylists() builds
    // the picker from it) and then dropped at this boundary.
    //
    // Best-effort by contract: Track::playlists is populated where the
    // reader supports it, so an empty list means "not known", never "in
    // no playlist", and the UI has to keep those apart.
    QVariantList playlists;
    for (const auto &membership : track.playlists) {
        QVariantMap entry;
        entry["name"] = QString::fromStdString(membership.name);
        entry["position"] = membership.position;
        playlists << entry;
    }
    m["playlists"] = playlists;
    return m;
}

}  // namespace

QVariant LibraryConsistencyIssueListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_issues.size()) {
        return {};
    }
    const auto &issue = m_issues[static_cast<size_t>(index.row())];
    switch (role) {
    case KindRole:
        switch (issue.kind) {
        case LibraryConsistencyIssue::Kind::Repairable:
            return QStringLiteral("repairable");
        case LibraryConsistencyIssue::Kind::Conflict:
            return QStringLiteral("conflict");
        case LibraryConsistencyIssue::Kind::Missing:
            return QStringLiteral("missing");
        }
        return {};
    case FormatRole:
        return issueFormat(issue);
    case SurvivorRole:
        return issue.survivor ? brokenTrackToMap(*issue.survivor) : QVariantMap();
    case BrokenTracksRole: {
        QVariantList list;
        for (const auto &t : issue.brokenGroup) {
            list << brokenTrackToMap(t);
        }
        return list;
    }
    case CueMergeNeededRole:
        return !issue.survivorCues.empty();
    case StagedRole:
        return static_cast<size_t>(index.row()) < m_stagedDescriptions.size()
            && !m_stagedDescriptions[static_cast<size_t>(index.row())].isEmpty();
    case StagedDescriptionRole:
        return static_cast<size_t>(index.row()) < m_stagedDescriptions.size()
            ? m_stagedDescriptions[static_cast<size_t>(index.row())] : QString();
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryConsistencyIssueListModel::roleNames() const
{
    return {
        {KindRole, "kind"},
        {FormatRole, "format"},
        {SurvivorRole, "survivor"},
        {BrokenTracksRole, "brokenTracks"},
        {CueMergeNeededRole, "cueMergeNeeded"},
        {StagedRole, "staged"},
        {StagedDescriptionRole, "stagedDescription"},
    };
}

void LibraryConsistencyIssueListModel::clear()
{
    beginResetModel();
    m_issues.clear();
    m_stagedDescriptions.clear();
    endResetModel();
}

void LibraryConsistencyIssueListModel::appendIssues(std::vector<domain::LibraryConsistencyIssue> issues)
{
    if (issues.empty()) {
        return;
    }
    int first = static_cast<int>(m_issues.size());
    int last = first + static_cast<int>(issues.size()) - 1;
    beginInsertRows(QModelIndex(), first, last);
    m_stagedDescriptions.resize(m_issues.size() + issues.size());
    m_issues.insert(m_issues.end(), std::make_move_iterator(issues.begin()), std::make_move_iterator(issues.end()));
    endInsertRows();
}

void LibraryConsistencyIssueListModel::removeIssueAt(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_issues.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), index, index);
    m_issues.erase(m_issues.begin() + index);
    m_stagedDescriptions.erase(m_stagedDescriptions.begin() + index);
    endRemoveRows();
}

void LibraryConsistencyIssueListModel::setStaged(int index, bool staged, const QString &description)
{
    if (index < 0 || static_cast<size_t>(index) >= m_issues.size()) {
        return;
    }
    m_stagedDescriptions[static_cast<size_t>(index)] = staged ? description : QString();
    emit dataChanged(this->index(index), this->index(index), {StagedRole, StagedDescriptionRole});
}

void LibraryConsistencyIssueListModel::clearStaged()
{
    if (m_issues.empty()) {
        return;
    }
    std::fill(m_stagedDescriptions.begin(), m_stagedDescriptions.end(), QString());
    emit dataChanged(index(0), index(static_cast<int>(m_issues.size()) - 1), {StagedRole, StagedDescriptionRole});
}

JunkCueIssueListModel::JunkCueIssueListModel(QObject *parent) : QAbstractListModel(parent)
{
    // countChanged follows the model's own structural signals rather than
    // being emitted by hand at each mutation site, so a mutation added
    // later cannot forget it -- which is how `count` came to be wrong in
    // the first place, only in QML rather than here.
    connect(this, &QAbstractItemModel::modelReset, this, &JunkCueIssueListModel::countChanged);
    connect(this, &QAbstractItemModel::rowsInserted, this, &JunkCueIssueListModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &JunkCueIssueListModel::countChanged);
}

int JunkCueIssueListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_issues.size());
}

QVariant JunkCueIssueListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_issues.size()) {
        return {};
    }
    const auto &issue = m_issues[static_cast<size_t>(index.row())];
    switch (role) {
    case FormatRole:
        return QString::fromStdString(issue.track.format);
    case TitleRole:
        return QString::fromStdString(issue.track.title);
    case ArtistRole:
        return QString::fromStdString(issue.track.artist);
    case TrackRole:
        return brokenTrackToMap(issue.track);
    case StagedRole:
        return static_cast<size_t>(index.row()) < m_staged.size() && m_staged[static_cast<size_t>(index.row())];
    case ReasonRole:
        return QString::fromStdString(issue.reason);
    case PositionMsRole:
        return issue.cue.positionMs;
    default:
        return {};
    }
}

QHash<int, QByteArray> JunkCueIssueListModel::roleNames() const
{
    return {
        {FormatRole, "format"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {TrackRole, "track"},
        {StagedRole, "staged"},
        {ReasonRole, "reason"},
        {PositionMsRole, "positionMs"},
    };
}

void JunkCueIssueListModel::clear()
{
    beginResetModel();
    m_issues.clear();
    m_staged.clear();
    endResetModel();
}

void JunkCueIssueListModel::appendIssues(std::vector<domain::JunkCueIssue> issues)
{
    if (issues.empty()) {
        return;
    }
    int first = static_cast<int>(m_issues.size());
    int last = first + static_cast<int>(issues.size()) - 1;
    beginInsertRows(QModelIndex(), first, last);
    m_staged.resize(m_issues.size() + issues.size(), false);
    m_issues.insert(m_issues.end(), std::make_move_iterator(issues.begin()), std::make_move_iterator(issues.end()));
    endInsertRows();
}

void JunkCueIssueListModel::removeAt(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_issues.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), index, index);
    m_issues.erase(m_issues.begin() + index);
    m_staged.erase(m_staged.begin() + index);
    endRemoveRows();
}

void JunkCueIssueListModel::setStaged(int index, bool staged)
{
    if (index < 0 || static_cast<size_t>(index) >= m_issues.size()) {
        return;
    }
    m_staged[static_cast<size_t>(index)] = staged;
    emit dataChanged(this->index(index), this->index(index), {StagedRole});
}

void JunkCueIssueListModel::clearStaged()
{
    if (m_issues.empty()) {
        return;
    }
    std::fill(m_staged.begin(), m_staged.end(), false);
    emit dataChanged(index(0), index(static_cast<int>(m_issues.size()) - 1), {StagedRole});
}

namespace
{

std::vector<domain::Track> scanTracks(const QString &format, const QString &path,
                                       std::shared_ptr<QtProgressReporter> reporter,
                                       application::CancellationToken cancel = application::CancellationToken::none())
{
    // No explicit format check here -- LibraryCatalogCache::tracksFor()
    // already throws for anything unrecognized (see its own realScan()),
    // caught by the same catch (const std::exception &) below either way.
    return LibraryCatalogCache::instance().tracksFor(format.toStdString(), path.toStdString(), *reporter, cancel);
}

// This format's own playlist membership tally, unfiltered -- called on
// the full track list before any TrackScope filtering below, so picking
// a playlist in JunkCuePage.qml's picker never shrinks its own list of
// choices. Mirrors the tally half of SyncController's own
// collectPlaylistSummary(), just for one format at a time (see
// LibraryConsistencyController::mergePlaylistSummary() for how each
// format's contribution gets folded into the cross-catalog union).
void tallyPlaylists(const std::vector<domain::Track> &tracks, LibraryConsistencyScanResult &result)
{
    std::map<std::string, int> countByName;
    for (const auto &track : tracks) {
        for (const auto &playlist : track.playlists) {
            countByName[playlist.name]++;
        }
    }
    for (const auto &[name, count] : countByName) {
        QString qName = QString::fromStdString(name);
        result.playlistNames << qName;
        result.playlistTrackCounts[qName] = count;
    }
}

// Runs entirely on a background thread (see LibraryConsistencyController::
// scanNextPendingFormat()) - no access to the controller itself. Scans
// exactly one format; the controller chains one of these per present
// catalog to get the progressive, format-at-a-time behavior. playlistName
// empty scans/checks the whole format's library, same as before this
// parameter existed; a real name scopes both the junk-cue list and the
// consistency check to just that playlist's tracks, via domain::TrackScope
// -- same seam SyncController::runAnalyzeTask already uses.
LibraryConsistencyScanResult runScanTask(QString format, QString path, QString playlistName,
                                          std::shared_ptr<QtProgressReporter> reporter,
                                          application::CancellationToken cancel,
                                          infrastructure::engine::ArtworkSourceByTrackFile artSources,
                                          QString backupDirectory)
{
    LibraryConsistencyScanResult result;
    try {
        auto tracks = scanTracks(format, path, reporter, cancel);

        tallyPlaylists(tracks, result);

        if (format == QStringLiteral("rekordbox")) {
            // What this catalog holds per audio file, for the Engine pass
            // that follows: Engine keeps its own copies of the art, so
            // when one of those is lost this is the only source still on
            // the stick.
            for (const domain::Track &track : tracks) {
                if (track.artworkPath.empty() || track.filePath.empty()) {
                    continue;
                }
                result.artSources.emplace(infrastructure::engine::artworkSourceKey(track.filePath),
                                          track.artworkPath);
            }
        }

        if (format == QStringLiteral("engine")) {
            // Cover art, checked while this format's library is open
            // anyway: one read of Track/AlbumArt and a stat per image,
            // nothing next to the scan itself.
            // The rescue sources outlive the scan: the repair that
            // follows asks the same object for the bytes, so a backup
            // archive is opened once rather than once per cover.
            result.rescue = std::make_shared<ArtworkRescueSources>(
                backupDirectory.toStdString(),
                std::filesystem::path(path.toStdString()).parent_path().string());
            result.artwork =
                infrastructure::engine::auditArtwork(path.toStdString(), artSources, result.rescue->probe());
            // The same pass asks each row for its sample rate, and each
            // file whose row cannot say. Reading a header costs about
            // 0.07 ms (see TagLibMetadataProbe), so a library where
            // nothing is missing costs nothing and one where everything
            // is costs a second.
            result.sampleRates = infrastructure::engine::auditSampleRates(
                path.toStdString(), [](const std::string &audioFile) -> double {
#ifdef SEABASS_HAVE_TAGLIB
                    infrastructure::audio::TagLibMetadataProbe probe;
                    const auto metadata = probe.read(audioFile);
                    return metadata ? static_cast<double>(metadata->sampleRate) : 0.0;
#else
                    (void)audioFile;
                    return 0.0;
#endif
                });
        }

        if (!playlistName.isEmpty()) {
            tracks = domain::filterByScope(tracks, domain::TrackScope::playlist(playlistName.toStdString()));
        }

        // Junk-cue detection doesn't care about file existence at all,
        // computed on the full (post-scope) track list before the
        // healthy/broken split below moves tracks out of it. Streaming
        // tracks are excluded here too, same "never touch these" policy
        // as every other consistency action in this class (see Track::
        // streamingSource's own doc comment).
        for (auto &issue : domain::JunkCueFinder::find(tracks)) {
            if (issue.track.streamingSource.empty()) {
                result.junkCues.push_back(std::move(issue));
            }
        }
        // The other shape of cue nobody set: three or more hot cues
        // crowded into the first two seconds, which is what an earlier
        // write path left behind when it touched one cue list and not
        // the other (#33, #41). They join the same list, because what a
        // user does about one is what they do about the other: look at
        // it, and stage its removal or leave it. Each row carries its own
        // reason, so the page does not have to describe a cue at 1.2 s
        // as being at the start of the track.
        //
        // removableClusterCues() leaves out any the rule above already
        // offered, so the same cue cannot be staged from two rows.
        for (const auto &cluster : domain::ClusteredCueFinder::find(tracks)) {
            if (!cluster.track.streamingSource.empty()) {
                continue;
            }
            const std::string reason = "one of " + std::to_string(cluster.cluster.size())
                + " hot cues in the first two seconds, which is not a pattern anyone plays";
            for (const auto &cue : domain::removableClusterCues(cluster)) {
                result.junkCues.push_back(domain::JunkCueIssue{cluster.track, cue, reason});
            }
        }

        std::vector<domain::Track> healthy;
        std::vector<domain::Track> broken;
        for (auto &t : tracks) {
            // Streaming tracks (Engine/TIDAL) have no real local file by
            // design, neither healthy nor broken, just not a local-
            // file consistency concern at all. See
            // domain::Track::streamingSource's own doc comment.
            if (!t.streamingSource.empty()) {
                continue;
            }
            // is_regular_file, via the shared rule: a directory at a
            // track's path exists() but cannot be played, and calling it
            // healthy is how an unreadable file was reported as fine.
            (application::trackFileIsPresent(t) ? healthy : broken).push_back(std::move(t));
        }
        result.issues = domain::LibraryConsistencyChecker::check(healthy, broken);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

// An issue's identity across rescans: its format, survivor and broken
// row ids. Also the staged change's key.
}  // namespace

LibraryConsistencyController::LibraryConsistencyController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<LibraryConsistencyScanResult>::finished, this,
            &LibraryConsistencyController::onScanFinished);
    connect(&m_repairWatcher, &QFutureWatcher<infrastructure::media::FilesystemRepairResult>::finished, this,
            &LibraryConsistencyController::onFilesystemRepairFinished);
    // Where this computer keeps full stick backups: read once, the same
    // way and from the same key AppSettingsController writes it, so a
    // cover lost from the stick can be looked for in them.
    QSettings settings = openSeabassSettings();
    m_backupDirectory = settings.value(QStringLiteral("stickBackupDirectory"),
                                       AppSettingsController::defaultStickBackupDirectory())
                            .toString();
}

std::shared_ptr<QtProgressReporter> LibraryConsistencyController::makeReporter()
{
    auto reporter = std::make_shared<QtProgressReporter>();
    connect(reporter.get(), &QtProgressReporter::started, this,
            [this](const QString &, int total) { setScanProgress(0, total); });
    connect(reporter.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setScanProgress(current, m_scanTotal); });
    return reporter;
}

QString LibraryConsistencyController::pathForFormat(const QString &format) const
{
    return format == "engine" ? m_enginePath : m_rekordboxPath;
}

int LibraryConsistencyController::artworkImportedCount() const
{
    return static_cast<int>(std::count_if(m_artwork.unreadable.begin(), m_artwork.unreadable.end(),
                                          [](const infrastructure::engine::ArtworkEntry &entry) {
                                              return entry.storage
                                                  == infrastructure::engine::ArtworkStorage::ImportedPath;
                                          }));
}

int LibraryConsistencyController::artworkMissingFileCount() const
{
    return static_cast<int>(std::count_if(m_artwork.unreadable.begin(), m_artwork.unreadable.end(),
                                          [](const infrastructure::engine::ArtworkEntry &entry) {
                                              return entry.storage
                                                  == infrastructure::engine::ArtworkStorage::CachedFileMissing;
                                          }));
}

int LibraryConsistencyController::artworkEmptyFileCount() const
{
    return static_cast<int>(std::count_if(m_artwork.unreadable.begin(), m_artwork.unreadable.end(),
                                          [](const infrastructure::engine::ArtworkEntry &entry) {
                                              return entry.storage
                                                  == infrastructure::engine::ArtworkStorage::CachedFileUnreadable;
                                          }));
}

int LibraryConsistencyController::artworkBrokenRowCount() const
{
    return static_cast<int>(std::count_if(m_artwork.unreadable.begin(), m_artwork.unreadable.end(),
                                          [](const infrastructure::engine::ArtworkEntry &entry) {
                                              return entry.storage
                                                  == infrastructure::engine::ArtworkStorage::RowWithoutHash;
                                          }));
}

void LibraryConsistencyController::scan(const QString &rekordboxPath, const QString &enginePath,
                                          const QString &playlistName)
{
    if (m_busy) {
        return;
    }
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    m_currentPlaylistName = playlistName;
    m_model.clear();
    m_junkCueModel.clear();
    // The cover-art counts belong to the scan that produced them. Left
    // standing, a rescan showed the previous run's headline beside an
    // emptied issue list, and an Engine leg that then errored left those
    // counts on screen for as long as the page lived.
    m_artwork = {};
    m_sampleRates = {};
    // A sqlite row and 24 bytes of a pdb header: cheap enough to read
    // with the scan rather than behind its own button.
    m_importState = infrastructure::engine::readRekordboxImportState(m_enginePath.toStdString(),
                                                                     m_rekordboxPath.toStdString());
    emit importStateChanged();
    // The staged fill is NOT cleared here, for the same reason the staged
    // artwork is not: a rescan re-reads the library, it does not unstage
    // what someone asked for. Clearing the flag while the change stayed
    // in the session left a fix that could not be taken back and would
    // still be written by Save.
    emit sampleRatesChanged();
    m_artSources.clear();
    emit artworkChanged();
    const QString stickRoot = QString::fromStdString(
        std::filesystem::path((m_enginePath.isEmpty() ? m_rekordboxPath : m_enginePath).toStdString())
            .parent_path()
            .string());
    const bool wasReadOnly = m_stickReadOnly;
    m_stickReadOnly = !stickRoot.isEmpty()
        && infrastructure::media::isMountedReadOnly(stickRoot.toStdString());
    if (m_stickReadOnly != wasReadOnly) {
        emit stickHealthChanged();
    }
    // Deliberately NOT clearing m_playlistNames/m_playlistTrackCounts
    // here: this scan() call is itself reachable synchronously from
    // inside PlaylistPickerCombo's own row-delegate onClicked handler
    // (JunkCuePage.qml's onPlaylistPicked calls straight back into
    // scan() before that handler's own next line, popup.close(), runs).
    // Clearing them here used to fire issuesChanged() synchronously mid-
    // click, which re-evaluated JunkCuePage.qml's playlistPickerModel
    // and reset the combo's own popup model out from under the very
    // delegate item whose click handler was still executing -- Quick
    // then crashed on a later frame's polishItems() pass, dereferencing
    // that now-destroyed item (confirmed via gdb: SIGSEGV in
    // QQuickItem::setY -> QQuickWindowPrivate::polishItems(), and the
    // popup visibly failing to close, both symptoms of the corrupted
    // item tree). These two never actually needed clearing mid-page-
    // lifetime anyway: rekordboxPath/enginePath don't change within one
    // page's life, only playlistName does, and a playlist's existence/
    // track count doesn't change from a cue repair -- see this
    // property's own doc comment ("picking one never shrinks the
    // picker's own list of choices"), which this now actually holds
    // for every scan() call, not just the first.
    emit issuesChanged();
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);

    m_scanCancel = application::CancellationToken();
    attachSession();
    m_pendingScanFormats.clear();
    if (!rekordboxPath.isEmpty()) {
        m_pendingScanFormats.push_back("rekordbox");
    }
    if (!enginePath.isEmpty()) {
        m_pendingScanFormats.push_back("engine");
    }
    if (!rekordboxPath.isEmpty() &&
        infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString())) {
        m_pendingScanFormats.push_back("onelibrary");
    }

    setBusy(true);
    scanNextPendingFormat();
}

void LibraryConsistencyController::scanNextPendingFormat()
{
    if (m_pendingScanFormats.empty()) {
        setScanningFormat({});
        setBusy(false);
        return;
    }
    QString format = m_pendingScanFormats.front();
    m_pendingScanFormats.erase(m_pendingScanFormats.begin());
    setScanningFormat(format);
    m_watcher.setFuture(QtConcurrent::run(runScanTask, format, pathForFormat(format), m_currentPlaylistName,
                                          makeReporter(), m_scanCancel, m_artSources, m_backupDirectory));
}

void LibraryConsistencyController::cancelScan()
{
    if (scanCancellable()) {
        m_scanCancel.cancel();
    }
}

void LibraryConsistencyController::onScanFinished()
{
    QString thrown;
    LibraryConsistencyScanResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }
    if (result.cancelled) {
        // Whatever earlier formats contributed stays on screen (it is
        // complete for those formats); the rest of the queue is dropped.
        m_pendingScanFormats.clear();
        setScanningFormat({});
        setBusy(false);
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
    } else {
        m_model.appendIssues(std::move(result.issues));
        m_junkCueModel.appendIssues(std::move(result.junkCues));
        if (!result.artSources.empty()) {
            m_artSources = std::move(result.artSources);
        }
        if (result.rescue) {
            m_rescue = result.rescue;
        }
        if (result.artwork.tracksWithArt > 0 || !result.artwork.error.empty()) {
            m_artwork = std::move(result.artwork);
            emit artworkChanged();
        }
        if (result.sampleRates.tracksChecked > 0 || !result.sampleRates.error.empty()) {
            m_sampleRates = std::move(result.sampleRates);
            emit sampleRatesChanged();
        }
        // Rows staged before this rescan keep their mark if they are
        // still listed (the change itself lives in the session).
        for (const auto &[key, info] : m_stagedIssues) {
            int index = indexOfIssueKey(key);
            if (index >= 0) {
                m_model.setStaged(index, true, info.description);
            }
        }
        for (const auto &[key, changeId] : m_stagedJunk) {
            int index = indexOfJunkKey(key);
            if (index >= 0) {
                m_junkCueModel.setStaged(index, true);
            }
        }
        mergePlaylistSummary(result.playlistNames, result.playlistTrackCounts);
        emit issuesChanged();
    }
    scanNextPendingFormat();
}

void LibraryConsistencyController::mergePlaylistSummary(const QStringList &names, const QVariantMap &counts)
{
    for (const auto &name : names) {
        int count = counts.value(name).toInt();
        if (m_playlistTrackCounts.contains(name)) {
            m_playlistTrackCounts[name] = std::max(m_playlistTrackCounts.value(name).toInt(), count);
        } else {
            m_playlistNames << name;
            m_playlistTrackCounts[name] = count;
        }
    }
    // Formats scan (and complete) one at a time, each contributing its
    // own already-sorted slice -- re-sorting the whole list after every
    // merge keeps the picker's overall order stable/alphabetical rather
    // than however the per-format slices happened to interleave.
    m_playlistNames.sort();
}

int LibraryConsistencyIssueListModel::repairableUnstaged() const
{
    int n = 0;
    for (std::size_t i = 0; i < m_issues.size(); ++i) {
        const bool staged = i < m_stagedDescriptions.size() && !m_stagedDescriptions[i].isEmpty();
        if (!staged && m_issues[i].kind == LibraryConsistencyIssue::Kind::Repairable) {
            n++;
        }
    }
    return n;
}

int JunkCueIssueListModel::unstagedCount() const
{
    int n = 0;
    for (std::size_t i = 0; i < m_issues.size(); ++i) {
        if (i >= m_staged.size() || !m_staged[i]) {
            n++;
        }
    }
    return n;
}

int LibraryConsistencyController::repairableCount() const
{
    int n = 0;
    for (const auto &issue : m_model.issues()) {
        if (issue.kind == LibraryConsistencyIssue::Kind::Repairable) {
            n++;
        }
    }
    return n;
}

bool LibraryConsistencyController::writing() const
{
    return m_session && m_session->writing();
}

bool LibraryConsistencyController::canUndo() const
{
    return m_session && m_session->canUndo();
}

int LibraryConsistencyController::indexOfIssueKey(const QString &issueKey) const
{
    const auto &issues = m_model.issues();
    for (size_t i = 0; i < issues.size(); ++i) {
        if (issueKeyFor(issues[i]) == issueKey) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int LibraryConsistencyController::indexOfJunkKey(const QString &junkKey) const
{
    const auto &issues = m_junkCueModel.issues();
    for (size_t i = 0; i < issues.size(); ++i) {
        if (junkKeyFor(issues[i].track) == junkKey) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LibraryConsistencyController::attachSession()
{
    auto *registry = EditSessionRegistry::instance();
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(any));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::stateChanged, this, &LibraryConsistencyController::writingChanged);
            connect(m_session, &LibraryEditSession::canUndoChanged, this,
                    &LibraryConsistencyController::canUndoChanged);
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                if (changeId == QStringLiteral("undo:last-save")) {
                    scan(m_rekordboxPath, m_enginePath, m_currentPlaylistName);
                    return;
                }
                if (changeId == MarkRekordboxImportedChange::idFor()) {
                    m_importMarkStaged = false;
                    m_rescanAfterSave = true;
                    clearStagedStatusIfNothingStaged();
                    emit importStateChanged();
                    return;
                }
                if (auto staged = m_stagedSampleRates.find(changeId); staged != m_stagedSampleRates.end()) {
                    // Counted like the cover art: the numbers come from
                    // the database this just wrote, so they are re-read
                    // once the whole save is done.
                    m_stagedSampleRates.erase(staged);
                    m_sampleRateFillStaged = !m_stagedSampleRates.empty();
                    m_rescanAfterSave = true;
                    clearStagedStatusIfNothingStaged();
                    emit sampleRatesChanged();
                    return;
                }
                if (auto staged = m_stagedArtwork.find(changeId); staged != m_stagedArtwork.end()) {
                    // The counts come from the database this just wrote, so
                    // they are re-read once the whole save is done rather
                    // than guessed at per track.
                    m_stagedArtwork.erase(staged);
                    m_rescanAfterSave = true;
                    clearStagedStatusIfNothingStaged();
                    emit artworkChanged();
                    return;
                }
                for (auto it = m_stagedIssues.begin(); it != m_stagedIssues.end(); ++it) {
                    if (it->second.changeId == changeId) {
                        // A rekordbox repair also writes its cue merge
                        // and row removal into OneLibrary -- no longer
                        // best-effort (2137fcd4: a mirror it cannot
                        // write fails the change), but a mirror write
                        // that DOES land stales an already-listed
                        // OneLibrary issue just the same, in ways only a
                        // fresh check across every format could catch:
                        // re-scan once the save is done (see
                        // saveFinished below).
                        if (changeId.startsWith(QStringLiteral("repair:rekordbox:"))) {
                            m_rescanAfterSave = true;
                        }
                        int index = indexOfIssueKey(it->first);
                        m_stagedIssues.erase(it);
                        if (index >= 0) {
                            m_model.removeIssueAt(index);
                        }
                        clearStagedStatusIfNothingStaged();
                        emit issuesChanged();
                        return;
                    }
                }
                for (auto it = m_stagedJunk.begin(); it != m_stagedJunk.end(); ++it) {
                    if (it->second == changeId) {
                        // Removing a memory cue at 0:00 can't change file
                        // existence or which broken row matches which
                        // survivor, so the row just goes (a re-scan after
                        // every removal was the entire cost of "removing
                        // stray cues is slow").
                        int index = indexOfJunkKey(it->first);
                        m_stagedJunk.erase(it);
                        if (index >= 0) {
                            m_junkCueModel.removeAt(index);
                        }
                        clearStagedStatusIfNothingStaged();
                        emit issuesChanged();
                        return;
                    }
                }
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &) {
                if (m_rescanAfterSave) {
                    m_rescanAfterSave = false;
                    scan(m_rekordboxPath, m_enginePath, m_currentPlaylistName);
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                m_stagedIssues.clear();
                m_stagedJunk.clear();
                m_stagedArtwork.clear();
                m_stagedSampleRates.clear();
                m_sampleRateFillStaged = false;
                m_importMarkStaged = false;
                emit importStateChanged();
                emit sampleRatesChanged();
                m_model.clearStaged();
                m_junkCueModel.clearStaged();
                clearStagedStatusIfNothingStaged();
                emit artworkChanged();
                emit issuesChanged();
            });
        }
    }
    if (m_session) {
        m_session->setLibraryPaths(m_rekordboxPath, m_enginePath);
    }
}

bool LibraryConsistencyController::ensureSessionForStaging()
{
    if (!m_session) {
        attachSession();
        if (!m_session) {
            setErrorMessage("This stick's library could not be identified; nothing was changed.");
            return false;
        }
    }
    if (m_session->writing()) {
        setErrorMessage("A save is running. Stage more once it has finished.");
        return false;
    }
    return true;
}

// How many database writes this save may make against `format`'s
// catalog (drives the scratch-copy decision): every listed Repairable
// issue of that format, the most that could be staged.
int LibraryConsistencyController::repairItemCountHint(const QString &format) const
{
    int count = 0;
    for (const auto &issue : m_model.issues()) {
        if (issue.kind == LibraryConsistencyIssue::Kind::Repairable && issueFormat(issue) == format) {
            count += static_cast<int>(issue.brokenGroup.size()) + (issue.survivorCues.empty() ? 0 : 1);
        }
    }
    return count;
}

void LibraryConsistencyController::stageIssue(int index)
{
    const auto &issues = m_model.issues();
    if (index < 0 || static_cast<size_t>(index) >= issues.size()) {
        return;
    }
    const auto &issue = issues[static_cast<size_t>(index)];
    QString format = issueFormat(issue);
    std::unique_ptr<PendingChange> change;
    if (issue.kind == LibraryConsistencyIssue::Kind::Repairable) {
        change = std::make_unique<RepairIssueChange>(pathForFormat(format), issue, repairItemCountHint(format));
    } else if (issue.kind == LibraryConsistencyIssue::Kind::Missing && format == "onelibrary") {
        change = std::make_unique<DeleteOrphanChange>(m_rekordboxPath, issue);
    } else {
        return;
    }
    if (!ensureSessionForStaging()) {
        return;
    }
    QString key = issueKeyFor(issue);
    QString changeId = change->id();
    QString description = change->description();
    if (!m_session->stage(std::move(change))) {
        return;  // the session reported the lock refusal; the page shows it
    }
    m_stagedIssues[key] = {changeId, description};
    m_model.setStaged(index, true, description);
    emit issuesChanged();
}

void LibraryConsistencyController::repairAll()
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    int staged = 0;
    const size_t count = m_model.issues().size();
    for (size_t i = 0; i < count; ++i) {
        const auto &issue = m_model.issues()[i];
        if (issue.kind != LibraryConsistencyIssue::Kind::Repairable || m_stagedIssues.count(issueKeyFor(issue))) {
            continue;
        }
        stageIssue(static_cast<int>(i));
        staged++;
        if (m_session && !m_session->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
    if (staged > 0) {
        setStagedStatusMessage(
            QStringLiteral("Staged %1 repair(s). Press Save to write them to the stick.").arg(staged));
    }
}

void LibraryConsistencyController::repairOne(int index)
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    stageIssue(index);
}

void LibraryConsistencyController::deleteOrphan(int index)
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    stageIssue(index);
}

void LibraryConsistencyController::unstageIssue(int index)
{
    const auto &issues = m_model.issues();
    if (index < 0 || static_cast<size_t>(index) >= issues.size()) {
        return;
    }
    auto it = m_stagedIssues.find(issueKeyFor(issues[static_cast<size_t>(index)]));
    if (it == m_stagedIssues.end()) {
        return;
    }
    if (m_session) {
        m_session->unstage(it->second.changeId);
    }
    m_stagedIssues.erase(it);
    m_model.setStaged(index, false, QString());
    clearStagedStatusIfNothingStaged();
    emit issuesChanged();
}

void LibraryConsistencyController::stageJunkCue(int index)
{
    const auto &issues = m_junkCueModel.issues();
    if (index < 0 || static_cast<size_t>(index) >= issues.size()) {
        return;
    }
    const domain::Track &track = issues[static_cast<size_t>(index)].track;
    if (!ensureSessionForStaging()) {
        return;
    }
    QString key = junkKeyFor(track);
    // Every cue this track has a row for that isJunkCue() cannot see --
    // the clustered hot cues, which sit past the first second. The
    // change strips what it is handed rather than re-deriving it, so a
    // row the user staged cannot survive the rewrite while the counters
    // say it went.
    std::vector<domain::CuePoint> alsoRemove;
    for (const auto &issue : issues) {
        if (junkKeyFor(issue.track) == key && !domain::isJunkCue(issue.cue)) {
            alsoRemove.push_back(issue.cue);
        }
    }
    auto change = std::make_unique<RemoveJunkCueChange>(pathForFormat(QString::fromStdString(track.format)), track,
                                                         std::move(alsoRemove));
    QString changeId = change->id();
    if (!m_session->stage(std::move(change))) {
        return;
    }
    m_stagedJunk[key] = changeId;
    // Every row of that track, not just the one clicked. One row is one
    // CUE, one change is one TRACK, and the change removes every stray
    // cue the track carries (cuesWithoutJunk). Marking only the clicked
    // row left the others reading as unstaged although their cues were
    // going: the list showed work still to do, unstagedJunkCueCount()
    // never reached zero, so the button that goes quiet once it has
    // staged everything it can offer never went quiet, and the staged
    // count below undercounted the cues by one per extra cue on a track.
    for (size_t row = 0; row < issues.size(); ++row) {
        if (junkKeyFor(issues[row].track) == key) {
            m_junkCueModel.setStaged(static_cast<int>(row), true);
        }
    }
    emit issuesChanged();
}

void LibraryConsistencyController::removeJunkCue(int index)
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    stageJunkCue(index);
}

void LibraryConsistencyController::removeAllJunkCues()
{
    if (m_busy) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    // Two numbers, and the sentence needs the first: cues are what the
    // user is looking at and what Save reports having written, tracks are
    // how the work is carried (one change each). A real stick made the
    // difference visible -- 185 stray cues on 173 tracks -- and the
    // message said 173 while 185 cues went.
    int tracksStaged = 0;
    int cuesStaged = 0;
    const size_t count = m_junkCueModel.issues().size();
    for (size_t i = 0; i < count; ++i) {
        const QString key = junkKeyFor(m_junkCueModel.issues()[i].track);
        if (m_stagedJunk.count(key)) {
            continue;
        }
        stageJunkCue(static_cast<int>(i));
        if (!m_stagedJunk.count(key)) {
            // Staging was refused (no session, or the lock has gone).
            // Counting it here would report work that is not staged.
            break;
        }
        tracksStaged++;
        for (size_t row = i; row < count; ++row) {
            if (junkKeyFor(m_junkCueModel.issues()[row].track) == key) {
                cuesStaged++;
            }
        }
        if (m_session && !m_session->lockHeld()) {
            return;
        }
    }
    if (cuesStaged > 0) {
        setStagedStatusMessage(QStringLiteral("Staged removing %1 stray cue(s) on %2 track(s). Press Save to write "
                                              "it to the stick.")
                                   .arg(cuesStaged)
                                   .arg(tracksStaged));
    }
}

void LibraryConsistencyController::repairStickFilesystem()
{
    if (m_repairingFilesystem || m_busy) {
        return;
    }
    const std::string stickRoot =
        std::filesystem::path((m_enginePath.isEmpty() ? m_rekordboxPath : m_enginePath).toStdString())
            .parent_path()
            .string();
    if (stickRoot.empty()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_repairingFilesystem = true;
    m_filesystemMessage.clear();
    emit stickHealthChanged();
    // The check unmounts, repairs and mounts again: minutes on a full
    // stick, and none of it belongs on the UI thread.
    m_repairWatcher.setFuture(QtConcurrent::run(
        [stickRoot]() { return infrastructure::media::repairFilesystem(stickRoot); }));
}

void LibraryConsistencyController::onFilesystemRepairFinished()
{
    const auto result = gui::takeResult(m_repairWatcher);
    m_repairingFilesystem = false;
    m_filesystemMessage = QString::fromStdString(result.message);
    const std::string stickRoot =
        std::filesystem::path((m_enginePath.isEmpty() ? m_rekordboxPath : m_enginePath).toStdString())
            .parent_path()
            .string();
    m_stickReadOnly = infrastructure::media::isMountedReadOnly(stickRoot);
    emit stickHealthChanged();
    const bool worked = result.repaired && !m_stickReadOnly;
    emit filesystemRepairFinished(worked, result.declined, m_filesystemMessage);
    if (result.declined) {
        return;  // the user said no; not a fault to report as one
    }
    if (worked) {
        setStatusMessage(m_filesystemMessage);
        // Everything found before was found on a stick that could not be
        // written to; read it again now that it can.
        scan(m_rekordboxPath, m_enginePath, m_currentPlaylistName);
    } else {
        setErrorMessage(m_filesystemMessage);
    }
}

void LibraryConsistencyController::repairArtwork()
{
    if (m_busy || artworkRepairStaged()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    std::vector<infrastructure::engine::ArtworkEntry> repairable;
    for (const auto &entry : m_artwork.unreadable) {
        // Whatever the scan found a source for -- art beside it on the
        // stick, the track's own tags, a backup. Filtering on the on-stick
        // file alone made the page promise a count it then refused to act
        // on, which is the one thing a count must never do.
        if (!entry.imageOnStick.empty() || entry.otherSource) {
            repairable.push_back(entry);
        }
    }
    if (repairable.empty()) {
        setErrorMessage("No copy of these covers was found on this stick, in the tracks themselves, or in a backup, "
                        "so there is nothing to put back.");
        return;
    }
    if (!ensureSessionForStaging()) {
        return;
    }
    // One change per track, the unit the save summary counts and the
    // progress bar ticks. The hint tells the first of them how many are
    // coming, so the whole run goes through one scratch copy of m.db.
    //
    // Staged in one call: a thousand separate stage() calls is quadratic
    // twice over -- a duplicate-id scan per change, and a pendingChanged()
    // per change that has QML rebuild the whole pending-description list
    // for the Save button's tooltip -- which froze the window on one press
    // of a button whose whole job is to be pressed once.
    const int count = static_cast<int>(repairable.size());
    std::vector<std::unique_ptr<PendingChange>> changes;
    changes.reserve(repairable.size());
    std::set<QString> ids;
    for (const auto &entry : repairable) {
        // Only the first declares the database, so the save takes one
        // checkpoint copy of it rather than one per track: see
        // RepairArtworkChange::filesToBackup().
        changes.push_back(
            std::make_unique<RepairArtworkChange>(m_enginePath, entry, count, changes.empty(), m_rescue));
        ids.insert(RepairArtworkChange::idFor(entry.trackId));
    }
    if (!m_session->stageAll(std::move(changes))) {
        return;  // the session reported the refusal; the page shows it
    }
    m_stagedArtwork = std::move(ids);
    emit artworkChanged();
    setStagedStatusMessage(
        QStringLiteral("Staged cover art for %1 track(s). Press Save to write it to the stick.").arg(count));
}

void LibraryConsistencyController::fillSampleRates()
{
    if (m_busy || m_sampleRateFillStaged) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    std::vector<infrastructure::engine::SampleRateEntry> writable;
    for (const auto &entry : m_sampleRates.missing) {
        if (entry.sampleRateFromFile > 0.0) {
            writable.push_back(entry);
        }
    }
    if (writable.empty()) {
        setErrorMessage("None of these tracks' files could say what sample rate they are, so there is nothing to "
                        "write.");
        return;
    }
    if (!ensureSessionForStaging()) {
        return;
    }
    const int count = static_cast<int>(writable.size());
    // One change per track, the unit the save summary counts and the
    // progress bar ticks, staged in one call for the same reason the
    // artwork repair is: a thousand separate stage() calls is quadratic
    // twice over.
    std::vector<std::unique_ptr<PendingChange>> changes;
    std::set<QString> ids;
    changes.reserve(writable.size());
    for (const auto &entry : writable) {
        changes.push_back(
            std::make_unique<FillSampleRateChange>(m_enginePath, entry, count, changes.empty()));
        ids.insert(FillSampleRateChange::idFor(entry.trackId));
    }
    if (!m_session->stageAll(std::move(changes))) {
        return;  // the session reported the refusal; the page shows it
    }
    m_stagedSampleRates = std::move(ids);
    m_sampleRateFillStaged = true;
    emit sampleRatesChanged();
    setStagedStatusMessage(
        QStringLiteral("Staged the sample rate for %1 track(s). Press Save to write it to the stick.").arg(count));
}

void LibraryConsistencyController::markRekordboxImported()
{
    if (m_busy || m_importMarkStaged || !m_importState.playerWillOfferImport()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    if (!ensureSessionForStaging()) {
        return;
    }
    if (!m_session->stage(
            std::make_unique<MarkRekordboxImportedChange>(m_enginePath, m_importState.librarySequence))) {
        return;  // the session reported the refusal; the page shows it
    }
    m_importMarkStaged = true;
    emit importStateChanged();
    setStagedStatusMessage(
        QStringLiteral("Staged marking this stick's rekordbox library as imported. Press Save to write it."));
}

void LibraryConsistencyController::unstageRekordboxImportMark()
{
    if (!m_importMarkStaged) {
        return;
    }
    if (m_session) {
        m_session->unstage(MarkRekordboxImportedChange::idFor());
    }
    m_importMarkStaged = false;
    emit importStateChanged();
    clearStagedStatusIfNothingStaged();
}

void LibraryConsistencyController::unstageSampleRateFill()
{
    if (!m_sampleRateFillStaged) {
        return;
    }
    if (m_session) {
        QStringList staged;
        for (const QString &id : m_stagedSampleRates) {
            staged << id;
        }
        m_session->unstageAll(staged);
    }
    m_stagedSampleRates.clear();
    m_sampleRateFillStaged = false;
    emit sampleRatesChanged();
    clearStagedStatusIfNothingStaged();
}

void LibraryConsistencyController::unstageArtworkRepair()
{
    if (m_stagedArtwork.empty()) {
        return;
    }
    if (m_session) {
        QStringList ids;
        for (const QString &staged : m_stagedArtwork) {
            ids << staged;
        }
        m_session->unstageAll(ids);
    }
    m_stagedArtwork.clear();
    emit artworkChanged();
    // One way of clearing the staged line, shared with the other two:
    // clearing it here by hand would keep working while the mechanism
    // rotted, and a test of this path would prove nothing.
    clearStagedStatusIfNothingStaged();
}

void LibraryConsistencyController::unstageJunkCue(int index)
{
    const auto &issues = m_junkCueModel.issues();
    if (index < 0 || static_cast<size_t>(index) >= issues.size()) {
        return;
    }
    const QString key = junkKeyFor(issues[static_cast<size_t>(index)].track);
    auto it = m_stagedJunk.find(key);
    if (it == m_stagedJunk.end()) {
        return;
    }
    if (m_session) {
        m_session->unstage(it->second);
    }
    m_stagedJunk.erase(it);
    // Every row of that track, the way staging set them. One change
    // covers all of a track's stray cues, so clearing only the row that
    // was clicked left its neighbours badged "staged" with an Unstage
    // button that does nothing -- their key is gone from m_stagedJunk,
    // so this function returns above -- for work that will never be
    // written. It also left unstagedJunkCueCount() too low, which is
    // what decides whether "Remove All" still has anything to offer.
    for (size_t row = 0; row < issues.size(); ++row) {
        if (junkKeyFor(issues[row].track) == key) {
            m_junkCueModel.setStaged(static_cast<int>(row), false);
        }
    }
    clearStagedStatusIfNothingStaged();
    emit issuesChanged();
}

void LibraryConsistencyController::ignoreJunkCue(int index)
{
    unstageJunkCue(index);  // a dismissed row must not be written after all
    m_junkCueModel.removeAt(index);
}

void LibraryConsistencyController::ignoreAllJunkCues()
{
    for (int i = static_cast<int>(m_junkCueModel.issues().size()) - 1; i >= 0; --i) {
        unstageJunkCue(i);
    }
    m_junkCueModel.clear();
}

void LibraryConsistencyController::undoLastOperation()
{
    if (m_busy || !m_session) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_session->undoLastSave();
}

void LibraryConsistencyController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void LibraryConsistencyController::setScanProgress(int current, int total)
{
    if (m_scanCurrent == current && m_scanTotal == total) {
        return;
    }
    m_scanCurrent = current;
    m_scanTotal = total;
    emit scanProgressChanged();
}

void LibraryConsistencyController::setScanningFormat(const QString &format)
{
    if (m_scanningFormat == format) {
        return;
    }
    m_scanningFormat = format;
    emit scanningFormatChanged();
}

void LibraryConsistencyController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void LibraryConsistencyController::setStagedStatusMessage(const QString &message)
{
    setStatusMessage(message);
    m_statusIsAboutStaging = !message.isEmpty();
}

void LibraryConsistencyController::clearStagedStatusIfNothingStaged()
{
    if (m_statusIsAboutStaging && m_stagedIssues.empty() && m_stagedJunk.empty() && m_stagedArtwork.empty()
        && !m_sampleRateFillStaged && !m_importMarkStaged) {
        setStatusMessage({});
    }
}

void LibraryConsistencyController::setStatusMessage(const QString &message)
{
    m_statusIsAboutStaging = false;
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
