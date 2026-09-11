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
                                          application::CancellationToken cancel)
{
    LibraryConsistencyScanResult result;
    try {
        auto tracks = scanTracks(format, path, reporter, cancel);

        tallyPlaylists(tracks, result);

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
            std::error_code ec;
            bool exists = !t.filePath.empty() && fs::exists(t.filePath, ec);
            (exists ? healthy : broken).push_back(std::move(t));
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
                                          makeReporter(), m_scanCancel));
}

void LibraryConsistencyController::cancelScan()
{
    if (scanCancellable()) {
        m_scanCancel.cancel();
    }
}

void LibraryConsistencyController::onScanFinished()
{
    LibraryConsistencyScanResult result = m_watcher.result();
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
                for (auto it = m_stagedIssues.begin(); it != m_stagedIssues.end(); ++it) {
                    if (it->second.changeId == changeId) {
                        // A rekordbox repair mirrors its cue merge/row
                        // removal into OneLibrary best-effort, which can
                        // silently stale an already-listed OneLibrary
                        // issue in ways only a fresh check across every
                        // format could catch: re-scan once the save is
                        // done (see saveFinished below).
                        if (changeId.startsWith(QStringLiteral("repair:rekordbox:"))) {
                            m_rescanAfterSave = true;
                        }
                        int index = indexOfIssueKey(it->first);
                        m_stagedIssues.erase(it);
                        if (index >= 0) {
                            m_model.removeIssueAt(index);
                        }
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
                m_model.clearStaged();
                m_junkCueModel.clearStaged();
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
        setErrorMessage("A save is running -- stage more once it has finished.");
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
        setStatusMessage(QStringLiteral("Staged %1 repair(s). Press Save to write them to the stick.").arg(staged));
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
    auto change = std::make_unique<RemoveJunkCueChange>(pathForFormat(QString::fromStdString(track.format)), track);
    QString key = junkKeyFor(track);
    QString changeId = change->id();
    if (!m_session->stage(std::move(change))) {
        return;
    }
    m_stagedJunk[key] = changeId;
    m_junkCueModel.setStaged(index, true);
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
    int staged = 0;
    const size_t count = m_junkCueModel.issues().size();
    for (size_t i = 0; i < count; ++i) {
        if (m_stagedJunk.count(junkKeyFor(m_junkCueModel.issues()[i].track))) {
            continue;
        }
        stageJunkCue(static_cast<int>(i));
        staged++;
        if (m_session && !m_session->lockHeld()) {
            return;
        }
    }
    if (staged > 0) {
        setStatusMessage(
            QStringLiteral("Staged removing %1 stray cue(s). Press Save to write it to the stick.").arg(staged));
    }
}

void LibraryConsistencyController::unstageJunkCue(int index)
{
    const auto &issues = m_junkCueModel.issues();
    if (index < 0 || static_cast<size_t>(index) >= issues.size()) {
        return;
    }
    auto it = m_stagedJunk.find(junkKeyFor(issues[static_cast<size_t>(index)].track));
    if (it == m_stagedJunk.end()) {
        return;
    }
    if (m_session) {
        m_session->unstage(it->second);
    }
    m_stagedJunk.erase(it);
    m_junkCueModel.setStaged(index, false);
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

void LibraryConsistencyController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

}  // namespace seabass::gui
