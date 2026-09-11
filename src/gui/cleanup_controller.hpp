// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QStringList>
#include <QVariantMap>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/use_cases/real_file_sizes.hpp"
#include "domain/duplicate_cleanup.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Read-only Qt list model over the cleanup plans CleanupController last
// computed. Only groups DuplicateCleanupPlanner found something
// actually removable for (toRemove non-empty) are exposed. A
// degenerate single-track "group" has nothing to clean up.
class CleanupPlanListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by CleanupController; not constructible from QML")

public:
    enum Roles {
        SurvivorRole = Qt::UserRole + 1,
        ToRemoveRole,
        DiffersRole,
        WastedBytesHumanRole,
        NewCueCountRole,
        IncludedRole,
        HasUnpreservableDataAtRiskRole,
        // How many of this group's copies are files no catalog
        // references, and how many of those the planner refuses to
        // delete. Separate from toRemove's own count because the two are
        // removed by different acts -- a row is dropped, a file is
        // deleted -- and the page has to say which it is about to do.
        UnreferencedCountRole,
        UnreferencedHeldBackCountRole,
        // This group's clean-up is staged in the edit session.
        StagedRole,
        StagedDescriptionRole,
    };

    explicit CleanupPlanListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QHash<int, QByteArray> roleNames() const override;

    void setPlans(std::vector<domain::DuplicateCleanupPlan> plans);
    const std::vector<domain::DuplicateCleanupPlan> &plans() const { return m_plans; }
    bool included(size_t index) const;
    int includedCount() const;
    // Removes specific plans (by index into plans()/included(), not
    // visible row) without a full rescan -- for CleanupController::
    // apply() after a successful write: those groups are now merged
    // and gone, and removing them can't change any other group's own
    // classification (DuplicateTrackFinder groups by filename/title+
    // artist+duration only, never cues or row existence). Keeps
    // m_included and m_visibleIndices (the current search filter) in
    // sync with the shrunk m_plans, unlike a naive setPlans() call,
    // which would reset every remaining row's checkbox back to its
    // default and lose whatever the user had actually checked.
    void removePlansAt(std::vector<int> indices);
    // Sets every row's included flag at once (select all / deselect all) -
    // only the currently *visible* rows (see setFilter()), so selecting all
    // while a search is active doesn't silently touch groups scrolled out
    // of view by the filter.
    void setAllIncluded(bool included);

    // Case-insensitive substring match against every track's title/artist
    // in each group (the survivor and every copy to remove). A group is
    // visible if any of them match. Filtering never touches m_included:
    // it's purely a view over the same underlying selection state, so
    // clearing/changing the search text never loses what was checked.
    // Empty query shows everything.
    void setFilter(const QString &query);

    // The plans() index behind a visible row, -1 when out of range.
    int rawIndexForRow(int row) const;
    void setStaged(size_t rawIndex, bool staged, const QString &description);
    void clearStaged();

private:
    std::vector<domain::DuplicateCleanupPlan> m_plans;
    std::vector<QString> m_stagedDescriptions;  // empty = not staged; parallel to m_plans
    // Parallel to m_plans, default true unless the plan differs (see
    // DuplicateCleanupPlan::differs' own doc comment for why that
    // defaults to excluded).
    std::vector<bool> m_included;
    // Indices into m_plans/m_included that pass the current filter, in
    // order, what QML's row-based data()/setData() actually iterate.
    // included(size_t)/plans() stay index-into-m_plans-based (unfiltered)
    // since apply() needs to act on every included group regardless of
    // what the search box currently shows.
    std::vector<size_t> m_visibleIndices;
};

// Read-only Qt list model over the audio files a past cleanup's DB edit
// orphaned but hasn't deleted from disk yet (see PendingDeletionManifest's
// own class comment). Mirrors CleanupPlanListModel's included/excluded
// per-row selection pattern exactly, for the same reason: "act on
// everything found" is rarely what you want to click through blind.
class PendingDeletionListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by CleanupController; not constructible from QML")

