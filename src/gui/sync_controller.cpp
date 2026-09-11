// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "sync_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "application/ports/backup_store.hpp"
#include "application/use_cases/scan_library.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "domain/cross_source_sync_conflict.hpp"
#include "domain/track_scope.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using domain::SyncPlan;

SyncPlanListModel::SyncPlanListModel(QObject *parent) : QAbstractListModel(parent) {}

int SyncPlanListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_plans.size());
}

namespace
{

// No waveform field here -- see sync_controller.hpp's own comment on
// SyncPlanListModel::setPlans() for why: QML fetches a track's waveform
// on demand via PlaybackController::waveformFor() instead of this
// controller decoding one eagerly for every actionable track up front.
QVariantMap trackToMap(const domain::Track &track)
{
    QVariantMap m;
    m["side"] = QString::fromStdString(track.format);
    m["sourceId"] = QString::fromStdString(track.sourceId);
    m["title"] = QString::fromStdString(track.title);
    m["artist"] = QString::fromStdString(track.artist);
    m["filePath"] = QString::fromStdString(track.filePath);
    m["artworkPath"] = QString::fromStdString(track.artworkPath);
    m["durationMs"] = track.durationSeconds * 1000.0;

    QVariantList cues;
    for (const auto &c : track.cues) {
        QVariantMap cueMap;
        cueMap["kind"] = c.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("hot") : QStringLiteral("memory");
        cueMap["hotCueNumber"] = c.hotCueNumber;
        cueMap["positionMs"] = c.positionMs;
        cueMap["isLoop"] = c.isLoop;
        cueMap["loopEndMs"] = c.loopEndMs;
        cueMap["color"] = QString::fromStdString(c.color);
        cueMap["comment"] = QString::fromStdString(c.comment);
        cues << cueMap;
    }
    m["cues"] = cues;
    return m;
}

}  // namespace

QVariant SyncPlanListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_plans.size()) {
        return {};
    }
    const auto &plan = m_plans[static_cast<size_t>(index.row())];
    bool toB = plan.direction == SyncPlan::Direction::ToB;
    const domain::Track &source = toB ? plan.match.trackA : plan.match.trackB;
    const domain::Track &target = toB ? plan.match.trackB : plan.match.trackA;
    switch (role) {
    case SourceFormatRole:
        return QString::fromStdString(source.format);
    case TargetFormatRole:
        return QString::fromStdString(target.format);
    case FilenameRole:
        return QString::fromStdString(source.filename);
    case DescriptionRole:
        return describeCues(plan.cuesToApply);
    case ConflictRole:
        return plan.kind == SyncPlan::Kind::Conflict;
    case TracksRole:
        return QVariantList{trackToMap(plan.match.trackA), trackToMap(plan.match.trackB)};
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

QHash<int, QByteArray> SyncPlanListModel::roleNames() const
{
    return {
        {SourceFormatRole, "sourceFormat"},
        {TargetFormatRole, "targetFormat"},
        {FilenameRole, "filename"},
        {DescriptionRole, "description"},
        {ConflictRole, "conflict"},
        {TracksRole, "tracks"},
        {StagedRole, "staged"},
        {StagedDescriptionRole, "stagedDescription"},
    };
}

void SyncPlanListModel::setPlans(std::vector<domain::SyncPlan> plans)
{
    beginResetModel();
    m_plans = std::move(plans);
    m_stagedDescriptions.assign(m_plans.size(), QString());
    endResetModel();
}

void SyncPlanListModel::addPlan(domain::SyncPlan plan)
{
    int row = static_cast<int>(m_plans.size());
    beginInsertRows(QModelIndex(), row, row);
    m_plans.push_back(std::move(plan));
    m_stagedDescriptions.push_back(QString());
    endInsertRows();
}

void SyncPlanListModel::removePlanAt(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_plans.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), index, index);
    m_plans.erase(m_plans.begin() + index);
    m_stagedDescriptions.erase(m_stagedDescriptions.begin() + index);
    endRemoveRows();
}

