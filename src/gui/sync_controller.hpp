// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/cross_source_sync_conflict.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track_scope.hpp"
#include "application/ports/cancellation_token.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/staged_cue_edit_controller.hpp"
#include "gui/staged_plan_model.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Read-only Qt list model over the SyncPlans SyncController last computed,
// across every pair of catalogs actually present on the stick (rekordbox
// <->Engine, rekordbox<->OneLibrary, Engine<->OneLibrary -- see
// SyncController's own class comment). Only plans with an actual
// direction are exposed; AlreadyConsistent/NoCues need no attention.
class SyncPlanListModel : public QAbstractListModel, public StagedPlanModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by SyncController; not constructible from QML")

public:
    enum Roles {
        // "rekordbox"/"engine"/"onelibrary" -- which catalog the cues are
        // coming from/going to for this specific plan. Read off the
        // matched tracks' own Track::format rather than assuming a fixed
        // pair, so the same role works for any of the three pairs.
        SourceFormatRole = Qt::UserRole + 1,
        TargetFormatRole,
        FilenameRole,
        DescriptionRole,
        ConflictRole,
        TracksRole,
        // This plan is staged in the edit session: what Save will write.
        StagedRole,
        StagedDescriptionRole,
    };

    explicit SyncPlanListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Waveforms are NOT precomputed here -- see this class's own history:
    // this used to take a waveformsByKey map built eagerly (one file read
    // per actionable track) during the whole analyze() scan, which for a
    // stick where most of the library is actionable meant thousands of
    // individual reads against removable media before the user had even
    // looked at a single row -- confirmed as the actual cause of a real
    // "scanning takes forever" report. QML now fetches a given track's
    // waveform on demand via PlaybackController::waveformFor(), the same
    // already-proven pattern LibraryConsistencyPage's own track cards use
    // -- only the rows actually rendered (ListView's own virtualization)
    // ever pay for a waveform read at all.
    void setPlans(std::vector<domain::SyncPlan> plans);
    const std::vector<domain::SyncPlan> &plans() const { return m_plans; }

    // Appends one plan without disturbing the rest -- for a manually-
    // resolved cross-source conflict (see SyncController::
    // resolveConflict()) becoming an ordinary, immediately-appliable plan.
    void addPlan(domain::SyncPlan plan);
    // Removes one plan without a full rescan -- for SyncController::
    // applyOne() after a successful write: that one plan is now
    // consistent, nothing else in the model could have changed (see
    // SyncController::onWriteFinished()'s own comment on why).
    void removePlanAt(int index) override;
    void setStaged(int index, bool staged, const QString &description) override;
    void clearStaged() override;

    int planCount() const override { return static_cast<int>(m_plans.size()); }
    // A plan's identity across re-analyses: the track it would write to,
    // as "<format>:<sourceId>". Two plans can share a target (that is what
    // CrossSourceConflictDetector looks for), so at most one of them is
    // ever listed at a time.
    QString planKeyAt(int index) const override;

private:
    std::vector<domain::SyncPlan> m_plans;
    std::vector<QString> m_stagedDescriptions;  // empty = not staged; parallel to m_plans
};

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
// Edits are staged, not written: apply()/applyOne()/resolveConflict()
// stage one SyncPlanChange per target track in the library's
// LibraryEditSession (the first one takes the edit lock), the row shows
// it, and the page's Save writes them all. A row whose change reached the
// stick disappears from the list.
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
    // One entry per (sourceFormat, targetFormat, count) actually present
    // among the current plans, e.g. [{sourceFormat:"engine",
    // targetFormat:"rekordbox", count:12}, ...] -- replaces the old fixed
    // toEngineCount/toRekordboxCount pair, which had no way to represent
    // a third catalog's own counts.
    Q_PROPERTY(QVariantList directionCounts READ directionCounts NOTIFY analysisChanged)
    // One entry per still-unresolved domain::CrossSourceSyncConflict --
    // two different pairs proposing genuinely different cues to the same
    // target track, which SyncPlanner alone can't detect (it only ever
    // sees two catalogs at a time). See resolveConflict(). Never
    // includes plans already in `plans` -- a target stays out of the
    // appliable list entirely until its conflict here is resolved.
    Q_PROPERTY(QVariantList unresolvedConflicts READ unresolvedConflicts NOTIFY conflictsChanged)

