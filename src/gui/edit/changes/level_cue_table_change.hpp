// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <string>
#include <vector>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Library Health's repair for #57: from each listed OneLibrary row's cue
// table, removes the cues its analysis file does not hold, with their hot
// cue bank links. Every other cue row stays as it is (colour, comment,
// bank links), and nothing else is written: not the analysis file the
// players read, not export.pdb. One change for every row staged, so the
// database is checked and written once however many rows there are.
//
// Each row is checked again at save time: one whose table is within its
// file by then, or whose file cannot be read, is left alone.
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

    LevelCueTableChange(QString pioneerRoot, std::vector<Row> rows);

    static QString idFor();

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    // Every staged row; those the save left alone, unitsSkipped().
    int unitsWritten() const override;
    int unitsSkipped() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    std::vector<Row> m_rows;
    int m_skipped = 0;
};

}  // namespace seabass::gui
