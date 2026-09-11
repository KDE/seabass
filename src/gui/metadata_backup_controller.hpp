#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/metadata_backup_plan.hpp"
#include "gui/metadata_backup_proposal_model.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/local/metadata_store.hpp"

namespace seabass::gui
{

// Read-only list model over a page of the metadata store.
//
// Fed page by page from SQL rather than by handing the whole store to
// QML and filtering there: the store outlives any one stick and grows
// with every one it sees, and a browse view that costs the whole store
// to show twenty rows stops being lightweight on exactly the libraries
// this feature is for.
class StoredTrackListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by MetadataBackupController; not constructible from QML")

public:
    enum Roles {
        TrackIdRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        FilenameRole,
        RelativePathRole,
        DurationTextRole,
        BpmRole,
        MusicKeyRole,
        RatingRole,  // -1 when unrated, so QML can tell it from 0 stars
        CommentRole,
        CueCountRole,
        PlaylistCountRole,
        ArtworkUrlRole,
        StickLabelRole,
        UpdatedAtRole,
        StagedForDeletionRole,
    };

    explicit StoredTrackListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reset(std::vector<infrastructure::local::StoredTrack> rows);
    void append(const std::vector<infrastructure::local::StoredTrack> &rows);

    // ---- staged for deletion ------------------------------------------
    // Kept by row id rather than by row number, so it survives the list
    // growing a page, being searched, or being reordered. A mark that
    // silently moved to a different track would be the worst possible
    // bug in a feature whose button deletes things.
    //
    // And it survives a reset() too, which it did not used to. The old
    // rule threw the marks away whenever the list was rebuilt, on the
    // grounds that a mark you cannot see is a mark you might delete by
    // accident -- but it threw them away without telling the controller,
    // so typing in the search box left the page insisting three tracks
    // were staged while every tick box on it had cleared. The floating
    // Save button is the better answer to the same worry: it carries a
    // live count and lists what would go, so a staged row scrolled or
    // searched out of sight is still on screen in the one place that
    // matters. clearStagingFor() is for rows that have genuinely ceased
    // to exist.
    void setStagedForDeletion(int row, bool staged);
    void stageAllLoadedForDeletion();
    void clearDeletionStaging();
    // Forget marks for ids that are gone from the store, called after a
    // delete actually lands.
    void clearStagingFor(const QList<qint64> &trackIds);
    int stagedForDeletionCount() const { return static_cast<int>(m_stagedForDeletion.size()); }
    QList<qint64> stagedForDeletionIds() const;
    bool isStagedForDeletion(qint64 trackId) const { return m_stagedForDeletion.contains(trackId); }
    // One line per staged row, for the save button's tooltip. Only rows
    // currently loaded can describe themselves; the rest are counted.
    QStringList stagedDescriptions() const;

    qint64 trackIdAt(int row) const;

private:
    void emitRowChanged(int row, const QList<int> &roles);

    std::vector<infrastructure::local::StoredTrack> m_rows;
    QSet<qint64> m_stagedForDeletion;
};

// What one backup run did. Built entirely on a worker thread.
struct MetadataBackupTaskResult
{
    bool succeeded = false;
    QString errorMessage;
    infrastructure::local::MetadataBackupSummary summary;
    // Catalogs that were read, and catalogs present but unreadable. A
    // run that only saw one of three says so: this is a backup, so a
    // partial read costs coverage rather than correctness, but a user
    // who thinks their Engine cues are safe when they were never read
    // has been misled.
    QStringList catalogsRead;
    QStringList catalogsUnreadable;
};

// What a scan of one stick found: the plan, and the playlists to filter
// it by. Built entirely on a worker thread.
struct MetadataBackupScanResult
{
    domain::MetadataBackupPlan plan;
    QStringList playlistNames;
    QVariantMap playlistTrackCounts;
    int storedTrackCount = 0;
    QStringList catalogsRead;
    QStringList catalogsUnreadable;
    QString errorMessage;
    bool cancelled = false;
};

