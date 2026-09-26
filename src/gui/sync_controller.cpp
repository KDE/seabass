// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "sync_controller.hpp"

#include "gui/future_result.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "application/path_key.hpp"
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
#include "infrastructure/file_clock.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/qt_path.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using domain::SyncPlan;

namespace
{

std::chrono::system_clock::time_point fileMtime(const std::string &path)
{
    return infrastructure::toSystemClock(std::filesystem::last_write_time(pathFromUtf8(path)));
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
SyncTaskResult runAnalyzeTask(QString rekordboxPath, QString enginePath, QString playlistName,
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
            // Cues, not Full: a sync compares cues and never a file size,
            // and Full would wait for the size pass behind the cue pass.
            rekordboxTracks = catalogCache.tracksFor("rekordbox", rekordboxPath.toStdString(),
                                                     LibraryCatalogCache::Detail::Cues, *reporter, cancel);
            rekordboxMtime = fileMtime(pathToUtf8(pathFromQString(rekordboxPath) / "rekordbox" / "export.pdb"));
            hasOneLibrary = infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString());
        }
        if (hasEngine) {
            engineTracks = catalogCache.tracksFor("engine", enginePath.toStdString(), LibraryCatalogCache::Detail::Cues, *reporter, cancel);
            // Streaming tracks (TIDAL) have no real local file. Never
            // sync cues onto/from one. See domain::Track::streamingSource's
            // own doc comment.
            engineTracks.erase(std::remove_if(engineTracks.begin(), engineTracks.end(),
                                               [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                                engineTracks.end());
            engineMtime = fileMtime(pathToUtf8(pathFromQString(enginePath) / "Database2" / "m.db"));
        }
        if (hasOneLibrary) {
            oneLibraryTracks = catalogCache.tracksFor("onelibrary", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Cues, *reporter, cancel);
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
        //
        // First, though, every plan whose two sides have different hot cues.
        // Those are not plans at all but choices: no clock can say whose hot
        // cues the DJ meant (see SyncPlan::hotCuesNeedChoice), so they are
        // listed for a pick and never staged until one is made.
        auto hotCueChoices = domain::CrossSourceConflictDetector::takeHotCueChoices(actionable,
                                                                                    application::normalizedPathKey);
        auto conflictSplit = domain::CrossSourceConflictDetector::detect(actionable);
        actionable = std::move(conflictSplit.nonConflicting);
        result.conflicts = std::move(hotCueChoices);
        for (auto &conflict : conflictSplit.conflicts) {
            result.conflicts.push_back(std::move(conflict));
        }

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
    // Every count the page shows is derived from the rows, and the rows
    // change from many places: a scan, the search, a tick, a staged mark,
    // a row leaving once its change is saved. The model announces all of
    // them, so none of those paths can forget to.
    connect(&m_model, &SyncPlanListModel::countsChanged, this, &SyncController::listChanged);
}

void SyncController::analyze(const QString &rekordboxPath, const QString &enginePath, const QString &playlistName)
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

    if (busy()) {
        return;  // never overlap two analyses
    }
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    attachSession();
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    m_watcher.setFuture(QtConcurrent::run(runAnalyzeTask, rekordboxPath, enginePath, playlistName, makeReporter(),
                                          beginScan()));
}

void SyncController::onAnalyzeFinished()
{
    QString thrown;
    SyncTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }

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
    // A rescan is a fresh snapshot -- any decision made against the
    // previous one no longer means anything (the plan it produced is
    // already gone too, replaced by whatever this scan found), so this
    // never tries to carry old resolutions forward.
    m_model.setAnalysis(std::move(result.plans), std::move(result.conflicts));
    // Plans staged before this re-analyze keep their mark if they are
    // still listed (the change itself lives in the session either way).
    for (const auto &[targetKey, info] : m_stagedByKey) {
        int index = indexOfStagedKey(targetKey);
        if (index >= 0) {
            m_model.setStaged(index, true, info.description);
        }
    }
    emit analysisChanged();
    setBusy(false);
}

void SyncController::search(const QString &query)
{
    m_model.setFilter(query);
}

void SyncController::setIncluded(int planIndex, bool included)
{
    m_model.setIncluded(planIndex, included);
}

void SyncController::setAllIncluded(bool included)
{
    m_model.setAllIncluded(included);
}

bool SyncController::wouldStage(int planIndex, bool matchingSearchOnly) const
{
    if (!m_model.included(planIndex) || m_stagedByKey.count(m_model.planKeyAt(planIndex))) {
        return false;
    }
    if (m_model.plans()[static_cast<size_t>(planIndex)].direction == SyncPlan::Direction::None) {
        return false;
    }
    return !matchingSearchOnly || m_model.isPlanVisible(planIndex);
}

void SyncController::stageSelected(bool matchingSearchOnly)
{
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    int staged = 0;
    const int count = m_model.planCount();
    for (int i = 0; i < count; ++i) {
        if (!wouldStage(i, matchingSearchOnly)) {
            continue;
        }
        stagePlan(i);
        staged++;
        if (session() && !session()->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
    if (staged > 0) {
        setStagedStatusMessage(
            QStringLiteral("Staged %1 track(s). Press Save to write the cues to the stick.").arg(staged));
    }
}

QVariantList SyncController::directionCountsFor(bool matchingSearchOnly) const
{
    std::map<std::pair<std::string, std::string>, int> counts;
    for (int i = 0; i < m_model.planCount(); ++i) {
        if (!wouldStage(i, matchingSearchOnly)) {
            continue;
        }
        const SyncPlan &plan = m_model.plans()[static_cast<size_t>(i)];
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
    return list;
}

void SyncController::resolveConflict(int conflictIndex, bool useSourceA)
{
    if (conflictIndex < 0 || conflictIndex >= m_model.conflictCount()) {
        return;
    }
    // A copy: the decision leaves the model below, before this is done.
    const domain::CrossSourceSyncConflict conflict = m_model.conflicts()[static_cast<size_t>(conflictIndex)];

    domain::SyncPlan plan;
    if (conflict.samePair) {
        // One pair's own two sides: the chosen side's hot cues go onto the
        // other side of that same pair, with the memory cues the planner
        // prepared for that direction.
        plan.kind = domain::SyncPlan::Kind::Conflict;
        plan.match.trackA = conflict.sourceA;
        plan.match.trackB = conflict.sourceB;
        plan.direction = useSourceA ? domain::SyncPlan::Direction::ToB : domain::SyncPlan::Direction::ToA;
        plan.cuesToApply = useSourceA ? conflict.cuesFromA : conflict.cuesFromB;
    } else {
        plan.kind = domain::SyncPlan::Kind::AOnly;
        plan.match.trackA = useSourceA ? conflict.sourceA : conflict.sourceB;
        plan.match.trackB = conflict.target;
        plan.direction = domain::SyncPlan::Direction::ToB;
        plan.cuesToApply = useSourceA ? conflict.cuesFromA : conflict.cuesFromB;
    }
    m_model.removeConflictAt(conflictIndex);
    m_model.addPlan(std::move(plan));
    // The decision is the edit: staged right away, Save writes it.
    stagePlan(m_model.planCount() - 1);
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

}  // namespace seabass::gui
