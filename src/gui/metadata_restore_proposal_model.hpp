// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>
#include <QStringList>

#include <map>
#include <string>
#include <vector>

#include "domain/metadata_restore.hpp"

namespace seabass::gui
{

// One sentence for a person: what restoring this proposal writes to the
// track on the stick, in which catalogs, and -- when the same stored track
// matches more than one track there (copiesOnStick > 1) -- that the others
// get it too, each on its own row.
QString restoreSummaryFor(const domain::MetadataRestoreProposal &proposal, int copiesOnStick);

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
        StoredIdRole,      // the store row this came from; shared by every stick track that matched it
        RatingRole,        // the rating this restore would write, -1 for none
        CommentRole,       // the comment it would write, empty for none
        StoredFromRole,    // the stick this copy was last backed up from
        ArtworkUrlRole,    // the cover the store copied, as a file:// URL
        RestoreSummaryRole,  // what a restore writes to this track, and to how many tracks
        StagedRole,
    };

    explicit RestoreProposalListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setProposals(std::vector<domain::MetadataRestoreProposal> proposals);
    const std::vector<domain::MetadataRestoreProposal> &proposals() const { return m_proposals; }
    // Staging is kept per proposal -- as the change ids it staged, one per
    // catalog that lists the track -- and not per stored track. One stored
    // track can match several tracks on the stick, each its own proposal,
    // and keyed by the stored id Select All staged only the first of them
    // while Select None cleared the second's entry and then found nothing
    // left to clear for the first: about every second row stayed ticked on
    // a stick with its files duplicated.
    void setStagedChanges(int index, QStringList changeIds);
    QStringList stagedChanges(int index) const;
    bool isStaged(int index) const;
    int stagedCount() const;        // rows with anything staged
    int stagedChangeCount() const;  // changes staged across all rows
    // The proposal a staged change belongs to, or -1.
    int indexOfChange(const QString &changeId) const;
    void removeAt(int index);

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
    void recountCopies();

    std::vector<domain::MetadataRestoreProposal> m_proposals;
    std::vector<QStringList> m_stagedChanges;  // parallel to m_proposals; empty = not staged
    // How many proposals each stored track has, for the row that says a
    // restore also goes to other tracks on the stick.
    std::map<std::string, int> m_copiesByStoredId;
    // Indices into m_proposals, in order, for the rows this model shows.
    std::vector<int> m_visible;
    QString m_filter;
};

}  // namespace seabass::gui
