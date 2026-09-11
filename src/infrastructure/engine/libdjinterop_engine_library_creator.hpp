// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"

namespace seabass::infrastructure::engine
{

// Which Engine OS/DJ software generation a freshly created library
// declares itself as. Real hardware firmware compatibility with a given
// schema generation varies by unit and firmware version in ways this
// project has no documented, verified matrix for -- exposed as a real,
// user-visible choice rather than a silent guess, so a mismatch on real
// hardware can be fixed by trying another generation instead of needing
// code changes. See EngineLibraryCreator's own class comment.
enum class EngineSchemaGeneration
{
    V1,  // Engine OS 1.x / older standalone hardware generations
    V2,  // Engine OS 2.x
    V3,  // Engine OS 3.x / newest Engine DJ desktop and hardware generations
};

struct EngineLibraryCreationResult
{
    int tracksCreated = 0;
    int tracksSkipped = 0;  // e.g. no resolved local file to reference
    int cuesCopied = 0;
    int tracksTotal = 0;       // what was asked for, created or not
    bool cancelled = false;    // stopped via the token; nothing was written to `directory`
    std::string errorMessage;  // empty on success
};

// Creates a brand-new Engine Library database from scratch (via
// djinterop::engine::create_database()) and populates it from an
// already-scanned track list, typically read from a rekordbox export on
// the same stick. This is the first feature in this codebase that
// fabricates an entire new database + directory structure rather than
// modifying an existing, already-recognized one -- see
// docs/experimental-features.md for why it's gated as experimental.
//
// Deliberately narrow for this first version. Carried over: title,
// artist, BPM, key (parsed via rekordbox_key_parser.hpp), duration,
// bitrate, rating, comment, file size, hot + memory cues (the same cue
// conversion LibdjinteropEngineCueWriter uses, applied directly to each
// track's own snapshot before it's created -- see the .cpp's own comment
// for why that matters for a bulk import specifically), and a *simple*,
// approximate two-point beatgrid computed from BPM and duration alone
// (assumes the track starts exactly on a downbeat at sample 0 -- not
// always true, but a reasonable approximation absent a real per-beat
// grid read from rekordbox's own analysis data, which this project
// doesn't parse yet despite the underlying Kaitai spec already defining
// that section).
//
// Deliberately NOT carried over in this version: album art (libdjinterop's
// own album_art API is unfinished -- see its header, marked "TODO -
// implement rest of album_art class" -- and track_snapshot has no
// artwork field at all), a real per-beat grid, waveform data (reading
// rekordbox's own waveform preview already works elsewhere in this
// project, but no rekordbox->Engine waveform format conversion exists
// yet), and playlists.
class EngineLibraryCreator
{
public:
    // directory: where to create the new "Engine Library" folder,
    // typically the stick's own root (sibling to "PIONEER"/"Contents").
    // Refuses (returns a populated errorMessage, creates nothing) if a
    // database already exists there -- this never overwrites an existing
    // Engine Library.
    //
    // reporter: two sequential phases, each its own start()/tick()/finish()
    // run -- "Creating Engine Library" (once per track, against a fast
    // local scratch copy of the database) then "Copying to stick" (the one
    // pass that actually writes to `directory`, see the .cpp's own comment
    // for why the database isn't built there directly). A library of any
    // real size takes long enough that silently doing nothing visible looks
    // indistinguishable from a genuine hang. Defaults to NullProgressReporter
    // for callers (tests) that don't care.
    //
    // cancel: checked between two tracks of the first phase only. A
    // cancel there throws the scratch build away and returns with
    // `cancelled` set and nothing created at `directory`; the second
    // phase is one copy and runs to its end once started.
    static EngineLibraryCreationResult create(const std::string &directory,
                                               const std::vector<domain::Track> &tracks,
                                               EngineSchemaGeneration schemaGeneration,
                                               application::ProgressReporter &reporter =
                                                   application::NullProgressReporter::instance(),
                                               const application::CancellationToken &cancel =
                                                   application::CancellationToken::none());
};

}  // namespace seabass::infrastructure::engine
