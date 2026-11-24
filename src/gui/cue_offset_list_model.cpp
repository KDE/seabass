// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/cue_offset_list_model.hpp"

#include <QStringList>

#include "domain/sync_planning.hpp"

namespace seabass::gui
{

using domain::CueOffsetFinding;
using domain::CueOffsetSide;

QString sideName(CueOffsetSide side)
{
    switch (side) {
    case CueOffsetSide::Engine:
        return QStringLiteral("engine");
    case CueOffsetSide::Rekordbox:
        return QStringLiteral("rekordbox");
    case CueOffsetSide::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

namespace
{

QString cueName(const domain::CuePoint &cue)
{
    if (cue.kind == domain::CuePoint::Kind::Hot) {
        return QStringLiteral("%1 %2").arg(cue.isLoop ? QStringLiteral("Loop") : QStringLiteral("Pad")).arg(cue.hotCueNumber);
    }
    return cue.isLoop ? QStringLiteral("Memory loop") : QStringLiteral("Memory cue");
}

QString cueLines(const CueOffsetFinding &finding)
{
    QStringList lines;
    const QString rekordboxName = QString::fromStdString(domain::catalogDisplayName(finding.rekordbox.format));
    for (size_t i = 0; i < finding.cues.size(); ++i) {
        const auto &offset = finding.cues[i];
        const auto &rekordboxCue = finding.rekordbox.cues[offset.rekordboxIndex];
        // Both as each catalog stores them: the same number on both is
        // what a copy that did not move them left.
        lines << QStringLiteral("%1: %2 %3, Engine %4")
                     .arg(cueName(finding.engine.cues[offset.engineIndex]), rekordboxName,
                          QString::fromStdString(domain::formatCuePosition(rekordboxCue.positionMs)),
                          QString::fromStdString(domain::formatCuePosition(finding.engineStoredMs(i))));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace

CueOffsetListModel::CueOffsetListModel(QObject *parent) : QAbstractListModel(parent) {}

int CueOffsetListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant CueOffsetListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }
    const CueOffsetFinding &finding = m_findings[static_cast<size_t>(index.row())];
    const domain::Track &track = finding.rekordbox;
    switch (role) {
    case TitleRole:
        return QString::fromStdString(track.title.empty() ? track.filename : track.title);
    case ArtistRole:
        return QString::fromStdString(track.artist);
    case SideRole:
        return sideName(finding.sideToMove);
    case ReasonRole:
        return QString::fromStdString(finding.reason());
    case CuesRole:
        return cueLines(finding);
    case RekordboxNameRole:
        return QString::fromStdString(domain::catalogDisplayName(track.format));
    case StagedSideRole:
        return m_stagedSide[static_cast<size_t>(index.row())];
    case HasKnownRole:
        return finding.hasKnown();
    case HasUnknownRole:
        return finding.hasUnknown();
    default:
        return {};
    }
}

QHash<int, QByteArray> CueOffsetListModel::roleNames() const
{
    return {
        {TitleRole, "title"},       {ArtistRole, "artist"},
        {SideRole, "side"},         {ReasonRole, "reason"},
        {CuesRole, "cues"},         {RekordboxNameRole, "rekordboxName"},
        {StagedSideRole, "stagedSide"}, {HasKnownRole, "hasKnown"},
        {HasUnknownRole, "hasUnknown"},
    };
}

void CueOffsetListModel::setFindings(std::vector<CueOffsetFinding> findings)
{
    beginResetModel();
    m_findings = std::move(findings);
    m_stagedSide.assign(m_findings.size(), QString());
    endResetModel();
    emit countChanged();
}

void CueOffsetListModel::setStagedSide(int row, const QString &side)
{
    if (row < 0 || row >= count()) {
        return;
    }
    m_stagedSide[static_cast<size_t>(row)] = side;
    const QModelIndex at = index(row);
    emit dataChanged(at, at, {StagedSideRole});
}

void CueOffsetListModel::clearStaged()
{
    for (int row = 0; row < count(); ++row) {
        setStagedSide(row, QString());
    }
}

void CueOffsetListModel::removeAt(int row)
{
    if (row < 0 || row >= count()) {
        return;
    }
    beginRemoveRows({}, row, row);
    m_findings.erase(m_findings.begin() + row);
    m_stagedSide.erase(m_stagedSide.begin() + row);
    endRemoveRows();
    emit countChanged();
}

QString CueOffsetListModel::stagedSide(int row) const
{
    return row >= 0 && row < count() ? m_stagedSide[static_cast<size_t>(row)] : QString();
}

QString CueOffsetListModel::keyOf(const CueOffsetFinding &finding)
{
    return QString::fromStdString(finding.rekordbox.format + ":" + finding.rekordbox.sourceId + "|engine:"
                                  + finding.engine.sourceId);
}

int CueOffsetListModel::indexOfKey(const QString &key) const
{
    for (int row = 0; row < count(); ++row) {
        if (keyOf(m_findings[static_cast<size_t>(row)]) == key) {
            return row;
        }
    }
    return -1;
}

int CueOffsetListModel::knownUnstagedCount() const
{
    int known = 0;
    for (int row = 0; row < count(); ++row) {
        known += m_findings[static_cast<size_t>(row)].hasKnown()
            && m_stagedSide[static_cast<size_t>(row)].isEmpty();
    }
    return known;
}

int CueOffsetListModel::stagedCount() const
{
    int staged = 0;
    for (const QString &side : m_stagedSide) {
        staged += !side.isEmpty();
    }
    return staged;
}

}  // namespace seabass::gui
