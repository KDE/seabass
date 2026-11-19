// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <QString>
#include <QStringList>

#include "application/ports/analysis_writer.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// What each change of a Beat Grids & Waveforms save left on its row, by
// change id, for the page to say after the save: the save summary counts
// tracks, and the page says beat grids, waveforms and tracks completed.
// Written on the save's thread, read on the GUI thread once the save has
// finished; the page counts only the ids the session reports applied, so
// a change that was rolled back, or never reached, is never counted.
class AnalysisSaveTally
{
public:
    struct Row
    {
        bool grid = false;
        bool waveform = false;
        bool completed = false;
    };
    void record(const QString &changeId, Row row)
    {
        std::lock_guard lock(m_mutex);
        m_rows[changeId] = row;
    }
    std::optional<Row> rowFor(const QString &changeId) const
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_rows.find(changeId);
        return it == m_rows.end() ? std::nullopt : std::optional<Row>(it->second);
    }

private:
    mutable std::mutex m_mutex;
    std::map<QString, Row> m_rows;
};

// Beat Grids & Waveforms, one Engine row: rekordbox's analysis (its beat
// grid, its waveform, and for a row the player has not analysed, all it
// needs to load without analysis) through
// LibdjinteropEngineCueWriter::importAnalysis(), in the save's Engine write
// session like every other Engine write in a save. Staged by
// BeatGridsController for each ticked track its preview said would change.
//
// One change per track, for the reason FillSampleRateChange gives: the
// unit the summary counts and the one progress bar ticks is a track, a
// cancel stops between tracks and keeps the ones that landed, and a row the
// save finds changed since the preview (locked on the player meanwhile, or
// already the same) is that one track skipped, not the whole run.
//
// The writer checks the row again when the save reaches it, with the
// options the preview ran with: a refusal then writes nothing and is a
// skip.
//
// The overview file. A row's waveform lives twice: in the database and in
// "Database2/OverviewData/<uuid>/<id>.rgb" beside it, which the scratch
// copy of the database does not hold. So the file is written to the real
// library, during apply(), which is BEFORE the database's commit when the
// save writes through a scratch copy (the copy goes back onto the stick
// when the save finishes) and, in a direct write, before the analysed flag
// of a completed row (importAnalysis() writes the file before it raises
// the flag). Either way a row is never flagged analysed in the real
// database without its file. Every file is backed up before it is written:
// the one the preview named is declared in filesToBackup(), so the save's
// up-front backup takes it (or notes it absent, so Undo removes it again);
// one the preview did not foresee is backed up the same way by the sink
// before its first byte, and protected for the change. A change that fails
// takes its file back out with its rows; a save that fails puts every file
// back as it was (SaveContext::rollBackSave), so no new overview file
// outlives a failed save. A cancelled save keeps the rows that landed, and
// their files with them, as it keeps their database rows.
//
// The ledger (<stick>/Seabass/beat-grids.tsv, the record of the grids
// Seabass wrote) is written once per save, after the commit, with the
// fingerprint of every row whose change landed with its grid written or
// already the same; nothing when nothing landed. The first change of a
// batch declares m.db and the ledger as its backups, the ledger as absent
// when the stick has none yet, so Undo Last Save puts back the database,
// the ledger and the overview files together.
class CopyAnalysisChange : public PendingChange
{
public:
    // stickRoot: the stick's root (UTF-8), where the ledger lives.
    // `analysis` is rekordbox's, as the preview read it; `options` the
    // writer options the preview ran with (previewOnly is cleared here).
    // `overviewFile`: the overview file the preview said would be written,
    // from the Engine Library folder; empty for none.
    CopyAnalysisChange(QString enginePath, std::string stickRoot, application::BeatGridCopyItem item,
                       application::RekordboxAnalysis analysis, application::AnalysisImportOptions options,
                       std::string overviewFile, int itemCountHint, bool declaresFiles,
                       std::shared_ptr<AnalysisSaveTally> tally = {});

    static QString idFor(const std::string &engineTrackId);
    static QString backupLabel();

    QString id() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    void beforeSave(SaveContext &ctx) override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_enginePath;
    std::string m_stickRoot;
    application::BeatGridCopyItem m_item;
    application::RekordboxAnalysis m_analysis;
    application::AnalysisImportOptions m_options;
    std::string m_overviewFile;
    int m_itemCountHint = 1;
    bool m_declaresFiles = true;
    std::shared_ptr<AnalysisSaveTally> m_tally;
};

}  // namespace seabass::gui
