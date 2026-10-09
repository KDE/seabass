// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <string>

#include "application/ports/track_metadata_probe.hpp"
#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Sync after Rekordbox Export: one track rekordbox has and Engine does not,
// added to the Engine library as a row a player owns would hold it
// (infrastructure::engine::createEngineTrack): the rekordbox row's title,
// artist, BPM, key, duration, bitrate, rating, comment and size, its path
// relative to the stick's real Engine Library folder, its hot and memory
// cues at the file's own sample rate, its cover, and left for the player
// to analyse. Playlists are not this change's: EngineMembershipChange
// puts the new row into them, after this one in the same save (it finds
// the row by its file).
//
// The sample rate is read from the file inside apply(), through the
// TrackMetadataProbe handed in, never assumed: Engine keeps every cue as
// a sample offset, so a guessed rate puts every cue in the wrong place.
// When the probe cannot read the rate the change fails, naming the file,
// with nothing written; the save stops there (runSaveLoop), the changes
// staged before it having landed and the ones after it still pending.
//
// Writes the save's one Engine write session (sharedFormatWriteSession:
// a scratch copy of m.db when the save is big enough), backs up m.db,
// and refuses an Engine 1.x library. The cover goes under the real
// library's Artwork/ (or into the database, as the library keeps them),
// each file protected for this change before it is written
// (SaveContext::protectForThisChange), so a failed change takes it back
// out. Undo Last Save restores m.db; a cover file written by a save that
// is later undone stays in Artwork/, named by no row, as Repair Artwork's
// do.
class AddEngineTrackChange : public PendingChange
{
public:
    // `track`: the rekordbox row to copy (TrackToAdd::rekordbox), its
    // filePath absolute on the stick as it is mounted now. `probe` is
    // required (std::invalid_argument).
    AddEngineTrackChange(QString enginePath, domain::Track track,
                         std::shared_ptr<application::TrackMetadataProbe> probe, int itemCountHint);

    static QString idFor(const std::string &filePath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    // After an apply() that succeeded: the new row's id. -1 before, or
    // after one that failed.
    std::int64_t createdId() const { return m_createdId; }
    // Why the track came without the cover it was given, empty when it
    // has it or was given none. The row stays either way.
    const std::string &coverProblem() const { return m_coverProblem; }

private:
    QString m_enginePath;
    domain::Track m_track;
    std::shared_ptr<application::TrackMetadataProbe> m_probe;
    int m_itemCountHint = 1;
    std::int64_t m_createdId = -1;
    std::string m_coverProblem;
};

}  // namespace seabass::gui
