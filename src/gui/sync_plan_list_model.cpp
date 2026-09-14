// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/sync_plan_list_model.hpp"

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>

#include "gui/local_file_url.hpp"

namespace seabass::gui
{

using domain::SyncPlan;

namespace
{

// No waveform field -- see setAnalysis()'s comment.
QVariantMap trackToMap(const domain::Track &track)
{
    QVariantMap m;
    m["side"] = QString::fromStdString(track.format);
    m["sourceId"] = QString::fromStdString(track.sourceId);
    m["title"] = QString::fromStdString(track.title);
    m["artist"] = QString::fromStdString(track.artist);
    m["filename"] = QString::fromStdString(track.filename);
    m["filePath"] = QString::fromStdString(track.filePath);
    // A URL, as every other page's track maps carry it: the row's cover
    // and the player bar both hand it straight to an Image.
    m["artworkPath"] = toLocalFileUrl(track.artworkPath);
    m["durationMs"] = track.durationSeconds * 1000.0;
    m["bpm"] = track.bpm;
    m["key"] = QString::fromStdString(track.key);

    QVariantList cues;
    for (const auto &c : track.cues) {
        QVariantMap cueMap;
        cueMap["kind"] = c.kind == domain::CuePoint::Kind::Hot ? QStringLiteral("hot") : QStringLiteral("memory");
        cueMap["hotCueNumber"] = c.hotCueNumber;
        cueMap["positionMs"] = c.positionMs;
        cueMap["isLoop"] = c.isLoop;
        cueMap["loopEndMs"] = c.loopEndMs;
        cueMap["color"] = QString::fromStdString(c.color);
        cueMap["comment"] = QString::fromStdString(c.comment);
        cues << cueMap;
    }
    m["cues"] = cues;
    return m;
}

const domain::Track &sourceOf(const SyncPlan &plan)
{
    return plan.direction == SyncPlan::Direction::ToB ? plan.match.trackA : plan.match.trackB;
}

const domain::Track &targetOf(const SyncPlan &plan)
{
    return plan.direction == SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
}

// What picking each side of a decision would write. A same-pair decision
// writes a side's own cues; a decision between two catalogs proposing cues
// for a third writes what each proposal carries.
domain::Track optionA(const domain::CrossSourceSyncConflict &conflict)
{
    domain::Track track = conflict.sourceA;
    if (!conflict.samePair) {
        track.cues = conflict.cuesFromA;
    }
    return track;
}

domain::Track optionB(const domain::CrossSourceSyncConflict &conflict)
{
    domain::Track track = conflict.sourceB;
    if (!conflict.samePair) {
        track.cues = conflict.cuesFromB;
    }
    return track;
}

int hotCount(const std::vector<domain::CuePoint> &cues)
{
    return static_cast<int>(std::count_if(cues.begin(), cues.end(), [](const domain::CuePoint &cue) {
        return cue.kind == domain::CuePoint::Kind::Hot;
    }));
}

bool trackMatches(const domain::Track &track, const QString &query)
{
    return QString::fromStdString(track.title).contains(query, Qt::CaseInsensitive)
        || QString::fromStdString(track.artist).contains(query, Qt::CaseInsensitive);
}

}  // namespace

SyncPlanListModel::SyncPlanListModel(QObject *parent) : QAbstractListModel(parent) {}

int SyncPlanListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant SyncPlanListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows[static_cast<std::size_t>(index.row())];

