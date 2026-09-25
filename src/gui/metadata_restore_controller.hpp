// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/metadata_restore.hpp"
#include "gui/metadata_restore_proposal_model.hpp"
#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

class LibraryEditSession;

struct MetadataRestoreTaskResult
{
    std::vector<domain::MetadataRestoreProposal> proposals;
    // The catalog path the scan read, which staging and waveform reads
    // derive every format's own path from.
    QString libraryPath;
    int stickTrackCount = 0;
    int storedTrackCount = 0;
    int conflictCount = 0;  // tracks whose cues differ, whichever side the merge rule then chose
    QString errorMessage;
    bool cancelled = false;
};

// Puts the local metadata store's cues back on a stick that has lost
// them. See docs/metadata-backup-plan.md.
//
// The counterpart to MetadataBackupController, and deliberately not the
// same page: reading a stick into the store risks nothing and needs no
// confirmation, while writing to the stick is a save like every other
// one in Seabass. So nothing here writes directly. Each accepted
// proposal is staged into the library's LibraryEditSession as a
// RestoreMetadataChange, the page's Save writes them through
// runSaveLoop, and that backs up before it touches a file.
class MetadataRestoreController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    Q_PROPERTY(int progressCurrent READ progressCurrent NOTIFY progressChanged)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged)
    Q_PROPERTY(QString currentPhase READ currentPhase NOTIFY currentPhaseChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(RestoreProposalListModel *proposals READ proposals CONSTANT)
    Q_PROPERTY(bool hasScanned READ hasScanned NOTIFY analysisChanged)
    Q_PROPERTY(int stickTrackCount READ stickTrackCount NOTIFY analysisChanged)
    Q_PROPERTY(int storedTrackCount READ storedTrackCount NOTIFY analysisChanged)
    Q_PROPERTY(int conflictCount READ conflictCount NOTIFY analysisChanged)
    Q_PROPERTY(int conflictsLeftAlone READ conflictsLeftAlone NOTIFY analysisChanged)
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY analysisChanged)
    // What a save will actually write: one change per catalog that
    // lists the file, so this is >= stagedCount whenever a stick
    // carries the same tracks in more than one catalog.
    Q_PROPERTY(int stagedChangeCount READ stagedChangeCount NOTIFY analysisChanged)
    // Proposals offering a comment that DeviceLibrary alone cannot
    // store. Not a failure and not hidden: the page says so before the
    // save rather than the log saying so after it.
    Q_PROPERTY(int commentsRekordboxCannotTake READ commentsRekordboxCannotTake NOTIFY analysisChanged)
    Q_PROPERTY(int proposalCount READ proposalCount NOTIFY analysisChanged)
    // ---- the scope: which stick's backup, which playlist ---------------
    // The same two narrowings the backup page offers, and they narrow the
    // restore itself, not only the list: Select All stages what is in
    // scope, and changing the scope unstages what falls outside it. So the
    // save writes exactly the in-scope tracks that are ticked, and every
    // count below is a count of that set.
    //
    // One entry per stick the proposals were backed up from:
    // {key, name, count}. The page puts "every stick" in front of it.
    Q_PROPERTY(QVariantList sourceSticks READ sourceSticks NOTIFY analysisChanged)
    Q_PROPERTY(QString selectedSourceKey READ selectedSourceKey NOTIFY analysisChanged)
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY analysisChanged)
    Q_PROPERTY(QVariantMap playlistTrackCounts READ playlistTrackCounts NOTIFY analysisChanged)
    Q_PROPERTY(QString selectedPlaylist READ selectedPlaylist NOTIFY analysisChanged)
    // Proposals from the picked stick, whichever playlist: the count
    // beside the playlist picker's "All tracks".
    Q_PROPERTY(int sourceProposalCount READ sourceProposalCount NOTIFY analysisChanged)
    // Proposals inside the scope, whatever the search is hiding.
    Q_PROPERTY(int scopedProposalCount READ scopedProposalCount NOTIFY analysisChanged)
    // Rows on the list: the scope, narrowed further by the search.
    Q_PROPERTY(int visibleProposalCount READ visibleProposalCount NOTIFY analysisChanged)
    Q_PROPERTY(bool allStaged READ allStaged NOTIFY analysisChanged)
    // The merge rule as prose, from the domain function that implements
    // it, so this page's help and the backup page's say the same thing
    // because they are the same string.
    Q_PROPERTY(QString mergeRuleHelp READ mergeRuleHelp CONSTANT)

