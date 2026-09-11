#include "metadata_backup_proposal_model.hpp"

#include <QStringList>

#include <algorithm>

#include "gui/local_file_url.hpp"
#include "gui/metadata_row_text.hpp"

namespace seabass::gui
{

using domain::MetadataBackupProposal;

namespace
{

// What this row would change, in the fewest words that stay true.
//
// A count, not a verb: the row already carries a tick box that says what
// pressing it does, and repeating "back up" on every line of a list of
// things to back up is noise. What varies between rows -- and what
// decides whether you tick this one -- is what is actually at stake on
// it.
QString changeSummaryOf(const MetadataBackupProposal &proposal)
{
    if (proposal.isNew) {
        return QStringLiteral("new");
    }
    QStringList parts;
    if (proposal.cuesOffered) {
        const int added = proposal.cuesAdded();
        if (added > 0) {
            parts << QStringLiteral("+%1 %2").arg(added).arg(added == 1 ? QStringLiteral("cue")
                                                                        : QStringLiteral("cues"));
        } else {
            // Offered but adding nothing means the same number of cues
            // in different places, which is a replacement and has to say
            // so: "+0 cues" reads as "nothing happens".
            parts << QStringLiteral("replaces %1").arg(proposal.storedCueCount);
        }
    }
    if (proposal.ratingOffered) {
        parts << QStringLiteral("rating");
    }
    if (proposal.commentOffered) {
        parts << QStringLiteral("comment");
    }
    return parts.join(QStringLiteral(", "));
}

}  // namespace

BackupProposalListModel::BackupProposalListModel(QObject *parent) : QAbstractListModel(parent) {}

int BackupProposalListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_visible.size());
}

QHash<int, QByteArray> BackupProposalListModel::roleNames() const
{
    return {
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {FilenameRole, "filename"},
        {RelativePathRole, "relativePath"},
        {DurationTextRole, "durationText"},
        {ArtworkUrlRole, "artworkUrl"},
        {RatingRole, "rating"},
        {CommentRole, "comment"},
        {CueCountRole, "cueCount"},
        {CuesAddedRole, "cuesAdded"},
        {CueSummaryRole, "cueSummary"},
        {IsNewRole, "isNew"},
        {CuesConflictRole, "cuesConflict"},
        {CuesOfferedRole, "cuesOffered"},
        {StoredCueCountRole, "storedCueCount"},
        {RatingOfferedRole, "ratingOffered"},
        {CommentOfferedRole, "commentOffered"},
        {StoredFromRole, "storedFrom"},
        {ChangeSummaryRole, "changeSummary"},
        {StagedRole, "staged"},
    };
}

QVariant BackupProposalListModel::data(const QModelIndex &index, int role) const
{
    const int source = sourceIndexOfRow(index.row());
    if (source < 0) {
        return {};
    }
    const auto &proposal = m_proposals[static_cast<std::size_t>(source)];
    const auto &track = proposal.stickTrack;
    switch (role) {
    case TitleRole:
        return QString::fromStdString(track.title);
    case ArtistRole:
        return QString::fromStdString(track.artist);
    case FilenameRole:
        return QString::fromStdString(track.filename);
    case RelativePathRole:
        // The stick's own path, which is absolute. The row this fills is
        // labelled "File" rather than anything promising otherwise.
        return QString::fromStdString(track.filePath);
    case DurationTextRole:
        return metadataDurationText(track.durationSeconds);
    case ArtworkUrlRole:
        // The stick's cover, because the stick is the side being read
        // here -- the mirror of the restore list, which shows the
        // store's.
        return toLocalFileUrl(track.artworkPath);
    case RatingRole:
        // -1 for unrated, which is a different fact from zero stars.
        return track.rating ? *track.rating : -1;
    case CommentRole:
        return QString::fromStdString(track.comment);
    case CueCountRole:
        return static_cast<int>(track.cues.size());
    case CuesAddedRole:
        return proposal.cuesAdded();
    case CueSummaryRole:
        return metadataCueSummary(track.cues);
    case IsNewRole:
        return proposal.isNew;
    case CuesConflictRole:
        return proposal.cuesConflict;
    case CuesOfferedRole:
        return proposal.cuesOffered;
    case StoredCueCountRole:
        return proposal.storedCueCount;
    case RatingOfferedRole:
        return proposal.ratingOffered;
    case CommentOfferedRole:
        return proposal.commentOffered;
    case StoredFromRole:
        return QString::fromStdString(proposal.storedFrom);
    case ChangeSummaryRole:
        return changeSummaryOf(proposal);
    case StagedRole:
        return m_staged[static_cast<std::size_t>(source)];
    default:
        return {};
    }
}

