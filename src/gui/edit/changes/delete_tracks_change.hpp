// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Deletes tracks that are in no playlist (Library Health, "Tracks not in
// any playlist") the way Clean Up deletes a duplicate: each file's row
// comes out of every library on the stick that lists it, and the file
// goes on the stick's pending-deletions list (Seabass/orphaned), naming
// the backup that still holds the rows, for Delete Orphaned Files to
// remove. One change for the whole selection, so each database is read
// once. Every file is checked again first: one that is in a playlist in
// any library by then is left alone and counted as skipped.
class DeleteTracksChange : public PendingChange
{
public:
    struct Entry
    {
        std::string filePath;
        std::string title;
        std::string artist;
        std::vector<domain::Track> rows;  // every library's row for the file
    };

    DeleteTracksChange(QString pioneerRoot, QString enginePath, std::vector<Entry> entries);

    static QString idFor();

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    int unitsWritten() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    bool has(const std::string &format) const;

    QString m_pioneerRoot;
    QString m_enginePath;
    std::vector<Entry> m_entries;
    int m_deleted = 0;
};

}  // namespace seabass::gui
