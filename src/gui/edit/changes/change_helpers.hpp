// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <vector>

#include "domain/library_consistency.hpp"
#include "domain/track_scope.hpp"
#include "domain/track.hpp"
#include <optional>

#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "gui/edit/format_write_session.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"

namespace seabass::gui
{

class SaveContext;

// The few naming helpers a change class and its controller both need.
// Everything else a change needs lives in that change's own source file.
//
// The keys matter beyond cosmetics: staging is keyed on PendingChange::id(),
// so a key that is not stable across a re-scan would stage the same repair
// twice, and the controller derives the same key when it asks whether an
// issue is already staged. One definition, used by both, keeps those two
// answers from drifting apart.

// Which catalog an issue came from. Every reader sets Track::format, and a
// consistency check only ever runs against one catalog, so the survivor or
// the first broken row name it identically.
QString issueFormat(const domain::LibraryConsistencyIssue &issue);

// Identifies an issue across re-scans: its format plus every row id it
// covers, survivor first.
QString issueKeyFor(const domain::LibraryConsistencyIssue &issue);

// Identifies one track's stray-cue issue: format plus row id.
QString junkKeyFor(const domain::Track &track);

// "3 hot, 1 memory (...)" -- the human summary of a cue set. Mirrors the
// command line's own wording so both report a sync identically.
QString describeCues(const std::vector<domain::CuePoint> &cues);

// What a write does, which decides which files it touches. rekordbox keeps
// cue data in per-track analysis files and rows in export.pdb, so a
// cue-only write never touches the catalog -- naming it anyway would put a
// file in the backup record that the save does not change, and Undo would
// then restore a file this save never touched.
struct WriteScope
{
    // Writes this track's cue data. Usually true; false for a change that
    // only rewrites catalog rows -- a Library Health repair with nothing
    // to merge onto the survivor removes rows without touching a single
    // analysis file, and naming one would be an over-declaration.
    bool cueData = true;
    // Also rewrites rows in the catalog itself (rekordbox's export.pdb).
    // A cue write does not: rekordbox keeps cue data in per-track analysis
    // files, so naming the catalog would put a file in the backup record
    // that the save never changes, and Undo would restore it.
    bool catalogRows = false;
    // Also writes the OneLibrary copy of the same library. Some workflows
    // mirror there and some do not, and it is not inferable from the
    // format -- Add Cue, Local Cue restore and stray-cue removal mirror;
    // Sync and duplicate-cue consolidation do not. Getting this wrong in
    // either direction is a real defect: too few leaves a file overwritten
    // with no backup, too many has Undo restore a file the save never
    // touched.
    bool oneLibraryMirror = false;
};

// Every file a write of `kind` to `track` under `root` would overwrite.
//
// Path resolution ONLY: no writers, no databases opened, no scratch copy.
// That is the whole reason it exists apart from each change's own
// makeContext(), which resolves the same paths but also constructs
// everything -- calling that from filesToBackup() would open every
// database before the save had decided to proceed.
//
// Keyed on domain::TrackId (format + sourceId) rather than a bare id.
// A sourceId is not unique across formats, and this function decides which
// file gets overwritten; the pair exists precisely so that cannot be got
// wrong silently.
//
// Returns nothing for an unusable sourceId rather than guessing a path
// from it -- apply() reports the bad id, and a wrong path here would back
// up one file while the save overwrote another.
std::vector<std::string> filesWrittenFor(WriteScope scope, const domain::TrackId &track, const QString &root,
                                         SaveContext &ctx);

// The save's one analysis-path index for this catalog, built by whichever
// change asks first and shared by the rest.
//
// Without it a rekordbox save parses the whole 1.4 MB export.pdb twice per
// item -- once to find the file to back up, once inside the cue writer --
// so a 200-item save parsed it 400 times. This makes it once.
//
// Returns nullptr when the database cannot be read at all, which leaves
// every caller on the old per-call lookup rather than failing the save.
const infrastructure::rekordbox::AnlzPathIndex *sharedAnlzPathIndex(SaveContext &ctx, const QString &pioneerRoot);

// The save's one OneLibrary writer for this database, built by whichever
// change asks first and shared by the rest.
//
// Nine call sites used to construct one per item. Each construction is
// cheap, but the first write through it opens two SQLCipher connections,
// and each open derives the key from a passphrase: 115 ms of CPU that a
// scratch copy or a faster disk does nothing for. That derivation is the
// single largest per-item cost in a save against a real stick. One writer
// per save pays it once.
//
// realStickRoot matters when the database being written is a scratch copy:
// its parent is a temp directory, not the stick, and every content-path
// lookup resolves against the real stick's layout. Pass it explicitly
// there.
infrastructure::onelibrary::OneLibraryCueWriter &sharedOneLibraryWriter(
    SaveContext &ctx, const std::string &pioneerRoot,
    const std::optional<std::string> &realStickRoot = std::nullopt);

// The save's one Engine cue writer for this library, for the call sites
// that would otherwise build one per item. Opening an Engine library is a
// full SQLite open plus schema detection, about 151 ms against a stick.
infrastructure::engine::LibdjinteropEngineCueWriter &sharedEngineCueWriter(SaveContext &ctx,
                                                                           const std::string &engineLibraryPath);

// The save's one write target for a catalog database -- the scratch
// decision, the backup and the commit -- for every change that writes it,
// whichever feature staged that change.
//
// Keyed on the database file, deliberately, and not on the feature.
// FormatWriteSession's own class comment has always promised that "every
// change of one save that writes the same catalog uses the same copy",
// but each feature kept a session inside its own per-feature context, so
// the promise held only within a feature. Two features writing one
// database in one save is not hypothetical: Clean Up scratches
// export.pdb while Restore Metadata writes a rating into it, and whoever
// commits second wins -- either the ratings vanish while the page reports
// them applied, or a save that landed reports failure. Sharing the
// session means there is one copy, one commit, and nothing to race.
//
// itemCountHint and label come from whichever change asks first, since
// the scratch decision is made once, at construction. That makes the
// hint a lower bound rather than a total, which only ever costs speed:
// too low a hint writes directly to a database that would have been
// faster to scratch. Correctness does not depend on it.
FormatWriteSession &sharedFormatWriteSession(SaveContext &ctx, const std::string &format,
                                              const std::string &catalogPath, int itemCountHint,
                                              const std::string &label);

}  // namespace seabass::gui