void SyncPlanListModel::setStaged(int index, bool staged, const QString &description)
{
    if (index < 0 || static_cast<size_t>(index) >= m_plans.size()) {
        return;
    }
    m_stagedDescriptions[static_cast<size_t>(index)] = staged ? description : QString();
    emit dataChanged(this->index(index), this->index(index), {StagedRole, StagedDescriptionRole});
}

void SyncPlanListModel::clearStaged()
{
    if (m_plans.empty()) {
        return;
    }
    std::fill(m_stagedDescriptions.begin(), m_stagedDescriptions.end(), QString());
    emit dataChanged(index(0), index(static_cast<int>(m_plans.size()) - 1), {StagedRole, StagedDescriptionRole});
}

namespace
{

std::chrono::system_clock::time_point fileMtime(const std::string &path)
{
    return std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(path));
}

// Builds SyncTaskResult::playlistNames/playlistTrackCounts from the union
// of every catalog's own (unfiltered) tracks -- called before any
// TrackScope filtering below, so picking a playlist never shrinks the
// picker's own list of choices. A given playlist name's count is the max
// across whichever catalogs have it, not a sum: the same playlist
// typically exists independently in each catalog present on a stick with
// near-identical membership, and summing would roughly double-count it
// whenever two catalogs are present, without meaning "distinct real
// tracks" (that would need real cross-catalog matching, not just a
// display count).
void collectPlaylistSummary(const std::vector<domain::Track> &rekordboxTracks,
                             const std::vector<domain::Track> &engineTracks,
                             const std::vector<domain::Track> &oneLibraryTracks, SyncTaskResult &result)
{
    // std::map (ordered), not unordered_map -- iterating it directly below
    // gives sorted names for free, no separate std::set pass just to get
    // an ordering.
    std::map<std::string, int> maxCountByName;
    auto tally = [&](const std::vector<domain::Track> &tracks) {
        std::unordered_map<std::string, int> countThisCatalog;
        for (const auto &track : tracks) {
            for (const auto &playlist : track.playlists) {
                countThisCatalog[playlist.name]++;
            }
        }
        for (const auto &[name, count] : countThisCatalog) {
            int &best = maxCountByName[name];
            best = std::max(best, count);
        }
    };
    tally(rekordboxTracks);
    tally(engineTracks);
    tally(oneLibraryTracks);

    for (const auto &[name, count] : maxCountByName) {
        QString qName = QString::fromStdString(name);
        result.playlistNames << qName;
        result.playlistTrackCounts[qName] = count;
    }
}

// Runs entirely on a background thread (see SyncController::analyze()) -
// no access to the controller itself. Scans whichever of the three
// catalogs are present (via the shared LibraryCatalogCache -- a repeat
// analyze()/Re-Analyze on an unchanged stick pays no disk-read cost at
// all), scopes them to playlistName when it's non-empty, then runs the
// exact same real diff+direction logic (domain::SyncLibraries) once per
// pair actually available on this stick, combining every pair's
// actionable plans into one list.
// Case-insensitive title/artist substring match, same rule
// ScanController's own search box uses -- QString-based, not
// domain::TrackScope::search(), which is deliberately ASCII-only (see its
// own doc comment); real music metadata needs Unicode-aware case folding.
std::vector<domain::Track> filterBySearchQuery(const std::vector<domain::Track> &tracks, const QString &query)
{
    if (query.isEmpty()) {
        return tracks;
    }
    QString lowered = query.toLower();
    std::vector<domain::Track> filtered;
    for (const auto &track : tracks) {
        QString title = QString::fromStdString(track.title).toLower();
        QString artist = QString::fromStdString(track.artist).toLower();
        if (title.contains(lowered) || artist.contains(lowered)) {
            filtered.push_back(track);
        }
    }
    return filtered;
}

