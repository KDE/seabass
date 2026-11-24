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

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "domain/cross_source_sync_conflict.hpp"
#include "domain/sync_planning.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "domain/cue_offset_repair.hpp"
#include "gui/cue_offset_list_model.hpp"
#include "gui/staged_cue_edit_controller.hpp"
#include "gui/sync_plan_list_model.hpp"

namespace seabass::gui
{

class LibraryEditSession;
struct SyncAnalysisStaging;

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
    // Whether a Denon player will offer to import the rekordbox library
    // over the Engine one (see engine_import_state.hpp), read with the
    // catalogs so the page can offer to settle it with the sync.
    infrastructure::engine::RekordboxImportState importState;
    // With an analysis switch on only: the tracks in scope no plan or
    // conflict carries to Engine whose analysis a copy would change (rows
    // of their own, SyncPlanListModel), and what the plans whose cues go
    // from rekordbox onto Engine carry of it, by Engine sourceId.
    std::vector<SyncAnalysisOnlyTrack> analysisOnly;
    std::map<std::string, SyncRiderAnalysis> riders;
    // Whether the analysis could be copied at all (rekordbox and Engine on
    // the stick), and the switches the preview ran with (both off when
    // not): staging compares Preferences with these.
    bool analysisApplies = false;
    bool analysedGrids = false;
    bool analysedWaveforms = false;
    // Tracks whose analysis the preview could not check, and whether the
    // whole preview failed: unsure, said on the page, never "in step".
    int analysisUnchecked = 0;
    bool analysisCheckFailed = false;
    // Matched tracks whose cues are a few milliseconds off on
    // one side (#121, domain::findCueOffsets): rekordbox and Engine rows
    // only, MP3s whose lead-in was read, with the stick's log as evidence.
    std::vector<domain::CueOffsetFinding> cueOffsets;
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
    // The player's import prompt, the same fact Library Health shows. A
    // sync is where a DJ finds out the hard way: the stick's rekordbox
    // library moved on since the player last imported it (a rekordbox
    // export), the player offers the import, and accepting it replaces
    // the Engine side, the cues this page just levelled included. The
    // save loop keeps the two level only when they were level before a
    // save; an offer to mark it imported belongs here as well.
    // Cues that arrived on their targets in the saves made since the last
    // staging began: what the page says once the list is empty ("12 cues
    // synced"). Counted the way a row counts them (cueChangeOf: gained hot
    // and memory cues, in the source's terms), added as each change lands.
    Q_PROPERTY(int syncedCueCount READ syncedCueCount NOTIFY syncedChanged)
    Q_PROPERTY(bool playerWillOfferImport READ playerWillOfferImport NOTIFY importStateChanged)
    Q_PROPERTY(bool importMarkStaged READ importMarkStaged NOTIFY importStateChanged)
    // With "Copy beat grids/waveforms from rekordbox to Engine" on
    // (Preferences): how many of the ready tracks have their cues already
    // in step and get only rekordbox's analysis (SyncAnalysisOnlyTrack).
    // They are rows like the others, counted in planCount and the
    // selected counts, ticked and staged like them. 0 with both switches
    // off.
    Q_PROPERTY(int analysisOnlyCount READ analysisOnlyCount NOTIFY listChanged)
    // With an analysis switch on: "Beat grids and waveforms could not be
    // checked for N tracks." when the preview failed for some tracks, or
    // "... could not be checked." when it failed as a whole; empty when
    // every track was checked. Never shown as "nothing to sync".
    Q_PROPERTY(QString analysisUncheckedText READ analysisUncheckedText NOTIFY analysisChanged)
    // "Cues a few milliseconds off" (#121): matched tracks whose
    // cues sit a few tens of milliseconds off on one side, a section of
    // their own below the list. A row whose side is known is moved by
    // "Move All Known" or its own button; one nothing explains is listed
    // with both positions and moved only by the DJ's pick of a side.
    Q_PROPERTY(seabass::gui::CueOffsetListModel *cueOffsets READ cueOffsetsModel CONSTANT)
    Q_PROPERTY(int cueOffsetKnownCount READ cueOffsetKnownCount NOTIFY cueOffsetsChanged)
    Q_PROPERTY(int cueOffsetStagedCount READ cueOffsetStagedCount NOTIFY cueOffsetsChanged)

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
    int syncedCueCount() const { return m_syncedCueCount; }
    bool playerWillOfferImport() const { return m_importState.playerWillOfferImport(); }
    bool importMarkStaged() const { return m_importMarkStaged; }
    int analysisOnlyCount() const { return m_model.analysisOnlyCount(); }
    QString analysisUncheckedText() const;
    CueOffsetListModel *cueOffsetsModel() { return &m_cueOffsets; }
    int cueOffsetKnownCount() const { return m_cueOffsets.knownUnstagedCount(); }
    int cueOffsetStagedCount() const { return m_cueOffsets.stagedCount(); }

