// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/rekordbox_export_sync_list_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "gui/local_file_url.hpp"
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

// In Section's order.
constexpr std::array<const char *, 10> SectionNames = {
    "conflicts",    "playlists",           "tracksToAdd",   "tracksToRemove", "membership",
    "metadataToEngine", "cuesToEngine", "restoresToRekordbox", "engineOwnKept", "notAdded",
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


// The details lines (RekordboxExportSyncListModel::details). Plain
// sentences in the data font: "Label: value", one fact a line.

QString bpmText(double bpm)
{
    if (bpm <= 0.0) {
        return QStringLiteral("unknown");
    }
    QString text = QString::number(bpm, 'f', 2);
    while (text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text;
}

QString lengthText(double seconds)
{
    if (seconds <= 0.0) {
        return QStringLiteral("unknown");
    }
    const long long total = std::llround(seconds);
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QString at(double positionMs)
{
    return q(domain::formatCuePosition(positionMs));
}

// "1:07.751", or "loop 1:07.751 to 1:11.751".
QString cuePlace(const domain::CuePoint &cue)
{
    return cue.isLoop ? QStringLiteral("loop %1 to %2").arg(at(cue.positionMs), at(cue.loopEndMs)) : at(cue.positionMs);
}

QString cueName(const domain::CuePoint &cue)
{
    return cue.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("Pad %1").arg(cue.hotCueNumber)
                                                   : QStringLiteral("Memory cue");
}

std::vector<domain::CuePoint> sortedCues(std::vector<domain::CuePoint> cues)
{
    std::stable_sort(cues.begin(), cues.end(), [](const domain::CuePoint &a, const domain::CuePoint &b) {
        const bool aHot = a.kind == domain::CuePoint::Kind::Hot;
        const bool bHot = b.kind == domain::CuePoint::Kind::Hot;
        if (aHot != bHot) {
            return aHot;
        }
        if (aHot && a.hotCueNumber != b.hotCueNumber) {
            return a.hotCueNumber < b.hotCueNumber;
        }
        return a.positionMs < b.positionMs;
    });
    return cues;
}

// Every cue of a set, pads first.
QStringList cueListLines(const std::vector<domain::CuePoint> &cues)
{
    QStringList lines;
    for (const auto &cue : sortedCues(cues)) {
        lines << cueName(cue) + QStringLiteral(": ") + cuePlace(cue);
    }
    if (lines.isEmpty()) {
        lines << QStringLiteral("Cues: none");
    }
    return lines;
}

bool samePlace(const domain::CuePoint &a, const domain::CuePoint &b, double toleranceMs)
{
    return a.isLoop == b.isLoop && std::abs(a.positionMs - b.positionMs) < toleranceMs
        && (!a.isLoop || std::abs(a.loopEndMs - b.loopEndMs) < toleranceMs);
}

// What a cue write changes on its side: each pad that differs, old and
// new, and each memory cue that comes or goes. Colours never count.
QStringList cueChangeLines(const std::vector<domain::CuePoint> &before, const std::vector<domain::CuePoint> &after,
                           double toleranceMs)
{
    QStringList lines;
    std::map<int, const domain::CuePoint *> padsBefore;
    std::map<int, const domain::CuePoint *> padsAfter;
    std::vector<const domain::CuePoint *> memoryBefore;
    std::vector<const domain::CuePoint *> memoryAfter;
    for (const auto &cue : before) {
        if (cue.kind == domain::CuePoint::Kind::Hot) {
            padsBefore.emplace(cue.hotCueNumber, &cue);
        } else {
            memoryBefore.push_back(&cue);
        }
    }
    for (const auto &cue : after) {
        if (cue.kind == domain::CuePoint::Kind::Hot) {
            padsAfter.emplace(cue.hotCueNumber, &cue);
        } else {
            memoryAfter.push_back(&cue);
        }
    }
    std::set<int> pads;
    for (const auto &[pad, cue] : padsBefore) {
        pads.insert(pad);
    }
    for (const auto &[pad, cue] : padsAfter) {
        pads.insert(pad);
    }
    for (int pad : pads) {
        const auto was = padsBefore.find(pad);
        const auto now = padsAfter.find(pad);
        const QString name = QStringLiteral("Pad %1: ").arg(pad);
        if (was != padsBefore.end() && now != padsAfter.end()) {
            if (!samePlace(*was->second, *now->second, toleranceMs)) {
                lines << name + QStringLiteral("was %1, now %2").arg(cuePlace(*was->second), cuePlace(*now->second));
            }
        } else if (now != padsAfter.end()) {
            lines << name + QStringLiteral("new, %1").arg(cuePlace(*now->second));
        } else {
            lines << name + QStringLiteral("%1, cleared").arg(cuePlace(*was->second));
        }
    }
    const auto sortByPlace = [](std::vector<const domain::CuePoint *> &cues) {
        std::sort(cues.begin(), cues.end(), [](const auto *a, const auto *b) { return a->positionMs < b->positionMs; });
    };
    sortByPlace(memoryBefore);
    sortByPlace(memoryAfter);
    const auto findIn = [toleranceMs](const std::vector<const domain::CuePoint *> &cues, const domain::CuePoint &cue) {
        return std::any_of(cues.begin(), cues.end(), [&](const auto *c) { return samePlace(*c, cue, toleranceMs); });
    };
    for (const auto *cue : memoryAfter) {
        if (!findIn(memoryBefore, *cue)) {
            lines << QStringLiteral("Memory cue: new, %1").arg(cuePlace(*cue));
        }
    }
    for (const auto *cue : memoryBefore) {
        if (!findIn(memoryAfter, *cue)) {
            lines << QStringLiteral("Memory cue: %1, cleared").arg(cuePlace(*cue));
        }
    }
    if (lines.isEmpty()) {
        lines << QStringLiteral("Cues: the same places, written again");
    }
    return lines;
}

// The cues a CueEdit writes, on the side it writes them, old and new.
QStringList cueEditLines(const CueEdit &e)
{
    const bool toEngine = e.plan.direction == SyncPlan::Direction::ToB;
    const Track &target = toEngine ? e.plan.match.trackB : e.plan.match.trackA;
    QStringList lines;
    lines << (toEngine ? QStringLiteral("Writes Engine's cues:") : QStringLiteral("Writes rekordbox's cues:"));
    lines << cueChangeLines(target.cues, e.plan.cuesToApply, e.plan.positionToleranceMs);
    if (!e.plan.cuesLeftOut.empty()) {
        lines << q(domain::describeCuesLeftOut(e.plan.cuesLeftOut));
    }
    return lines;
}

QString commentText(const std::string &comment)
{
    return comment.empty() ? QStringLiteral("none") : quotedText(comment);
}

// A track as a catalog has it: what it is and where.
QStringList trackLines(const Track &track, const std::string &stickRoot)
{
    QStringList lines;
    lines << QStringLiteral("Title: ") + q(track.title.empty() ? track.filename : track.title);
    if (!track.artist.empty()) {
        lines << QStringLiteral("Artist: ") + q(track.artist);
    }
    lines << QStringLiteral("File: ") + pathOnStick(track, stickRoot);
    lines << QStringLiteral("BPM: %1, key: %2, length: %3")
                 .arg(bpmText(track.bpm), track.key.empty() ? QStringLiteral("unknown") : q(track.key),
                      lengthText(track.durationSeconds));
    lines << QStringLiteral("Rating: ") + stars(track.rating);
    lines << QStringLiteral("Comment: ") + commentText(track.comment);
    return lines;
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
    switch (section) {
    case Section::Playlists:
    case Section::TracksToAdd:
    case Section::TracksToRemove:
    case Section::Membership:
    case Section::MetadataToEngine:
    case Section::CuesToEngine:
    case Section::RestoresToRekordbox:
        return true;
    case Section::Conflicts:
    case Section::EngineOwnKept:
    case Section::NotAdded:
        break;
    }
    return false;
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
    case DetailsRole:
        return detailsOf(row);
    case HasTrackRole:
        return row.hasTrack;
    case ArtworkPathRole:
        return toLocalFileUrl(row.artworkPath);
    case FallbackArtworkPathRole:
        return toLocalFileUrl(row.fallbackArtworkPath);
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
        {DetailsRole, "details"},
        {HasTrackRole, "hasTrack"},
        {ArtworkPathRole, "artworkPath"},
        {FallbackArtworkPathRole, "fallbackArtworkPath"},
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
        void track(const Track &t, const Track *other = nullptr)
        {
            row.title = q(t.title.empty() ? t.filename : t.title);
            row.artist = q(t.artist);
            row.hasTrack = true;
            row.artworkPath = t.artworkPath;
            row.fallbackArtworkPath = other ? other->artworkPath : std::string();
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
            track(toEngine ? e.rekordbox : e.engine, toEngine ? &e.engine : &e.rekordbox);
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
            track(e.plan.match.trackA, &e.plan.match.trackB);
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
    // The questions first (Section's order).
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
            row.hasTrack = true;
            row.artworkPath = track->artworkPath;
            if (const std::optional<Track> other = trackOfChoice(c.engineChoice); other) {
                row.fallbackArtworkPath = other->artworkPath;
            }
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
    for (const auto &k : proposal.engineOwnKept) {
        Row row = makeRow(Section::EngineOwnKept, QStringLiteral("kept"), k.header);
        if (!k.playlistPath.empty()) {
            row.title = q(k.playlistPath);
            row.detail = quotedText(k.playlistPath);
        } else {
            row.title = q(k.engine.title.empty() ? k.engine.filename : k.engine.title);
            row.hasTrack = true;
            row.artworkPath = k.engine.artworkPath;
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
    if (m_bulk) {
        return;  // the reset at the end says it all
    }
    if (!m_rows.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1),
                         {IncludedRole, ResolvedSideRole, DirectionRole, DetailsRole});
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
            if (!m_bulk) {
                beginRemoveRows(QModelIndex(), i, i);
            }
            m_rows.erase(m_rows.begin() + i);
            if (!m_bulk) {
                endRemoveRows();
            }
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
        if (!m_bulk) {
            beginInsertRows(QModelIndex(), at, at);
        }
        m_rows.insert(m_rows.begin() + at, std::move(row));
        if (!m_bulk) {
            endInsertRows();
        }
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

void RekordboxExportSyncListModel::resolveAllConflicts(bool rekordboxSide)
{
    std::vector<int> uids;
    for (const auto &row : m_rows) {
        if (row.section == Section::Conflicts) {
            uids.push_back(row.uid);
        }
    }
    if (uids.empty()) {
        return;
    }
    beginResetModel();
    m_bulk = true;
    // By uid: every answer inserts rows, though after the conflicts.
    for (int uid : uids) {
        resolveConflict(rowIndexOfUid(uid), rekordboxSide);
    }
    m_bulk = false;
    endResetModel();
    emit countsChanged();
}

void RekordboxExportSyncListModel::clearAllResolutions()
{
    std::vector<int> uids;
    for (const auto &row : m_rows) {
        if (row.section == Section::Conflicts && !row.resolvedSide.isEmpty()) {
            uids.push_back(row.uid);
        }
    }
    if (uids.empty()) {
        return;
    }
    beginResetModel();
    m_bulk = true;
    for (int uid : uids) {
        clearResolution(rowIndexOfUid(uid));
    }
    m_bulk = false;
    endResetModel();
    emit countsChanged();
}

QStringList RekordboxExportSyncListModel::details(int rowIndex) const
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size())) {
        return {};
    }
    return detailsOf(m_rows[static_cast<std::size_t>(rowIndex)]);
}

// A track's title (and artist) by its pathKey, from the rows that name
// it; the key itself when none does (an Engine member this save leaves
// alone).
QString RekordboxExportSyncListModel::titleByPathKey(const std::string &pathKey) const
{
    for (const auto &row : m_rows) {
        if (!row.edit) {
            continue;
        }
        const Track *track = nullptr;
        if (const auto *member = std::get_if<MembershipEdit>(&*row.edit); member && member->pathKey == pathKey) {
            track = &member->track;
        } else if (const auto *add = std::get_if<TrackToAdd>(&*row.edit); add && add->pathKey == pathKey) {
            track = &add->rekordbox;
        }
        if (track) {
            const QString title = q(track->title.empty() ? track->filename : track->title);
            return track->artist.empty() ? title : title + QStringLiteral(" by ") + q(track->artist);
        }
    }
    return q(pathKey);
}

// One edit, as the lines it writes: a conflict's choices are told with
// these, each under its side.
QStringList RekordboxExportSyncListModel::editLines(const EngineUpdateEdit &edit) const
{
    QStringList lines;
    if (const auto *e = std::get_if<PlaylistCreate>(&edit)) {
        int members = 0;
        for (const auto &row : m_rows) {
            if (row.edit) {
                if (const auto *m = std::get_if<MembershipEdit>(&*row.edit);
                    m && m->kind == MembershipEdit::Kind::Add && m->playlistPath == e->path) {
                    ++members;
                }
            }
        }
        lines << QStringLiteral("Creates %1 %2 on Engine")
                     .arg(e->folder ? QStringLiteral("folder") : QStringLiteral("playlist"), quotedText(e->path));
        if (!e->folder) {
            lines << QStringLiteral("Members added in this save: %1").arg(members);
        }
    } else if (const auto *e = std::get_if<PlaylistRename>(&edit)) {
        lines << QStringLiteral("Renames %1 on Engine").arg(quotedText(e->fromPath));
        lines << QStringLiteral("New name: ") + quotedText(e->toPath);
    } else if (const auto *e = std::get_if<PlaylistDelete>(&edit)) {
        lines << QStringLiteral("Deletes %1 %2 from Engine")
                     .arg(e->folder ? QStringLiteral("folder") : QStringLiteral("playlist"), quotedText(e->path));
        lines << QStringLiteral("Engine members it holds: %1 (the tracks stay in the library)").arg(e->engineMembers);
    } else if (const auto *e = std::get_if<TrackToAdd>(&edit)) {
        lines << QStringLiteral("Adds this track to Engine, as rekordbox has it:");
        lines << trackLines(e->rekordbox, m_stickRoot);
        lines << cueListLines(e->rekordbox.cues);
        lines << (e->rekordbox.artworkPath.empty() ? QStringLiteral("Cover: none") : QStringLiteral("Cover: yes"));
        QStringList joins;
        for (const auto &row : m_rows) {
            if (row.edit && row.included) {
                if (const auto *m = std::get_if<MembershipEdit>(&*row.edit);
                    m && m->kind == MembershipEdit::Kind::Add && m->pathKey == e->pathKey) {
                    joins << QStringLiteral("Joins ") + quotedText(m->playlistPath) + positionIn(m->track, m->playlistPath);
                }
            }
        }
        if (joins.isEmpty()) {
            joins << QStringLiteral("Joins no playlist in this save");
        }
        lines << joins;
    } else if (const auto *e = std::get_if<TrackToRemove>(&edit)) {
        lines << QStringLiteral("Removes this track from Engine's library, as Engine has it:");
        lines << trackLines(e->engine, m_stickRoot);
        lines << QStringLiteral("Cues on Engine: %1").arg(e->engine.cues.size());
        QStringList leaves;
        for (const auto &membership : e->engine.playlists) {
            leaves << QStringLiteral("Leaves ") + quotedText(membership.name);
        }
        if (leaves.isEmpty()) {
            leaves << (e->playlistCount > 0 ? QStringLiteral("Leaves %1 playlist entries").arg(e->playlistCount)
                                            : QStringLiteral("In no Engine playlist"));
        }
        lines << leaves;
        lines << QStringLiteral("The file stays on the stick");
    } else if (const auto *e = std::get_if<MembershipEdit>(&edit)) {
        const QString title = q(e->track.title.empty() ? e->track.filename : e->track.title);
        const QString place = positionIn(e->track, e->playlistPath).trimmed();
        if (e->kind == MembershipEdit::Kind::Add) {
            lines << QStringLiteral("Puts %1 into %2 on Engine").arg(title, quotedText(e->playlistPath));
            lines << QStringLiteral("Position in rekordbox: ") + (place.isEmpty() ? QStringLiteral("unknown") : place);
            lines << (e->afterPathKey.empty() ? QStringLiteral("Goes first in the playlist")
                                              : QStringLiteral("Goes after ") + titleByPathKey(e->afterPathKey));
            if (!e->afterPathKey.empty()) {
                lines << QStringLiteral("If that track is left out, it goes at the end");
            }
        } else {
            lines << QStringLiteral("Takes %1 out of %2 on Engine").arg(title, quotedText(e->playlistPath));
            if (!place.isEmpty()) {
                lines << QStringLiteral("Position on Engine: ") + place;
            }
            lines << QStringLiteral("The track stays in the library");
        }
        lines << QStringLiteral("File: ") + pathOnStick(e->track, m_stickRoot);
    } else if (const auto *e = std::get_if<MetadataEdit>(&edit)) {
        const bool toEngine = e->direction == MetadataEdit::Direction::ToEngine;
        const Track &target = toEngine ? e->engine : e->rekordbox;
        const QString side = toEngine ? QStringLiteral("Engine's") : QStringLiteral("rekordbox's");
        if (e->field == MetadataEdit::Field::Rating) {
            lines << QStringLiteral("Sets %1 rating").arg(side);
            lines << QStringLiteral("Rating: was %1, now %2").arg(stars(target.rating), stars(e->rating));
        } else {
            lines << QStringLiteral("Sets %1 comment").arg(side);
            lines << QStringLiteral("Comment: was %1, now %2").arg(commentText(target.comment), commentText(e->comment));
        }
    } else if (const auto *e = std::get_if<CueEdit>(&edit)) {
        lines << cueEditLines(*e);
    }
    return lines;
}

QStringList RekordboxExportSyncListModel::detailsOf(const Row &row) const
{
    QStringList lines;
    const QString why = q(row.header.reasonText);
    if (row.section == Section::Conflicts && row.conflict) {
        const auto &c = *row.conflict;
        if (!why.isEmpty()) {
            lines << QStringLiteral("Why: ") + why;
        }
        const auto side = [&](const QString &name, const std::string &label,
                              const std::vector<EngineUpdateEdit> &choice) {
            lines << name + (label.empty() ? QString() : QStringLiteral(" (") + q(label) + QLatin1Char(')'))
                    + QLatin1Char(':');
            if (choice.empty()) {
                lines << QStringLiteral("  writes nothing; Engine keeps what it has");
            }
            for (const auto &edit : choice) {
                for (const QString &line : editLines(edit)) {
                    lines << QStringLiteral("  ") + line;
                }
            }
        };
        if (c.rekordboxChoice.empty() && c.engineChoice.empty()) {
            lines << QStringLiteral("Neither side can be written here; the reason says what to do instead");
            return lines;
        }
        side(QStringLiteral("Rekordbox's side"), c.rekordboxSide, c.rekordboxChoice);
        side(QStringLiteral("Engine's side"), c.engineSide, c.engineChoice);
        return lines;
    }
    if (row.section == Section::EngineOwnKept) {
        lines << QStringLiteral("Kept as Engine has it; nothing is written");
        lines << QStringLiteral("Why: ") + (why.isEmpty() ? QStringLiteral("Engine changed it") : why);
        lines << QStringLiteral("Item: ") + row.detail;
        return lines;
    }
    if (row.section == Section::NotAdded) {
        lines << QStringLiteral("Not added; nothing is written");
        lines << QStringLiteral("Why: ") + why;
        lines << QStringLiteral("Rekordbox expects: ") + row.detail;
        return lines;
    }
    if (!row.edit) {
        return lines;
    }
    lines << editLines(*row.edit);
    if (row.section == Section::RestoresToRekordbox && !why.isEmpty()) {
        lines << QStringLiteral("Why: ") + why;
    }
    if (row.fromConflictUid >= 0) {
        lines << QStringLiteral("From an answered conflict");
    }
    return lines;
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

QVariantMap RekordboxExportSyncListModel::sectionCheckedCounts() const
{
    QVariantMap counts;
    std::array<int, SectionNames.size()> n{};
    for (const auto &row : m_rows) {
        if (row.included) {
            ++n[static_cast<std::size_t>(row.section)];
        }
    }
    for (std::size_t i = 0; i < SectionNames.size(); ++i) {
        counts.insert(QString::fromLatin1(SectionNames[i]), n[i]);
    }
    return counts;
}

QVariantMap RekordboxExportSyncListModel::categoryCounts() const
{
    int newTracks = 0;
    int newPlaylists = 0;
    int changed = 0;
    int removed = 0;
    int other = 0;
    for (const auto &row : m_rows) {
        switch (row.section) {
        case Section::TracksToAdd:
            ++newTracks;
            break;
        case Section::TracksToRemove:
            ++removed;
            break;
        case Section::Playlists:
            if (row.kind.startsWith(QLatin1String("create"))) {
                ++newPlaylists;
            } else if (row.kind.startsWith(QLatin1String("delete"))) {
                ++removed;
            } else {
                ++other;
            }
            break;
        case Section::Membership:
            if (row.kind == QLatin1String("removeMember")) {
                ++removed;
            } else {
                ++changed;
            }
            break;
        case Section::MetadataToEngine:
        case Section::CuesToEngine:
        case Section::RestoresToRekordbox:
            ++changed;
            break;
        case Section::Conflicts:
        case Section::EngineOwnKept:
        case Section::NotAdded:
            break;
        }
    }
    return {{QStringLiteral("newTracks"), newTracks},
            {QStringLiteral("newPlaylists"), newPlaylists},
            {QStringLiteral("changed"), changed},
            {QStringLiteral("removed"), removed},
            {QStringLiteral("other"), other}};
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
