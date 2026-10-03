// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "library_consistency_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "application/path_key.hpp"
#include "application/phased_progress.hpp"
#include "application/track_file_presence.hpp"
#include "domain/clustered_cue.hpp"
#include "domain/junk_cue.hpp"
#include "domain/track_scope.hpp"
#include "gui/stick_path.hpp"
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
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cleanup_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/delete_orphan_change.hpp"
#include "gui/edit/changes/remove_junk_cue_change.hpp"
#include "gui/edit/changes/repair_artwork_change.hpp"

#include "gui/future_result.hpp"
#include "gui/main_thread_shared.hpp"
#include "infrastructure/media/filesystem_health.hpp"
#include "gui/artwork_rescue_sources.hpp"
#include "gui/edit/changes/fill_sample_rate_change.hpp"
#include "gui/edit/changes/finish_cleanup_change.hpp"
#include "gui/edit/changes/recolour_engine_cues_change.hpp"
#include "gui/edit/changes/repair_legacy_memory_list_change.hpp"
#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif
#include "gui/app_settings_controller.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/edit/changes/repair_issue_change.hpp"
#include "gui/qt_path.hpp"

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
                                       application::ProgressReporter &progress,
                                       application::CancellationToken cancel = application::CancellationToken::none())
{
    // No explicit format check here -- LibraryCatalogCache::tracksFor()
    // already throws for anything unrecognized (see its own realScan()),
    // caught by the same catch (const std::exception &) below either way.
    return LibraryCatalogCache::instance().tracksFor(format.toStdString(), path.toStdString(), progress, cancel);
}

// What one leg of the scan will announce, from the catalogs' row counts:
// the cache's own passes, then the per-row checks this file runs on top
// (see runScanTask). The Engine leg's "Reading cover images" has no
// cheap count and is left out; a plan short by a few rows costs the bar
// a moment at the end, not a restart.
std::optional<size_t> planLeg(const QString &format, const QString &path, const QString &rekordboxPath,
                              LibraryConsistencyController::ScanDepth depth)
{
    LibraryCatalogCache &cache = LibraryCatalogCache::instance();
    const std::string fmt = format.toStdString();
    const std::string at = path.toStdString();
    const auto read = cache.plannedUnits(fmt, at, LibraryCatalogCache::Detail::Full);
    if (!read) {
        return std::nullopt;
    }
    size_t units = *read;
    if (depth != LibraryConsistencyController::Full) {
        return units;
    }
    const auto rows = cache.countTracks(fmt, at);
    if (!rows) {
        return std::nullopt;
    }
    if (format == QStringLiteral("rekordbox")) {
        units += *rows;  // "Checking cue lists"
    } else if (format == QStringLiteral("engine")) {
        units += *rows;  // "Checking cover art"
        units += 1;      // "Counting tracks the player will analyse"
        units += *rows;  // "Checking sample rates"
    } else if (format == QStringLiteral("onelibrary")) {
        // "Looking for Clean Up leftovers": a key per rekordbox row and
        // per OneLibrary row (its deleted-file list is not counted). The
        // rekordbox read it makes first is a cache hit by then.
        const auto rekordboxRows = cache.countTracks("rekordbox", rekordboxPath.toStdString());
        units += *rows + rekordboxRows.value_or(0);
        units += *rows;  // "Checking cue lists"
    }
    return units;
}

ScanChainPlan planChain(const QStringList &formats, const QString &rekordboxPath, const QString &enginePath,
                        LibraryConsistencyController::ScanDepth depth)
{
    ScanChainPlan plan;
    plan.formats = formats;
    plan.known = true;
    for (const QString &format : formats) {
        const QString path = format == QStringLiteral("engine") ? enginePath : rekordboxPath;
        const auto units = planLeg(format, path, rekordboxPath, depth);
        plan.unitsPerLeg.push_back(units.value_or(0));
        if (!units) {
            plan.known = false;
        }
    }
    return plan;
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

}  // namespace

size_t ScanChainPlan::total() const
{
    size_t sum = 0;
    for (size_t units : unitsPerLeg) {
        sum += units;
    }
    return sum;
}

size_t ScanChainPlan::offsetOf(const QString &format) const
{
    size_t offset = 0;
    for (qsizetype i = 0; i < formats.size() && i < static_cast<qsizetype>(unitsPerLeg.size()); ++i) {
        if (formats[i] == format) {
            return offset;
        }
        offset += unitsPerLeg[static_cast<size_t>(i)];
    }
    return offset;
}

size_t ScanChainPlan::unitsOf(const QString &format) const
{
    for (qsizetype i = 0; i < formats.size() && i < static_cast<qsizetype>(unitsPerLeg.size()); ++i) {
        if (formats[i] == format) {
            return unitsPerLeg[static_cast<size_t>(i)];
        }
    }
    return 0;
}