    // Stages moving the off cues of cueOffsets' row `row` on `side`
    // ("engine" or "rekordbox"); an empty side takes the row's own, and a
    // row whose side is unknown is then left alone (unsure: the DJ picks).
    // Staging the other side replaces what was staged. Save writes it,
    // backed up first.
    Q_INVOKABLE bool stageCueOffset(int row, const QString &side = QString());
    Q_INVOKABLE void unstageCueOffset(int row);
    // Every row whose side is known and is not staged yet; never an
    // unknown one. Returns how many were staged.
    Q_INVOKABLE int stageKnownCueOffsets();

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
    // which is what the page does when a search is active: staging is
    // not destructive (Save is a step away, and cancelling it undoes
    // every stage), so there is no confirmation in front of it.
    Q_INVOKABLE void stageSelected(bool matchingSearchOnly = false);

    // After a stage: the staged tracks whose sync onto Engine leaves cues
    // off it, because Engine's eight pads were full (SyncPlan::cuesLeftOut).
    // The page shows them once per stage, unless the notice is suppressed
    // for those tracks or for good. Each entry is {title, artist, count}.
    Q_INVOKABLE void suppressCuesLeftOutNotice(bool forTheseTracks, bool ever);
    // Test seam: forgets both suppressions.
    Q_INVOKABLE void resetCuesLeftOutNotice();

    // Stages the one-row Engine write that tells the player the rekordbox
    // library beside it is the one it knows (MarkRekordboxImportedChange);
    // Save writes it. The same change Library Health stages.
    Q_INVOKABLE void markRekordboxImported();
    Q_INVOKABLE void unstageRekordboxImportMark();
    // Test seam: the state as if read from a stick.
    Q_INVOKABLE void setImportStateForTesting(bool playerWillOfferImport);
    // Test seam: the list as it is, plus `tracks` rows of tracks needing
    // only rekordbox's analysis ("Analysis Only 1", ... by "Seabass Test"),
    // `grids` of them a beat grid, `waveforms` a waveform and `completed`
    // completed. Every row ticked again, nothing marked staged.
    Q_INVOKABLE void setAnalysisOnlyForTesting(int tracks, int grids, int waveforms, int completed);
    // Test seam: as if the analysis' preview had failed for `tracks`
    // tracks, or as a whole.
    Q_INVOKABLE void setAnalysisUncheckedForTesting(int tracks, bool wholeCheckFailed);
    // Test seam: as if a save had brought this many cues across.