SyncTaskResult runAnalyzeTask(QString rekordboxPath, QString enginePath, QString playlistName, QString searchQuery,
                               std::shared_ptr<QtProgressReporter> reporter, application::CancellationToken cancel)
{
    SyncTaskResult result;
    try {
        bool hasRekordbox = !rekordboxPath.isEmpty();
        bool hasEngine = !enginePath.isEmpty();
        bool hasOneLibrary = false;

        std::vector<domain::Track> rekordboxTracks, engineTracks, oneLibraryTracks;
        std::chrono::system_clock::time_point rekordboxMtime, engineMtime, oneLibraryMtime;

        auto &catalogCache = LibraryCatalogCache::instance();

        if (hasRekordbox) {
            rekordboxTracks = catalogCache.tracksFor("rekordbox", rekordboxPath.toStdString(), *reporter, cancel);
            rekordboxMtime = fileMtime((fs::path(rekordboxPath.toStdString()) / "rekordbox" / "export.pdb").string());
            hasOneLibrary = infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString());
        }
        if (hasEngine) {
            engineTracks = catalogCache.tracksFor("engine", enginePath.toStdString(), *reporter, cancel);
            // Streaming tracks (TIDAL) have no real local file. Never
            // sync cues onto/from one. See domain::Track::streamingSource's
            // own doc comment.
            engineTracks.erase(std::remove_if(engineTracks.begin(), engineTracks.end(),
                                               [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                                engineTracks.end());
            engineMtime = fileMtime((fs::path(enginePath.toStdString()) / "Database2" / "m.db").string());
        }
        if (hasOneLibrary) {
            oneLibraryTracks = catalogCache.tracksFor("onelibrary", rekordboxPath.toStdString(), *reporter, cancel);
            oneLibraryMtime =
                fileMtime(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(rekordboxPath.toStdString()));
        }

        collectPlaylistSummary(rekordboxTracks, engineTracks, oneLibraryTracks, result);

        if (!playlistName.isEmpty()) {
            domain::TrackScope scope = domain::TrackScope::playlist(playlistName.toStdString());
            rekordboxTracks = domain::filterByScope(rekordboxTracks, scope);
            engineTracks = domain::filterByScope(engineTracks, scope);
            oneLibraryTracks = domain::filterByScope(oneLibraryTracks, scope);
        }
        if (!searchQuery.isEmpty()) {
            rekordboxTracks = filterBySearchQuery(rekordboxTracks, searchQuery);
            engineTracks = filterBySearchQuery(engineTracks, searchQuery);
            oneLibraryTracks = filterBySearchQuery(oneLibraryTracks, searchQuery);
        }

        result.rekordboxTrackCount = static_cast<int>(rekordboxTracks.size());
        result.engineTrackCount = static_cast<int>(engineTracks.size());
        result.oneLibraryTrackCount = static_cast<int>(oneLibraryTracks.size());

        std::vector<SyncPlan> actionable;
        auto addPairPlans = [&](const std::vector<domain::Track> &tracksA, const std::vector<domain::Track> &tracksB,
                                 std::chrono::system_clock::time_point mtimeA,
                                 std::chrono::system_clock::time_point mtimeB) {
            for (auto &plan : application::SyncLibraries().execute(tracksA, tracksB, mtimeA, mtimeB)) {
                if (plan.direction != SyncPlan::Direction::None) {
                    actionable.push_back(std::move(plan));
                }
            }
        };
        if (hasRekordbox && hasEngine) {
            addPairPlans(rekordboxTracks, engineTracks, rekordboxMtime, engineMtime);
        }
        if (hasEngine && hasOneLibrary) {
            addPairPlans(engineTracks, oneLibraryTracks, engineMtime, oneLibraryMtime);
        }
        // rekordbox <-> OneLibrary is deliberately NOT planned as a pair.
        // They are one library written in two formats, not two catalogs to
        // reconcile: every write to rekordbox already mirrors into
        // OneLibrary (see SyncPlanChange::apply and the three other
        // cue-writing workflows), so they cannot drift apart, and there is
        // nothing for a pair plan to do.
        //
        // Worse, a pair plan would be computed from the state before the
        // save and applied after it, so it could write pre-mirror data back
        // over what the mirror had just written -- order-dependent, and
        // silent, because every write succeeds.
        //
        // This does mean a library whose two halves ALREADY disagree is not
        // repaired here. That is a one-off reconciliation and belongs with
        // Library Health, which is where cross-catalog disagreement is
        // reported; sync's job is to keep formats level, not to fix a
        // library that arrived crooked.

        // Two different pairs can independently target the same third
        // catalog's track (e.g. both rekordbox and Engine have cues
        // OneLibrary lacks) -- neither pairwise SyncPlanner can see the
        // other pair, so it can't know this is happening. Split those
        // out into unresolved conflicts (requires a manual pick, see
        // SyncController::resolveConflict()) before anything below
        // treats `actionable` as safe to apply directly.
        auto conflictSplit = domain::CrossSourceConflictDetector::detect(actionable);
        actionable = std::move(conflictSplit.nonConflicting);
        result.conflicts = std::move(conflictSplit.conflicts);

        // Waveforms are deliberately NOT loaded here -- see
        // sync_controller.hpp's own comment on SyncPlanList Model::
        // setPlans() for why eagerly decoding one per actionable track
        // (thousands, on a real library where most of it is actionable)
        // was the actual cause of a real "scanning takes forever" report,
        // confirmed at over 5 minutes on real removable media for a
        // ~1400-track library, against ~4 seconds for everything else in
        // this function combined. QML fetches a waveform on demand
        // instead, only for whichever rows are actually rendered.
        result.plans = std::move(actionable);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

SyncController::SyncController(QObject *parent) : StagedCueEditController(parent)
{
    connect(&m_watcher, &QFutureWatcher<SyncTaskResult>::finished, this, &SyncController::onAnalyzeFinished);
}

void SyncController::analyze(const QString &rekordboxPath, const QString &enginePath, const QString &playlistName,
                              const QString &searchQuery)
{
    // Recorded even on the early return below: the QML picker is already
    // disabled while busy (SyncPage.qml), so this path shouldn't be
    // reachable from user interaction, but the fields must never go stale
    // relative to the most recently *requested* scope regardless -- the
    // next analyze() this controller issues itself (onWriteFinished()'s
    // own post-write re-analyze) reads them, and silently keeping a
    // superseded value there would resurrect this exact bug for any
    // future caller that isn't gated by that one QML property.
    m_currentPlaylistName = playlistName;
    m_currentSearchQuery = searchQuery;

    if (busy()) {
        return;  // never overlap two analyses
    }
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    attachSession();
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    m_watcher.setFuture(QtConcurrent::run(runAnalyzeTask, rekordboxPath, enginePath, playlistName, searchQuery,
                                          makeReporter(), beginScan()));
}

void SyncController::onAnalyzeFinished()
{
    SyncTaskResult result = m_watcher.result();

    if (result.cancelled) {
        setBusy(false);
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        setBusy(false);
        return;
    }

    m_rekordboxTrackCount = result.rekordboxTrackCount;
    m_engineTrackCount = result.engineTrackCount;
    m_oneLibraryTrackCount = result.oneLibraryTrackCount;
    m_playlistNames = std::move(result.playlistNames);
    m_playlistTrackCounts = std::move(result.playlistTrackCounts);
    m_model.setPlans(std::move(result.plans));
    // Plans staged before this re-analyze keep their mark if they are
    // still listed (the change itself lives in the session either way).
    for (const auto &[targetKey, info] : m_stagedByKey) {
        int index = indexOfStagedKey(targetKey);
        if (index >= 0) {
            m_model.setStaged(index, true, info.description);
        }
    }
    recomputeDirectionCounts();
    // A rescan is a fresh snapshot -- any conflict resolved against the
    // previous one no longer means anything (the plan it produced is
    // already gone too, replaced by whatever this scan found), so this
    // never tries to carry old resolutions forward.
    m_conflicts = std::move(result.conflicts);
    rebuildUnresolvedConflictsList();
    setBusy(false);
}

namespace
{

QString summarizeCueCounts(const std::vector<domain::CuePoint> &cues)
{
    int hot = 0;
    int hotLoop = 0;
    int memory = 0;
    for (const auto &cue : cues) {
        if (cue.kind == domain::CuePoint::Kind::Hot) {
            (cue.isLoop ? hotLoop : hot)++;
        } else {
            memory++;
        }
    }
    QStringList parts;
    if (hot > 0) {
        parts << QString("%1 hot cue(s)").arg(hot);
    }
    if (hotLoop > 0) {
        parts << QString("%1 hot loop(s)").arg(hotLoop);
    }
    if (memory > 0) {
        parts << QString("%1 memory cue(s)").arg(memory);
    }
    return parts.isEmpty() ? "no cues" : parts.join(", ");
}

}  // namespace

void SyncController::rebuildUnresolvedConflictsList()
{
    QVariantList list;
    for (const auto &conflict : m_conflicts) {
        // Same trackToMap() shape (cues, artwork, play-ready
        // filePath/sourceId) the ordinary plan list already renders with
        // TrackWaveformCard -- so a conflict can be investigated the
        // same way any other plan already is, not just read as a
        // one-line summary. Waveform itself is fetched on demand by
        // QML, not included here (see trackToMap()'s own comment).
        domain::Track sourceATrack = conflict.sourceA;
        sourceATrack.cues = conflict.cuesFromA;
        domain::Track sourceBTrack = conflict.sourceB;
        sourceBTrack.cues = conflict.cuesFromB;

        QVariantMap m;
        m["targetPath"] = QString::fromStdString(conflict.target.filePath);
        m["targetTitle"] = QString::fromStdString(conflict.target.title);
        m["targetArtist"] = QString::fromStdString(conflict.target.artist);
        m["targetFormat"] = QString::fromStdString(conflict.target.format);
        m["sourceAFormat"] = QString::fromStdString(conflict.sourceA.format);
        m["sourceASummary"] = summarizeCueCounts(conflict.cuesFromA);
        m["sourceAHasJunkCue"] = conflict.sourceAHasJunkCue;
        m["sourceATrack"] = trackToMap(sourceATrack);
        m["sourceBFormat"] = QString::fromStdString(conflict.sourceB.format);
        m["sourceBSummary"] = summarizeCueCounts(conflict.cuesFromB);
        m["sourceBHasJunkCue"] = conflict.sourceBHasJunkCue;
        m["sourceBTrack"] = trackToMap(sourceBTrack);
        list << m;
    }
    m_unresolvedConflicts = list;
    emit conflictsChanged();
}

void SyncController::resolveConflict(int index, bool useSourceA)
{
    if (index < 0 || static_cast<size_t>(index) >= m_conflicts.size()) {
        return;
    }
    const domain::CrossSourceSyncConflict &conflict = m_conflicts[static_cast<size_t>(index)];

    domain::SyncPlan plan;
    plan.kind = domain::SyncPlan::Kind::AOnly;
    plan.match.trackA = useSourceA ? conflict.sourceA : conflict.sourceB;
    plan.match.trackB = conflict.target;
    plan.direction = domain::SyncPlan::Direction::ToB;
    plan.cuesToApply = useSourceA ? conflict.cuesFromA : conflict.cuesFromB;
    m_model.addPlan(std::move(plan));
    recomputeDirectionCounts();
    // The decision is the edit: staged right away, Save writes it.
    stagePlan(static_cast<int>(m_model.plans().size()) - 1);

    m_conflicts.erase(m_conflicts.begin() + index);
    rebuildUnresolvedConflictsList();
}

void SyncController::recomputeDirectionCounts()
{
    std::map<std::pair<std::string, std::string>, int> counts;
    for (const auto &plan : m_model.plans()) {
        bool toB = plan.direction == SyncPlan::Direction::ToB;
        const std::string &sourceFormat = toB ? plan.match.trackA.format : plan.match.trackB.format;
        const std::string &targetFormat = toB ? plan.match.trackB.format : plan.match.trackA.format;
        counts[{sourceFormat, targetFormat}]++;
    }
    QVariantList list;
    for (const auto &[key, count] : counts) {
        QVariantMap m;
        m["sourceFormat"] = QString::fromStdString(key.first);
        m["targetFormat"] = QString::fromStdString(key.second);
        m["count"] = count;
        list << m;
    }
    m_directionCounts = list;
    emit analysisChanged();
}

// The target track identifies a plan across re-analyses: at most one plan
// per target track ever exists (SyncPlanner classifies each matched pair
// once), so this is also the staged change's key.
QString SyncPlanListModel::planKeyAt(int index) const
{
    if (index < 0 || static_cast<size_t>(index) >= m_plans.size()) {
        return {};
    }
    const SyncPlan &plan = m_plans[static_cast<size_t>(index)];
    const domain::Track &target = plan.direction == SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
    return QString::fromStdString(target.format) + ":" + QString::fromStdString(target.sourceId);
}

// The base wires the session's state and staged-change signals; the only
// part specific to this page is that a sync spans two catalogs, so the
// session is told about both paths.
//
// A row whose change lands is dropped rather than re-derived: that pair
// is consistent now, and SyncPlanner classifies each pair independently,
// so no other row's classification can change. See
// StagedCueEditController::onSessionChangeApplied().
void SyncController::attachSession()
{
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    attachSessionForPath(any);
    if (LibraryEditSession *s = session()) {
        s->setLibraryPaths(m_rekordboxPath, m_enginePath);
    }
}

void SyncController::stagePlan(int index)
{
    const auto &plans = m_model.plans();
    if (index < 0 || static_cast<size_t>(index) >= plans.size()) {
        return;
    }
    const SyncPlan &plan = plans[static_cast<size_t>(index)];
    if (plan.direction == SyncPlan::Direction::None) {
        return;
    }
    if (!session()) {
        attachSession();
    }
    // How many writes this save may make against the target's database
    // (drives the scratch-copy decision): every listed plan for that
    // format, the most that could be staged.
    QString targetKey = m_model.planKeyAt(index);
    QString targetFormat = targetKey.section(':', 0, 0);
    int itemCountHint = 0;
    for (int i = 0; i < m_model.planCount(); ++i) {
        if (m_model.planKeyAt(i).section(':', 0, 0) == targetFormat) {
            itemCountHint++;
        }
    }
    stageChange(index, targetKey,
                std::make_unique<SyncPlanChange>(m_rekordboxPath, m_enginePath, plan, itemCountHint));
}

// Stages every plan currently in the model; the page's Save writes them.
void SyncController::apply()
{
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    int staged = 0;
    const size_t count = m_model.plans().size();
    for (size_t i = 0; i < count; ++i) {
        if (m_model.plans()[i].direction != SyncPlan::Direction::None
            && !m_stagedByKey.count(m_model.planKeyAt(static_cast<int>(i)))) {
            stagePlan(static_cast<int>(i));
            staged++;
            if (session() && !session()->lockHeld()) {
                return;  // refused at the first one; no point trying the rest
            }
        }
    }
    if (staged > 0) {
        setStatusMessage(QStringLiteral("Staged %1 track(s). Press Save to write the cues to the stick.").arg(staged));
    }
}

void SyncController::applyOne(int index)
{
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    stagePlan(index);
}

}  // namespace seabass::gui
