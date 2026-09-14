// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "domain/cross_source_sync_conflict.hpp"
#include "domain/sync_planning.hpp"
#include "application/ports/cancellation_token.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/staged_cue_edit_controller.hpp"
#include "gui/sync_plan_list_model.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Result of a background analyze task, see SyncController::analyze().
// Built entirely on a worker thread, with no access to the controller.
struct SyncTaskResult
{
    std::vector<domain::SyncPlan> plans;  // combined across every present pair, actionable only, conflict-free
    std::vector<domain::CrossSourceSyncConflict> conflicts;  // see CrossSourceConflictDetector::detect()
    int rekordboxTrackCount = 0;
    int engineTrackCount = 0;
    int oneLibraryTrackCount = 0;  // 0 when this stick has no OneLibrary export
    // Union of playlist names across every catalog scanned, built from the
    // *unfiltered* scan regardless of which TrackScope analyze() was asked
    // for -- so picking a playlist never shrinks the picker's own list of
    // choices. Same shape as ScanController's own playlistNames/
    // playlistTrackCounts.
    QStringList playlistNames;
    QVariantMap playlistTrackCounts;
    QString errorMessage;  // empty on success
    bool cancelled = false;  // stopped via cancelScan(); nothing else is set
};

// Wraps SyncLibraries for QML: two-phase, non-destructive sync across
// every pair of catalogs actually present on a stick. Originally this
// only ever compared rekordbox against Engine, with OneLibrary bolted on
// as a one-directional best-effort mirror whenever a write landed on
// rekordbox; that meant OneLibrary's own cues (if it had any rekordbox/
// Engine didn't) never propagated anywhere, and the mirror only fired for
// one of the three possible pairs. Every pair now gets the exact same
// real diff+direction treatment (domain::TrackMatcher / domain::
// SyncPlanner, matching primarily by exact resolved file path -- the
// same physical file on the same stick, format-agnostic and far more
// reliable than title+artist+duration), and all three pairs' actionable
// plans are combined into one list. analyze() only ever reads.
//
// Edits are staged, not written, the way Clean Up stages them: every
// ready track starts ticked, stageSelected() stages the ticked ones as one
// SyncPlanChange per target track in the library's LibraryEditSession
// (the first one takes the edit lock), and the page's Save writes them
// all. resolveConflict() is the other way in -- a pick is itself the
// edit, so it stages at once. A row whose change reached the stick
// disappears from the list.
class SyncController : public StagedCueEditController
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::SyncPlanListModel *plans READ plansModel CONSTANT)
    Q_PROPERTY(int rekordboxTrackCount READ rekordboxTrackCount NOTIFY analysisChanged)
    Q_PROPERTY(int engineTrackCount READ engineTrackCount NOTIFY analysisChanged)
    Q_PROPERTY(int oneLibraryTrackCount READ oneLibraryTrackCount NOTIFY analysisChanged)
    // Backs the Playlist picker in SyncPage.qml -- same shape/convention as
    // ScanController's own playlistNames/playlistTrackCounts (index 0 of
    // ["All tracks"] + these is the "no filter" choice; see
    // PlaylistListView.qml).
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY analysisChanged)
    Q_PROPERTY(QVariantMap playlistTrackCounts READ playlistTrackCounts NOTIFY analysisChanged)
    // The list's own numbers, straight from SyncPlanListModel. A "plan" is
    // a track ready to sync and a "conflict" one waiting for a decision;
    // "visible" is what the search shows; "selected" is ticked and not yet
    // staged, which is exactly what Stage Selected would stage.
    Q_PROPERTY(int planCount READ planCount NOTIFY listChanged)
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY listChanged)
    Q_PROPERTY(int visiblePlanCount READ visiblePlanCount NOTIFY listChanged)
    Q_PROPERTY(int visibleConflictCount READ visibleConflictCount NOTIFY listChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY listChanged)
    Q_PROPERTY(int selectedVisibleCount READ selectedVisibleCount NOTIFY listChanged)

