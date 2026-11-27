// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/changes/kept_analysis_save.hpp"
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
    // The stick's identity (EditSessionRegistry::libraryIdForPath()), which
    // the playlist menu's counts are kept under; empty: by its root.
    std::string stickId;
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
// one change per playlist track that has rekordbox's analysis kept by
// Analyse again (the player's goes back with it where it is kept too; the
// player's kept alone, by Beat Grids & Waveforms, is that page's to
// restore and is not touched here), with the review of its cues (old grid the .DAT's now,
// new grid the kept one's) switched on where it follows on its own under
// the request's threshold. A track whose kept entry cannot be read is
// left out with the reason. Refused (`refusal`) when no track of the
// playlist has anything of Analyse again's kept. A track whose grid was
// changed outside Seabass since its original was kept, or whose rows name
// other analysis files than the ones its original was kept from, is left
// out, with that reason, and never staged. Where rekordbox's libraries
// hold different cue points for a track, its review says why its cue
// points cannot go back (CueReview::cuesCannotMove) and its switch stays
// off. Reads the stick and writes nothing: not for
// the GUI thread. Throws application::OperationCancelled on a cancel.
PreparedAnalysisRestore prepareAnalysisRestore(const AnalysisRestoreRequest &request,
                                               const application::CancellationToken &cancel);

// How many of the playlist's tracks have rekordbox's original analysis
// kept by Analyse again, for the playlist menu: a track counts when a kept
// file of its name is in the rekordbox kept-analysis folder
// (keptAnalysisNamesOf()), nothing parsed, so it can count a kept entry
// prepareAnalysisRestore() then finds damaged or of another row, or
// leaves out because its rows name other analysis files now (knowing that
// would take parsing every kept entry and reading the rows' analysis
// paths, which the count is made to avoid). The
// playlist's names are kept for keptCountFromCache(). -1 when the stick
// cannot be read. Reads the stick: not for the GUI thread.
int countKeptOriginals(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel);

// The kept folders listed afresh, then, only when the rekordbox kept
// folder holds an entry, every playlist of the stick read once at the
// first stage of its catalogs (the rows and their playlists, which Browse
// has loaded already) and its tracks' names kept: for a stick Browse has
// just loaded, and after a save (an Undo puts kept files back without
// telling). With nothing kept, no library is read and every count is 0.
// countKeptOriginals() likewise reads no library with nothing kept. Reads
// the stick: not for the GUI thread.
void warmKeptCounts(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel);

// For a test: how many times the stick's catalogs were read for a count,
// a warm or a restore since the process started.
int keptCountCatalogReadsForTesting();

}  // namespace seabass::gui