public:
    enum Roles {
        FormatRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        FilePathRole,
        BackupIdRole,
        TimestampRole,
        IncludedRole,
        SizeHumanRole,
    };

    explicit PendingDeletionListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QHash<int, QByteArray> roleNames() const override;

    void setEntries(std::vector<infrastructure::cleanup::PendingDeletion> entries);
    // Only the rows currently checked, what deleteSelectedPendingFiles() acts on.
    std::vector<infrastructure::cleanup::PendingDeletion> includedEntries() const;
    int includedCount() const;
    // Sets every row's included flag at once (select all / deselect all).
    void setAllIncluded(bool included);
    // Sum of every listed entry's fileSizeBytes, and just the checked
    // ones, "how much space is here" vs "how much would this delete
    // free up right now."
    std::uint64_t totalBytes() const;
    std::uint64_t includedBytes() const;

private:
    std::vector<infrastructure::cleanup::PendingDeletion> m_entries;
    // Parallel to m_entries, default true (everything selected), same
    // default-selected convention as CleanupPlanListModel except when a
    // plan `differs` (there's no equivalent ambiguity here to default
    // away from).
    std::vector<bool> m_included;
};

// The stray-file half of a scan, as the page needs to talk about it.
// A count on its own would be the failure mode this whole area has:
// every field here exists so the page can say what the number is based
// on rather than presenting a partial answer as a complete one. See
// infrastructure/cleanup/stray_file_scan.hpp.
struct StrayFileSummary
{
    int filesFound = 0;
    qulonglong bytesFound = 0;
    int unreadable = 0;
    // The catalogs' own format names ("rekordbox", "engine",
    // "onelibrary"), not display text: FormatLabels.qml is the single
    // source of truth for what a DJ is shown a format called, and this
    // side of the app has no business deciding it.
    QStringList catalogsConsulted;
    bool walkIncomplete = false;
    bool probeAvailable = false;
    bool usable = false;
    QString refusal;  // shown instead of a count when !usable
};

// Result of a background scan+plan task, see CleanupController::
// rescan(). Built entirely on a worker thread, no access to the
// controller.
struct CleanupTaskResult
{
    std::vector<domain::DuplicateCleanupPlan> plans;
    StrayFileSummary strays;
    // Gathered before scoping, so the picker never shrinks its own
    // choices once a playlist is selected -- same rule as
    // SyncController's playlistNames.
    QStringList playlistNames;
    QString errorMessage;
    bool cancelled = false;  // stopped via cancelScan(); nothing else is set

    // Groups the scan found and deliberately did not offer, because
    // applying them would take a recording out of one catalog entirely
    // (DuplicateCleanupPlan::wouldStrandAFormat). Counted rather than
    // silently dropped: a group that vanishes with no explanation reads
    // as a scan that missed it, and the DJ would go looking.
    int groupsHeldBackStranding = 0;

    // What the filesystem said about the files these plans would remove.
    // See application::measureRealFileSizes(): the catalogs cannot answer
    // this, because two of the three record no file size at all.
    application::MeasuredFileSizes sizes;

    // True when rows from every catalog on the stick were folded into
    // files before grouping. False when a catalog could not be read, in
    // which case this scan saw one catalog's rows and cannot say what
    // the others hold -- see runRescanTask() for why that forbids
    // collapsing rather than merely reducing what is found.
    bool collapsedAcrossCatalogs = false;
};

// Result of a background pending-deletion apply task, see
// CleanupController::deleteSelectedPendingFiles(). There's nothing here to undo (a deleted audio file
// is gone, the DB edit that orphaned it was already backed up
// separately, back when it was first removed from the library), so
// there's no UndoableBackup list.
struct PendingDeletionApplyResult
{
    QString errorMessage;
    QString statusMessage;
    int deleted = 0;  // gone from disk (or found already gone)
    int total = 0;    // files the fresh scan confirmed safe to delete
    bool cancelled = false;
};