void BackupProposalListModel::setProposals(std::vector<MetadataBackupProposal> proposals)
{
    beginResetModel();
    m_proposals = std::move(proposals);
    // Staging goes with the old plan. These proposals were computed
    // against one stick at one moment; carrying a tick across a rescan
    // would stage a decision made about numbers that have since changed.
    m_staged.assign(m_proposals.size(), false);
    rebuildVisible();
    endResetModel();
}

void BackupProposalListModel::clear()
{
    setProposals({});
}

// ---- staging --------------------------------------------------------

bool BackupProposalListModel::isStaged(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_staged.size())) {
        return false;
    }
    return m_staged[static_cast<std::size_t>(index)];
}

void BackupProposalListModel::setStaged(int index, bool staged)
{
    if (index < 0 || index >= static_cast<int>(m_staged.size())) {
        return;
    }
    if (m_staged[static_cast<std::size_t>(index)] == staged) {
        return;
    }
    m_staged[static_cast<std::size_t>(index)] = staged;
    // Only the row actually showing this proposal, and only if one is:
    // a staged row can be filtered out from under the tick that staged
    // it, which is allowed -- what is not allowed is emitting
    // dataChanged for a row number that is now somebody else's.
    const auto found = std::find(m_visible.begin(), m_visible.end(), index);
    if (found != m_visible.end()) {
        const int row = static_cast<int>(std::distance(m_visible.begin(), found));
        emit dataChanged(this->index(row), this->index(row), {StagedRole});
    }
}

void BackupProposalListModel::stageAll()
{
    if (m_proposals.empty()) {
        return;
    }
    // Every proposal, not every visible one. A search narrows what you
    // are looking at and must not silently narrow what a button called
    // "Select All" acts on, because the difference is invisible the
    // moment the search is cleared. The tooltip says so.
    m_staged.assign(m_proposals.size(), true);
    if (!m_visible.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_visible.size()) - 1), {StagedRole});
    }
}

void BackupProposalListModel::unstageAll()
{
    if (m_proposals.empty()) {
        return;
    }
    m_staged.assign(m_proposals.size(), false);
    if (!m_visible.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_visible.size()) - 1), {StagedRole});
    }
}

int BackupProposalListModel::stagedCount() const
{
    return static_cast<int>(std::count(m_staged.begin(), m_staged.end(), true));
}

std::vector<domain::Track> BackupProposalListModel::stagedTracks() const
{
    std::vector<domain::Track> tracks;
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (m_staged[i]) {
            tracks.push_back(m_proposals[i].stickTrack);
        }
    }
    return tracks;
}

QStringList BackupProposalListModel::stagedDescriptions() const
{
    QStringList lines;
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (!m_staged[i]) {
            continue;
        }
        const auto &track = m_proposals[i].stickTrack;
        QString name = QString::fromStdString(track.title.empty() ? track.filename : track.title);
        if (!track.artist.empty()) {
            name = QString::fromStdString(track.artist) + QStringLiteral(" - ") + name;
        }
        lines << QStringLiteral("Back up %1 (%2)").arg(name, changeSummaryOf(m_proposals[i]));
    }
    return lines;
}

// ---- the filter -----------------------------------------------------

void BackupProposalListModel::setFilter(const QString &search, const QString &playlist)
{
    if (m_search == search && m_playlist == playlist) {
        return;
    }
    m_search = search;
    m_playlist = playlist;
    beginResetModel();
    rebuildVisible();
    endResetModel();
}

bool BackupProposalListModel::matchesFilter(const MetadataBackupProposal &proposal) const
{
    const auto &track = proposal.stickTrack;
    if (!m_playlist.isEmpty()) {
        const std::string wanted = m_playlist.toStdString();
        const bool inPlaylist = std::any_of(track.playlists.begin(), track.playlists.end(),
                                             [&wanted](const domain::PlaylistMembership &member) {
                                                 return member.name == wanted;
                                             });
        if (!inPlaylist) {
            return false;
        }
    }
    if (m_search.isEmpty()) {
        return true;
    }
    const auto contains = [this](const std::string &haystack) {
        return QString::fromStdString(haystack).contains(m_search, Qt::CaseInsensitive);
    };
    return contains(track.title) || contains(track.artist) || contains(track.filename);
}

void BackupProposalListModel::rebuildVisible()
{
    m_visible.clear();
    m_visible.reserve(m_proposals.size());
    for (std::size_t i = 0; i < m_proposals.size(); ++i) {
        if (matchesFilter(m_proposals[i])) {
            m_visible.push_back(static_cast<int>(i));
        }
    }
}

int BackupProposalListModel::sourceIndexOfRow(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_visible.size())) {
        return -1;
    }
    return m_visible[static_cast<std::size_t>(row)];
}

}  // namespace seabass::gui
