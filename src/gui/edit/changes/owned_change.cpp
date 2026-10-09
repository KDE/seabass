// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/owned_change.hpp"

#include <stdexcept>

namespace seabass::gui
{

QString rekordboxExportSyncOwner()
{
    return QStringLiteral("rekordbox-export-sync");
}

OwnedChange::OwnedChange(QString owner, std::unique_ptr<PendingChange> inner)
    : m_owner(std::move(owner)), m_inner(std::move(inner))
{
    if (m_owner.isEmpty()) {
        throw std::invalid_argument("OwnedChange: an owner is required");
    }
    if (m_owner.contains(QLatin1Char(':'))) {
        // The owner is the id's first ':' field; one with a ':' in it
        // would not read back as itself.
        throw std::invalid_argument("OwnedChange: an owner cannot hold a ':'");
    }
    if (!m_inner) {
        throw std::invalid_argument("OwnedChange: a change to wrap is required");
    }
}

QString OwnedChange::id() const
{
    return m_owner + QLatin1Char(':') + m_inner->id();
}

QString OwnedChange::description() const
{
    return m_inner->description();
}

QString OwnedChange::subject() const
{
    return m_inner->subject();
}

QString OwnedChange::unit() const
{
    return m_inner->unit();
}

int OwnedChange::unitsWritten() const
{
    return m_inner->unitsWritten();
}

int OwnedChange::unitsSkipped() const
{
    return m_inner->unitsSkipped();
}

QString OwnedChange::verb() const
{
    return m_inner->verb();
}

QStringList OwnedChange::formatsTouched() const
{
    return m_inner->formatsTouched();
}

QString OwnedChange::owner() const
{
    return m_owner;
}

std::vector<BackupTarget> OwnedChange::filesToBackup(SaveContext &ctx) const
{
    return m_inner->filesToBackup(ctx);
}

std::vector<RekordboxWrite> OwnedChange::rekordboxWrites() const
{
    return m_inner->rekordboxWrites();
}

void OwnedChange::beforeSave(SaveContext &ctx)
{
    m_inner->beforeSave(ctx);
}

ChangeOutcome OwnedChange::apply(SaveContext &ctx)
{
    return m_inner->apply(ctx);
}

std::unique_ptr<PendingChange> ownedByRekordboxExportSync(std::unique_ptr<PendingChange> change)
{
    return std::make_unique<OwnedChange>(rekordboxExportSyncOwner(), std::move(change));
}

}  // namespace seabass::gui
