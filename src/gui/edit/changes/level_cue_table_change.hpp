// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <string>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Library Health's repair for #57: one OneLibrary row whose cue table
// holds cues its analysis file does not gets the file's cues in its
// table. Only the table is written; the analysis file (what the players
// read) and export.pdb stay as they are. The row is checked again at save
// time and left alone when its table is within its file by then, or the
// file cannot be read: an empty table is not what this repair makes out
// of a table that is the only copy of a track's cues.
class LevelCueTableChange : public PendingChange
{
public:
    struct Row
    {
        int64_t contentId = 0;
        std::string filePath;
        std::string title;
        double bpm = 0.0;  // for the cue tolerance, as the check used it
    };

    LevelCueTableChange(QString pioneerRoot, Row row);

    static QString idFor(int64_t contentId);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    Row m_row;
};

}  // namespace seabass::gui