// Reads a stick's metadata into the local store, and browses what is in
// there. See docs/metadata-backup-plan.md.
//
// Only ever writes to the store. Nothing on the stick is at risk from a
// backup, so this needs no edit session and no lock -- unlike its
// counterpart, which puts data back.
//
// It does take the shape of one, though. A backup used to be a button
// that read the whole stick and told you afterwards what it had done;
// now it plans first, shows what would change, and waits. The staging,
// the floating Save button and the leave guard are the same pattern
// every editing page in Seabass uses, and the properties below named
// pendingCount/dirty/state/pendingDescriptions exist so SaveOverlayButton
// can drive this controller without knowing it is not an edit session.
// That button takes its session as an untyped `var` precisely so a page
// with something else to save can hand it something else.
//
// Two populations share one list, chosen by which source the page picks:
// a stick, and the list is that stick measured against the store; or the
// store itself, and the list is what has been backed up. Deletions are
// staged in the second and additions in the first, and one Save commits
// whatever is staged in either.
//
// The three catalogs are collapsed into files before planning
// (application::collapseCatalogRows), so a track that rekordbox, Engine
// and OneLibrary all list is one row holding the union of their cues,
// not three rows overwriting each other in turn.
class MetadataBackupController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int progressCurrent READ progressCurrent NOTIFY progressChanged)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged)
    Q_PROPERTY(QString currentPhase READ currentPhase NOTIFY currentPhaseChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(QVariantMap lastRun READ lastRun NOTIFY resultChanged)

    // ---- the browse population ----------------------------------------
    Q_PROPERTY(StoredTrackListModel *browseModel READ browseModel CONSTANT)
    Q_PROPERTY(int storedTrackCount READ storedTrackCount NOTIFY storeChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY storeChanged)
    Q_PROPERTY(QString storeLocation READ storeLocation CONSTANT)
    Q_PROPERTY(qint64 artworkBytes READ artworkBytes NOTIFY storeChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY storeChanged)
    Q_PROPERTY(int loadedCount READ loadedCount NOTIFY storeChanged)
    Q_PROPERTY(int stagedForDeletionCount READ stagedForDeletionCount NOTIFY selectionChanged)
    Q_PROPERTY(bool allLoadedStaged READ allLoadedStaged NOTIFY selectionChanged)

    // ---- the diff population ------------------------------------------
    Q_PROPERTY(BackupProposalListModel *proposals READ proposals CONSTANT)
    // Which population the one list is showing. True means the store
    // itself; false means the stick named by sourceStickLabel.
    Q_PROPERTY(bool browsingStore READ browsingStore NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceStickLabel READ sourceStickLabel NOTIFY sourceChanged)
    Q_PROPERTY(bool hasScanned READ hasScanned NOTIFY analysisChanged)
    Q_PROPERTY(int proposalCount READ proposalCount NOTIFY analysisChanged)
    Q_PROPERTY(int visibleProposalCount READ visibleProposalCount NOTIFY analysisChanged)
    Q_PROPERTY(int stagedAddCount READ stagedAddCount NOTIFY selectionChanged)
    Q_PROPERTY(bool allProposalsStaged READ allProposalsStaged NOTIFY selectionChanged)
    Q_PROPERTY(int tracksSeen READ tracksSeen NOTIFY analysisChanged)
    Q_PROPERTY(int alreadyCurrent READ alreadyCurrent NOTIFY analysisChanged)
    Q_PROPERTY(int withoutIdentity READ withoutIdentity NOTIFY analysisChanged)
    // Index 0 is "All tracks", the same convention every other playlist
    // picker in Seabass follows.
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY analysisChanged)
    Q_PROPERTY(QVariantMap playlistTrackCounts READ playlistTrackCounts NOTIFY analysisChanged)
    Q_PROPERTY(QString selectedPlaylist READ selectedPlaylist NOTIFY filterChanged)

    // ---- what SaveOverlayButton reads ---------------------------------
    // Named for the edit-session interface it is standing in for, not
    // for what this class calls things internally. See the class comment.
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY selectionChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY selectionChanged)
    Q_PROPERTY(QString state READ state NOTIFY busyChanged)
    Q_PROPERTY(QStringList pendingDescriptions READ pendingDescriptions NOTIFY selectionChanged)

    // The merge rule as prose, straight from the domain function that
    // implements it, so the help popup cannot describe a policy the
    // program no longer follows.
    Q_PROPERTY(QString mergeRuleHelp READ mergeRuleHelp CONSTANT)

