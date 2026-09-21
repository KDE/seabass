// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/metadata_restore_proposal_model.hpp"

#include <algorithm>

#include "gui/local_file_url.hpp"
#include "gui/metadata_row_text.hpp"

namespace seabass::gui
{

using domain::MetadataRestoreProposal;

namespace
{

QString catalogName(const std::string &format)
{
    if (format == "rekordbox") {
        return QStringLiteral("DeviceLibrary");
    }
    if (format == "onelibrary") {
        return QStringLiteral("OneLibrary");
    }
    if (format == "engine") {
        return QStringLiteral("Engine");
    }
    return QString::fromStdString(format);
}

// "a", "a and b", "a, b and c".
QString spokenList(const QStringList &items)
{
    if (items.size() <= 1) {
        return items.join(QString());
    }
    return items.mid(0, items.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") + items.last();
}

}  // namespace

QString restoreSummaryFor(const MetadataRestoreProposal &proposal, int copiesOnStick)
{
    // Staging writes one change per catalog row; with none there is
    // nothing it can write, whatever the store has.
    if (proposal.stickTrack.catalogRows.empty()) {
        return QStringLiteral("Nothing can be restored to this track: no catalog on the stick lists it.");
    }
    QStringList catalogs;
    // A comment needs a catalog that can grow one; export.pdb cannot.
    QStringList commentCatalogs;
    for (const auto &row : proposal.stickTrack.catalogRows) {
        const QString name = catalogName(row.format);
        if (!catalogs.contains(name)) {
            catalogs << name;
        }
        if (row.format != "rekordbox" && !commentCatalogs.contains(name)) {
            commentCatalogs << name;
        }
    }
    const bool commentHasSomewhereToGo = !commentCatalogs.isEmpty();

    QStringList what;
    if (proposal.cuesOffered) {
        const int count = static_cast<int>(proposal.cues.size());
        const QString cues = count == 1 ? QStringLiteral("1 cue") : QStringLiteral("%1 cues").arg(count);
        if (proposal.cuesConflict) {
            const int current = static_cast<int>(proposal.stickTrack.cues.size());
            what << QStringLiteral("%1, replacing the %2 the track has now").arg(cues).arg(current);
        } else {
            what << cues;
        }
    }
    if (proposal.ratingOffered && proposal.rating) {
        what << (*proposal.rating == 0   ? QStringLiteral("its zero-star rating")
                 : *proposal.rating == 1 ? QStringLiteral("its 1-star rating")
                                         : QStringLiteral("its %1-star rating").arg(*proposal.rating));
    }
    if (proposal.commentOffered && commentHasSomewhereToGo) {
        what << QStringLiteral("its comment");
    }

    QString text;
    if (what.isEmpty()) {
        text = QStringLiteral("Restoring writes nothing to this track.");
    } else {
        text = QStringLiteral("Restoring writes the stored %1 to this track on the stick").arg(spokenList(what));
        text += catalogs.isEmpty() ? QStringLiteral(".") : QStringLiteral(", in %1.").arg(spokenList(catalogs));
    }
    if (proposal.commentOffered && commentHasSomewhereToGo && catalogs.contains(QStringLiteral("DeviceLibrary"))) {
        text += QStringLiteral(" The comment goes only to %1: DeviceLibrary has no room for one.")
                    .arg(spokenList(commentCatalogs));
    }
    if (proposal.commentOffered && !commentHasSomewhereToGo) {
        text += QStringLiteral(" Its comment stays behind: DeviceLibrary has no room to add one.");
    }
    if (copiesOnStick > 1) {
        const int others = copiesOnStick - 1;
        text += others == 1
            ? QStringLiteral(" The same stored track also matches 1 other track on this stick, which has its own row.")
            : QStringLiteral(" The same stored track also matches %1 other tracks on this stick, each with its own row.")
                  .arg(others);
    }
    return text;
}

// ---- model ----------------------------------------------------------

RestoreProposalListModel::RestoreProposalListModel(QObject *parent) : QAbstractListModel(parent) {}

int RestoreProposalListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}

QHash<int, QByteArray> RestoreProposalListModel::roleNames() const
{
    return {
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {FilenameRole, "filename"},
        {RelativePathRole, "relativePath"},
        {DurationTextRole, "durationText"},
        {CueCountRole, "cueCount"},
        {CuesAddedRole, "cuesAdded"},
        {CueSummaryRole, "cueSummary"},
        {FillsAGapRole, "fillsAGap"},
        {ConflictRole, "conflict"},
        {CuesOfferedRole, "cuesOffered"},
        {StoredIdRole, "storedId"},
        {RatingRole, "rating"},
        {CommentRole, "comment"},
        {StoredFromRole, "storedFrom"},
        {ArtworkUrlRole, "artworkUrl"},
        {RestoreSummaryRole, "restoreSummary"},
        {StagedRole, "staged"},
    };
}

QVariant RestoreProposalListModel::data(const QModelIndex &index, int role) const
{
    const int source = sourceIndexOfRow(index.row());
    if (source < 0) {
        return {};
    }
    const auto &proposal = m_proposals[static_cast<std::size_t>(source)];
    switch (role) {
    case TitleRole:
        return QString::fromStdString(proposal.stickTrack.title);
    case ArtistRole:
        return QString::fromStdString(proposal.stickTrack.artist);
    case FilenameRole:
        return QString::fromStdString(proposal.stickTrack.filename);
    case RelativePathRole:
        // The stick's own path, which is absolute, and the only one a
        // proposal has: the stored side deliberately carries no path at
        // all (MetadataStore::readAll). The row this fills is labelled
        // "File" rather than anything promising a relative one.
        return QString::fromStdString(proposal.stickTrack.filePath);
    case DurationTextRole:
        return metadataDurationText(proposal.stickTrack.durationSeconds);
    case CueSummaryRole:
        return metadataCueSummary(proposal.cues);
    case StoredFromRole:
        return QString::fromStdString(proposal.storedFrom);
    case ArtworkUrlRole:
        // The store's own copy of the cover, not the stick's. On the
        // stick this page is aimed at -- one that has lost its metadata
        // -- the stick's copy is exactly what is missing.
        return toLocalFileUrl(proposal.artworkPath);
    case CueCountRole:
        return static_cast<int>(proposal.cues.size());
    case CuesAddedRole:
        return proposal.cuesAdded();
    case FillsAGapRole:
        return proposal.cuesFillAGap;
    case ConflictRole:
        return proposal.cuesConflict;
    case CuesOfferedRole:
        return proposal.cuesOffered;
    case StoredIdRole:
        return QString::fromStdString(proposal.storedId);
    case RatingRole:
        // -1 when this restore offers no rating, so a row can tell "no
        // rating on offer" from "zero stars on offer".
        return proposal.ratingOffered && proposal.rating ? *proposal.rating : -1;
    case CommentRole:
        return proposal.commentOffered ? QString::fromStdString(proposal.comment) : QString();
    case RestoreSummaryRole: {
        const auto copies = m_copiesByStoredId.find(proposal.storedId);
        return restoreSummaryFor(proposal, copies == m_copiesByStoredId.end() ? 1 : copies->second);
    }
    case StagedRole:
        return !m_stagedChanges[static_cast<std::size_t>(source)].isEmpty();
    default:
        return {};
    }
}

void RestoreProposalListModel::setProposals(std::vector<MetadataRestoreProposal> proposals)
{
    beginResetModel();
    m_proposals = std::move(proposals);
    m_stagedChanges.assign(m_proposals.size(), QStringList());
    recountCopies();
    rebuildVisible();
    endResetModel();
}

void RestoreProposalListModel::rebuildVisible()
{
    m_visible.clear();
    m_visible.reserve(m_proposals.size());
    const QString needle = m_filter.trimmed().toLower();
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (needle.isEmpty()) {
            m_visible.push_back(static_cast<int>(i));
            continue;
        }
        const auto &track = m_proposals[i].stickTrack;
        // The same three fields the browse list searches, so one search
        // term means the same thing on both pages.
        const QString haystack = (QString::fromStdString(track.title) + QLatin1Char('\n')
                                  + QString::fromStdString(track.artist) + QLatin1Char('\n')
                                  + QString::fromStdString(track.filename))
                                     .toLower();
        if (haystack.contains(needle)) {
            m_visible.push_back(static_cast<int>(i));
        }
    }
}

