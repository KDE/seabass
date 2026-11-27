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
// one change per playlist track that has rekordbox's analysis kept by
// Analyse again (the player's goes back with it where it is kept too; the
// player's kept alone, by Beat Grids & Waveforms, is that page's to
// restore and is not touched here), with the review of its cues (old grid the .DAT's now,
// new grid the kept one's) switched on where it follows on its own under
// the request's threshold. A track whose kept entry cannot be read is
// left out with the reason. Refused (`refusal`) when no track of the
// playlist has anything of Analyse again's kept. Reads the stick and writes nothing: not for
// the GUI thread. Throws application::OperationCancelled on a cancel.
PreparedAnalysisRestore prepareAnalysisRestore(const AnalysisRestoreRequest &request,
                                               const application::CancellationToken &cancel);

// How many of the playlist's tracks have rekordbox's original analysis
// kept by Analyse again, for the playlist menu: a track counts when a kept
// file of its name is in the rekordbox kept-analysis folder
// (keptAnalysisNamesOf()), nothing parsed, so
// it can count a kept entry prepareAnalysisRestore() then finds damaged or
// of another row. -1 when the stick cannot be read. Reads the stick: not
// for the GUI thread.
int countKeptOriginals(const AnalysisRestoreRequest &request, const application::CancellationToken &cancel);

// The file names in a stick's two kept-analysis folders, listed once and
// kept until forgetKeptAnalysisNames(): the menu asks each time it opens,
// and a folder of thousands of entries is not listed each time. Safe from
// any thread. A listing made while the names were forgotten is not kept.
struct KeptAnalysisNames
{
    std::set<std::string> rekordbox;
    std::set<std::string> engine;
};
std::shared_ptr<const KeptAnalysisNames> keptAnalysisNamesOf(const std::filesystem::path &stickRoot);
// Drops every stick's names: after a save (which may keep or restore), and
// after Free Up Space removed kept files.
void forgetKeptAnalysisNames();
// How many times the names were forgotten: an answer counted from names
// of an earlier generation is out of date.
std::uint64_t keptAnalysisNamesGeneration();

}  // namespace seabass::gui
