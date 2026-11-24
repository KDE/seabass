// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>

#include <vector>

#include "domain/cue_offset_repair.hpp"

namespace seabass::gui
{

// Sync Cue Points' "Cues a few milliseconds off" section (#121): one row a
// track whose cues sit a few tens of milliseconds off on one side
// (domain::findCueOffsets), with what says which side, and what is staged
// for it. Populated by SyncController's analysis.
class CueOffsetListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by SyncController; not constructible from QML")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        // A summary: "engine" or "rekordbox" (either rekordbox format)
        // when every cue's evidence names that side, "unknown" when they
        // do not all name the same one or any names none.
        SideRole,
        ReasonRole,
        // "Pad 1: rekordbox 0:01.050, Engine 0:01.050", one line a cue.
        CuesRole,
        // "rekordbox" or "OneLibrary": the other side's name.
        RekordboxNameRole,
        // "" when nothing is staged; "known" when the cues whose side is
        // known are staged, each on its own side; "engine" or "rekordbox"
        // when the DJ picked that side for the unknown ones as well.
        StagedSideRole,
        // Some cues' side is known (one button moves them), some unknown
        // (the DJ picks a side for those; the known ones keep theirs).
        HasKnownRole,
        HasUnknownRole,
    };

    explicit CueOffsetListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setFindings(std::vector<domain::CueOffsetFinding> findings);
    const std::vector<domain::CueOffsetFinding> &findings() const { return m_findings; }
    void setStagedSide(int row, const QString &side);
    void clearStaged();
    void removeAt(int row);
    QString stagedSide(int row) const;
    // A row's identity across rescans: both rows of the pair.
    static QString keyOf(const domain::CueOffsetFinding &finding);
    int indexOfKey(const QString &key) const;
    int count() const { return static_cast<int>(m_findings.size()); }
    // Rows whose side is known and nothing is staged for: what "Move All
    // Known" stages.
    int knownUnstagedCount() const;
    int stagedCount() const;

signals:
    void countChanged();

private:
    std::vector<domain::CueOffsetFinding> m_findings;
    std::vector<QString> m_stagedSide;  // parallel to m_findings
};

QString sideName(domain::CueOffsetSide side);

}  // namespace seabass::gui
