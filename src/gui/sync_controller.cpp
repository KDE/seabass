// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "sync_controller.hpp"

#include "gui/stick_path.hpp"
#include "gui/future_result.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "application/use_cases/onelibrary_sync_rows.hpp"
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
#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/qt_path.hpp"
#include "gui/seabass_settings.hpp"

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
            // Full, not Cues: matching refuses to pair two tracks of one
            // name while either length is unknown, and the lengths a
            // catalog leaves out are probed in the Full stage.
            rekordboxTracks = catalogCache.tracksFor("rekordbox", rekordboxPath.toStdString(),
                                                     LibraryCatalogCache::Detail::Full, *reporter, cancel);
            rekordboxMtime = fileMtime(pathToUtf8(pathFromQString(rekordboxPath) / "rekordbox" / "export.pdb"));
            hasOneLibrary = infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString());
        }
        if (hasEngine) {
            engineTracks = catalogCache.tracksFor("engine", enginePath.toStdString(), LibraryCatalogCache::Detail::Full, *reporter, cancel);
            // Streaming tracks (TIDAL) have no real local file. Never
            // sync cues onto/from one. See domain::Track::streamingSource's
            // own doc comment.
            engineTracks.erase(std::remove_if(engineTracks.begin(), engineTracks.end(),
                                               [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                                engineTracks.end());
            engineMtime = fileMtime(pathToUtf8(pathFromQString(enginePath) / "Database2" / "m.db"));
        }
        if (hasOneLibrary) {
            oneLibraryTracks = catalogCache.tracksFor("onelibrary", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Full, *reporter, cancel);
            oneLibraryMtime =
                fileMtime(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(rekordboxPath.toStdString()));
        }

        collectPlaylistSummary(rekordboxTracks, engineTracks, oneLibraryTracks, result);
        if (hasRekordbox && hasEngine) {
            result.importState = infrastructure::engine::readRekordboxImportState(enginePath.toStdString(),
                                                                                   rekordboxPath.toStdString());
        }

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
            for (auto &plan : application::SyncLibraries().execute(tracksA, tracksB, mtimeA, mtimeB, *reporter)) {
                if (plan.direction != SyncPlan::Direction::None) {
                    actionable.push_back(std::move(plan));
                }
            }
        };
        if (hasRekordbox && hasEngine) {
            addPairPlans(rekordboxTracks, engineTracks, rekordboxMtime, engineMtime);
        }
        if (hasEngine && hasOneLibrary) {
            // Only for the OneLibrary rows no DeviceLibrary row speaks for: a
            // row naming the same analysis file has the same cues (that
            // file is where a OneLibrary player takes them from, #59), and
            // a row at a path DeviceLibrary lists gets every DeviceLibrary
            // write mirrored into it. Planning those against Engine too
            // would decide one file twice, and whichever write landed last
            // would win. Matched against every row first, then narrowed:
            // see planEngineWithOneLibrary().
            for (auto &plan : application::planEngineWithOneLibrary(engineTracks, oneLibraryTracks, rekordboxTracks,
                                                                    engineMtime, oneLibraryMtime)) {
                if (plan.direction != SyncPlan::Direction::None) {
                    actionable.push_back(std::move(plan));
                }
            }
        }
        // rekordbox <-> OneLibrary is deliberately NOT planned as a pair.
        // For cues they are one source, not two: a OneLibrary row's cues
        // are read from, and written to, the analysis file its row names
        // (#59), which is the file the DeviceLibrary row for that track
        // names too. Comparing the two would compare a file with itself.
        //
        // Where they name different files (a re-analysed copy), or one
        // lists a track the other does not, that is a library that arrived
        // crooked: Library Health's to report and level, not a sync's.

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
    // Every count the page shows is derived from the rows, and the rows
    // change from many places: a scan, the search, a tick, a staged mark,
    // a row leaving once its change is saved. The model announces all of
    // them, so none of those paths can forget to.
    connect(&m_model, &SyncPlanListModel::countsChanged, this, &SyncController::listChanged);
}

