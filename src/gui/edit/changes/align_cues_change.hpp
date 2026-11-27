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

// One track's cue points moved onto the beats of its own library's grid
// (Maintain, "Align Cue Points"): the full cue list written again with the
// moved cues at their beats, the same "pass the complete replacement set"
// contract every other cue write here follows.
//
// `aligned` is the whole list, as the page worked it out from `track.cues`
// and the grid it read (domain::cuesAlignedToBeats), on the timeline of
// that grid: rekordbox's for a DeviceLibrary or OneLibrary row, Engine's
// own for an Engine row, whose cues as read (`track.cues`, on rekordbox's
// timeline) are `gridShiftMs` later. An Engine row is written as Engine
// stores it, with nothing taken off: the write then cannot depend on what
// the file's offset is taken to be at Save.
//
// Checked again at Save: a track whose cues on the stick are no longer the
// ones the page read is left alone and said so, never written over with
// the list as it was. A rekordbox cue that moves keeps its comment, its
// colour and a loop's length in beats (rekordbox::MovedCue).
//
// Its own editor ("align-cues"), not Library Health's: every change of
// that page carries a track's whole cue list as it was scanned, and two of
// them for one track in one save (a stray cue removed, the cues aligned)
// would each write its own list over the other's. A session takes one
// editor's changes at a time, so the two cannot meet.
class AlignCuesChange : public PendingChange
{
public:
    AlignCuesChange(QString path, domain::Track track, std::vector<domain::CuePoint> aligned, double gridShiftMs,
                    int moved);

    static QString idFor(const domain::Track &track);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    // The cue points (and loop ends) moved, not the cues written.
    int unitsWritten() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    QStringList targetPaths() const override { return {m_path}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_path;
    domain::Track m_track;
    std::vector<domain::CuePoint> m_aligned;
    double m_gridShiftMs = 0.0;
    int m_moved = 0;
    // 0 when the track was left alone (its cues had changed).
    int m_written = 0;
};

}  // namespace seabass::gui
