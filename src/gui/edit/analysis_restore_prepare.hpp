// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/changes/restore_analysis_change.hpp"
#include "gui/edit/track_import_staging.hpp"

namespace seabass::gui
{

// Restore original analysis: the tracks of a playlist whose analysis by
// rekordbox or the player is kept on the stick (Analyse again, Beat
// Grids & Waveforms), each given it back.
struct AnalysisRestoreRequest
{
    QString rekordboxPath;
    QString enginePath;
    // The playlist by its path ("Folder/List"), as the libraries name it.
    std::string playlist;
    // As ReanalysisRequest's: a track whose cue moves back are all under
    // this many ms has them follow without being asked; 0 asks about every
    // track.
    int cueFollowThresholdMs = 0;
};

struct PreparedAnalysisRestore
{
    // Why nothing can be restored on this stick at all, in words for the
    // person. Empty otherwise.
    std::string refusal;
    // One change per track with something kept, in playlist order.
    std::vector<std::shared_ptr<RestoreAnalysisChange>> changes;
    // The playlist's tracks (each file once), and those with something
    // kept that cannot be given it back, with the reason.
    int tracks = 0;
    std::vector<LeftOut> leftAlone;
};

// Reads the stick's libraries and its kept analyses, and stages nothing:
// one change per playlist track that has rekordbox's analysis or the
// player's kept, with the review of its cues (old grid the .DAT's now,
// new grid the kept one's) switched on where it follows on its own under
// the request's threshold. A track whose kept entry cannot be read is
// left out with the reason. Reads the stick and writes nothing: not for
// the GUI thread. Throws application::OperationCancelled on a cancel.
PreparedAnalysisRestore prepareAnalysisRestore(const AnalysisRestoreRequest &request,
                                               const application::CancellationToken &cancel);

// How many of the playlist's tracks have an original analysis kept (the
// changes prepareAnalysisRestore() would stage, counted without the cue
// reviews). Same rules; -1 when the stick cannot be read.
int countKeptOriginals(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel);

}  // namespace seabass::gui
