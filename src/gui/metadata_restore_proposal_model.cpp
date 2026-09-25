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
        {CuesRole, "cues"},
        {DurationMsRole, "durationMs"},
        {PlaylistNamesRole, "playlistNames"},
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
    case CuesRole:
        return metadataCueList(proposal.cuesOffered ? proposal.cues : proposal.stickTrack.cues);
    case DurationMsRole:
        return proposal.stickTrack.durationSeconds * 1000.0;
    case PlaylistNamesRole: {
        QStringList names;
        for (const auto &name : domain::restorePlaylistsOf(proposal)) {
            names << QString::fromStdString(name);
        }
        return names.join(QStringLiteral(", "));
    }
    default:
        return {};
    }
}

void RestoreProposalListModel::setProposals(std::vector<MetadataRestoreProposal> proposals)
{
    beginResetModel();
    m_proposals = std::move(proposals);
    m_stagedChanges.assign(m_proposals.size(), QStringList());
    m_stagedCount = 0;
    m_stagedChangeCount = 0;
    recountCopies();
    rebuildVisible();
    endResetModel();
}

void RestoreProposalListModel::rebuildVisible()
{
    m_visible.clear();
    m_visible.reserve(m_proposals.size());
    m_scopedCount = 0;
    const QString needle = m_filter.trimmed().toLower();
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (!domain::proposalInRestoreScope(m_proposals[i], m_scope)) {
            continue;
        }
        m_scopedCount++;
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

void RestoreProposalListModel::setScope(domain::MetadataRestoreScope scope)
{
    if (scope.sourceKey == m_scope.sourceKey && scope.playlist == m_scope.playlist) {
        return;
    }
    beginResetModel();
    m_scope = std::move(scope);
    rebuildVisible();
    endResetModel();
}

bool RestoreProposalListModel::inScope(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_proposals.size())) {
        return false;
    }
    return domain::proposalInRestoreScope(m_proposals[static_cast<std::size_t>(index)], m_scope);
}

std::vector<int> RestoreProposalListModel::unstagedInScope() const
{
    std::vector<int> indices;
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (m_stagedChanges[i].isEmpty() && domain::proposalInRestoreScope(m_proposals[i], m_scope)) {
            indices.push_back(static_cast<int>(i));
        }
    }
    return indices;
}

std::vector<int> RestoreProposalListModel::stagedOutsideScope() const
{
    std::vector<int> indices;
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (!m_stagedChanges[i].isEmpty() && !domain::proposalInRestoreScope(m_proposals[i], m_scope)) {
            indices.push_back(static_cast<int>(i));
        }
    }
    return indices;
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
    // m_visible is in proposal order (rebuildVisible), so a search, not a
    // walk: staging a whole list asks this once per row.
    const auto found = std::lower_bound(m_visible.begin(), m_visible.end(), sourceIndex);
    if (found == m_visible.end() || *found != sourceIndex) {
        return -1;
    }
    return static_cast<int>(found - m_visible.begin());
}

void RestoreProposalListModel::setStagedChanges(int index, QStringList changeIds)
{
    if (index < 0 || index >= static_cast<int>(m_proposals.size())) {
        return;
    }
    QStringList &slot = m_stagedChanges[static_cast<std::size_t>(index)];
    m_stagedCount += (changeIds.isEmpty() ? 0 : 1) - (slot.isEmpty() ? 0 : 1);
    m_stagedChangeCount += static_cast<int>(changeIds.size()) - static_cast<int>(slot.size());
    slot = std::move(changeIds);
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
    removeAll({index});
}

void RestoreProposalListModel::removeAll(std::vector<int> indices)
{
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    indices.erase(std::remove_if(indices.begin(), indices.end(),
                                 [this](int index) { return index < 0 || index >= static_cast<int>(m_proposals.size()); }),
                  indices.end());
    if (indices.empty()) {
        return;
    }
    beginResetModel();
    // One pass, keeping what is not being removed, so a thousand removals
    // move each survivor once rather than once per removal before it.
    std::size_t next = 0;  // into indices
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (next < indices.size() && static_cast<std::size_t>(indices[next]) == i) {
            next++;
            const QStringList &gone = m_stagedChanges[i];
            m_stagedCount -= gone.isEmpty() ? 0 : 1;
            m_stagedChangeCount -= static_cast<int>(gone.size());
            continue;
        }
        if (kept != i) {
            m_proposals[kept] = std::move(m_proposals[i]);
            m_stagedChanges[kept] = std::move(m_stagedChanges[i]);
        }
        kept++;
    }
    m_proposals.resize(kept);
    m_stagedChanges.resize(kept);
    recountCopies();
    rebuildVisible();
    endResetModel();
}

bool RestoreProposalListModel::isStaged(int index) const
{
    return !stagedChanges(index).isEmpty();
}


}  // namespace seabass::gui