    // Picks one side of conflicts()[conflictIndex] (the row's
    // conflictIndex role): it becomes an ordinary plan, appended and
    // staged right away -- the decision is the edit -- and the decision
    // leaves the list. useSourceA selects CrossSourceSyncConflict::
    // sourceA/cuesFromA when true, sourceB/cuesFromB when false.
    Q_INVOKABLE void resolveConflict(int conflictIndex, bool useSourceA);

signals:
    void analysisChanged();
    void listChanged();
    void cuesLeftOutNoticed(const QVariantList &tracks);
    void importStateChanged();
    void syncedChanged();
    void cueOffsetsChanged();

protected:
    StagedPlanModel *stagedPlanModel() override { return &m_model; }
    // After an undo what a running analysis read is out of date: restart.
    int otherStagedCount() const override { return static_cast<int>(m_cueOffsetChangeIds.size()); }
    void reanalyzeAfterUndo() override { startAnalysis(m_rekordboxPath, m_enginePath, m_currentPlaylistName, true); }
    void startAnalysis(const QString &rekordboxPath, const QString &enginePath, const QString &playlistName,
                       bool restart);

private:
    void onAnalyzeFinished(SyncTaskResult &&result);
    void attachSession();
    // Stages the plan at `index`; true when its change also carries
    // rekordbox's analysis for the Engine row (Preferences, see
    // SyncPlanChange::setAnalysisRider()). `staging`: Preferences' analysis
    // switches, read once by a caller staging many; null reads them here.
    // `staged`, when given, says whether the change was staged.
    bool stagePlan(int index, const SyncAnalysisStaging *staging = nullptr, bool *staged = nullptr);
    bool wouldStage(int planIndex, bool matchingSearchOnly) const;
    // Stages the analysis-only plan at `index` (SyncPlanListModel::
    // isAnalysisOnly) as a SyncAnalysisChange of its own, counted as one
    // track of the save; true when it was staged.
    bool stageAnalysisOnly(int index, const SyncAnalysisStaging &staging);
    // A first stage after everything staged before has landed or gone
    // starts a new count for "N cues synced".
    void startNewSyncedCount();
    // Preferences' analysis switches (or Experimental) are not what the
    // last analysis previewed with, where that preview applies.
    bool analysisSwitchesMoved(const SyncAnalysisStaging &staging) const;
    static QString analysisSwitchesMovedMessage();
    // Preferences' options narrowed to the parts a row's preview showed.
    static application::AnalysisCopyOptions previewedParts(const SyncAnalysisStaging &staging,
                                                           const SyncRowAnalysis &parts);
    void noticeCuesLeftOut(const std::vector<int> &stagedIndices);
    static QString leftOutKeyFor(const domain::SyncPlan &plan);
    void onImportSessionChangeApplied(const QString &changeId);
    void onImportSessionChangesDiscarded();
    void rereadImportState();
    void onCueOffsetChangeApplied(const QString &changeId);
    void onSaveFinished(const QVariantMap &summary);
    // A move of cues a few milliseconds off is staged for either track.
    bool cueOffsetStagedFor(const domain::SyncMatch &pair) const;
    // A sync is staged whose pair shares a track with the finding's.
    bool syncStagedFor(const domain::CueOffsetFinding &finding);

    SyncPlanListModel m_model;
    CueOffsetListModel m_cueOffsets;
    // Row key (CueOffsetListModel::keyOf) -> the ids of its staged
    // changes, one a side it moves.
    std::map<QString, std::vector<QString>> m_cueOffsetChangeIds;
    // Row key -> what its staged changes move: "known", or the pick.
    std::map<QString, QString> m_cueOffsetStagedSides;
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
    // The tracks of the last cuesLeftOutNoticed(), for "not again for these".
    QStringList m_lastLeftOutKeys;
    // Gained cues per staged change id, moved into m_syncedCueCount when
    // the change lands.
    std::map<QString, int> m_gainedByChangeId;
    int m_syncedCueCount = 0;
    // The last analysis' preview: whether it applied, the switches it ran
    // with, and what it could not check (SyncTaskResult).
    bool m_analysisApplies = false;
    bool m_analysedGrids = false;
    bool m_analysedWaveforms = false;
    int m_analysisUnchecked = 0;
    bool m_analysisCheckFailed = false;
    infrastructure::engine::RekordboxImportState m_importState;
    bool m_importMarkStaged = false;
    // The session the two import connections below are made on, so a
    // repeat attachSession() on the same session does not double them.
    LibraryEditSession *m_importSession = nullptr;
};

}  // namespace seabass::gui
