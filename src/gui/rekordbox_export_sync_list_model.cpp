// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/rekordbox_export_sync_list_model.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "gui/sync_plan_list_model.hpp"

namespace seabass::gui
{

namespace
{

using domain::CueEdit;
using domain::EngineUpdateEdit;
using domain::MembershipEdit;
using domain::MetadataEdit;
using domain::PlaylistCreate;
using domain::PlaylistDelete;
using domain::PlaylistRename;
using domain::SyncPlan;
using domain::Track;
using domain::TrackToAdd;
using domain::TrackToRemove;
using Section = RekordboxExportSyncListModel::Section;

constexpr std::array<const char *, 10> SectionNames = {
    "playlists",  "tracksToAdd",         "tracksToRemove", "membership",    "metadataToEngine",
    "cuesToEngine", "restoresToRekordbox", "conflicts",      "engineOwnKept", "notAdded",
};

QString q(const std::string &text)
{
    return QString::fromStdString(text);
}

QString quotedText(const std::string &text)
{
    return QLatin1Char('"') + q(text) + QLatin1Char('"');
}

// Where a track's file is on the stick, as the CLI prints it.
QString pathOnStick(const Track &track, const std::string &stickRoot)
{
    const std::string relative = baselineStickRelativePath(stickRoot, track.filePath);
    if (!relative.empty()) {
        return q(relative);
    }
    return track.filePath.empty() ? QStringLiteral("(no file)") : q(track.filePath);
}

// " #3" for the third entry of the playlist, as the reader numbers it;
// empty when the track is not there or the reader could not say.
QString positionIn(const Track &track, const std::string &playlistPath)
{
    for (const auto &membership : track.playlists) {
        if (membership.name == playlistPath && membership.position >= 0) {
            return QStringLiteral(" #%1").arg(membership.position);
        }
    }
    return {};
}

QString stars(const std::optional<int> &rating)
{
    if (!rating || *rating == 0) {
        return QStringLiteral("unrated");
    }
    return *rating == 1 ? QStringLiteral("1 star") : QStringLiteral("%1 stars").arg(*rating);
}

QString directionOf(Section section)
{
    switch (section) {
    case Section::Playlists:
    case Section::TracksToAdd:
    case Section::TracksToRemove:
    case Section::Membership:
    case Section::MetadataToEngine:
    case Section::CuesToEngine:
        return QStringLiteral("to Engine");
    case Section::RestoresToRekordbox:
        return QStringLiteral("back to rekordbox");
    case Section::EngineOwnKept:
        return QStringLiteral("kept");
    case Section::Conflicts:
    case Section::NotAdded:
        break;
    }
    return {};
}

const domain::EngineUpdateItemHeader &headerOf(const EngineUpdateEdit &edit)
{
    return std::visit([](const auto &e) -> const domain::EngineUpdateItemHeader & { return e.header; }, edit);
}

// The track a conflict is about, from whichever of its choices names one.
std::optional<Track> trackOfChoice(const std::vector<EngineUpdateEdit> &choice)
{
    for (const auto &edit : choice) {
        if (const auto *add = std::get_if<TrackToAdd>(&edit)) {
            return add->rekordbox;
        }
        if (const auto *remove = std::get_if<TrackToRemove>(&edit)) {
            return remove->engine;
        }
        if (const auto *member = std::get_if<MembershipEdit>(&edit)) {
            return member->track;
        }
        if (const auto *metadata = std::get_if<MetadataEdit>(&edit)) {
            return metadata->direction == MetadataEdit::Direction::ToEngine ? metadata->rekordbox : metadata->engine;
        }
        if (const auto *cues = std::get_if<CueEdit>(&edit)) {
            return cues->plan.match.trackA;
        }
    }
    return std::nullopt;
}

// The playlist path a conflict is about, from whichever choice names one.
std::string playlistOfChoice(const std::vector<EngineUpdateEdit> &choice)
{
    for (const auto &edit : choice) {
        if (const auto *create = std::get_if<PlaylistCreate>(&edit)) {
            return create->path;
        }
        if (const auto *rename = std::get_if<PlaylistRename>(&edit)) {
            return rename->toPath;
        }
        if (const auto *del = std::get_if<PlaylistDelete>(&edit)) {
            return del->path;
        }
        if (const auto *member = std::get_if<MembershipEdit>(&edit)) {
            return member->playlistPath;
        }
    }
    return {};
}

}  // namespace

RekordboxExportSyncListModel::RekordboxExportSyncListModel(QObject *parent) : QAbstractListModel(parent) {}

QString RekordboxExportSyncListModel::sectionName(Section section)
{
    return QString::fromLatin1(SectionNames[static_cast<std::size_t>(section)]);
}

std::optional<RekordboxExportSyncListModel::Section> RekordboxExportSyncListModel::sectionFromName(const QString &name)
{
    for (std::size_t i = 0; i < SectionNames.size(); ++i) {
        if (name == QLatin1String(SectionNames[i])) {
            return static_cast<Section>(i);
        }
    }
    return std::nullopt;
}

bool RekordboxExportSyncListModel::writable(Section section)
{
    return static_cast<int>(section) <= static_cast<int>(Section::RestoresToRekordbox);
}

int RekordboxExportSyncListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant RekordboxExportSyncListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const Row &row = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case SectionRole:
        return sectionName(row.section);
    case SectionIndexRole:
        return static_cast<int>(row.section);
    case KindRole:
        return row.kind;
    case KeyRole:
        return q(row.header.key);
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case DetailRole:
        return row.detail;
    case ReasonRole:
        return q(row.header.reasonText);
    case IsConflictRole:
        return row.section == Section::Conflicts;
    case RekordboxChoiceLabelRole:
        return row.conflict ? q(row.conflict->rekordboxSide) : QString();
    case EngineChoiceLabelRole:
        return row.conflict ? q(row.conflict->engineSide) : QString();
    case ResolvedSideRole:
        if (row.fromConflictUid >= 0) {
            const int at = rowIndexOfUid(row.fromConflictUid);
            return at >= 0 ? m_rows[static_cast<std::size_t>(at)].resolvedSide : QString();
        }
        return row.resolvedSide;
    case IncludedRole:
        return row.included;
    case DependsOnRole: {
        QStringList keys;
        for (const auto &key : row.header.dependsOn) {
            keys << q(key);
        }
        return keys;
    }
    case StagedRole:
        return row.staged;
    case StagedDescriptionRole:
        return row.stagedDescription;
    case DirectionRole:
        if (row.section == Section::Conflicts) {
            if (row.resolvedSide == QLatin1String("rekordbox")) {
                return QStringLiteral("to Engine");
            }
            if (row.resolvedSide == QLatin1String("engine")) {
                return QStringLiteral("kept");
            }
            return QString();
        }
        return directionOf(row.section);
    case FromConflictRole:
        return row.fromConflictUid >= 0;
    default:
        return {};
    }
}