public:
    explicit SyncController(QObject *parent = nullptr);

    SyncPlanListModel *plansModel() { return &m_model; }
    int rekordboxTrackCount() const { return m_rekordboxTrackCount; }
    int engineTrackCount() const { return m_engineTrackCount; }
    int oneLibraryTrackCount() const { return m_oneLibraryTrackCount; }
    QStringList playlistNames() const { return m_playlistNames; }
    QVariantMap playlistTrackCounts() const { return m_playlistTrackCounts; }
    QVariantList directionCounts() const { return m_directionCounts; }
    QVariantList unresolvedConflicts() const { return m_unresolvedConflicts; }

    // Phase 1: read-only. rekordboxPath/enginePath are the stick's
    // DetectedStick.rekordboxPath / .enginePath (either may be empty if
    // that catalog isn't present); OneLibrary is picked up automatically
    // whenever exportLibrary.db exists under rekordboxPath, same
    // convention as every other feature in this app. playlistName empty
    // (the default) analyzes/syncs the whole library, same as before this
    // parameter existed; a real name scopes matching, the plan list, and
    // rekordboxTrackCount/engineTrackCount/oneLibraryTrackCount to just
    // that playlist's tracks -- playlistNames/playlistTrackCounts
    // themselves stay unfiltered so the picker never shrinks its own
    // choices.
    // searchQuery empty (the default) applies no text filter; a real query
    // narrows by case-insensitive title/artist substring match, same rule
    // ScanController's own search box uses, applied on top of
    // playlistName (both can narrow at once, same as ScanPage's own
    // playlist+search combination).
    Q_INVOKABLE void analyze(const QString &rekordboxPath, const QString &enginePath,
                              const QString &playlistName = QString(), const QString &searchQuery = QString());

    // Stages every plan currently in the model, across every pair; the
    // page's Save writes them.
    Q_INVOKABLE void apply();

    // Same as apply(), scoped to the single plan at index.
    Q_INVOKABLE void applyOne(int index);

    // Picks one side of unresolvedConflicts[index] as the winner: turns
    // it into an ordinary actionable plan (added to `plans` and staged
    // right away -- the decision is the edit) and removes it from
    // unresolvedConflicts. useSourceA selects CrossSourceSyncConflict::
    // sourceA/cuesFromA when true, sourceB/cuesFromB when false.
    Q_INVOKABLE void resolveConflict(int index, bool useSourceA);

signals:
    void analysisChanged();
    void conflictsChanged();

protected:
    StagedPlanModel *stagedPlanModel() override { return &m_model; }
    void reanalyzeAfterUndo() override
    {
        analyze(m_rekordboxPath, m_enginePath, m_currentPlaylistName, m_currentSearchQuery);
    }
    // The per-direction counts are derived from the row set, so they only
    // move when a row actually left it.
    void onStagedChangeApplied(bool rowRemoved) override
    {
        if (rowRemoved) {
            recomputeDirectionCounts();
        }
    }

private:
    void onAnalyzeFinished();
    void recomputeDirectionCounts();
    void rebuildUnresolvedConflictsList();
    void attachSession();
    void stagePlan(int index);

    SyncPlanListModel m_model;
    QFutureWatcher<SyncTaskResult> m_watcher;
    QString m_rekordboxPath;
    QString m_enginePath;
    // The playlistName analyze() was last called with -- so the automatic
    // re-analyze onWriteFinished() runs after every apply()/applyOne()/
    // undoLastOperation() stays scoped to whatever playlist was selected,
    // instead of silently reverting to "All tracks".
    QString m_currentPlaylistName;
    // Same reasoning as m_currentPlaylistName -- the search box's own text
    // must also survive the automatic post-undo re-analyze.
    QString m_currentSearchQuery;
    int m_rekordboxTrackCount = 0;
    int m_engineTrackCount = 0;
    int m_oneLibraryTrackCount = 0;
    QStringList m_playlistNames;
    QVariantMap m_playlistTrackCounts;
    QVariantList m_directionCounts;
    std::vector<domain::CrossSourceSyncConflict> m_conflicts;
    QVariantList m_unresolvedConflicts;
};

}  // namespace seabass::gui
