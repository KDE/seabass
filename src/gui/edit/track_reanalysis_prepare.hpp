// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/changes/reanalyse_track_change.hpp"
#include "gui/edit/track_import_prepare.hpp"

namespace seabass::gui
{

// Analyse again: the tracks of a playlist, analysed by Seabass from their
// audio files on the stick.
struct ReanalysisRequest
{
    // The stick, as a page knows it; either may be empty.
    QString rekordboxPath;
    QString enginePath;
    // The playlist by its path ("Folder/List"), as the libraries name it.
    std::string playlist;
    ImportAnalyser analyser;
    int analysisJobs = 2;
};

struct PreparedReanalysis
{
    // Why nothing can be analysed on this stick at all, in words for the
    // person. Empty otherwise.
    std::string refusal;
    // One change per track that took a new analysis, in playlist order.
    std::vector<std::shared_ptr<ReanalyseTrackChange>> changes;
    // The playlist's tracks (each file once), and those left as they are
    // with the reason, by file.
    int tracks = 0;
    std::vector<LeftOut> leftAlone;
};

// Reads the stick's libraries, finds every file the playlist holds in any
// of them, and analyses each one whose audio is on the stick (analyseFiles(),
// so the import's progress reports and cancel hold). A track is left as it
// is, with its reason, when its file is missing or cannot be decoded, is
// longer than the analysis takes, has no grid in the analysis, is an MP3
// whose lead-in cannot be read (its grid has no known place on rekordbox's
// timeline), or has no analysis in any library to replace. The key goes
// into the change only where the file's tags name none.
//
// A stick with a library that is there and cannot be read is refused: a
// track's analysis belongs in every library of a stick.
//
// Reads the stick and writes nothing: not for the GUI thread. Throws
// application::OperationCancelled on a cancel.
PreparedReanalysis prepareReanalysis(const ReanalysisRequest &request, const application::CancellationToken &cancel,
                                     const ImportProgressSink &progress = {});

}  // namespace seabass::gui