QHash<int, QByteArray> RekordboxExportSyncListModel::roleNames() const
{
    return {
        {SectionRole, "section"},
        {SectionIndexRole, "sectionIndex"},
        {KindRole, "kind"},
        {KeyRole, "key"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {DetailRole, "detail"},
        {ReasonRole, "reason"},
        {IsConflictRole, "isConflict"},
        {RekordboxChoiceLabelRole, "rekordboxChoiceLabel"},
        {EngineChoiceLabelRole, "engineChoiceLabel"},
        {ResolvedSideRole, "resolvedSide"},
        {IncludedRole, "included"},
        {DependsOnRole, "dependsOn"},
        {StagedRole, "staged"},
        {StagedDescriptionRole, "stagedDescription"},
        {DirectionRole, "direction"},
        {FromConflictRole, "fromConflict"},
    };
}

RekordboxExportSyncListModel::Row RekordboxExportSyncListModel::makeRow(Section section, QString kind,
                                                                         const domain::EngineUpdateItemHeader &header)
{
    Row row;
    row.section = section;
    row.kind = std::move(kind);
    row.header = header;
    row.included = writable(section) && header.checkedByDefault && !header.conflict;
    row.uid = m_nextUid++;
    return row;
}

// The row an edit is, in its section, with the CLI's wording for where it
// goes. Not yet numbered (uid) or keyed (planKey).
RekordboxExportSyncListModel::Row RekordboxExportSyncListModel::rowForEdit(const EngineUpdateEdit &edit) const
{
    Row row;
    row.header = headerOf(edit);
    row.edit = edit;
    const std::string &root = m_stickRoot;
    struct Fill
    {
        Row &row;
        const std::string &root;
        void track(const Track &t)
        {
            row.title = q(t.title.empty() ? t.filename : t.title);
            row.artist = q(t.artist);
        }
        void operator()(const PlaylistCreate &e)
        {
            row.section = Section::Playlists;
            row.kind = e.folder ? QStringLiteral("createFolder") : QStringLiteral("createPlaylist");
            row.title = q(e.path);
            row.detail = QStringLiteral("create %1 %2").arg(e.folder ? QStringLiteral("folder") : QStringLiteral("playlist"),
                                                            quotedText(e.path));
        }
        void operator()(const PlaylistRename &e)
        {
            row.section = Section::Playlists;
            row.kind = QStringLiteral("renamePlaylist");
            row.title = q(e.toPath);
            row.detail = QStringLiteral("rename %1 to %2").arg(quotedText(e.fromPath), quotedText(e.toPath));
        }
        void operator()(const PlaylistDelete &e)
        {
            row.section = Section::Playlists;
            row.kind = e.folder ? QStringLiteral("deleteFolder") : QStringLiteral("deletePlaylist");
            row.title = q(e.path);
            row.detail = QStringLiteral("delete %1 %2 (%3 member(s))")
                             .arg(e.folder ? QStringLiteral("folder") : QStringLiteral("playlist"), quotedText(e.path))
                             .arg(e.engineMembers);
        }
        void operator()(const TrackToAdd &e)
        {
            row.section = Section::TracksToAdd;
            row.kind = QStringLiteral("addTrack");
            track(e.rekordbox);
            row.detail = q(e.stickRelativePath);
        }
        void operator()(const TrackToRemove &e)
        {
            row.section = Section::TracksToRemove;
            row.kind = QStringLiteral("removeTrack");
            track(e.engine);
            row.detail = QStringLiteral("%1, in %2 playlist(s)").arg(pathOnStick(e.engine, root)).arg(e.playlistCount);
        }
        void operator()(const MembershipEdit &e)
        {
            row.section = Section::Membership;
            track(e.track);
            if (e.kind == MembershipEdit::Kind::Remove) {
                row.kind = QStringLiteral("removeMember");
                row.detail = QStringLiteral("take out of %1%2: %3")
                                 .arg(quotedText(e.playlistPath), positionIn(e.track, e.playlistPath),
                                      pathOnStick(e.track, root));
                return;
            }
            row.kind = QStringLiteral("addMember");
            row.detail = QStringLiteral("put into %1%2: %3%4")
                             .arg(quotedText(e.playlistPath), positionIn(e.track, e.playlistPath), pathOnStick(e.track, root),
                                  e.afterPathKey.empty() ? QStringLiteral(", first")
                                                         : QStringLiteral(", after ") + q(e.afterPathKey));
        }
        void operator()(const MetadataEdit &e)
        {
            const bool toEngine = e.direction == MetadataEdit::Direction::ToEngine;
            row.section = toEngine ? Section::MetadataToEngine : Section::RestoresToRekordbox;
            track(toEngine ? e.rekordbox : e.engine);
            if (e.field == MetadataEdit::Field::Rating) {
                row.kind = QStringLiteral("rating");
                row.detail = QStringLiteral("rating: %1").arg(stars(e.rating));
            } else {
                row.kind = QStringLiteral("comment");
                row.detail = e.comment.empty() ? QStringLiteral("comment: none")
                                               : QStringLiteral("comment: ") + quotedText(e.comment);
            }
        }
        void operator()(const CueEdit &e)
        {
            const bool toEngine = e.plan.direction == SyncPlan::Direction::ToB;
            row.section = toEngine ? Section::CuesToEngine : Section::RestoresToRekordbox;
            row.kind = QStringLiteral("cues");
            track(e.plan.match.trackA);
            row.detail = SyncPlanListModel::cueSummary(e.plan);
        }
    };
    std::visit(Fill{row, root}, edit);
    return row;
}

void RekordboxExportSyncListModel::assignPlanKey(Row &row)
{
    const QString prefix = (row.fromConflictUid >= 0 ? QStringLiteral("answer|") : QString()) + sectionName(row.section)
        + QLatin1Char('|') + row.kind + QLatin1Char('|') + q(row.header.key) + QLatin1Char('|');
    row.planKey = prefix + QString::number(m_ordinals[prefix]++);
}

void RekordboxExportSyncListModel::appendEdit(const EngineUpdateEdit &edit)
{
    Row row = rowForEdit(edit);
    row.uid = m_nextUid++;
    row.included = writable(row.section) && row.header.checkedByDefault && !row.header.conflict;
    assignPlanKey(row);
    m_rows.push_back(std::move(row));
}

void RekordboxExportSyncListModel::setProposal(const domain::EngineUpdateProposal &proposal,
                                               const std::string &stickRoot)
{
    beginResetModel();
    m_rows.clear();
    m_ordinals.clear();
    m_stickRoot = stickRoot;
    for (const auto &e : proposal.playlistsToCreate) {
        appendEdit(e);
    }
    for (const auto &e : proposal.playlistsToRename) {
        appendEdit(e);
    }
    for (const auto &e : proposal.playlistsToDelete) {
        appendEdit(e);
    }
    for (const auto &e : proposal.tracksToAdd) {
        appendEdit(e);
    }
    for (const auto &e : proposal.tracksToRemove) {
        appendEdit(e);
    }
    for (const auto &e : proposal.membership) {
        appendEdit(e);
    }
    for (const auto &e : proposal.metadataToEngine) {
        appendEdit(e);
    }
    for (const auto &e : proposal.cuesToEngine) {
        appendEdit(e);
    }
    for (const auto &e : proposal.restoresToRekordbox) {
        appendEdit(e);
    }
    for (const auto &e : proposal.cuesToRekordbox) {
        appendEdit(e);
    }
    for (const auto &c : proposal.conflicts) {
        Row row = makeRow(Section::Conflicts, QStringLiteral("conflict"), c.header);
        row.conflict = c;
        std::optional<Track> track = trackOfChoice(c.rekordboxChoice);
        if (!track) {
            track = trackOfChoice(c.engineChoice);
        }
        std::string playlist = playlistOfChoice(c.rekordboxChoice);
        if (playlist.empty()) {
            playlist = playlistOfChoice(c.engineChoice);
        }
        if (track) {
            row.title = q(track->title.empty() ? track->filename : track->title);
            row.artist = q(track->artist);
            row.detail = pathOnStick(*track, stickRoot);
            if (!playlist.empty()) {
                row.detail += QStringLiteral(" in ") + quotedText(playlist);
            }
        } else if (!playlist.empty()) {
            row.title = q(playlist);
            row.detail = quotedText(playlist);
        } else {
            row.title = q(c.pathKey.empty() ? c.header.key : c.pathKey);
        }
        assignPlanKey(row);
        m_rows.push_back(std::move(row));
    }
    for (const auto &k : proposal.engineOwnKept) {
        Row row = makeRow(Section::EngineOwnKept, QStringLiteral("kept"), k.header);
        if (!k.playlistPath.empty()) {
            row.title = q(k.playlistPath);
            row.detail = quotedText(k.playlistPath);
        } else {
            row.title = q(k.engine.title.empty() ? k.engine.filename : k.engine.title);
            row.artist = q(k.engine.artist);
            row.detail = pathOnStick(k.engine, stickRoot);
        }
        assignPlanKey(row);
        m_rows.push_back(std::move(row));
    }
    for (const auto &a : proposal.notAdded) {
        Row row = rowForEdit(a);
        row.section = Section::NotAdded;
        row.kind = QStringLiteral("notAdded");
        row.uid = m_nextUid++;
        row.included = false;
        assignPlanKey(row);
        m_rows.push_back(std::move(row));
    }
    endResetModel();
    emit countsChanged();
}

void RekordboxExportSyncListModel::clear()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit countsChanged();
}

