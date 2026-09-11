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
#include "domain/metadata_restore.hpp"
#include "gui/qt_progress_reporter.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Read-only model over the proposals the controller last planned.
class RestoreProposalListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by MetadataRestoreController; not constructible from QML")

public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        FilenameRole,
        RelativePathRole,  // where the file sits on THIS stick, absolute
        DurationTextRole,
        CueCountRole,      // how many cues the write would leave on the track
        CuesAddedRole,     // how many of those are new
        CueSummaryRole,    // every offered cue on a line, for the badge's tooltip
        FillsAGapRole,     // the track has no cues at all today
        ConflictRole,      // the track has cues and they differ
        CuesOfferedRole,   // and whether the merge rule then chose the stored set
        StoredIdRole,      // the store row this came from; unique, unlike a filename
        RatingRole,        // the rating this restore would write, -1 for none
        CommentRole,       // the comment it would write, empty for none
        StoredFromRole,    // the stick this copy was last backed up from
        StagedRole,
    };

    explicit RestoreProposalListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setProposals(std::vector<domain::MetadataRestoreProposal> proposals);
    const std::vector<domain::MetadataRestoreProposal> &proposals() const { return m_proposals; }
    void setStaged(int index, bool staged);
    void removeAt(int index);
    int indexOfStoredId(const std::string &storedId) const;

    bool isStaged(int index) const;

    // ---- the search --------------------------------------------------
    // Filtered here rather than in the delegate. A ListView still lays
    // out, spaces and counts a delegate that has hidden itself, so a
    // search matching three of six hundred rows left hundreds of blank
    // gaps to scroll through and a count that disagreed with the list.
    //
    // Every index above is an index into the full proposal list, not a
    // row number: the filter must not renumber the things staging and
    // undo hold on to. sourceIndexOfRow() is the one place the two
    // numbering schemes meet.
    void setFilter(const QString &text);
    int sourceIndexOfRow(int row) const;
    int rowOfSourceIndex(int sourceIndex) const;
    int totalCount() const { return static_cast<int>(m_proposals.size()); }

private:
    void rebuildVisible();

    std::vector<domain::MetadataRestoreProposal> m_proposals;
    std::vector<bool> m_staged;  // parallel to m_proposals
    // Indices into m_proposals, in order, for the rows this model shows.
    std::vector<int> m_visible;
    QString m_filter;
};

struct MetadataRestoreTaskResult
{
    std::vector<domain::MetadataRestoreProposal> proposals;
    int stickTrackCount = 0;
    int storedTrackCount = 0;
    int conflictCount = 0;  // tracks whose cues differ, whichever side the merge rule then chose
    int conflictsLeftAlone = 0;  // and how many of those the stick kept
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
    // Proposals offering a comment that DeviceLibrary alone cannot
    // store. Not a failure and not hidden: the page says so before the
    // save rather than the log saying so after it.
    Q_PROPERTY(int commentsRekordboxCannotTake READ commentsRekordboxCannotTake NOTIFY analysisChanged)
    Q_PROPERTY(int proposalCount READ proposalCount NOTIFY analysisChanged)
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
    int stagedCount() const { return static_cast<int>(m_stagedByStoredId.size()); }
    int commentsRekordboxCannotTake() const { return m_commentsRekordboxCannotTake; }
    int proposalCount() const { return static_cast<int>(m_model.proposals().size()); }
    bool allStaged() const { return proposalCount() > 0 && stagedCount() == proposalCount(); }
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
    // Ticking a row stages it, and that is the whole of the selection
    // model on this page. There was briefly a selection parallel to
    // staging, with a button to turn one into the other; it meant two
    // ways to say the same thing and a button whose tooltip had to
    // explain which of them it did. One page, one verb, one button: the
    // standard floating Save, labelled Restore.
    Q_INVOKABLE void stage(int row);
    Q_INVOKABLE void unstage(int row);
    Q_INVOKABLE void search(const QString &text);
    // Every proposal, not every visible one: a search narrows what you
    // are looking at, and must not silently narrow what a button called
    // "Select All" acts on, because the difference is invisible the
    // moment the search is cleared.
    Q_INVOKABLE void stageAll();
    Q_INVOKABLE void unstageAll();
    // Both row-taking entry points above funnel here, so a bulk unstage
    // cannot drift from what one row's button does.
    void unstageAt(int index);
    // stage(), plus what the save is expected to write in total -- see
    // RestoreMetadataChange's own itemCountHint.
    void stageOne(int index, int itemCountHint);

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

    QFutureWatcher<MetadataRestoreTaskResult> m_watcher;
    RestoreProposalListModel m_model;
    QPointer<LibraryEditSession> m_session;
    application::CancellationToken m_cancel;
    // One proposal can stage several changes, one per catalog that lists
    // the track, so this maps a proposal to all of them.
    std::map<std::string, QStringList> m_stagedByStoredId;

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
    QString m_currentPhase;
    QString m_errorMessage;
};

}  // namespace seabass::gui