// Wraps DuplicateCleanupPlanner + both formats' LibraryCleanupWriter
// implementations for QML: scans a library, finds duplicate groups,
// plans a survivor for each, and (for included groups) removes every
// other copy, merging their cues onto the survivor first and fixing
// up playlist membership on both formats, backing up before every
// write, mirroring every other controller's safety pattern exactly.
//
// Deliberately does NOT delete the doomed tracks' audio files: apply()
// stops at removing their library entries and appending them to
// PendingDeletionManifest, so the actual (irreversible, cross-format)
// file deletion stays a distinct, separately-reviewed step.
//
// Clean-ups are staged, not written: apply() stages one CleanupGroupChange
// per included group in the library's LibraryEditSession (the first one
// takes the edit lock), rows show it, and the page's Save writes them; a
// group whose change reached the stick disappears. deleteSelectedPendingFiles()
// stays a direct write (it deletes files, there is nothing to stage).
class CleanupController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::CleanupPlanListModel *plans READ plansModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // True while a read-only rescan or manual-merge plan runs (never
    // during a write): it can be stopped via cancelScan(), after which
    // scanCancelled() fires.
    Q_PROPERTY(bool scanCancellable READ scanCancellable NOTIFY busyChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)
    // A pending-file deletion is running and can still be stopped
    // between two files (see cancelWrite()).
    Q_PROPERTY(bool writeCancellable READ writeCancellable NOTIFY writingChanged)
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY plansChanged)
    Q_PROPERTY(int scanCurrent READ scanCurrent NOTIFY scanProgressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY scanProgressChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(QString totalWastedBytesHuman READ totalWastedBytesHuman NOTIFY plansChanged)
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY plansChanged)
    // Raw byte counts, for the space diagram on the Clean Up page. The
    // *Human strings above stay because they are what the prose reads
    // from; a bar drawn to scale needs the numbers themselves, and
    // parsing "8.7 GB" back out of a formatted string to draw it would
    // be both lossy and absurd.
    Q_PROPERTY(qlonglong totalWastedBytes READ totalWastedBytes NOTIFY plansChanged)
    Q_PROPERTY(qlonglong includedWastedBytes READ includedWastedBytes NOTIFY includedChanged)
    // Capacity of the stick being cleaned, so the diagram can show what
    // is reclaimed against what is actually there. 0 when it cannot be
    // read (no stick, or a path that is not on one) -- QML must treat 0
    // as "unknown" and draw nothing rather than an empty disk.
    Q_PROPERTY(qlonglong stickTotalBytes READ stickTotalBytes NOTIFY plansChanged)
    Q_PROPERTY(qlonglong stickFreeBytes READ stickFreeBytes NOTIFY plansChanged)
    Q_PROPERTY(int includedCount READ includedCount NOTIFY includedChanged)
    // The stray-file half of the last scan, for the page's own account
    // of what it looked at: {filesFound, bytesHuman, unreadable,
    // catalogsConsulted, walkIncomplete, probeAvailable, usable,
    // refusal}. A map rather than eight properties because it is one
    // paragraph of text on one page, and every field of it is only ever
    // read together with the others.
    Q_PROPERTY(QVariantMap unreferencedFiles READ unreferencedFiles NOTIFY plansChanged)
    Q_PROPERTY(seabass::gui::PendingDeletionListModel *pendingDeletions READ pendingDeletionsModel CONSTANT)
    Q_PROPERTY(int pendingDeletionsIncludedCount READ pendingDeletionsIncludedCount NOTIFY pendingDeletionsChanged)
    Q_PROPERTY(QString totalPendingBytesHuman READ totalPendingBytesHuman NOTIFY pendingDeletionsChanged)
    Q_PROPERTY(QString includedPendingBytesHuman READ includedPendingBytesHuman NOTIFY pendingDeletionsChanged)