public:
    explicit MetadataBackupController(QObject *parent = nullptr);
    ~MetadataBackupController() override;

    bool busy() const { return m_busy; }
    int progressCurrent() const { return m_progressCurrent; }
    int progressTotal() const { return m_progressTotal; }
    QString currentPhase() const { return m_currentPhase; }
    QString errorMessage() const { return m_errorMessage; }
    bool hasResult() const { return m_hasResult; }
    QVariantMap lastRun() const { return m_lastRun; }

    StoredTrackListModel *browseModel() { return &m_browseModel; }
    int storedTrackCount() const { return m_storedTrackCount; }
    int matchCount() const { return m_matchCount; }
    QString storeLocation() const;
    qint64 artworkBytes() const { return m_artworkBytes; }
    bool canLoadMore() const { return m_browseModel.rowCount() < m_matchCount; }
    int loadedCount() const { return m_browseModel.rowCount(); }
    int stagedForDeletionCount() const { return m_browseModel.stagedForDeletionCount(); }
    bool allLoadedStaged() const
    {
        return m_browseModel.rowCount() > 0 && m_browseModel.stagedForDeletionCount() == m_browseModel.rowCount();
    }

    BackupProposalListModel *proposals() { return &m_proposalModel; }
    bool browsingStore() const { return m_browsingStore; }
    QString sourceStickLabel() const { return m_sourceStickLabel; }
    bool hasScanned() const { return m_hasScanned; }
    int proposalCount() const { return m_proposalModel.totalCount(); }
    int visibleProposalCount() const { return m_proposalModel.rowCount(); }
    int stagedAddCount() const { return m_proposalModel.stagedCount(); }
    bool allProposalsStaged() const
    {
        return m_proposalModel.totalCount() > 0 && m_proposalModel.stagedCount() == m_proposalModel.totalCount();
    }
    int tracksSeen() const { return m_tracksSeen; }
    int alreadyCurrent() const { return m_alreadyCurrent; }
    int withoutIdentity() const { return m_withoutIdentity; }
    QStringList playlistNames() const { return m_playlistNames; }
    QVariantMap playlistTrackCounts() const { return m_playlistTrackCounts; }
    QString selectedPlaylist() const { return m_playlist; }

    int pendingCount() const { return stagedAddCount() + stagedForDeletionCount(); }
    bool dirty() const { return pendingCount() > 0; }
    QString state() const;
    QStringList pendingDescriptions() const;

    QString mergeRuleHelp() const;

    // ---- choosing what the list shows ---------------------------------
    // libraryPath is any catalog directory on the stick (".../PIONEER",
    // ".../Engine Library"); every catalog on that stick is read,
    // whichever one the caller happened to have.
    //
    // Refuses while anything is staged: the page asks first, because the
    // staging was decided against the numbers this scan is about to
    // replace. discardStagingAndSelectStick() is what the dialog's
    // "discard" button calls.
    Q_INVOKABLE bool selectStick(const QString &libraryPath, const QString &libraryId, const QString &stickLabel);
    Q_INVOKABLE void discardStagingAndSelectStick(const QString &libraryPath, const QString &libraryId,
                                                   const QString &stickLabel);
    Q_INVOKABLE bool browseStore();
    Q_INVOKABLE void discardStagingAndBrowseStore();
    Q_INVOKABLE void cancel();

    // ---- staging ------------------------------------------------------
    // Every index a page passes is a ListView row, which is not a
    // proposal index whenever a filter is narrowing the list. The
    // translation happens here, at the one boundary where QML and the
    // proposal list meet, rather than being something each call site has
    // to remember.
    Q_INVOKABLE void toggleStagedForAdd(int row);
    Q_INVOKABLE void stageAllForAdd();
    Q_INVOKABLE void unstageAllForAdd();

    // Ticking a stored row and pressing its delete button are one state,
    // not two: there is no separate selection to keep in step with the
    // marks, and so no way for the two to disagree.
    Q_INVOKABLE void toggleStagedForDeletion(int row);
    Q_INVOKABLE void stageAllForDeletion();
    Q_INVOKABLE void clearDeletionStaging();
    // Everything, in both populations. What the page's "nothing is
    // staged after all" escape hatch calls.
    Q_INVOKABLE void clearAllStaging();

    // ---- committing ---------------------------------------------------
    // Writes the staged additions into the store and deletes the staged
    // removals from it. Nothing on any stick is touched either way.
    Q_INVOKABLE void save();

    // ---- filtering ----------------------------------------------------
    // Narrows whichever population is showing. The browse list pages
    // from SQL so its search is a re-query; the proposal list is already
    // in memory so its is a filter.
    Q_INVOKABLE void search(const QString &text);
    Q_INVOKABLE void setPlaylist(const QString &name);
    Q_INVOKABLE void loadMore();
    // Re-reads counts and the first page of the browse list.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QVariantList cuesFor(qint64 trackId);
    Q_INVOKABLE QStringList playlistsFor(qint64 trackId);
    // Every cue on one line each, for the hover tooltip on a browse
    // row's cue badge. Called on hover rather than per row: the browse
    // list is paged precisely so that showing twenty rows costs twenty
    // rows, and fetching every cue of every row to fill tooltips nobody
    // opens would undo that.
    Q_INVOKABLE QString cueSummaryFor(qint64 trackId);