int RekordboxExportSyncListModel::rowIndexOfUid(int uid) const
{
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].uid == uid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RekordboxExportSyncListModel::announceIncluded()
{
    if (!m_rows.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1),
                         {IncludedRole, ResolvedSideRole, DirectionRole});
    }
    emit countsChanged();
}

bool RekordboxExportSyncListModel::setIncluded(int rowIndex, bool included)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size())) {
        return false;
    }
    const Row &start = m_rows[static_cast<std::size_t>(rowIndex)];
    if (start.section == Section::Conflicts) {
        // Ticked only by an answer; unticking takes the answer back.
        return included ? false : clearResolution(rowIndex);
    }
    if (!writable(start.section)) {
        return false;
    }

    bool changed = false;
    std::vector<std::string> pending{start.header.key};
    std::set<std::string> seen;
    while (!pending.empty()) {
        const std::string key = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(key).second) {
            continue;
        }
        for (auto &row : m_rows) {
            if (!writable(row.section)) {
                continue;
            }
            if (row.header.key == key) {
                // One item: every row of the key goes with it.
                if (row.included != included) {
                    row.included = included;
                    changed = true;
                }
                if (included) {
                    // Ticking brings what it cannot be applied without.
                    pending.insert(pending.end(), row.header.dependsOn.begin(), row.header.dependsOn.end());
                }
            } else if (!included && std::find(row.header.dependsOn.begin(), row.header.dependsOn.end(), key)
                                        != row.header.dependsOn.end()) {
                // Unticking takes what cannot be applied without it.
                pending.push_back(row.header.key);
            }
        }
    }
    if (changed) {
        announceIncluded();
    }
    return changed;
}