public:
    explicit MetadataRestoreController(QObject *parent = nullptr);
    ~MetadataRestoreController() override;

    bool busy() const { return m_busy; }
    bool writing() const;
    int progressCurrent() const { return m_progressCurrent; }
    int progressTotal() const { return m_progressTotal; }
    QString currentPhase() const { return m_currentPhase; }
    QString errorMessage() const { return m_errorMessage; }
    RestoreProposalListModel *proposals() { return &m_model; }
    bool hasScanned() const { return m_hasScanned; }
    int stickTrackCount() const { return m_stickTrackCount; }
    int storedTrackCount() const { return m_storedTrackCount; }
    int conflictCount() const { return m_conflictCount; }
    int conflictsLeftAlone() const { return m_conflictsLeftAlone; }
    int stagedCount() const { return m_model.stagedCount(); }
    int stagedChangeCount() const { return m_model.stagedChangeCount(); }
    int commentsRekordboxCannotTake() const { return m_commentsRekordboxCannotTake; }
    int proposalCount() const { return static_cast<int>(m_model.proposals().size()); }
    QVariantList sourceSticks() const { return m_sourceSticks; }
    QString selectedSourceKey() const { return QString::fromStdString(m_model.scope().sourceKey); }
    QStringList playlistNames() const { return m_playlistNames; }
    QVariantMap playlistTrackCounts() const { return m_playlistTrackCounts; }
    QString selectedPlaylist() const { return QString::fromStdString(m_model.scope().playlist); }
    int scopedProposalCount() const { return m_model.scopedCount(); }
    int sourceProposalCount() const { return m_sourceProposalCount; }
    int visibleProposalCount() const { return m_model.rowCount(); }
    bool allStaged() const { return scopedProposalCount() > 0 && stagedCount() == scopedProposalCount(); }
    QString mergeRuleHelp() const;

    // libraryPath is any catalog directory on the stick; every catalog
    // on it is read and folded into files first, so one proposal covers
    // a track however many formats list it.
    //
    // No policy argument: what a restore offers is decided per field by
    // the shared merge rule, the same one the backup direction uses.
    Q_INVOKABLE void scan(const QString &libraryPath);
    Q_INVOKABLE void cancelScan();
    // Every index a page passes is a ListView row, which is not a
    // proposal index whenever a search is narrowing the list. The
    // translation happens here, at the one boundary where QML and the
    // proposal list meet, rather than being something each call site has
    // to remember.
    // Ticking a row stages it, and that is the whole of the selection on
    // this page: no second selection, no per-row button doing the same
    // thing. The floating Save, labelled Restore, writes what is staged.
    Q_INVOKABLE void stage(int row);
    Q_INVOKABLE void unstage(int row);
    Q_INVOKABLE void search(const QString &text);
    // Narrow the restore to one stick's backup (a key from sourceSticks,
    // empty for every stick) or one playlist (empty for every track).
    // Staged tracks the new scope leaves out are unstaged, and the page
    // is told how many: a tick that stays on a row you can no longer see
    // would be written by a save that claims to cover only what you
    // picked.
    Q_INVOKABLE void setSourceStick(const QString &key);
    Q_INVOKABLE void setPlaylist(const QString &name);
    // Every proposal in scope, not every visible one: the search narrows
    // what you are looking at within the scope, and must not silently
    // narrow what a button called "Select All" acts on, because the
    // difference is invisible the moment the search is cleared. The
    // stick and playlist pickers are the other way round on purpose:
    // they say what the restore is for, and they stay on screen.
    Q_INVOKABLE void stageAll();
    Q_INVOKABLE void unstageAll();
    // Both row-taking entry points above funnel here, so a bulk unstage
    // cannot drift from what ticking one row does.
    void unstageAt(int index);
    // stage(), plus what the save is expected to write in total -- see
    // RestoreMetadataChange's own itemCountHint.
    void stageOne(int index, int itemCountHint);

    // Where the waveform of the track on list row `row` can be read:
    // {format, libraryPath, sourceId} for PlaybackController::waveformFor,
    // or an empty map when no catalog on the stick has one to offer. A
    // metadata backup stores no waveforms, so the stick is the only
    // source there is. Called by a row when it is opened, never for the
    // whole list.
    Q_INVOKABLE QVariantMap waveformSourceAt(int row) const;

    // What a finished scan hands over, and the one way proposals reach
    // the page. Public so a test can fill the page without a stick.
    void applyScanResult(MetadataRestoreTaskResult result);

signals:
    void busyChanged();
    void writingChanged();
    void progressChanged();
    void currentPhaseChanged();
    void errorMessageChanged();
    void analysisChanged();
    void canUndoChanged();
    void actionFeedback(const QString &message, bool isError);

private:
    void onScanFinished();
    void attachSession();
    void setBusy(bool busy);
    void setProgress(int current, int total);
    void setCurrentPhase(const QString &phase);
    void setErrorMessage(const QString &message);
    std::shared_ptr<QtProgressReporter> makeReporter();
    void applyScope(domain::MetadataRestoreScope scope);
    // The picker entries and every scoped count, from the proposals and
    // the scope as they are now.
    void refreshScope();
    // Unstages proposals by index as one batch: one session call, one
    // analysisChanged.
    void unstageIndices(const std::vector<int> &indices);
    // What a save's changeApplied burst landed, taken off the list once
    // the burst is over rather than per change (see attachSession).
    void takeAppliedChanges();

    // analysisChanged, or, while an AnalysisBatch is alive, one emission
    // when the outermost batch ends. Every property the page shows hangs
    // off that one signal, and each emission has QML re-read all of them
    // and rebuild both pickers' models, so a bulk operation that emits
    // per track is quadratic on the UI thread.
    void noteAnalysisChanged();
    class AnalysisBatch;
    int m_analysisBatchDepth = 0;
    bool m_analysisPending = false;
    QSet<QString> m_appliedChanges;
    bool m_takeAppliedQueued = false;

    QFutureWatcher<MetadataRestoreTaskResult> m_watcher;
    RestoreProposalListModel m_model;
    QPointer<LibraryEditSession> m_session;
    application::CancellationToken m_cancel;

    QString m_libraryPath;
    bool m_busy = false;
    bool m_hasScanned = false;
    int m_progressCurrent = 0;
    int m_progressTotal = 0;
    int m_phaseBaseline = 0;
    int m_currentPhaseTotal = 0;
    int m_stickTrackCount = 0;
    int m_storedTrackCount = 0;
    int m_conflictCount = 0;
    int m_conflictsLeftAlone = 0;
    int m_commentsRekordboxCannotTake = 0;
    QVariantList m_sourceSticks;
    int m_sourceProposalCount = 0;
    QStringList m_playlistNames;
    QVariantMap m_playlistTrackCounts;
    QString m_currentPhase;
    QString m_errorMessage;
};

}  // namespace seabass::gui