public:
    explicit SyncController(QObject *parent = nullptr);

    SyncPlanListModel *plansModel() { return &m_model; }
    int rekordboxTrackCount() const { return m_rekordboxTrackCount; }
    int engineTrackCount() const { return m_engineTrackCount; }
    int oneLibraryTrackCount() const { return m_oneLibraryTrackCount; }
    QStringList playlistNames() const { return m_playlistNames; }
    QVariantMap playlistTrackCounts() const { return m_playlistTrackCounts; }
    int planCount() const { return m_model.planCount(); }
    int conflictCount() const { return m_model.conflictCount(); }
    int visiblePlanCount() const { return m_model.visiblePlanCount(); }
    int visibleConflictCount() const { return m_model.visibleConflictCount(); }
    int selectedCount() const { return m_model.selectedCount(); }
    int selectedVisibleCount() const { return m_model.selectedVisibleCount(); }

    // Phase 1: read-only. rekordboxPath/enginePath are the stick's
    // DetectedStick.rekordboxPath / .enginePath (either may be empty if
    // that catalog isn't present); OneLibrary is picked up automatically
    // whenever exportLibrary.db exists under rekordboxPath, same
    // convention as every other feature in this app. playlistName empty
    // (the default) analyzes the whole library; a real name scopes
    // matching, the list, and the three track counts to just that
    // playlist's tracks -- playlistNames/playlistTrackCounts themselves
    // stay unfiltered so the picker never shrinks its own choices.
    Q_INVOKABLE void analyze(const QString &rekordboxPath, const QString &enginePath,
                              const QString &playlistName = QString());

    // Narrows the list by title/artist without rescanning, and without
    // touching a tick. It used to be a parameter of analyze(), so every
    // keystroke (debounced) started a background scan and every result
    // threw away whatever had been ticked or decided. Survives
    // re-analyses: it belongs to the page, not to a scan.
    Q_INVOKABLE void search(const QString &query);

    // planIndex is the row's planIndex role, not its row.
    Q_INVOKABLE void setIncluded(int planIndex, bool included);
    // The ready tracks the search shows; see SyncPlanListModel.
    Q_INVOKABLE void setAllIncluded(bool included);

    // Stages every ticked, not yet staged track; the page's Save writes
    // them. matchingSearchOnly leaves out ticked tracks the search hides,
    // which is what the page asks for by default when a search is active
    // -- the same choice Clean Up offers.
    Q_INVOKABLE void stageSelected(bool matchingSearchOnly = false);

    // What stageSelected(matchingSearchOnly) would stage, grouped by
    // direction: [{sourceFormat, targetFormat, count}, ...]. For the
    // confirmation dialog, which lists each direction.
    Q_INVOKABLE QVariantList directionCountsFor(bool matchingSearchOnly) const;

    // Picks one side of conflicts()[conflictIndex] (the row's
    // conflictIndex role): it becomes an ordinary plan, appended and
    // staged right away -- the decision is the edit -- and the decision
    // leaves the list. useSourceA selects CrossSourceSyncConflict::
    // sourceA/cuesFromA when true, sourceB/cuesFromB when false.
    Q_INVOKABLE void resolveConflict(int conflictIndex, bool useSourceA);

signals:
    void analysisChanged();
    void listChanged();

protected:
    StagedPlanModel *stagedPlanModel() override { return &m_model; }
    void reanalyzeAfterUndo() override { analyze(m_rekordboxPath, m_enginePath, m_currentPlaylistName); }

private:
    void onAnalyzeFinished();
    void attachSession();
    void stagePlan(int index);
    bool wouldStage(int planIndex, bool matchingSearchOnly) const;

    SyncPlanListModel m_model;
    QFutureWatcher<SyncTaskResult> m_watcher;
    QString m_rekordboxPath;
    QString m_enginePath;
    // The playlistName analyze() was last called with -- so the automatic
    // re-analyze after an undo stays scoped to whatever playlist was
    // selected, instead of silently reverting to "All tracks".
    QString m_currentPlaylistName;
    int m_rekordboxTrackCount = 0;
    int m_engineTrackCount = 0;
    int m_oneLibraryTrackCount = 0;
    QStringList m_playlistNames;
    QVariantMap m_playlistTrackCounts;
};

}  // namespace seabass::gui
