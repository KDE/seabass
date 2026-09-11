#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>

#include <string>
#include <vector>

#include "domain/metadata_backup_plan.hpp"

namespace seabass::gui
{

// What one planned backup run would change on the stick in front of you,
// one row per track, ticked to stage it.
//
// The counterpart to RestoreProposalListModel and deliberately the same
// shape: proposals in one vector, staging parallel to it, and a filter
// that narrows which rows are visible without renumbering anything the
// staging holds on to. The restore page learned that the hard way -- a
// search that renumbers rows moves a tick from the track it was made on
// to whichever track slid into that row number.
class BackupProposalListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by MetadataBackupController; not constructible from QML")

public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        FilenameRole,
        RelativePathRole,
        DurationTextRole,
        ArtworkUrlRole,
        RatingRole,       // -1 when unrated, so QML can tell it from 0 stars
        CommentRole,
        CueCountRole,     // cues the stick has, which is what a store would write
        CuesAddedRole,    // how many of them the store does not already hold
        CueSummaryRole,   // every cue on a line, for the badge's tooltip
        IsNewRole,        // the store has never seen this track
        CuesConflictRole, // both sides have cues and they differ
        CuesOfferedRole,  // ...and whether the merge rule then chose the stick's
        StoredCueCountRole,
        RatingOfferedRole,
        CommentOfferedRole,
        StoredFromRole,   // the stick this track's stored copy last came from
        ChangeSummaryRole,  // "new", "4 cues", "rating" -- what this row would change
        StagedRole,
    };

    explicit BackupProposalListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setProposals(std::vector<domain::MetadataBackupProposal> proposals);
    const std::vector<domain::MetadataBackupProposal> &proposals() const { return m_proposals; }
    void clear();

    // ---- staging ------------------------------------------------------
    // Indices below are indices into the full proposal list, never row
    // numbers: the filter must not renumber what staging holds on to.
    // sourceIndexOfRow() is the one place the two schemes meet.
    void setStaged(int index, bool staged);
    bool isStaged(int index) const;
    void stageAll();
    void unstageAll();
    int stagedCount() const;
    // The stick tracks of every staged proposal, which is exactly what
    // MetadataStore::store() takes.
    std::vector<domain::Track> stagedTracks() const;
    // One line per staged row, for the save button's tooltip.
    QStringList stagedDescriptions() const;

    // ---- the filter ---------------------------------------------------
    // Search text and a playlist, applied together. Filtered here rather
    // than in the delegate: a ListView still lays out, spaces and counts
    // a delegate that has hidden itself, so a search matching three of
    // six hundred rows leaves hundreds of blank gaps to scroll past and
    // a count that disagrees with the list.
    void setFilter(const QString &search, const QString &playlist);
    int sourceIndexOfRow(int row) const;
    int totalCount() const { return static_cast<int>(m_proposals.size()); }

private:
    void rebuildVisible();
    bool matchesFilter(const domain::MetadataBackupProposal &proposal) const;

    std::vector<domain::MetadataBackupProposal> m_proposals;
    std::vector<bool> m_staged;  // parallel to m_proposals
    // Indices into m_proposals, in order, for the rows this model shows.
    std::vector<int> m_visible;
    QString m_search;
    QString m_playlist;
};

}  // namespace seabass::gui
