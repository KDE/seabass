// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One track's 0:00 memory cue removed: the full cue list rewritten without
// it, the same "pass the complete replacement set" contract every other
// cue write here follows.
class RemoveJunkCueChange : public PendingChange
{
public:
    // `alsoRemove` names cues to strip that isJunkCue() does not cover:
    // the clustered hot cues (domain/clustered_cue.hpp), which sit past
    // the first second and so are invisible to that rule.
    //
    // Passed in rather than re-derived, and that is the point. This
    // class used to work out what to remove from the track alone, so a
    // cue the list had shown, the user had staged and the counter had
    // counted survived the rewrite -- the same shape as the 12 ms bug
    // its own comment records. What the rows named is what goes.
    RemoveJunkCueChange(QString path, domain::Track track, std::vector<domain::CuePoint> alsoRemove = {});

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    // Every stray cue on this track, not one: apply() rewrites the cue
    // list without any of them.
    int unitsWritten() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_path;
    domain::Track m_track;
    std::vector<domain::CuePoint> m_alsoRemove;
};

}  // namespace seabass::gui
