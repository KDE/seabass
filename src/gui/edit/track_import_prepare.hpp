// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/track_import_staging.hpp"

namespace seabass::gui
{

// What a person offered for import, and where it should go.
struct ImportRequest
{
    // Files and folders on the computer, absolute. A folder stands for
    // every audio file in it and below it, by path.
    std::vector<std::string> offered;
    // The stick, as a page knows it: its PIONEER folder and its Engine
    // Library folder; either may be empty.
    QString rekordboxPath;
    QString enginePath;
    // None: the tracks go into the libraries and into no playlist.
    std::optional<ImportPlaylist> playlist;
    // Source files the plan called a possible duplicate, to import all
    // the same.
    std::set<std::string> importAnyway;
    // The day of the import, "YYYY-MM-DD".
    std::string date;
    // Reads a file's cover (ImportTrackChange::setCover). Unset: no
    // covers.
    CoverReader coverReader;
    // Analyses each file that is staged (ImportTrackChange::setAnalysis),
    // once everything else is settled. Unset: the
    // tracks go in unanalysed.
    ImportAnalyser analyser;
    // How many files are analysed at once. Each holds its whole audio in
    // memory while it is (about 10 MB a minute of music, and as much
    // again in the analyser).
    int analysisJobs = 2;
};

struct PreparedImport
{
    // Why nothing can be imported onto this stick at all, in words for
    // the person. Empty otherwise.
    std::string refusal;
    // The changes of the save, and the files they carry.
    StagedImport staged;
    // Every offered file that is not staged, with its reason, in the
    // order offered.
    std::vector<LeftOut> notImported;
    // Those among them the plan called a possible duplicate: the person
    // may ask for them all the same (ImportRequest::importAnyway).
    std::vector<std::string> possibleDuplicates;
    std::uint64_t bytesToCopy = 0;
    // Of the staged tracks, how many have an analysis and how many a
    // beat grid in it.
    int analysed = 0;
    int withGrid = 0;
    // The staged files whose grid follows a moving tempo, by name: the
    // ones to look at.
    std::vector<std::string> followedGrids;
};

// How far the preparing of an import is, for a bar that is not in the
// way: which step, and how many of its files are done. `total` 0 for a
// step that cannot count ahead.
struct ImportProgress
{
    enum class Step {
        // The stick's libraries and files are read.
        Stick,
        // The offered files are found and their tags read.
        Files,
        // The plan is made and rehearsed against the stick's libraries.
        Check,
        // The staged files are decoded and analysed: beat grid and
        // waveform.
        Analysis,
    };
    Step step = Step::Stick;
    std::size_t done = 0;
    std::size_t total = 0;
    // The file in hand, by its name; empty between files. In the
    // analysis, where several are in hand at once: the one last done.
    std::string file;
    // Every file in hand now, by name: one while files are read, as many
    // as there are workers in the analysis.
    std::vector<std::string> inHand;
};

// Called on the thread that prepares, often, and never from two threads
// at once: a sink for another thread passes it on itself.
using ImportProgressSink = std::function<void(const ImportProgress &)>;

// From what was dropped to the changes of one save: the offered folders
// are walked, each file's tags read, the stick's libraries and audio
// files read, the plan made (domain::planTrackImport) and its imports
// staged (stageTrackImport). Reads the computer and the stick and writes
// neither: not for the GUI thread.
//
// A stick with a library that is there and cannot be read takes no
// import: a track belongs in every library of a stick, and one that
// cannot be read cannot be written with any confidence either.
//
// Throws application::OperationCancelled on a cancel.
PreparedImport prepareTrackImport(const ImportRequest &request, const application::CancellationToken &cancel,
                                  const ImportProgressSink &progress = {});

}  // namespace seabass::gui
