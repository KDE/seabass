#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/local/metadata_store.hpp"
#include "gui/qt_progress_reporter.hpp"

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
        SelectedRole,
        StagedForDeletionRole,
    };

    explicit StoredTrackListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reset(std::vector<infrastructure::local::StoredTrack> rows);
    void append(const std::vector<infrastructure::local::StoredTrack> &rows);

    // ---- selection ----------------------------------------------------
    // Kept by row id rather than by row number, so it survives the list
    // growing a page and the list being rebuilt after a delete. A
    // selection that silently moved to different tracks when more rows
    // loaded would be the worst possible bug in a feature whose other
    // button deletes things.
    void setSelected(int row, bool selected);
    void selectAllLoaded();
    void clearSelection();
    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    QList<qint64> selectedIds() const;
    bool isSelected(qint64 trackId) const { return m_selected.contains(trackId); }

    // ---- staged for deletion ------------------------------------------
    void setStagedForDeletion(int row, bool staged);
    void stageSelectedForDeletion();
    void clearDeletionStaging();
    int stagedForDeletionCount() const { return static_cast<int>(m_stagedForDeletion.size()); }
    QList<qint64> stagedForDeletionIds() const;

    qint64 trackIdAt(int row) const;

private:
    void emitRowChanged(int row, const QList<int> &roles);

    std::vector<infrastructure::local::StoredTrack> m_rows;
    QSet<qint64> m_selected;
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

// Reads every catalog on a stick into the local metadata store, and
// browses what is in there. See docs/metadata-backup-plan.md.
//
// Only ever writes to the store. Nothing on the stick is at risk from a
// backup, so this needs no edit session, no lock and no confirmation --
// unlike its counterpart, which puts data back.
//
// The three catalogs are collapsed into files before storing
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
    Q_PROPERTY(StoredTrackListModel *browseModel READ browseModel CONSTANT)
    Q_PROPERTY(int storedTrackCount READ storedTrackCount NOTIFY storeChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY storeChanged)
    Q_PROPERTY(QString storeLocation READ storeLocation CONSTANT)
    Q_PROPERTY(qint64 artworkBytes READ artworkBytes NOTIFY storeChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY storeChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(int loadedCount READ loadedCount NOTIFY storeChanged)
    Q_PROPERTY(bool allLoadedSelected READ allLoadedSelected NOTIFY selectionChanged)
    Q_PROPERTY(int stagedForDeletionCount READ stagedForDeletionCount NOTIFY selectionChanged)
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
    int selectedCount() const { return m_browseModel.selectedCount(); }
    int loadedCount() const { return m_browseModel.rowCount(); }
    bool allLoadedSelected() const
    {
        return m_browseModel.rowCount() > 0 && m_browseModel.selectedCount() == m_browseModel.rowCount();
    }
    int stagedForDeletionCount() const { return m_browseModel.stagedForDeletionCount(); }
    QString mergeRuleHelp() const;

    // libraryPath is any catalog directory on the stick (".../PIONEER",
    // ".../Engine Library"); every catalog on that stick is read,
    // whichever one the caller happened to have.
    Q_INVOKABLE void backUp(const QString &libraryPath, const QString &libraryId, const QString &stickLabel);
    Q_INVOKABLE void cancel();

    // ---- selection and deletion ---------------------------------------
    Q_INVOKABLE void setSelected(int row, bool selected);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void selectNone();
    // The per-row delete button: marks one row, nothing is removed yet.
    Q_INVOKABLE void toggleStagedForDeletion(int row);
    // The button under the list: everything ticked joins whatever the
    // per-row buttons already marked.
    Q_INVOKABLE void stageSelectedForDeletion();
    // Actually deletes. Returns how many rows went, so the page can say
    // so rather than guess from what it asked for.
    Q_INVOKABLE int deleteStaged();

    // Replaces the browse list with the first page matching `text`.
    Q_INVOKABLE void search(const QString &text);
    Q_INVOKABLE void loadMore();
    // Re-reads counts and the first page. Cheap, and called after a
    // backup so the list shows what just went in.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QVariantList cuesFor(qint64 trackId);
    Q_INVOKABLE QStringList playlistsFor(qint64 trackId);
    // Every cue on one line each, for the hover tooltip on a row's cue
    // badge. Called on hover rather than per row: the browse list is
    // paged precisely so that showing twenty rows costs twenty rows, and
    // fetching every cue of every row to fill tooltips nobody opens
    // would undo that.
    Q_INVOKABLE QString cueSummaryFor(qint64 trackId);

signals:
    void busyChanged();
    void selectionChanged();
    void progressChanged();
    void currentPhaseChanged();
    void errorMessageChanged();
    void resultChanged();
    void storeChanged();

private:
    void onBackupFinished();
    void setBusy(bool busy);
    void setProgress(int current, int total);
    void setCurrentPhase(const QString &phase);
    void setErrorMessage(const QString &message);
    std::shared_ptr<QtProgressReporter> makeReporter();
    // Opened lazily, on the UI thread, for browsing only. A backup opens
    // its own connection on its own thread.
    infrastructure::local::MetadataStore *store();

    QFutureWatcher<MetadataBackupTaskResult> m_watcher;
    StoredTrackListModel m_browseModel;
    std::unique_ptr<infrastructure::local::MetadataStore> m_store;
    application::CancellationToken m_cancel;

    bool m_busy = false;
    bool m_hasResult = false;
    int m_progressCurrent = 0;
    int m_progressTotal = 0;
    // The stick read and the store write share one continuous bar rather
    // than each restarting from zero.
    int m_phaseBaseline = 0;
    int m_currentPhaseTotal = 0;
    int m_storedTrackCount = 0;
    int m_matchCount = 0;
    qint64 m_artworkBytes = 0;
    QString m_search;
    QString m_currentPhase;
    QString m_errorMessage;
    QVariantMap m_lastRun;
};

}  // namespace seabass::gui
