// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cstdint>
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
};

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
PreparedImport prepareTrackImport(const ImportRequest &request, const application::CancellationToken &cancel);

}  // namespace seabass::gui
