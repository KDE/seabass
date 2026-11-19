// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include "application/use_cases/copy_beat_grids.hpp"
#include "domain/beat_grid.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Copy Beat Grids, one Engine row: rekordbox's beat grid into the row's
// adjusted grid (LibdjinteropEngineCueWriter::writeBeatGrid), through the
// save's Engine write session like every other Engine write in a save.
// Staged by BeatGridsController for each row its preview said a copy
// would write.
//
// One change per track, not one for the whole run, for the reason
// FillSampleRateChange gives: the unit the summary counts and the one
// progress bar ticks is a track, a cancel stops between tracks and keeps
// the ones that landed, and a row the save finds changed since the
// preview (locked on the player meanwhile, or already the same) is that
// one track skipped, not the whole run reported as done.
//
// The writer checks the row again when the save reaches it, with the
// options the preview ran with: a refusal then (locked, edited on the
// player, not analysed, already the same) writes nothing and is a skip.
//
// The ledger (<stick>/Seabass/beat-grids.tsv, the record of the grids
// Seabass wrote) is written once per save, after the commit, with the
// fingerprint of every row whose change landed as written or already the
// same; nothing when nothing landed. The first change of a batch declares
// m.db and the ledger as its backups, the ledger as absent when the stick
// has none yet, so Undo Last Save puts both back.
class CopyBeatGridChange : public PendingChange
{
public:
    // stickRoot: the stick's root (UTF-8), where the ledger lives.
    // `beats` are rekordbox's, as the preview read them. `ownFingerprint`
    // is what the ledger said for this track when the preview ran.
    CopyBeatGridChange(QString enginePath, std::string stickRoot, application::BeatGridCopyItem item,
                       std::vector<domain::Beat> beats, std::optional<std::string> ownFingerprint,
                       bool overwriteEngineEdits, int itemCountHint, bool declaresFiles);

    static QString idFor(const std::string &engineTrackId);
    static QString backupLabel();

    QString id() const override;
    QString owner() const override;
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
    std::vector<domain::Beat> m_beats;
    std::optional<std::string> m_ownFingerprint;
    bool m_overwriteEngineEdits = false;
    int m_itemCountHint = 1;
    bool m_declaresFiles = true;
};

}  // namespace seabass::gui