public:
    explicit CleanupController(QObject *parent = nullptr);

    CleanupPlanListModel *plansModel() { return &m_model; }
    bool busy() const { return m_busy; }
    bool writing() const;
    int stagedCount() const { return static_cast<int>(m_stagedBySurvivor.size()); }
    int scanCurrent() const { return m_scanCurrent; }
    int scanTotal() const { return m_scanTotal; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }
    bool canUndo() const;
    QString totalWastedBytesHuman() const;
    QStringList playlistNames() const { return m_playlistNames; }
    qlonglong totalWastedBytes() const;
    qlonglong includedWastedBytes() const;
    qlonglong stickTotalBytes() const;
    qlonglong stickFreeBytes() const;
    int includedCount() const { return m_model.includedCount(); }
    QVariantMap unreferencedFiles() const;
    PendingDeletionListModel *pendingDeletionsModel() { return &m_pendingModel; }
    int pendingDeletionsIncludedCount() const { return m_pendingModel.includedCount(); }
    QString totalPendingBytesHuman() const;
    QString includedPendingBytesHuman() const;

    // format is "rekordbox" or "engine"; path is the corresponding
    // DetectedStick.rekordboxPath / .enginePath.
    //
    // playlistName and searchQuery scope the review to part of the
    // library, empty meaning the whole of it -- same pair, same
    // meanings, as SyncController::analyze(), so the two pages narrow
    // the same way. Both can narrow at once. Scoping applies to the
    // finished track list, stray files included: a stray copy of a
    // scoped track must stay reachable, or narrowing would hide exactly
    // the copies this page exists to find.
    Q_INVOKABLE void scan(const QString &format, const QString &path, const QString &playlistName = QString(),
                           const QString &searchQuery = QString());

    // Same convention as ScanController::hasOneLibrary(), lets QML show
    // that a rekordbox-format cleanup also mirrors into OneLibrary when
    // it's present on this stick.
    Q_INVOKABLE bool hasOneLibrary(const QString &pioneerRoot) const;

    // Builds and populates `plans` with a single manually-declared merge
    // of exactly two tracks (by sourceId), reusing the same survivor/cue-
    // merge planning DuplicateCleanupPlanner already does for auto-
    // detected groups, for ScanPage.qml's "Merge with..." picker,
    // where the user (not DuplicateTrackFinder) has already decided these
    // two are the same track. apply() then works exactly as it does for
    // an auto-detected plan, no separate apply path needed.
    Q_INVOKABLE void planManualMerge(const QString &format, const QString &path, const QString &sourceIdA,
                                      const QString &sourceIdB);

    Q_INVOKABLE void setIncluded(int index, bool included);
    Q_INVOKABLE void setAllIncluded(bool included);

    // Filters the visible duplicate groups by title/artist (see
    // CleanupPlanListModel::setFilter()). Purely a view filter, never
    // changes which groups are included, and apply() still acts on every
    // included group regardless of the current search text.
    Q_INVOKABLE void search(const QString &query);

    // Stages cleaning up every currently-included group: cues merged onto
    // the survivor, playlist membership fixed up on both formats, backed
    // up first. Does NOT delete any audio file, see the class comment.
    Q_INVOKABLE void apply();
    // row is the visible row (the delegate index).
    Q_INVOKABLE void unstage(int row);
    // Every staged group at once, for Escape. Loses no work: the
    // checkboxes keep their state, so staging again restores exactly
    // what was there.
    Q_INVOKABLE void unstageAll();

    // Reverts every file the last save touched (the session's undo).
    Q_INVOKABLE void undoLastOperation();

    // Re-reads this format's pending-deletion entries from
    // Seabass/orphaned/pending-deletions.jsonl on the stick and repopulates
    // pendingDeletions. Cheap (a small text file plus a stat() per entry
    // for its current size), so this runs synchronously rather than on a
    // background thread, called automatically after every scan()/
    // apply(), but QML can also call it directly.
    Q_INVOKABLE void refreshPendingDeletions();

    // Sets format/path and loads pendingDeletions only, unlike scan(),
    // does NOT kick off the (potentially slow, whole-library) duplicate-
    // group rescan. For PendingDeletionsPage, whose entire purpose is the
    // pending-deletions list: forcing a full rescan just to show a small
    // manifest file would be a pointless wait.
    Q_INVOKABLE void loadPendingDeletionsOnly(const QString &format, const QString &path);

    Q_INVOKABLE void setPendingDeletionIncluded(int index, bool included);
    Q_INVOKABLE void setAllPendingDeletionIncluded(bool included);

    // For every currently-checked pendingDeletions entry: re-scans the
    // library fresh and, ONLY for entries resolvePendingDeletions()
    // confirms are genuinely no longer referenced by any current track,
    // deletes the file from disk and clears it from the manifest.
    // Entries still referenced (by anything, whether checked or not) are
    // never deleted no matter what the manifest said, see
    // resolvePendingDeletions()'s own doc comment for why the manifest
    // alone is never trusted. Irreversible: unlike apply(), there is no
    // undo for an actual file deletion.
    Q_INVOKABLE void deleteSelectedPendingFiles();
    // Stops deleteSelectedPendingFiles() after the file being deleted
    // right now; pendingDeletionsWriteFinished() then says how many went.
    Q_INVOKABLE void cancelWrite();
    bool writeCancellable() const { return m_writing && !m_pendingDeleteCancel.cancelled(); }

    bool scanCancellable() const { return m_busy && !writing() && m_watcher.isRunning(); }
    Q_INVOKABLE void cancelScan();