void RestoreProposalListModel::setFilter(const QString &text)
{
    if (m_filter == text) {
        return;
    }
    beginResetModel();
    m_filter = text;
    rebuildVisible();
    endResetModel();
}

int RestoreProposalListModel::sourceIndexOfRow(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_visible.size())) {
        return -1;
    }
    return m_visible[static_cast<std::size_t>(row)];
}

int RestoreProposalListModel::rowOfSourceIndex(int sourceIndex) const
{
    for (std::size_t row = 0; row < m_visible.size(); ++row) {
        if (m_visible[row] == sourceIndex) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

void RestoreProposalListModel::setStagedChanges(int index, QStringList changeIds)
{
    if (index < 0 || index >= static_cast<int>(m_proposals.size())) {
        return;
    }
    m_stagedChanges[static_cast<std::size_t>(index)] = std::move(changeIds);
    // Nothing to repaint when the row is filtered out, but the state
    // still has to be kept: the search is a view of the list, not a
    // different list.
    const int row = rowOfSourceIndex(index);
    if (row >= 0) {
        emit dataChanged(this->index(row), this->index(row), {StagedRole});
    }
}

QStringList RestoreProposalListModel::stagedChanges(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_stagedChanges.size())) {
        return {};
    }
    return m_stagedChanges[static_cast<std::size_t>(index)];
}

