// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/track_metadata_probe.hpp"
#include "application/ports/track_tag_reader.hpp"
#include "domain/track_import_plan.hpp"
#include "gui/edit/changes/import_track_change.hpp"
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
    // Reads a file's cover for the libraries (ImportTrackChange::setCover).
    // Unset: imported tracks get no cover.
    CoverReader coverReader;
    // The tracks will be analysed before the save
    // (ImportTrackChange::setAnalysis): their rows are checked and
    // rehearsed with an analysis file's name in them.
    bool analysisPlanned = false;
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
    // The file is on the stick and where it was asked to be (listed, and
    // in the playlist if one was named): left out because there was
    // nothing to do, not because something could not be done. A file on
    // the stick that could not be listed or put into the playlist is not
    // marked.
    bool onStick = false;
};

// What the stick's libraries say about an audio file that is on it.
struct OnStickTrack
{
    // The libraries with a row for the file, and those of them that
    // already list it in the playlist asked about.
    QStringList listedIn;
    QStringList inPlaylist;
    // Those with more than one row for it: a playlist change finds a
    // track by its file, and leaves a library alone where that is not
    // one row.
    QStringList severalRowsIn;
    QString title;
};

// `file`: absolute, as the plan's existingPath spells it. `playlist`: by
// its full path.
using OnStickLookup = std::function<OnStickTrack(const std::string &file, const std::string &playlist)>;

struct StagedImport
{
    // Why nothing at all is staged, in words for the person: the stick's
    // libraries are not the ones named, or the playlist cannot take the
    // tracks in every library. Empty otherwise.
    std::string refusal;
    // Something the person should know about what is staged (the tracks
    // go to the end of the playlist, not to the place asked for). Empty
    // otherwise.
    std::string note;
    // In save order: the playlist made first when it has to be, then per
    // file its import and its playlist entry at the end, then one move of
    // the whole run to the place asked for, when one was.
    std::vector<std::shared_ptr<PendingChange>> changes;
    // The source files that are staged, in order.
    std::vector<std::string> staged;
    // The source files that are on the stick already and are staged to
    // go into the playlist, in order. Nothing of them is copied.
    std::vector<std::string> addedToPlaylist;
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
// The batch is also rehearsed against the stick's export.pdb in memory
// (ImportRehearsal), row after row in save order, so a track whose row
// the file cannot take once the ones before it are in is left out too.
//
// A comment too long for export.pdb's page is cut to its first 250
// characters rather than costing the track; the file's own tag is not
// touched.
//
// The playlist is asked of the stick as it is now (playlistWhereabouts,
// which reads each library: not for the GUI thread). An existing one has
// to be in every library the stick has, once, and be a playlist there
// rather than a folder; a new one is made at the
// top level in the libraries that lack it. Otherwise nothing is staged
// and `refusal` says why: tracks listed in a playlist in two libraries
// and not in the third is the disagreement this app is for removing.
//
// A file the plan found on the stick already (AlreadyOnStick) is not
// imported again, and with a playlist and `onStick` given it is put into
// the playlist like the others, at its place among them: by its row in
// every library, so only when every library has exactly one, and not a second
// time into a playlist every library already lists it in. Otherwise it
// is left out with the reason.
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
                              const std::string &date, const std::optional<ImportPlaylist> &playlist,
                              const OnStickLookup &onStick = {});

}  // namespace seabass::gui