void SyncController::analyze(const QString &rekordboxPath, const QString &enginePath, const QString &playlistName)
{
    startAnalysis(rekordboxPath, enginePath, playlistName, false);
}

void SyncController::startAnalysis(const QString &rekordboxPath, const QString &enginePath,
                                   const QString &playlistName, bool restart)
{
    // Every request is answered now (docs/async-requests.md): the same
    // scope again by the analysis already running, another scope by a new
    // one that supersedes it. The scope is recorded here, for the request
    // that is actually read -- it used to be recorded before a busy check
    // that then dropped the request, so the page named one playlist over
    // another's plans.
    const QString key = rekordboxPath + QLatin1Char('\n') + enginePath + QLatin1Char('\n') + playlistName;
    if (!restart && scanServes(key)) {
        return;
    }
    m_currentPlaylistName = playlistName;
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    attachSession();
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    const QString catalog = rekordboxPath.isEmpty() ? enginePath : rekordboxPath;
    auto reporter = makeReporter();
    startScan<SyncTaskResult>(
        key,
        stickRootOf(catalog), restart,
        [rekordboxPath, enginePath, playlistName, reporter](application::CancellationToken cancel) {
            return runAnalyzeTask(rekordboxPath, enginePath, playlistName, reporter, cancel);
        },
        [this](SyncTaskResult &&result) { onAnalyzeFinished(std::move(result)); });
}