namespace
{

// How many runScanTask calls are running right now, in any controller.
// Read by the tests that check leaving a page leaves no scan behind.
std::atomic_int &runningScanTasks()
{
    static std::atomic_int running{0};
    return running;
}

// Runs entirely on a background thread (see LibraryConsistencyController::
// scanNextPendingFormat()) - no access to the controller itself. Scans
// exactly one format; the controller chains one of these per present
// catalog to get the progressive, format-at-a-time behavior. playlistName
// empty scans/checks the whole format's library, same as before this
// parameter existed; a real name scopes both the junk-cue list and the
// consistency check to just that playlist's tracks, via domain::TrackScope
// -- same seam SyncController::runAnalyzeTask already uses. `depth`
// CuesOnly stops at the stray-cue finders (see ScanDepth).
//
// `chain` is the whole scan's plan (ScanChainPlan): empty on the first
// leg, which counts it here, on this thread, and hands it back in the
// result; the legs after it get it from the controller and continue the
// one bar at their own offset. `chainFormats`, `rekordboxPath` and
// `enginePath` are for that count.
LibraryConsistencyScanResult runScanTask(QString format, QString path, QString playlistName,
                                          LibraryConsistencyController::ScanDepth depth,
                                          std::shared_ptr<QtProgressReporter> reporter,
                                          application::CancellationToken cancel,
                                          infrastructure::engine::ArtworkSourceByTrackFile artSources,
                                          QString backupDirectory, ScanChainPlan chain, QStringList chainFormats,
                                          QString rekordboxPath, QString enginePath)
{
    runningScanTasks().fetch_add(1);
    struct Running
    {
        ~Running() { runningScanTasks().fetch_sub(1); }
    } running;
    LibraryConsistencyScanResult result;
    // One bar for the chain (#58): announced by the first leg with the
    // whole scan's total, continued by the rest from where the leg
    // before left it. Every stretch below, the cache's passes included,
    // lands on it end to end with its own label.
    const bool firstLeg = chain.empty();
    if (firstLeg) {
        chain = planChain(chainFormats, rekordboxPath, enginePath, depth);
        result.plan = chain;
    }
    const size_t total = chain.known ? chain.total() : 0;
    const bool lastLeg = !chain.formats.isEmpty() && chain.formats.last() == format;
    application::PhasedProgress progress(*reporter, "Checking the library", total, chain.offsetOf(format),
                                         /*announce=*/firstLeg, /*endsOperation=*/lastLeg);
    try {
        auto tracks = scanTracks(format, path, progress, cancel);
        cancel.throwIfCancelled();

        tallyPlaylists(tracks, result);
        const bool full = depth == LibraryConsistencyController::Full;

        if (full && format == QStringLiteral("onelibrary")) {
            // #8: what a Clean Up removed from export.pdb and never from
            // here. Before the playlist scope below, like the Engine
            // audits: a leftover is a fact about the library, and one
            // outside the chosen playlist is still listed twice on a
            // player. The rekordbox rows come from the catalog cache the
            // rekordbox leg just filled. No audio probe: a pair only the
            // decoded audio would tie together is reported, not repaired.
            try {
                const auto rekordboxTracks = scanTracks(QStringLiteral("rekordbox"), path, progress, cancel);
                const auto deleted = infrastructure::rekordbox::deletedTrackFilePaths(path.toStdString());
                cancel.throwIfCancelled();
                // The token rides in on the key: spelling every path of
                // both catalogs is most of the finder's time (0.2 s of a
                // Debug build on the committed fixture), and a stop there
                // must not wait for all of it. So does the progress: one
                // key per deleted file and per row of either catalog,
                // then a few more for the rows it pairs up, which the
                // count holds at the total rather than running past it.
                const size_t keysExpected = deleted.size() + rekordboxTracks.size() + tracks.size();
                progress.start("Looking for Clean Up leftovers", keysExpected);
                size_t keysSpelled = 0;
                const auto keyOrStop = [&cancel, &progress, &keysSpelled, keysExpected](const std::string &file) {
                    cancel.throwIfCancelled();
                    if (keysSpelled < keysExpected) {
                        progress.tick(++keysSpelled);
                    }
                    return application::normalizedPathKey(file);
                };
                result.cleanupLeftovers =
                    domain::CleanupLeftoverFinder::find(tracks, rekordboxTracks, deleted, keyOrStop);
                result.cleanupLeftoversChecked = true;
            } catch (const application::OperationCancelled &) {
                throw;
            } catch (const std::exception &e) {
                // Its own error, so the rest of this leg still reports.
                result.cleanupLeftoversError = e.what();
            }
        }

        if (full && format == QStringLiteral("onelibrary")) {
            // #59, #60: a OneLibrary row names its analysis file too, and
            // the players take that row's cues from it. The files
            // export.pdb also names were checked by the rekordbox leg;
            // these are the rest, appended to what it found.
            try {
                // A stick with OneLibrary alone has no export.pdb: then no
                // file is DeviceLibrary's and every one is checked here.
                std::set<std::string> deviceLibraryKeys;
                std::error_code pdbError;
                if (std::filesystem::exists(pathFromQString(path) / "rekordbox" / "export.pdb", pdbError)) {
                    const infrastructure::rekordbox::AnlzPathIndex analysisPaths(path.toStdString());
                    for (const auto &p : analysisPaths.paths()) {
                        deviceLibraryKeys.insert(application::normalizedPathKey(p));
                    }
                }
                std::vector<std::string> analyzePaths;
                std::map<std::string, const domain::Track *> trackOf;
                for (const domain::Track &track : tracks) {
                    const std::string key = track.analysisFile.empty()
                                                ? std::string()
                                                : application::normalizedPathKey(track.analysisFile);
                    if (!key.empty()) {
                        result.oneLibraryAnalysisKeys.insert(key);
                    }
                    if (key.empty() || deviceLibraryKeys.count(key)) {
                        analyzePaths.emplace_back();
                        continue;
                    }
                    analyzePaths.push_back(track.analysisFile);
                    trackOf.emplace(key, &track);
                }
                const auto named = [&](const std::string &p) {
                    const std::string key = application::normalizedPathKey(p);
                    return deviceLibraryKeys.count(key) > 0 || result.oneLibraryAnalysisKeys.count(key) > 0;
                };
                progress.start("Checking cue lists", tracks.size());
                size_t checked = 0;
                auto scan = infrastructure::rekordbox::scanCueLists(path.toStdString(), analyzePaths, named, [&] {
                    cancel.throwIfCancelled();
                    progress.tick(++checked);
                });
                for (auto &finding : scan.findings) {
                    const domain::Track *track = trackOf.at(application::normalizedPathKey(finding.analyzePath));
                    result.legacyMemoryLists.push_back({*track, std::move(finding)});
                }
                result.cueListTally = scan.tally;
                result.cueListsAppend = true;
                result.legacyMemoryListsChecked = true;
            } catch (const application::OperationCancelled &) {
                throw;
            } catch (const std::exception &e) {
                result.cueListsAppend = true;
                result.legacyMemoryListsError = e.what();
            }
        }

        if (full && format == QStringLiteral("rekordbox")) {
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
            // #55 and #60: each analysis file's legacy memory list, its
            // two generations of cue list compared, and what a hung
            // player left beside it. Two small reads per track over USB
            // (the cue sections only, not the waveforms), so it has its
            // own counted phase, and its own error so the rest of this
            // leg still reports.
            try {
                const infrastructure::rekordbox::AnlzPathIndex analysisPaths(path.toStdString());
                std::vector<std::string> analyzePaths;
                std::map<std::string, const domain::Track *> trackOf;
                for (const domain::Track &track : tracks) {
                    // The row's own analyze_path, as the writer looks it
                    // up (an empty one is skipped and still ticks).
                    std::optional<std::string> analyzePath;
                    if (!track.sourceId.empty()
                        && std::all_of(track.sourceId.begin(), track.sourceId.end(),
                                       [](unsigned char c) { return std::isdigit(c) != 0; })) {
                        analyzePath = analysisPaths.pathFor(static_cast<uint32_t>(std::stoul(track.sourceId)));
                    }
                    analyzePaths.push_back(analyzePath.value_or(std::string()));
                    if (analyzePath) {
                        trackOf.emplace(application::normalizedPathKey(*analyzePath), &track);
                    }
                }
                progress.start("Checking cue lists", tracks.size());
                size_t checked = 0;
                const auto named = [&analysisPaths](const std::string &p) { return analysisPaths.names(p); };
                auto scan = infrastructure::rekordbox::scanCueLists(path.toStdString(), analyzePaths, named, [&] {
                    cancel.throwIfCancelled();
                    progress.tick(++checked);
                });
                for (auto &finding : scan.findings) {
                    const domain::Track *track = trackOf.at(application::normalizedPathKey(finding.analyzePath));
                    result.legacyMemoryLists.push_back({*track, std::move(finding)});
                }
                result.cueListTally = scan.tally;
                result.legacyMemoryListsChecked = true;
            } catch (const application::OperationCancelled &) {
                throw;
            } catch (const std::exception &e) {
                result.legacyMemoryListsError = e.what();
            }
        }

        if (full && format == QStringLiteral("engine")) {
            // Pads the player hides: no catalog but Engine has them, and
            // the finder needs nothing beyond the rows just read.
            result.hiddenEngineCues = domain::HiddenEngineCueFinder::find(tracks);
            result.hiddenCuesChecked = true;
        }
        if (full && format == QStringLiteral("engine")) {
            // Cover art, checked while this format's library is open
            // anyway: one read of Track/AlbumArt and a stat per image,
            // nothing next to the scan itself.
            // The rescue sources outlive the scan: the repair that
            // follows asks the same object for the bytes, so a backup
            // archive is opened once rather than once per cover.
            //
            // Each audit checks the token per row, and the scan checks it
            // between them: the controller's destructor waits for this
            // task, so how quickly a stop lands is how quickly Back leaves.
            result.rescue = std::make_shared<ArtworkRescueSources>(
                backupDirectory.toStdString(),
                pathToUtf8(pathFromQString(path).parent_path()));
            result.artwork = infrastructure::engine::auditArtwork(path.toStdString(), artSources,
                                                                  result.rescue->probe(), cancel, progress);
            cancel.throwIfCancelled();
            // The same pass asks each row for its sample rate, and each
            // file whose row cannot say. A library where nothing is
            // missing costs nothing. One where much is costs a file open
            // per row: measured on 50 MP3s, about 0.1 ms each from the page
            // cache and 0.65 ms cold from an SSD, reading 128 KiB and 12
            // read calls per file (see TagLibMetadataProbe). On a USB stick
            // that is minutes for a few thousand rows, which is why the
            // stray-cue pages scan at CuesOnly and never get here.
            // #38: one count query against Track, no file reads, so it
            // costs nothing next to the two audits around it. Reported as
            // one step all the same, so the phase the page shows is this
            // one and not the audit before it.
            progress.start("Counting tracks the player will analyse", 1);
            result.analysisState = infrastructure::engine::auditAnalysisState(path.toStdString());
            progress.tick(1);
            cancel.throwIfCancelled();
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
                },
                cancel, progress);
            cancel.throwIfCancelled();
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

        if (!full) {
            // The stray-cue pages are done: what follows stats every
            // track's file, one round trip per track on a USB stick, for
            // the missing-file issues those pages never show.
            return result;
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
    connect(&m_repairWatcher, &QFutureWatcher<infrastructure::media::FilesystemRepairResult>::finished, this,
            &LibraryConsistencyController::onFilesystemRepairFinished);
    connect(&m_importStateWatcher, &QFutureWatcher<infrastructure::engine::RekordboxImportState>::finished, this,
            [this]() {
                QString thrown;
                m_importState = gui::takeResult(m_importStateWatcher, &thrown);
                if (!thrown.isEmpty()) {
                    m_importState.error = thrown.toStdString();
                }
                emit importStateChanged();
            });
    // Where this computer keeps full stick backups: read once, the same
    // way and from the same key AppSettingsController writes it, so a
    // cover lost from the stick can be looked for in them.
    QSettings settings = openSeabassSettings();
    m_backupDirectory = settings.value(QStringLiteral("stickBackupDirectory"),
                                       AppSettingsController::defaultStickBackupDirectory())
                            .toString();
}

// Leaving a page destroys its controller, and nothing this controller
// started may outlive it.
//
// A scan is stopped and waited for. Left running, it kept the Engine
// database and export.pdb open for a page that had gone: an eject
// straight after met "device busy", a save on another page met a
// database still being read, and coming back started a second scan
// alongside the first. Every leg checks the token at row grain (the
// catalog readers, each Engine audit, the Clean Up leftover check), so
// the wait is the time to the next row, not the rest of the scan. The one
// step it cannot cut short is OneLibrary's key derivation on open, a
// single SQLCipher call of about 0.13 s. tst_ScanLifetime measures it.
//
// The filesystem repair is this controller's one write of its own (the
// fixes it stages are written by the edit session, which outlives it).
// It unmounts, checks and remounts the stick, and cannot be cancelled,
// so it is waited for to the end: abandoned, the stick would come back
// with nobody here to read how it went or to rescan it. The page that
// starts it keeps Back disabled while it runs, so this wait is for the
// window closing under it.
LibraryConsistencyController::~LibraryConsistencyController()
{
    // The scan is m_scan's: cancelled and let go. The repair is a write,
    // and is waited for.
    m_pendingScanFormats.clear();
    awaitQuietly(m_repairWatcher);
    awaitQuietly(m_importStateWatcher);
}

int LibraryConsistencyController::runningScanTasksForTesting()
{
    return runningScanTasks().load();
}

std::shared_ptr<QtProgressReporter> LibraryConsistencyController::makeReporter()
{
    // Speaks only for the leg started right after it, while that leg is
    // the one outstanding: a superseded leg reports until it notices.
    auto reporter = makeMainThreadShared<QtProgressReporter>();
    const auto current = m_scan.speaksForNext();
    // One announcement for the whole chain, from its first leg; the
    // phases name the stretch, and the count only ever goes up.
    connect(reporter.get(), &QtProgressReporter::started, this, [this, current](const QString &, int total) {
        if (current()) {
            setScanProgress(0, total);
        }
    });
    connect(reporter.get(), &QtProgressReporter::phaseChanged, this, [this, current](const QString &label) {
        if (current()) {
            setScanPhase(label);
        }
    });
    connect(reporter.get(), &QtProgressReporter::progressed, this, [this, current](int done) {
        if (current()) {
            setScanProgress(done, m_scanTotal);
        }
    });
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

int LibraryConsistencyController::artworkEmptyInDatabaseCount() const
{
    return static_cast<int>(std::count_if(m_artwork.unreadable.begin(), m_artwork.unreadable.end(),
                                          [](const infrastructure::engine::ArtworkEntry &entry) {
                                              return entry.storage
                                                  == infrastructure::engine::ArtworkStorage::InDatabaseUnreadable;
                                          }));
}

void LibraryConsistencyController::scan(const QString &rekordboxPath, const QString &enginePath,
                                          const QString &playlistName)
{
    startScanChain(rekordboxPath, enginePath, playlistName, false);
}

void LibraryConsistencyController::rescanAfterWrite()
{
    // What a scan still running read is from before the write: this one
    // starts afresh rather than being answered by it. It used to be
    // dropped when a scan was running, with the rescan flag already
    // cleared, and the page showed issues the save had just fixed.
    startScanChain(m_rekordboxPath, m_enginePath, m_currentPlaylistName, true);
}

void LibraryConsistencyController::startScanChain(const QString &rekordboxPath, const QString &enginePath,
                                                  const QString &playlistName, bool restart)
{
    // The depth is part of the request: a Full scan asked for while a
    // CuesOnly one runs is not answered by it.
    const QString scope = rekordboxPath + QLatin1Char('\n') + enginePath + QLatin1Char('\n') + playlistName
        + QLatin1Char('\n') + QString::number(m_scanDepth);
    // The same scope again while it is read: that scan answers it.
    if (!restart && busy() && scope == m_scanScope) {
        return;
    }
    m_scanScope = scope;
    // Every leg of this chain reads at the depth it was asked for, even if
    // the page changes it before the last leg starts.
    m_chainDepth = m_scanDepth;
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
    m_analysisState = {};
    // Not the staged fix, for the reason the sample-rate comment below
    // gives.
    m_cleanupLeftovers.clear();
    m_cleanupLeftoversChecked = false;
    m_cleanupLeftoversError.clear();
    emit cleanupLeftoversChanged();
    m_hiddenEngineCues.clear();
    m_hiddenCuesChecked = false;
    emit hiddenCuesChanged();
    m_legacyMemoryLists.clear();
    m_legacyMemoryListsChecked = false;
    m_legacyMemoryListsError.clear();
    m_cueListTally = {};
    // One choice per stick and scan, back to the player's list unless a
    // repair staged with the other is still waiting for Save.
    if (!m_legacyMemoryListFixStaged) {
        m_keepPlayerCueLists = true;
    }
    emit legacyMemoryListsChanged();
    // A sqlite row and 24 bytes of a pdb header: cheap enough to read
    // with the scan rather than behind its own button. Not on this thread,
    // though: it may wait for another thread's recovery of m.db, or copy a
    // pulled stick's m.db aside first.
    m_importState = {};
    emit importStateChanged();
    if (m_chainDepth == Full) {
        m_importStateWatcher.setFuture(
            QtConcurrent::run([engine = m_enginePath.toStdString(), rekordbox = m_rekordboxPath.toStdString()]() {
                return infrastructure::engine::readRekordboxImportState(engine, rekordbox);
            }));
    }
    // The staged fill is NOT cleared here, for the same reason the staged
    // artwork is not: a rescan re-reads the library, it does not unstage
    // what someone asked for. Clearing the flag while the change stayed
    // in the session left a fix that could not be taken back and would
    // still be written by Save.
    emit sampleRatesChanged();
    emit analysisStateChanged();
    m_artSources.clear();
    emit artworkChanged();
    const QString stickRoot =
        pathToQString(pathFromQString(m_enginePath.isEmpty() ? m_rekordboxPath : m_enginePath).parent_path());
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
    // The chain's bar is counted by its first leg (see runScanTask).
    m_chainPlan = {};
    m_chainFormats.clear();
    for (const QString &format : m_pendingScanFormats) {
        m_chainFormats << format;
    }

    // A scan still running for another scope is superseded by the first
    // leg of this one; its answer, whenever it comes, is dropped.
    scanNextPendingFormat(restart);
}

void LibraryConsistencyController::scanNextPendingFormat(bool restart)
{
    if (m_pendingScanFormats.empty()) {
        setScanningFormat({});
        setScanPhase({});
        return;
    }
    // The bar carries over from the leg before: it is the chain's, and
    // this leg continues it from where that one stopped (#58). The
    // phase does not: the last leg's step would otherwise be shown over
    // this one until its first stretch begins (and for good, when the
    // reader answers from its cache).
    setScanPhase({});
    QString format = m_pendingScanFormats.front();
    m_pendingScanFormats.erase(m_pendingScanFormats.begin());
    setScanningFormat(format);
    const QString path = pathForFormat(format);
    const QString playlist = m_currentPlaylistName;
    const auto artSources = m_artSources;
    const QString backupDirectory = m_backupDirectory;
    const ScanDepth depth = m_chainDepth;
    auto reporter = makeReporter();
    const ScanChainPlan chain = m_chainPlan;
    const QStringList chainFormats = m_chainFormats;
    const QString rekordboxPath = m_rekordboxPath;
    const QString enginePath = m_enginePath;
    AsyncRequest<LibraryConsistencyScanResult>::Work work =
        [format, path, playlist, depth, reporter, artSources, backupDirectory, chain, chainFormats, rekordboxPath,
         enginePath](application::CancellationToken cancel) {
            return runScanTask(format, path, playlist, depth, reporter, cancel, artSources, backupDirectory, chain,
                               chainFormats, rekordboxPath, enginePath);
        };
    AsyncRequest<LibraryConsistencyScanResult>::Ending ending{
        [this](LibraryConsistencyScanResult &&result) { onScanFinished(std::move(result)); },
        // The read itself failed or its stick went away: the legs still
        // queued would read the same stick, so they go too.
        [this](const QString &message) {
            m_pendingScanFormats.clear();
            setScanningFormat({});
            setScanPhase({});
            setErrorMessage(message);
        },
        [this]() { endScanCancelled(); },
    };
    const QString key = m_scanScope + QLatin1Char('\n') + format;
    const QString stickRoot = stickRootOf(path);
    if (restart) {
        m_scan.restart(key, stickRoot, std::move(work), std::move(ending));
    } else {
        m_scan.start(key, stickRoot, std::move(work), std::move(ending));
    }
}

void LibraryConsistencyController::cancelScan()
{
    if (scanCancellable()) {
        m_scan.cancel();
    }
}

void LibraryConsistencyController::endScanCancelled()
{
    // Whatever earlier formats contributed stays on screen (it is
    // complete for those formats); the rest of the queue is dropped.
    m_pendingScanFormats.clear();
    setScanningFormat({});
    setScanPhase({});
    emit scanCancelled();
}

void LibraryConsistencyController::onScanFinished(LibraryConsistencyScanResult &&result)
{
    if (!result.plan.empty()) {
        m_chainPlan = result.plan;
    }
    if (result.cancelled) {
        endScanCancelled();
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
        if (result.cleanupLeftoversChecked || !result.cleanupLeftoversError.empty()) {
            m_cleanupLeftovers = std::move(result.cleanupLeftovers);
            m_cleanupLeftoversChecked = result.cleanupLeftoversChecked;
            m_cleanupLeftoversError = QString::fromStdString(result.cleanupLeftoversError);
            emit cleanupLeftoversChanged();
        }
        if (result.hiddenCuesChecked) {
            m_hiddenEngineCues = std::move(result.hiddenEngineCues);
            m_hiddenCuesChecked = true;
            emit hiddenCuesChanged();
        }
        if (result.cueListsAppend) {
            // The OneLibrary leg's files, added to the rekordbox leg's; and
            // a file that leg took for debris is not, if OneLibrary names
            // it.
            for (auto &issue : m_legacyMemoryLists) {
                auto &debris = issue.finding.debris;
                const std::string own = pathToUtf8(pathFromUtf8(issue.finding.analyzePath).filename());
                const std::string rowDir = issue.finding.analyzePath.substr(
                    0, issue.finding.analyzePath.size() - own.size());
                const auto before = debris.size();
                debris.erase(std::remove_if(debris.begin(), debris.end(),
                                            [&](const std::string &file) {
                                                const std::string name = pathToUtf8(pathFromUtf8(file).filename());
                                                return result.oneLibraryAnalysisKeys.count(
                                                           application::normalizedPathKey(rowDir + name))
                                                       > 0;
                                            }),
                             debris.end());
                m_cueListTally.strayFiles -= before - debris.size();
            }
            m_legacyMemoryLists.erase(std::remove_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                                     [](const auto &issue) {
                                                         return !issue.finding.anything()
                                                                && issue.finding.unreadable.empty();
                                                     }),
                                      m_legacyMemoryLists.end());
            // A stray file the rekordbox leg already reported, beside a
            // track this leg found too, is the same file: counted once.
            std::set<std::string> reported;
            for (const auto &issue : m_legacyMemoryLists) {
                for (const auto &file : issue.finding.debris) {
                    reported.insert(application::normalizedPathKey(file));
                }
            }
            for (auto &issue : result.legacyMemoryLists) {
                auto &debris = issue.finding.debris;
                const auto before = debris.size();
                debris.erase(std::remove_if(debris.begin(), debris.end(),
                                            [&reported](const std::string &file) {
                                                return reported.count(application::normalizedPathKey(file)) > 0;
                                            }),
                             debris.end());
                result.cueListTally.strayFiles -= before - debris.size();
                if (issue.finding.anything() || !issue.finding.unreadable.empty()) {
                    m_legacyMemoryLists.push_back(std::move(issue));
                }
            }
            m_cueListTally += result.cueListTally;
            // This leg leaves the files export.pdb names to the rekordbox
            // leg. When that leg was in the scan and did not check them,
            // they were not checked at all, and a clean card over the
            // OneLibrary files alone would say otherwise.
            if (result.legacyMemoryListsChecked && !m_legacyMemoryListsChecked && m_legacyMemoryListsError.isEmpty()
                && m_chainFormats.contains(QStringLiteral("rekordbox"))) {
                m_legacyMemoryListsError = QStringLiteral(
                    "The rekordbox library could not be read, so only the analysis files OneLibrary alone names were "
                    "checked.");
            }
            m_legacyMemoryListsChecked = m_legacyMemoryListsChecked || result.legacyMemoryListsChecked;
            if (!result.legacyMemoryListsError.empty()) {
                m_legacyMemoryListsError = (m_legacyMemoryListsError.isEmpty() ? QString() : m_legacyMemoryListsError
                                                                                                + QStringLiteral("; "))
                                           + QStringLiteral("OneLibrary: ")
                                           + QString::fromStdString(result.legacyMemoryListsError);
            }
            emit legacyMemoryListsChanged();
        } else if (result.legacyMemoryListsChecked || !result.legacyMemoryListsError.empty()) {
            m_legacyMemoryLists = std::move(result.legacyMemoryLists);
            m_legacyMemoryListsChecked = result.legacyMemoryListsChecked;
            m_legacyMemoryListsError = QString::fromStdString(result.legacyMemoryListsError);
            m_cueListTally = result.cueListTally;
            emit legacyMemoryListsChanged();
        }
        // Same rule as the two above: a leg that read nothing must not
        // wipe what a leg that did read left behind. hasColumn is part
        // of the test because an Engine 1.x library legitimately reports
        // zero tracks checked and is still a real answer.
        if (result.analysisState.libraryPresent || result.analysisState.tracksChecked > 0
            || result.analysisState.hasColumn || !result.analysisState.error.empty()) {
            m_analysisState = std::move(result.analysisState);
            emit analysisStateChanged();
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
    scanNextPendingFormat(false);
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
                    rescanAfterWrite();
                    return;
                }
                if (changeId == MarkRekordboxImportedChange::idFor()) {
                    m_importMarkStaged = false;
                    m_rescanAfterSave = true;
                    clearStagedStatusIfNothingStaged();
                    emit importStateChanged();
                    return;
                }
                if (auto staged = m_stagedHiddenCueFixes.find(changeId); staged != m_stagedHiddenCueFixes.end()) {
                    // The track's pads are coloured now: off the list, and
                    // the Engine leg is read again after the save like the
                    // others, so the count is what the database says.
                    m_stagedHiddenCueFixes.erase(staged);
                    m_hiddenCueFixStaged = !m_stagedHiddenCueFixes.empty();
                    m_hiddenEngineCues.erase(
                        std::remove_if(m_hiddenEngineCues.begin(), m_hiddenEngineCues.end(),
                                       [&](const auto &h) { return RecolourEngineCuesChange::idFor(h.track.sourceId) == changeId; }),
                        m_hiddenEngineCues.end());
                    m_rescanAfterSave = true;
                    clearStagedStatusIfNothingStaged();
                    emit hiddenCuesChanged();
                    return;
                }
                if (auto staged = m_stagedLegacyMemoryListFixes.find(changeId);
                    staged != m_stagedLegacyMemoryListFixes.end()) {
                    m_stagedLegacyMemoryListFixes.erase(staged);
                    m_legacyMemoryListFixStaged = !m_stagedLegacyMemoryListFixes.empty();
                    m_legacyMemoryLists.erase(
                        std::remove_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                       [&](const auto &issue) {
                                           return RepairLegacyMemoryListChange::idFor(issue.finding.analyzePath)
                                                  == changeId;
                                       }),
                        m_legacyMemoryLists.end());
                    m_rescanAfterSave = true;
                    clearStagedStatusIfNothingStaged();
                    emit legacyMemoryListsChanged();
                    return;
                }
                if (auto staged = m_stagedCleanupLeftovers.find(changeId);
                    staged != m_stagedCleanupLeftovers.end()) {
                    // Re-read after the save, like the others: what is
                    // left over is whatever OneLibrary now says.
                    m_stagedCleanupLeftovers.erase(staged);
                    m_cleanupLeftoverFixStaged = !m_stagedCleanupLeftovers.empty();
                    m_rescanAfterSave = true;
                    clearStagedStatusIfNothingStaged();
                    emit cleanupLeftoversChanged();
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
                    rescanAfterWrite();
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                m_stagedIssues.clear();
                m_stagedJunk.clear();
                m_stagedArtwork.clear();
                m_stagedSampleRates.clear();
                m_sampleRateFillStaged = false;
                m_stagedCleanupLeftovers.clear();
                m_cleanupLeftoverFixStaged = false;
                emit cleanupLeftoversChanged();
                m_stagedHiddenCueFixes.clear();
                m_hiddenCueFixStaged = false;
                emit hiddenCuesChanged();
                m_stagedLegacyMemoryListFixes.clear();
                m_legacyMemoryListFixStaged = false;
                emit legacyMemoryListsChanged();
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
    if (busy()) {
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
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    stageIssue(index);
}

void LibraryConsistencyController::deleteOrphan(int index)
{
    if (busy()) {
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
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    stageJunkCue(index);
}

void LibraryConsistencyController::removeAllJunkCues()
{
    if (busy()) {
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

namespace
{
std::function<infrastructure::media::FilesystemRepairResult(const std::string &)> &filesystemRepairOverride()
{
    static std::function<infrastructure::media::FilesystemRepairResult(const std::string &)> repair;
    return repair;
}
}  // namespace

void LibraryConsistencyController::setFilesystemRepairForTesting(
    std::function<infrastructure::media::FilesystemRepairResult(const std::string &)> repair)
{
    filesystemRepairOverride() = std::move(repair);
}

void LibraryConsistencyController::repairStickFilesystem()
{
    if (m_repairingFilesystem || busy()) {
        return;
    }
    const std::string stickRoot =
        pathToUtf8(pathFromQString(m_enginePath.isEmpty() ? m_rekordboxPath : m_enginePath).parent_path());
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
    auto repair = filesystemRepairOverride();
    m_repairWatcher.setFuture(QtConcurrent::run([stickRoot, repair]() {
        return repair ? repair(stickRoot) : infrastructure::media::repairFilesystem(stickRoot);
    }));
}

void LibraryConsistencyController::onFilesystemRepairFinished()
{
    // The message goes somewhere. takeResult() without it swallows the
    // exception and hands back a default result -- repaired false,
    // declined false, message empty -- so a repair that THREW came out
    // the other side as setErrorMessage("") and the user was told
    // nothing at all. Every other takeResult in this file passes it;
    // this was the one that did not.
    QString thrown;
    const auto result = gui::takeResult(m_repairWatcher, &thrown);
    m_repairingFilesystem = false;
    m_filesystemMessage = thrown.isEmpty() ? QString::fromStdString(result.message) : thrown;
    const std::string stickRoot =
        pathToUtf8(pathFromQString(m_enginePath.isEmpty() ? m_rekordboxPath : m_enginePath).parent_path());
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
        rescanAfterWrite();
    } else {
        setErrorMessage(m_filesystemMessage);
    }
}

void LibraryConsistencyController::repairArtwork()
{
    if (busy() || artworkRepairStaged()) {
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
    if (busy() || m_sampleRateFillStaged) {
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
    if (busy() || m_importMarkStaged || !m_importState.playerWillOfferImport()) {
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

int LibraryConsistencyController::cleanupLeftoverFixableCount() const
{
    return static_cast<int>(std::count_if(m_cleanupLeftovers.begin(), m_cleanupLeftovers.end(), [](const auto &l) {
        return l.kind == domain::CleanupLeftover::Kind::Repairable;
    }));
}

QVariantList LibraryConsistencyController::cleanupLeftoversHeldBack() const
{
    QVariantList list;
    for (const auto &leftover : m_cleanupLeftovers) {
        QString reason;
        switch (leftover.kind) {
        case domain::CleanupLeftover::Kind::Repairable:
            continue;
        case domain::CleanupLeftover::Kind::NoSurvivor:
            reason = QStringLiteral("No copy of it is left in the rekordbox library to keep its playlists.");
            break;
        case domain::CleanupLeftover::Kind::SeveralSurvivors:
            reason = QStringLiteral("The rekordbox library has more than one copy of it, and nothing says which "
                                    "one Clean Up kept.");
            break;
        case domain::CleanupLeftover::Kind::SurvivorNotInOneLibrary:
            reason = QStringLiteral("The copy Clean Up kept is not in OneLibrary, so its playlists have nowhere "
                                    "to go.");
            break;
        }
        list.push_back(QVariantMap{{"title", QString::fromStdString(leftover.row.title)},
                                   {"artist", QString::fromStdString(leftover.row.artist)},
                                   {"reason", reason}});
    }
    return list;
}

void LibraryConsistencyController::finishCleanupLeftovers()
{
    if (busy() || m_cleanupLeftoverFixStaged) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    std::vector<const domain::CleanupLeftover *> repairable;
    for (const auto &leftover : m_cleanupLeftovers) {
        if (leftover.kind == domain::CleanupLeftover::Kind::Repairable) {
            repairable.push_back(&leftover);
        }
    }
    if (repairable.empty()) {
        return;
    }
    if (!ensureSessionForStaging()) {
        return;
    }
    // One change per duplicate, staged in one call, like the sample rates.
    std::vector<std::unique_ptr<PendingChange>> changes;
    std::set<QString> ids;
    changes.reserve(repairable.size());
    for (const auto *leftover : repairable) {
        changes.push_back(std::make_unique<FinishCleanupChange>(m_rekordboxPath, *leftover, changes.empty()));
        ids.insert(FinishCleanupChange::idFor(leftover->row.filePath));
    }
    if (!m_session->stageAll(std::move(changes))) {
        return;  // the session reported the refusal; the page shows it
    }
    m_stagedCleanupLeftovers = std::move(ids);
    m_cleanupLeftoverFixStaged = true;
    emit cleanupLeftoversChanged();
    setStagedStatusMessage(QStringLiteral("Staged removing %1 duplicate(s) Clean Up left in OneLibrary. Press Save "
                                          "to write it to the stick.")
                               .arg(repairable.size()));
}

int LibraryConsistencyController::hiddenCueCount() const
{
    int count = 0;
    for (const auto &hidden : m_hiddenEngineCues) {
        count += hidden.hidden();
    }
    return count;
}

QVariantList LibraryConsistencyController::hiddenCueTracks() const
{
    QVariantList list;
    for (const auto &hidden : m_hiddenEngineCues) {
        QVariantMap m;
        m["title"] = QString::fromStdString(hidden.track.title);
        m["artist"] = QString::fromStdString(hidden.track.artist);
        m["hotCues"] = hidden.hotCues;
        m["loops"] = hidden.loops;
        list << m;
    }
    return list;
}

void LibraryConsistencyController::recolourHiddenCues()
{
    if (busy() || m_hiddenCueFixStaged || m_hiddenEngineCues.empty()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    if (!ensureSessionForStaging()) {
        return;
    }
    const int count = static_cast<int>(m_hiddenEngineCues.size());
    std::set<QString> ids;
    for (const auto &hidden : m_hiddenEngineCues) {
        if (!m_session->stage(std::make_unique<RecolourEngineCuesChange>(m_enginePath, hidden, count))) {
            // Refused (the lock): what was staged before stays staged and
            // marked; the session has reported why.
            break;
        }
        ids.insert(RecolourEngineCuesChange::idFor(hidden.track.sourceId));
    }
    if (ids.empty()) {
        return;
    }
    m_stagedHiddenCueFixes = std::move(ids);
    m_hiddenCueFixStaged = true;
    emit hiddenCuesChanged();
    setStagedStatusMessage(QStringLiteral("Staged a colour for the hidden cues on %1 track(s). Press Save to write it.")
                               .arg(static_cast<int>(m_stagedHiddenCueFixes.size())));
}

void LibraryConsistencyController::unstageHiddenCueFix()
{
    if (!m_hiddenCueFixStaged) {
        return;
    }
    if (m_session) {
        for (const QString &id : m_stagedHiddenCueFixes) {
            m_session->unstage(id);
        }
    }
    m_stagedHiddenCueFixes.clear();
    m_hiddenCueFixStaged = false;
    emit hiddenCuesChanged();
    clearStagedStatusIfNothingStaged();
}

int LibraryConsistencyController::legacyMemoryListCount() const
{
    return static_cast<int>(std::count_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                          [](const auto &issue) { return issue.finding.memoryListFinding(); }));
}

int LibraryConsistencyController::legacyMemoryListFixableCount() const
{
    return static_cast<int>(std::count_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                          [](const auto &issue) { return issue.finding.memoryListFixable(); }));
}

int LibraryConsistencyController::legacyMemoryListDebrisCount() const
{
    int count = 0;
    for (const auto &issue : m_legacyMemoryLists) {
        count += static_cast<int>(issue.finding.debris.size());
    }
    return count;
}

QVariantList LibraryConsistencyController::legacyMemoryListTracks() const
{
    QVariantList list;
    for (const auto &issue : m_legacyMemoryLists) {
        if (!issue.finding.memoryListFinding()) {
            continue;
        }
        const auto &shape = issue.finding.shape;
        QStringList what;
        if (!shape.malformed.empty()) {
            what << QStringLiteral("a memory cue list Seabass cannot read (%1), left alone")
                        .arg(QString::fromStdString(shape.malformed));
        } else if (shape.damaged()) {
            QStringList how;
            if (shape.headerStale) {
                how << QStringLiteral("a header that says it is empty");
            }
            if (shape.unlinked) {
                how << QStringLiteral("entries not linked");
            }
            if (shape.zeroSlots) {
                how << QStringLiteral("an empty slot a player left");
            }
            what << QStringLiteral("a memory cue list of %1 %2 with %3")
                        .arg(shape.entries)
                        .arg(shape.entries == 1 ? QStringLiteral("entry") : QStringLiteral("entries"))
                        .arg(how.join(QStringLiteral(", ")));
        }
        if (!issue.finding.debris.empty()) {
            QStringList names;
            for (const auto &file : issue.finding.debris) {
                names << QString::fromStdString(pathToUtf8(pathFromUtf8(file).filename()));
            }
            what << QStringLiteral("%1 beside its analysis, which nothing refers to").arg(names.join(QStringLiteral(", ")));
        }
        QVariantMap m;
        m["title"] = QString::fromStdString(issue.track.title);
        m["artist"] = QString::fromStdString(issue.track.artist);
        m["what"] = what.join(QStringLiteral("; "));
        m["fixable"] = issue.finding.memoryListFixable();
        list << m;
    }
    return list;
}

namespace
{

// "1:07.8": where a cue sits, as a DJ reads it off a player.
QString cueTime(uint32_t ms)
{
    const uint32_t tenths = (ms + 50) / 100;
    return QStringLiteral("%1:%2.%3")
        .arg(tenths / 600)
        .arg((tenths / 10) % 60, 2, 10, QLatin1Char('0'))
        .arg(tenths % 10);
}

// One list in words: "1 pad (A 0:30.8)", "2 memory cues (0:00.1, 0:16.6)",
// "no pads". A list of more than eight is counted, not spelled out.
QString cueListWords(const std::vector<infrastructure::rekordbox::ListedCue> &cues, bool hot)
{
    const int n = static_cast<int>(cues.size());
    const QString noun = hot ? (n == 1 ? QStringLiteral("pad") : QStringLiteral("pads"))
                             : (n == 1 ? QStringLiteral("memory cue") : QStringLiteral("memory cues"));
    if (n == 0) {
        return QStringLiteral("no ") + noun;
    }
    QString text = QString::number(n) + QLatin1Char(' ') + noun;
    if (n > 8) {
        return text;
    }
    auto sorted = cues;
    std::stable_sort(sorted.begin(), sorted.end(), [hot](const auto &a, const auto &b) {
        return hot && a.pad != b.pad ? a.pad < b.pad : a.timeMs < b.timeMs;
    });
    QStringList each;
    for (const auto &cue : sorted) {
        QString one = hot && cue.pad >= 1 && cue.pad <= 26 ? QString(QChar('A' + int(cue.pad) - 1)) + QLatin1Char(' ')
                      : hot                                ? QStringLiteral("pad %1 ").arg(cue.pad)
                                                           : QString();
        one += cueTime(cue.timeMs);
        if (cue.isLoop) {
            one += QStringLiteral(" loop to ") + cueTime(cue.loopEndMs);
        }
        each << one;
    }
    return text + QStringLiteral(" (") + each.join(QStringLiteral(", ")) + QLatin1Char(')');
}

}  // namespace

int LibraryConsistencyController::cueListDisagreementCount() const
{
    // Only lists that were compared and differ; a list that could not be
    // read is named on the page, but not counted as showing other cues.
    return static_cast<int>(std::count_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                          [](const auto &issue) {
                                              return issue.finding.disagreement && issue.finding.disagreement->any();
                                          }));
}

int LibraryConsistencyController::cueListDisagreementFixableCount() const
{
    return static_cast<int>(std::count_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                          [](const auto &issue) { return issue.finding.listsFixable(); }));
}

int LibraryConsistencyController::cueListFindingCount() const
{
    return static_cast<int>(std::count_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                          [](const auto &issue) { return issue.finding.anything(); }));
}

int LibraryConsistencyController::cueListMalformedCount() const
{
    return static_cast<int>(std::count_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                          [](const auto &issue) { return !issue.finding.listsMalformed.empty(); }));
}

QVariantList LibraryConsistencyController::cueListUnreadableTracks() const
{
    QVariantList list;
    for (const auto &issue : m_legacyMemoryLists) {
        if (issue.finding.examined || issue.finding.unreadable.empty()) {
            continue;
        }
        QVariantMap m;
        m["title"] = QString::fromStdString(issue.track.title);
        m["artist"] = QString::fromStdString(issue.track.artist);
        m["what"] = QStringLiteral("could not be read: %1").arg(QString::fromStdString(issue.finding.unreadable));
        list << m;
    }
    return list;
}

int LibraryConsistencyController::cueListFixableCount() const
{
    return static_cast<int>(std::count_if(m_legacyMemoryLists.begin(), m_legacyMemoryLists.end(),
                                          [](const auto &issue) { return issue.finding.fixable(); }));
}

QVariantList LibraryConsistencyController::cueListDisagreementTracks() const
{
    QVariantList list;
    for (const auto &issue : m_legacyMemoryLists) {
        const auto &finding = issue.finding;
        if (!finding.listsFinding()) {
            continue;
        }
        QStringList what;
        QStringList player;
        QStringList seabass;
        if (!finding.listsMalformed.empty()) {
            what << QStringLiteral("cue lists Seabass cannot read (%1), left alone")
                        .arg(QString::fromStdString(finding.listsMalformed));
        } else {
            const auto &d = *finding.disagreement;
            if (d.hot) {
                player << cueListWords(d.lists.legacyHot, true);
                seabass << cueListWords(d.seabassHot, true);
            }
            if (d.memory) {
                player << cueListWords(d.lists.legacyMemory, false);
                seabass << cueListWords(d.seabassMemory, false);
            }
            if (d.viewsAgree) {
                // Seabass shows the legacy cues the newer list lacks, so
                // the two agree on screen; a reader of the newer list
                // alone would not.
                QStringList newer;
                if (d.hot) {
                    newer << cueListWords(d.lists.modernHot, true);
                }
                if (d.memory) {
                    newer << cueListWords(d.lists.modernMemory, false);
                }
                what << QStringLiteral("the player and Seabass show %1, but the newer list holds only %2")
                            .arg(player.join(QStringLiteral(" and ")), newer.join(QStringLiteral(" and ")));
            } else {
                what << QStringLiteral("the player shows %1; Seabass sees %2")
                            .arg(player.join(QStringLiteral(" and ")), seabass.join(QStringLiteral(" and ")));
            }
            if (!d.unrepairable.empty()) {
                what << QStringLiteral("left alone: %1").arg(QString::fromStdString(d.unrepairable));
            }
        }
        QVariantMap m;
        m["title"] = QString::fromStdString(issue.track.title);
        m["artist"] = QString::fromStdString(issue.track.artist);
        m["what"] = what.join(QStringLiteral("; "));
        m["player"] = player.join(QStringLiteral(" and "));
        m["seabass"] = seabass.join(QStringLiteral(" and "));
        m["fixable"] = finding.listsFixable();
        list << m;
    }
    return list;
}

QVariantMap LibraryConsistencyController::cueListCounts() const
{
    QVariantMap m;
    m["examined"] = static_cast<int>(m_cueListTally.examined);
    m["unreadable"] = static_cast<int>(m_cueListTally.unreadable);
    m["legacyHeader"] = static_cast<int>(m_cueListTally.legacyHeader);
    m["playerRewritten"] = static_cast<int>(m_cueListTally.playerRewritten);
    m["disagree"] = static_cast<int>(m_cueListTally.disagree);
    m["strayFiles"] = static_cast<int>(m_cueListTally.strayFiles);
    return m;
}

void LibraryConsistencyController::setKeepPlayerCueLists(bool keepPlayer)
{
    // Read when the repair is staged; a staged repair keeps the choice it
    // was staged with, so the choice is fixed until it is unstaged.
    if (m_keepPlayerCueLists == keepPlayer || m_legacyMemoryListFixStaged) {
        return;
    }
    m_keepPlayerCueLists = keepPlayer;
    emit legacyMemoryListsChanged();
}

void LibraryConsistencyController::repairLegacyMemoryLists()
{
    if (busy() || m_legacyMemoryListFixStaged || cueListFixableCount() == 0) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    if (!ensureSessionForStaging()) {
        return;
    }
    const auto keep = m_keepPlayerCueLists ? infrastructure::rekordbox::KeepCueList::Player
                                           : infrastructure::rekordbox::KeepCueList::Seabass;
    std::set<QString> ids;
    for (const auto &issue : m_legacyMemoryLists) {
        if (!issue.finding.fixable()) {
            continue;
        }
        if (!m_session->stage(std::make_unique<RepairLegacyMemoryListChange>(issue.track, issue.finding, keep))) {
            // Refused (the lock): what was staged before stays staged and
            // marked; the session has reported why.
            break;
        }
        ids.insert(RepairLegacyMemoryListChange::idFor(issue.finding.analyzePath));
    }
    if (ids.empty()) {
        return;
    }
    m_stagedLegacyMemoryListFixes = std::move(ids);
    m_legacyMemoryListFixStaged = true;
    emit legacyMemoryListsChanged();
    setStagedStatusMessage(QStringLiteral("Staged the cue list repair for %1 track(s). Press Save to write it.")
                               .arg(static_cast<int>(m_stagedLegacyMemoryListFixes.size())));
}

void LibraryConsistencyController::unstageLegacyMemoryListFix()
{
    if (!m_legacyMemoryListFixStaged) {
        return;
    }
    if (m_session) {
        for (const QString &id : m_stagedLegacyMemoryListFixes) {
            m_session->unstage(id);
        }
    }
    m_stagedLegacyMemoryListFixes.clear();
    m_legacyMemoryListFixStaged = false;
    emit legacyMemoryListsChanged();
    clearStagedStatusIfNothingStaged();
}

void LibraryConsistencyController::unstageCleanupLeftoverFix()
{
    if (!m_cleanupLeftoverFixStaged) {
        return;
    }
    if (m_session) {
        QStringList staged;
        for (const QString &id : m_stagedCleanupLeftovers) {
            staged << id;
        }
        m_session->unstageAll(staged);
    }
    m_stagedCleanupLeftovers.clear();
    m_cleanupLeftoverFixStaged = false;
    emit cleanupLeftoversChanged();
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
    if (busy() || !m_session) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    m_session->undoLastSave();
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

void LibraryConsistencyController::setScanDepth(ScanDepth depth)
{
    if (m_scanDepth == depth) {
        return;
    }
    m_scanDepth = depth;
    emit scanDepthChanged();
}

void LibraryConsistencyController::setScanningFormat(const QString &format)
{
    if (m_scanningFormat == format) {
        return;
    }
    m_scanningFormat = format;
    emit scanningFormatChanged();
}

void LibraryConsistencyController::setScanPhase(const QString &phase)
{
    if (m_scanPhase == phase) {
        return;
    }
    m_scanPhase = phase;
    emit scanPhaseChanged();
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
        && !m_sampleRateFillStaged && !m_cleanupLeftoverFixStaged && !m_hiddenCueFixStaged
        && !m_legacyMemoryListFixStaged && !m_importMarkStaged) {
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
