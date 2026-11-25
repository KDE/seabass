// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/track_metadata_probe.hpp"
#include "application/ports/track_tag_reader.hpp"
#include "domain/track_import_plan.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Where an import goes: the stick as it is mounted now and the libraries
// it has.
struct ImportTarget
{
    QString stickRoot;
    // Empty for a library the stick does not have.
    QString pioneerRoot;
    QString enginePath;
    // "rekordbox", "onelibrary", "engine": every library on the stick.
    QStringList libraries;
};

// The playlist the imported tracks go into, by its full path.
struct ImportPlaylist
{
    std::string path;
    // Made by the save when no library has it yet.
    bool create = false;
    // Where the first track goes, from 0; the rest follow it. None: at
    // the end.
    std::optional<int> atIndex;
};

// A file the plan would import that is left out of the save, and why, in
// words for the person who offered it.
struct LeftOut
{
    std::string sourceFile;
    std::string why;
};

struct StagedImport
{
    // In save order: the playlist made first when it has to be, then per
    // file its import and its playlist entry.
    std::vector<std::shared_ptr<PendingChange>> changes;
    // The source files that are staged, in order.
    std::vector<std::string> staged;
    std::vector<LeftOut> leftOut;
};

// Turns the plan's Import entries into the changes of one save. For each
// file its tags and its audio properties are read from the computer, the
// one description is made (domain::describeImportedTrack), and what any
// library on the stick would refuse a row for is asked now
// (ImportTrackChange::rowRefusal): a failed change takes the whole save
// back, so a file a library cannot list is left out here, with its
// reason, instead of failing the save for the others.
//
// A comment too long for export.pdb's page is cut to its first 250
// characters rather than costing the track; the file's own tag is not
// touched.
//
// Entries with any other verdict are not this function's: the plan
// already says what happens to them.
//
// `date`: the day of the import, "YYYY-MM-DD". Throws
// std::invalid_argument for a target without libraries, a date that is
// not a day, or a place in the playlist below 0. A file whose place on
// the stick is taken by now is left out.
StagedImport stageTrackImport(const domain::ImportPlan &plan, const ImportTarget &target,
                              application::TrackTagReader &tags, application::TrackMetadataProbe &probe,
                              const std::string &date, const std::optional<ImportPlaylist> &playlist);

}  // namespace seabass::gui