void SyncController::onAnalyzeFinished(SyncTaskResult &&result)
{
    if (result.cancelled) {
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        return;
    }

    m_rekordboxTrackCount = result.rekordboxTrackCount;
    m_engineTrackCount = result.engineTrackCount;
    m_oneLibraryTrackCount = result.oneLibraryTrackCount;
    m_playlistNames = std::move(result.playlistNames);
    m_playlistTrackCounts = std::move(result.playlistTrackCounts);
    m_importState = result.importState;
    emit importStateChanged();
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
    std::vector<int> staged;
    const int count = m_model.planCount();
    for (int i = 0; i < count; ++i) {
        if (!wouldStage(i, matchingSearchOnly)) {
            continue;
        }
        stagePlan(i);
        staged.push_back(i);
        if (session() && !session()->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
    if (!staged.empty()) {
        setStagedStatusMessage(
            QStringLiteral("Staged %1 track(s). Press Save to write the cues to the stick.").arg(staged.size()));
        noticeCuesLeftOut(staged);
    }
}

namespace
{
const QString LeftOutNoticeSuppressedKey = QStringLiteral("sync/cuesLeftOutNotice/suppressed");
const QString LeftOutNoticeSuppressedTracksKey = QStringLiteral("sync/cuesLeftOutNotice/suppressedTracks");
}  // namespace

// The file name, not a catalog row id: the same track on another stick
// or after a re-export is the same track to the DJ, and the notice is
// about the track.
QString SyncController::leftOutKeyFor(const SyncPlan &plan)
{
    const domain::Track &nonEngine = plan.match.trackA.format == "engine" ? plan.match.trackB : plan.match.trackA;
    return QString::fromStdString(nonEngine.filename).toLower();
}

void SyncController::noticeCuesLeftOut(const std::vector<int> &stagedIndices)
{
    QSettings settings = openSeabassSettings();
    if (settings.value(LeftOutNoticeSuppressedKey, false).toBool()) {
        return;
    }
    const QStringList suppressedTracks = settings.value(LeftOutNoticeSuppressedTracksKey).toStringList();
    QVariantList tracks;
    QStringList keys;
    for (int index : stagedIndices) {
        const SyncPlan &plan = m_model.plans()[static_cast<size_t>(index)];
        const domain::Track &target = plan.direction == SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
        if (plan.cuesLeftOut.empty() || target.format != "engine") {
            continue;
        }
        const QString key = leftOutKeyFor(plan);
        if (suppressedTracks.contains(key)) {
            continue;
        }
        const domain::Track &source = plan.direction == SyncPlan::Direction::ToB ? plan.match.trackA : plan.match.trackB;
        QVariantMap entry;
        entry["title"] = QString::fromStdString(source.title);
        entry["artist"] = QString::fromStdString(source.artist);
        entry["count"] = static_cast<int>(plan.cuesLeftOut.size());
        tracks << entry;
        keys << key;
    }
    if (tracks.isEmpty()) {
        return;
    }
    m_lastLeftOutKeys = keys;
    emit cuesLeftOutNoticed(tracks);
}

void SyncController::suppressCuesLeftOutNotice(bool forTheseTracks, bool ever)
{
    QSettings settings = openSeabassSettings();
    if (ever) {
        settings.setValue(LeftOutNoticeSuppressedKey, true);
    }
    if (forTheseTracks) {
        QStringList tracks = settings.value(LeftOutNoticeSuppressedTracksKey).toStringList();
        for (const QString &key : m_lastLeftOutKeys) {
            if (!tracks.contains(key)) {
                tracks << key;
            }
        }
        settings.setValue(LeftOutNoticeSuppressedTracksKey, tracks);
    }
}

void SyncController::resetCuesLeftOutNotice()
{
    QSettings settings = openSeabassSettings();
    settings.remove(LeftOutNoticeSuppressedKey);
    settings.remove(LeftOutNoticeSuppressedTracksKey);
}

void SyncController::markRekordboxImported()
{
    if (busy() || m_importMarkStaged || !m_importState.playerWillOfferImport()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    if (!session()) {
        attachSession();
    }
    if (!session()) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return;
    }
    if (!session()->stage(std::make_unique<MarkRekordboxImportedChange>(m_enginePath, m_importState.librarySequence))) {
        return;  // the session reported the refusal; the page shows it
    }
    m_importMarkStaged = true;
    emit importStateChanged();
    setStagedStatusMessage(
        QStringLiteral("Staged marking this stick's rekordbox library as imported. Press Save to write it."));
}

void SyncController::unstageRekordboxImportMark()
{
    if (!m_importMarkStaged) {
        return;
    }
    if (session()) {
        session()->unstage(MarkRekordboxImportedChange::idFor());
    }
    m_importMarkStaged = false;
    emit importStateChanged();
}

void SyncController::setImportStateForTesting(bool playerWillOfferImport)
{
    m_importState = {};
    m_importState.hasEngineLibrary = true;
    m_importState.hasRekordboxLibrary = true;
    m_importState.librarySequence = playerWillOfferImport ? 2 : 1;
    m_importState.engineCounter = 1;
    emit importStateChanged();
}

// A landed mark, or any landed save: what the stick says now. One 24-byte
// read and one row; fine on the main thread.
void SyncController::rereadImportState()
{
    if (m_rekordboxPath.isEmpty() || m_enginePath.isEmpty()) {
        return;
    }
    m_importState = infrastructure::engine::readRekordboxImportState(m_enginePath.toStdString(),
                                                                      m_rekordboxPath.toStdString());
    emit importStateChanged();
}

void SyncController::onImportSessionChangeApplied(const QString &changeId)
{
    if (changeId == MarkRekordboxImportedChange::idFor()) {
        m_importMarkStaged = false;
    }
    rereadImportState();
}

void SyncController::onImportSessionChangesDiscarded()
{
    m_importMarkStaged = false;
    emit importStateChanged();
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
    plan.cuesLeftOut = conflict.cuesLeftOut;
    m_model.removeConflictAt(conflictIndex);
    m_model.addPlan(std::move(plan));
    // The decision is the edit: staged right away, Save writes it.
    const int index = m_model.planCount() - 1;
    stagePlan(index);
    noticeCuesLeftOut({index});
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
        // The base disconnects everything from a session it leaves, these
        // included, so they are made again for a new one and once only.
        if (s != m_importSession) {
            m_importSession = s;
            connect(s, &LibraryEditSession::changeApplied, this, &SyncController::onImportSessionChangeApplied);
            connect(s, &LibraryEditSession::changesDiscarded, this, &SyncController::onImportSessionChangesDiscarded);
        }
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