void RekordboxExportSyncListModel::setSectionIncluded(Section section, bool included)
{
    if (section == Section::Conflicts) {
        if (included) {
            return;  // a conflict is ticked by its answer, one at a time
        }
        for (int i = static_cast<int>(m_rows.size()) - 1; i >= 0; --i) {
            if (m_rows[static_cast<std::size_t>(i)].section == Section::Conflicts) {
                clearResolution(i);
            }
        }
        return;
    }
    if (!writable(section)) {
        return;
    }
    // By uid: a tick never moves a row, but going by identity costs
    // nothing and survives one that does.
    std::vector<int> uids;
    for (const auto &row : m_rows) {
        if (row.section == section) {
            uids.push_back(row.uid);
        }
    }
    for (int uid : uids) {
        const int at = rowIndexOfUid(uid);
        if (at >= 0 && m_rows[static_cast<std::size_t>(at)].included != included) {
            setIncluded(at, included);
        }
    }
}

int RekordboxExportSyncListModel::endOfSection(Section section) const
{
    int end = 0;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (static_cast<int>(m_rows[i].section) <= static_cast<int>(section)) {
            end = static_cast<int>(i) + 1;
        }
    }
    return end;
}

void RekordboxExportSyncListModel::removeAnswerRows(int conflictUid)
{
    for (int i = static_cast<int>(m_rows.size()) - 1; i >= 0; --i) {
        if (m_rows[static_cast<std::size_t>(i)].fromConflictUid == conflictUid) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.erase(m_rows.begin() + i);
            endRemoveRows();
        }
    }
}

