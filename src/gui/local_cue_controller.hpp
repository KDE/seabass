// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/local_restore.hpp"
#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Read-only Qt list model over the RestoreCandidates LocalCueController
// last computed.
class RestoreCandidateListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by LocalCueController; not constructible from QML")

public:
    enum Roles {
        FilenameRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        DescriptionRole,
        StagedRole,  // this candidate's merge is staged in the edit session
    };

    explicit RestoreCandidateListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setCandidates(std::vector<domain::RestoreCandidate> candidates);
    const std::vector<domain::RestoreCandidate> &candidates() const { return m_candidates; }
    void removeCandidateAt(int index);
    void setStaged(int index, bool staged);
    void clearStaged();

private:
    std::vector<domain::RestoreCandidate> m_candidates;
    std::vector<bool> m_staged;  // parallel to m_candidates
};

// Result of a background task -- see LocalCueController::backupToComputer()
// / analyzeRestore(). Built entirely on a worker thread.
struct LocalCueTaskResult
{
    // backupToComputer: tracks upserted for each format actually present on
    // the stick, -1 for a format the stick doesn't have (so the UI can tell
    // "backed up 0 tracks" apart from "this stick has no Engine data").
    // analyzeRestore: all three unused.
    int tracksAffectedRekordbox = -1;
    int tracksAffectedEngine = -1;
    int tracksAffectedOneLibrary = -1;
    int stickTrackCount = 0;
    int localTrackCount = 0;
    std::vector<domain::RestoreCandidate> candidates;
    QString errorMessage;  // empty on success
    bool cancelled = false;  // an analyze stopped via cancelScan(); nothing else is set
};

