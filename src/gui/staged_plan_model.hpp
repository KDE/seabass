// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

namespace seabass::gui
{

// The slice of a plan list model that StagedCueEditController drives:
// enough to find a row's stable key, mark it staged, and drop it once its
// change reached the stick -- without the base controller ever learning
// whether the rows are duplicate-consolidation plans or cross-catalog
// sync plans.
//
// Deliberately not a QObject: ConsolidationPlanListModel and
// SyncPlanListModel already inherit QAbstractListModel, and QML talks to
// them through that. Only the controller base talks through this one, so
// it stays a plain abstract class with no metaobject of its own.
class StagedPlanModel
{
public:
    virtual ~StagedPlanModel() = default;

    virtual int planCount() const = 0;

    // A row's identity across rescans -- what StagedCueEditController
    // files its staged changes under. Must stay the same for the same
    // underlying group/target even when the row's index moves, since a
    // rescan rebuilds the list while staged changes outlive it.
    virtual QString planKeyAt(int index) const = 0;

    virtual void setStaged(int index, bool staged, const QString &description) = 0;
    virtual void removePlanAt(int index) = 0;

    // Drops every row's staged mark without removing any row -- what a
    // session-wide discard leaves behind.
    virtual void clearStaged() = 0;
};

}  // namespace seabass::gui