bool RekordboxExportSyncListModel::resolveConflict(int rowIndex, bool rekordboxSide)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size())) {
        return false;
    }
    if (m_rows[static_cast<std::size_t>(rowIndex)].section != Section::Conflicts
        || !m_rows[static_cast<std::size_t>(rowIndex)].conflict) {
        return false;
    }
    const domain::EngineUpdateConflict conflict = *m_rows[static_cast<std::size_t>(rowIndex)].conflict;
    if (conflict.rekordboxChoice.empty() && conflict.engineChoice.empty()) {
        return false;  // a refusal: its reason says what to do instead
    }
    const int conflictUid = m_rows[static_cast<std::size_t>(rowIndex)].uid;
    removeAnswerRows(conflictUid);

    const auto &choice = rekordboxSide ? conflict.rekordboxChoice : conflict.engineChoice;
    std::vector<int> added;
    for (const auto &edit : choice) {
        Row row = rowForEdit(edit);
        row.uid = m_nextUid++;
        row.fromConflictUid = conflictUid;
        row.header.key = conflict.header.key;
        row.header.checkedByDefault = true;
        row.header.conflict = false;
        for (const auto &key : conflict.header.dependsOn) {
            if (std::find(row.header.dependsOn.begin(), row.header.dependsOn.end(), key) == row.header.dependsOn.end()) {
                row.header.dependsOn.push_back(key);
            }
        }
        if (row.header.reasonText.empty()) {
            row.header.reasonText = conflict.header.reasonText;
        }
        // The conflict's cue items are what its answer decides.
        if (std::holds_alternative<CueEdit>(edit) && !conflict.header.cueItems.empty()) {
            row.header.cueItems = conflict.header.cueItems;
        }
        row.included = false;  // ticked below, with what it depends on
        assignPlanKey(row);
        const int at = endOfSection(row.section);
        beginInsertRows(QModelIndex(), at, at);
        m_rows.insert(m_rows.begin() + at, std::move(row));
        endInsertRows();
        added.push_back(m_rows[static_cast<std::size_t>(at)].uid);
    }
    const int conflictAt = rowIndexOfUid(conflictUid);
    m_rows[static_cast<std::size_t>(conflictAt)].resolvedSide =
        rekordboxSide ? QStringLiteral("rekordbox") : QStringLiteral("engine");
    m_rows[static_cast<std::size_t>(conflictAt)].included = true;
    for (int uid : added) {
        setIncluded(rowIndexOfUid(uid), true);
    }
    announceIncluded();
    return true;
}