// Wraps LocalCueStore for QML: backing up a stick's cues to a local
// SQLite database (application data dir, see LocalCueStore::defaultPath)
// and merging them back onto a stick -- adding whatever cues the backup
// has that the stick doesn't (a missing hot cue slot, a memory cue at a
// position nothing existing is close to), never overwriting a cue already
// there (see domain::LocalRestorePlanner::mergeCues() for the exact rule).
// Backup only ever writes to the local database; merge only ever writes to
// the stick (backup needs no confirmation, since nothing on the stick is
// ever at risk from it).
//
// Merges are staged, not written: applyRestore() stages one
// MergeCuesChange per candidate in the library's LibraryEditSession (the
// first one takes the edit lock), rows show it, and the page's Save
// writes them. A candidate whose change reached the stick disappears.
class LocalCueController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::RestoreCandidateListModel *restoreCandidates READ restoreCandidatesModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while a read-only analyze runs (never during a write or the
    // local backup): it can be stopped via cancelScan(), after which
    // scanCancelled() fires.
    Q_PROPERTY(bool scanCancellable READ scanCancellable NOTIFY busyChanged)
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    Q_PROPERTY(int stickTrackCount READ stickTrackCount NOTIFY analysisChanged)
    Q_PROPERTY(int localTrackCount READ localTrackCount NOTIFY analysisChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    // Mirrors the session: true while a save is writing to the stick.
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY analysisChanged)

public:
    explicit LocalCueController(QObject *parent = nullptr);

    RestoreCandidateListModel *restoreCandidatesModel() { return &m_model; }
    bool busy() const { return m_busy; }
    bool writing() const;
    int stagedCount() const { return static_cast<int>(m_stagedBySourceId.size()); }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    int stickTrackCount() const { return m_stickTrackCount; }
    int localTrackCount() const { return m_localTrackCount; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }
    bool canUndo() const;

    // Backs up whichever of rekordboxPath/enginePath is non-empty -- both,
    // for a stick that has both formats, in one call, rather than making
    // the user switch formats and click twice (and risk only ever backing
    // up whichever format happened to be selected). Scans each present
    // side, upserts every track with cues into the local database's merged
    // "current state" (never touches the stick, so needs no confirmation),
    // and freezes an independently-restorable snapshot of each under
    // `description`.
    Q_INVOKABLE void backupToComputer(const QString &stickLabel, const QString &description,
                                       const QString &rekordboxPath, const QString &enginePath,
                                       const QString &oneLibraryPath = QString());

    // Same convention as CleanupController::hasOneLibrary()/
    // DuplicatesController::hasOneLibrary()/ScanController::hasOneLibrary().
    Q_INVOKABLE bool hasOneLibrary(const QString &pioneerRoot) const;

    // Phase 1 (read-only): scans the stick and matches it against the
    // local backup's merged "current state," proposing to merge in
    // whichever cues the backup has that the stick doesn't yet (see
    // domain::LocalRestorePlanner::mergeCues()). reportFeedback controls
    // whether the outcome (found N / found nothing / busy / failed) is
    // announced via actionFeedback -- false for the automatic calls this
    // class makes itself (page load, format switch, the silent re-scan
    // after a successful write), true for a direct user click ("Re-Analyze
    // Latest"), so an automatic background refresh never pops up a message
    // the user didn't ask for, while a button they actually pressed always
    // gets a reaction, even a no-op one.
    Q_INVOKABLE void analyzeRestore(const QString &format, const QString &path, bool reportFeedback = false);

    // Same as analyzeRestore(), but matches against one specific past
    // snapshot instead of the merged current state. Always user-initiated
    // (the "Restore From Here" button, never called automatically), so
    // always reports feedback.
    Q_INVOKABLE void analyzeSnapshotRestore(qint64 snapshotId, const QString &format, const QString &path);

    // Stages every candidate currently proposed (from whichever of
    // analyzeRestore()/analyzeSnapshotRestore() ran last); the page's
    // Save writes them.
    Q_INVOKABLE void applyRestore();
    Q_INVOKABLE void unstage(int index);

    // Snapshot history management -- synchronous (a lightweight metadata
    // read/write, not a library scan).
    Q_INVOKABLE QVariantList listSnapshots();
    Q_INVOKABLE void setSnapshotDescription(qint64 id, const QString &description);
    Q_INVOKABLE bool deleteSnapshot(qint64 id);

    // Reverts every file the last save touched (the session's undo).
    Q_INVOKABLE void undoLastOperation();

    bool scanCancellable() const { return m_busy && !writing() && m_analyzeWatcher.isRunning(); }
    Q_INVOKABLE void cancelScan();

signals:
    void scanCancelled();
    void busyChanged();
    void scanProgressChanged();
    void analysisChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    void canUndoChanged();
    void writingChanged();
    // The one signal LocalCuePage.qml's popup actually listens to.
    // Unlike a Q_PROPERTY change notification (errorMessage/statusMessage
    // above), which only fires when the new value differs from the old
    // one, this fires on every single emission -- including two outcomes
    // in a row with identical text (e.g. "Restore From Here" on two
    // different snapshots that both turn out to offer nothing new) and a
    // busy-guard no-op, both of which a diffed property would risk
    // silently swallowing. isError just picks the popup's color.
    void actionFeedback(const QString &message, bool isError);

private:
    void onBackupFinished();
    void onAnalyzeFinished();
    void setBusy(bool busy);
    void setScanProgress(int current, int total);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);
    void attachSession();
    void stageCandidate(int index);
    int indexOfSourceId(const std::string &sourceId) const;
    std::shared_ptr<QtProgressReporter> makeReporter();

    RestoreCandidateListModel m_model;
    QFutureWatcher<LocalCueTaskResult> m_backupWatcher;
    QFutureWatcher<LocalCueTaskResult> m_analyzeWatcher;
    application::CancellationToken m_scanCancel;  // fresh per analyze
    QPointer<LibraryEditSession> m_session;
    std::map<std::string, QString> m_stagedBySourceId;  // stick track sourceId -> change id
    QString m_format;
    QString m_path;
    bool m_busy = false;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    int m_stickTrackCount = 0;
    int m_localTrackCount = 0;
    QString m_errorMessage;
    QString m_statusMessage;
    // Set right before kicking off the analyze task in flight, read back
    // in onAnalyzeFinished() once it completes -- see analyzeRestore()'s
    // own doc comment on why this needs to travel with the specific
    // request, not just be inferred from context.
    bool m_analyzeReportsFeedback = false;
};

}  // namespace seabass::gui