    if (row.decision) {
        const auto &conflict = m_conflicts[row.index];
        switch (role) {
        case SectionRole:
            return QStringLiteral("decision");
        case NeedsDecisionRole:
            return true;
        case PlanIndexRole:
            return -1;
        case ConflictIndexRole:
            return static_cast<int>(row.index);
        case SamePairRole:
            return conflict.samePair;
        case SourceFormatRole:
            return QString();
        case TargetFormatRole:
            return conflict.samePair ? QString() : QString::fromStdString(conflict.target.format);
        case FilenameRole:
            return QString::fromStdString(conflict.target.filename.empty() ? conflict.sourceA.filename
                                                                           : conflict.target.filename);
        case TracksRole:
            return QVariantList{trackToMap(optionA(conflict)), trackToMap(optionB(conflict))};
        case CueSummaryRole:
            return choiceSummary(conflict);
        case CueChangeRole:
            return QVariantMap();
        case JunkCuesRole:
            return QVariantList{conflict.sourceAHasJunkCue, conflict.sourceBHasJunkCue};
        case IncludedRole:
        case StagedRole:
            return false;
        case StagedDescriptionRole:
            return QString();
        default:
            return {};
        }
    }

    const auto &plan = m_plans[row.index];
    switch (role) {
    case SectionRole:
        return QStringLiteral("ready");
    case NeedsDecisionRole:
    case SamePairRole:
        return false;
    case PlanIndexRole:
        return static_cast<int>(row.index);
    case ConflictIndexRole:
        return -1;
    case SourceFormatRole:
        return QString::fromStdString(sourceOf(plan).format);
    case TargetFormatRole:
        return QString::fromStdString(targetOf(plan).format);
    case FilenameRole:
        return QString::fromStdString(sourceOf(plan).filename);
    case TracksRole:
        return QVariantList{trackToMap(sourceOf(plan)), trackToMap(targetOf(plan))};
    case CueSummaryRole:
        return cueSummary(plan);
    case CueChangeRole: {
        const domain::CueChange change = domain::describeCueChange(targetOf(plan).cues, plan.cuesToApply);
        return QVariantMap{
            {QStringLiteral("gainedHot"), change.gainedHot},
            {QStringLiteral("keptHot"), change.keptHot},
            {QStringLiteral("droppedHot"), change.droppedHot},
            {QStringLiteral("gainedMemory"), change.gainedMemory},
            {QStringLiteral("keptMemory"), change.keptMemory},
            {QStringLiteral("droppedMemory"), change.droppedMemory},
        };
    }
    case JunkCuesRole:
        return QVariantList{false, false};
    case IncludedRole:
        return static_cast<bool>(m_included[row.index]);
    case StagedRole:
        return !m_stagedDescriptions[row.index].isEmpty();
    case StagedDescriptionRole:
        return m_stagedDescriptions[row.index];
    default:
        return {};
    }
}

QHash<int, QByteArray> SyncPlanListModel::roleNames() const
{
    return {
        {SectionRole, "section"},
        {NeedsDecisionRole, "needsDecision"},
        {PlanIndexRole, "planIndex"},
        {ConflictIndexRole, "conflictIndex"},
        {SamePairRole, "samePair"},
        {SourceFormatRole, "sourceFormat"},
        {TargetFormatRole, "targetFormat"},
        {FilenameRole, "filename"},
        {TracksRole, "tracks"},
        {CueSummaryRole, "cueSummary"},
        {CueChangeRole, "cueChange"},
        {JunkCuesRole, "junkCues"},
        {IncludedRole, "included"},
        {StagedRole, "staged"},
        {StagedDescriptionRole, "stagedDescription"},
    };
}

void SyncPlanListModel::setAnalysis(std::vector<domain::SyncPlan> plans,
                                    std::vector<domain::CrossSourceSyncConflict> conflicts)
{
    beginResetModel();
    m_plans = std::move(plans);
    m_stagedDescriptions.assign(m_plans.size(), QString());
    m_included.assign(m_plans.size(), true);
    m_conflicts = std::move(conflicts);
    rebuildRows();
    endResetModel();
    emit countsChanged();
}

void SyncPlanListModel::addPlan(domain::SyncPlan plan)
{
    const std::size_t planIndex = m_plans.size();
    const bool visible = planMatches(plan);
    const int row = static_cast<int>(m_rows.size());
    if (visible) {
        beginInsertRows(QModelIndex(), row, row);
    }
    m_plans.push_back(std::move(plan));
    m_stagedDescriptions.push_back(QString());
    m_included.push_back(true);
    if (visible) {
        m_rows.push_back({false, planIndex});
        endInsertRows();
    }
    emit countsChanged();
}