signals:
    void busyChanged();
    void selectionChanged();
    void progressChanged();
    void currentPhaseChanged();
    void errorMessageChanged();
    void resultChanged();
    void storeChanged();
    void sourceChanged();
    void analysisChanged();
    void filterChanged();
    void actionFeedback(const QString &message, bool isError);

private:
    void startScan(const QString &libraryPath, const QString &libraryId, const QString &stickLabel);
    void onScanFinished();
    void onSaveFinished();
    void applyStagedDeletions();
    void setBusy(bool busy);
    void setWriting(bool writing);
    void setProgress(int current, int total);
    void setCurrentPhase(const QString &phase);
    void setErrorMessage(const QString &message);
    std::shared_ptr<QtProgressReporter> makeReporter();
    // Opened lazily, on the UI thread, for browsing only. A scan and a
    // save each open their own connection on their own thread.
    infrastructure::local::MetadataStore *store();

    QFutureWatcher<MetadataBackupScanResult> m_scanWatcher;
    QFutureWatcher<MetadataBackupTaskResult> m_saveWatcher;
    StoredTrackListModel m_browseModel;
    BackupProposalListModel m_proposalModel;
    std::unique_ptr<infrastructure::local::MetadataStore> m_store;
    application::CancellationToken m_cancel;

    bool m_busy = false;
    bool m_writing = false;
    bool m_hasResult = false;
    bool m_hasScanned = false;
    // The store is the starting population: a page that opens on a stick
    // it has not scanned would show an empty list and look broken.
    bool m_browsingStore = true;
    int m_progressCurrent = 0;
    int m_progressTotal = 0;
    // The stick read and the store write share one continuous bar rather
    // than each restarting from zero.
    int m_phaseBaseline = 0;
    int m_currentPhaseTotal = 0;
    int m_storedTrackCount = 0;
    int m_matchCount = 0;
    int m_tracksSeen = 0;
    int m_alreadyCurrent = 0;
    int m_withoutIdentity = 0;
    qint64 m_artworkBytes = 0;
    QString m_search;
    QString m_playlist;
    QString m_currentPhase;
    QString m_errorMessage;
    QVariantMap m_lastRun;
    QStringList m_playlistNames;
    QVariantMap m_playlistTrackCounts;

    // The stick the proposal list was planned against, kept so a save
    // can name its source and a rescan can repeat it.
    QString m_sourceLibraryPath;
    QString m_sourceLibraryId;
    QString m_sourceStickLabel;
};

}  // namespace seabass::gui