int RestoreProposalListModel::stagedCount() const
{
    return static_cast<int>(std::count_if(m_stagedChanges.begin(), m_stagedChanges.end(),
                                          [](const QStringList &changes) { return !changes.isEmpty(); }));
}

int RestoreProposalListModel::stagedChangeCount() const
{
    // Rows, not changes, is what stagedCount() answers. One proposal
    // becomes one change per catalog that lists the file, so a stick
    // whose tracks are in both rekordbox and Engine stages two changes
    // per row -- and comparing a row count against a change count then
    // looks like a bug in whichever one you trusted less.
    int total = 0;
    for (const QStringList &changes : m_stagedChanges) {
        total += static_cast<int>(changes.size());
    }
    return total;
}

int RestoreProposalListModel::indexOfChange(const QString &changeId) const
{
    for (std::size_t i = 0; i < m_stagedChanges.size(); ++i) {
        if (m_stagedChanges[i].contains(changeId)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RestoreProposalListModel::recountCopies()
{
    m_copiesByStoredId.clear();
    for (const auto &proposal : m_proposals) {
        m_copiesByStoredId[proposal.storedId]++;
    }
}

void RestoreProposalListModel::removeAt(int index)
{
    if (index < 0 || index >= static_cast<int>(m_proposals.size())) {
        return;
    }
    // A reset rather than beginRemoveRows: removing one proposal
    // renumbers every visible index after it, and the mapping is what
    // this model is for.
    beginResetModel();
    m_proposals.erase(m_proposals.begin() + index);
    m_stagedChanges.erase(m_stagedChanges.begin() + index);
    recountCopies();
    rebuildVisible();
    endResetModel();
}

bool RestoreProposalListModel::isStaged(int index) const
{
    return !stagedChanges(index).isEmpty();
}


}  // namespace seabass::gui