void SyncPlanListModel::removeConflictAt(int conflictIndex)
{
    if (conflictIndex < 0 || static_cast<std::size_t>(conflictIndex) >= m_conflicts.size()) {
        return;
    }
    const auto removed = static_cast<std::size_t>(conflictIndex);
    const int row = rowOfConflict(removed);
    if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_rows.erase(m_rows.begin() + row);
    }
    m_conflicts.erase(m_conflicts.begin() + conflictIndex);
    for (Row &r : m_rows) {
        if (r.decision && r.index > removed) {
            r.index--;
        }
    }
    if (row >= 0) {
        endRemoveRows();
    }
    emit countsChanged();
}

void SyncPlanListModel::removePlanAt(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_plans.size()) {
        return;
    }
    const auto removed = static_cast<std::size_t>(index);
    const int row = rowOfPlan(removed);
    if (row >= 0) {
        beginRemoveRows(QModelIndex(), row, row);
        m_rows.erase(m_rows.begin() + row);
    }
    m_plans.erase(m_plans.begin() + index);
    m_stagedDescriptions.erase(m_stagedDescriptions.begin() + index);
    m_included.erase(m_included.begin() + index);
    // Every row past the removed plan points one further than it should
    // now, whether or not the removed one was itself on screen.
    for (Row &r : m_rows) {
        if (!r.decision && r.index > removed) {
            r.index--;
        }
    }
    if (row >= 0) {
        endRemoveRows();
    }
    emit countsChanged();
}

void SyncPlanListModel::setFilter(const QString &query)
{
    if (query == m_filter) {
        return;
    }
    beginResetModel();
    m_filter = query;
    rebuildRows();
    endResetModel();
    emit countsChanged();
}

bool SyncPlanListModel::isPlanVisible(int planIndex) const
{
    return planIndex >= 0 && rowOfPlan(static_cast<std::size_t>(planIndex)) >= 0;
}

void SyncPlanListModel::setIncluded(int planIndex, bool included)
{
    if (planIndex < 0 || static_cast<std::size_t>(planIndex) >= m_plans.size()) {
        return;
    }
    m_included[static_cast<std::size_t>(planIndex)] = included;
    const int row = rowOfPlan(static_cast<std::size_t>(planIndex));
    if (row >= 0) {
        emit dataChanged(this->index(row), this->index(row), {IncludedRole});
    }
    emit countsChanged();
}

void SyncPlanListModel::setAllIncluded(bool included)
{
    for (const Row &row : m_rows) {
        if (!row.decision) {
            m_included[row.index] = included;
        }
    }
    if (!m_rows.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {IncludedRole});
    }
    emit countsChanged();
}

bool SyncPlanListModel::included(int planIndex) const
{
    return planIndex >= 0 && static_cast<std::size_t>(planIndex) < m_included.size()
        && m_included[static_cast<std::size_t>(planIndex)];
}

bool SyncPlanListModel::isStaged(int planIndex) const
{
    return planIndex >= 0 && static_cast<std::size_t>(planIndex) < m_stagedDescriptions.size()
        && !m_stagedDescriptions[static_cast<std::size_t>(planIndex)].isEmpty();
}

int SyncPlanListModel::visiblePlanCount() const
{
    return static_cast<int>(std::count_if(m_rows.begin(), m_rows.end(), [](const Row &r) { return !r.decision; }));
}

int SyncPlanListModel::visibleConflictCount() const
{
    return static_cast<int>(m_rows.size()) - visiblePlanCount();
}

int SyncPlanListModel::selectedCount() const
{
    int count = 0;
    for (int i = 0; i < planCount(); ++i) {
        if (included(i) && !isStaged(i)) {
            count++;
        }
    }
    return count;
}

