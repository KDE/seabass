// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>

#include <QString>
#include <QStringList>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Tells the Engine library that this stick's rekordbox library is already
// imported, so the player stops offering to import it over the top.
//
// One change, because it is one fact about the library rather than a
// per-track edit: a single number in Information, and a save summary
// reading "1 library marked" is exactly what happened.
class MarkRekordboxImportedChange : public PendingChange
{
public:
    MarkRekordboxImportedChange(QString enginePath, std::uint64_t librarySequence);

    static QString idFor();

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_enginePath;
    std::uint64_t m_librarySequence = 0;
};

}  // namespace seabass::gui