bool RekordboxExportSyncListModel::clearResolution(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size())) {
        return false;
    }
    Row &row = m_rows[static_cast<std::size_t>(rowIndex)];
    if (row.section != Section::Conflicts || row.resolvedSide.isEmpty()) {
        return false;
    }
    const int uid = row.uid;
    row.resolvedSide.clear();
    row.included = false;
    removeAnswerRows(uid);
    announceIncluded();
    return true;
}

QVariantMap RekordboxExportSyncListModel::sectionCounts() const
{
    QVariantMap counts;
    std::array<int, SectionNames.size()> n{};
    for (const auto &row : m_rows) {
        ++n[static_cast<std::size_t>(row.section)];
    }
    for (std::size_t i = 0; i < SectionNames.size(); ++i) {
        counts.insert(QString::fromLatin1(SectionNames[i]), n[i]);
    }
    return counts;
}

int RekordboxExportSyncListModel::checkedCount() const
{
    return static_cast<int>(
        std::count_if(m_rows.begin(), m_rows.end(), [](const Row &r) { return writable(r.section) && r.included; }));
}

int RekordboxExportSyncListModel::unresolvedConflictCount() const
{
    return static_cast<int>(std::count_if(m_rows.begin(), m_rows.end(), [](const Row &r) {
        return r.section == Section::Conflicts && r.resolvedSide.isEmpty();
    }));
}

QString RekordboxExportSyncListModel::planKeyAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return {};
    }
    return m_rows[static_cast<std::size_t>(index)].planKey;
}

void RekordboxExportSyncListModel::setStaged(int index, bool staged, const QString &description)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    Row &row = m_rows[static_cast<std::size_t>(index)];
    row.staged = staged;
    row.stagedDescription = staged ? description : QString();
    emit dataChanged(this->index(index), this->index(index), {StagedRole, StagedDescriptionRole});
}

void RekordboxExportSyncListModel::removePlanAt(int index)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return;
    }
    beginRemoveRows(QModelIndex(), index, index);
    m_rows.erase(m_rows.begin() + index);
    endRemoveRows();
    emit countsChanged();
}

void RekordboxExportSyncListModel::clearStaged()
{
    for (auto &row : m_rows) {
        row.staged = false;
        row.stagedDescription.clear();
    }
    if (!m_rows.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {StagedRole, StagedDescriptionRole});
    }
}

}  // namespace seabass::gui