int SyncPlanListModel::selectedVisibleCount() const
{
    int count = 0;
    for (const Row &row : m_rows) {
        const int i = static_cast<int>(row.index);
        if (!row.decision && included(i) && !isStaged(i)) {
            count++;
        }
    }
    return count;
}

QString SyncPlanListModel::planKeyAt(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_plans.size()) {
        return {};
    }
    const domain::Track &target = targetOf(m_plans[static_cast<std::size_t>(index)]);
    return QString::fromStdString(target.format) + ":" + QString::fromStdString(target.sourceId);
}

void SyncPlanListModel::setStaged(int index, bool staged, const QString &description)
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_plans.size()) {
        return;
    }
    m_stagedDescriptions[static_cast<std::size_t>(index)] = staged ? description : QString();
    const int row = rowOfPlan(static_cast<std::size_t>(index));
    if (row >= 0) {
        emit dataChanged(this->index(row), this->index(row), {StagedRole, StagedDescriptionRole});
    }
    emit countsChanged();
}

void SyncPlanListModel::clearStaged()
{
    std::fill(m_stagedDescriptions.begin(), m_stagedDescriptions.end(), QString());
    if (!m_rows.empty()) {
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1), {StagedRole, StagedDescriptionRole});
    }
    emit countsChanged();
}

// The way the row says it, for a DJ rather than for the writer: a hot cue
// the target already has is kept, not copied again, and one the write
// leaves out is replaced. See domain::describeCueChange().
QString SyncPlanListModel::cueSummary(const domain::SyncPlan &plan)
{
    const domain::CueChange change = domain::describeCueChange(targetOf(plan).cues, plan.cuesToApply);
    QStringList parts;
    if (change.gainedHot > 0) {
        parts << QStringLiteral("+%1 hot").arg(change.gainedHot);
    }
    if (change.gainedMemory > 0) {
        parts << QStringLiteral("+%1 memory").arg(change.gainedMemory);
    }
    if (change.keptHot + change.keptMemory > 0) {
        parts << QStringLiteral("keeps %1").arg(change.keptHot + change.keptMemory);
    }
    if (change.droppedHot + change.droppedMemory > 0) {
        parts << QStringLiteral("replaces %1").arg(change.droppedHot + change.droppedMemory);
    }
    return parts.isEmpty() ? QStringLiteral("no change") : parts.join(QStringLiteral(", "));
}

QString SyncPlanListModel::choiceSummary(const domain::CrossSourceSyncConflict &conflict)
{
    return QStringLiteral("%1 hot vs %2 hot").arg(hotCount(optionA(conflict).cues)).arg(hotCount(optionB(conflict).cues));
}

bool SyncPlanListModel::planMatches(const domain::SyncPlan &plan) const
{
    return m_filter.isEmpty() || trackMatches(plan.match.trackA, m_filter) || trackMatches(plan.match.trackB, m_filter);
}

bool SyncPlanListModel::conflictMatches(const domain::CrossSourceSyncConflict &conflict) const
{
    return m_filter.isEmpty() || trackMatches(conflict.target, m_filter) || trackMatches(conflict.sourceA, m_filter)
        || trackMatches(conflict.sourceB, m_filter);
}

void SyncPlanListModel::rebuildRows()
{
    m_rows.clear();
    for (std::size_t i = 0; i < m_conflicts.size(); ++i) {
        if (conflictMatches(m_conflicts[i])) {
            m_rows.push_back({true, i});
        }
    }
    for (std::size_t i = 0; i < m_plans.size(); ++i) {
        if (planMatches(m_plans[i])) {
            m_rows.push_back({false, i});
        }
    }
}

int SyncPlanListModel::rowOfPlan(std::size_t planIndex) const
{
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
        if (!m_rows[row].decision && m_rows[row].index == planIndex) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

int SyncPlanListModel::rowOfConflict(std::size_t conflictIndex) const
{
    for (std::size_t row = 0; row < m_rows.size(); ++row) {
        if (m_rows[row].decision && m_rows[row].index == conflictIndex) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

}  // namespace seabass::gui