signals:
    void scanCancelled();
    void busyChanged();
    void writingChanged();
    void scanProgressChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    void canUndoChanged();
    void plansChanged();
    void includedChanged();
    void pendingDeletionsChanged();
    // After deleteSelectedPendingFiles(): {written, total, unit, verb,
    // cancelled, error}, the OperationSummaryDialog shape.
    void pendingDeletionsWriteFinished(const QVariantMap &summary);
    // Another instance is editing this library; no file was deleted.
    void lockRefused(const QVariantMap &holder);

private:
    void rescan();
    void onRescanFinished();
    void onDeletePendingFinished();
    void attachSession();
    void stagePlan(size_t rawIndex);
    int cleanupItemCountHint() const;
    int indexOfSurvivor(const std::string &survivorSourceId) const;
    void setBusy(bool busy);
    void setWriting(bool writing);
    void setScanProgress(int current, int total);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);
    std::shared_ptr<QtProgressReporter> makeReporter();

    CleanupPlanListModel m_model;
    PendingDeletionListModel m_pendingModel;
    QFutureWatcher<CleanupTaskResult> m_watcher;
    StrayFileSummary m_strays;
    application::CancellationToken m_scanCancel;  // fresh per rescan()/planManualMerge()
    application::CancellationToken m_pendingDeleteCancel;  // fresh per deleteSelectedPendingFiles()
    bool m_holdsDirectWrite = false;
    QPointer<LibraryEditSession> m_session;
    struct StagedInfo
    {
        QString changeId;
        QString description;
    };
    std::map<std::string, StagedInfo> m_stagedBySurvivor;  // survivor sourceId -> what is staged
    QFutureWatcher<PendingDeletionApplyResult> m_pendingWriteWatcher;
    QString m_format;
    QString m_playlistName;
    QStringList m_playlistNames;
    QString m_searchQuery;
    QString m_path;
    bool m_busy = false;
    bool m_writing = false;
    int m_scanCurrent = 0;
    int m_scanTotal = 0;
    QString m_errorMessage;
    QString m_statusMessage;
};

}  // namespace seabass::gui
