// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
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
    int playlistsCreated = 0;  // folders included; they are playlists too
    int artworkCopied = 0;     // tracks that ended up with a cover
    int tracksLeftForDeviceAnalysis = 0;  // tracks the player is asked to analyse itself
    int tracksTotal = 0;       // what was asked for, created or not
    // The rekordbox sequence this library was recorded as imported from
    // (see create()'s rekordboxLibrarySequence). False when none was
    // given, or when the schema has no place for it -- in which case a
    // player may offer to import the rekordbox library on first insert.
    bool rekordboxImportRecorded = false;
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
// that section), and the source's playlists, rebuilt with their folder
// structure and each track at the position the source recorded.
//
// Cover art comes across too, written the way Engine stores it rather
// than through libdjinterop, whose album_art API is unfinished (its
// header is marked "TODO - implement rest of album_art class") and whose
// track_snapshot has no artwork field at all: the image goes under the
// library's own Artwork/ folder, named by its hash, with a matching
// AlbumArt row. The source is the cover the rekordbox side already
// resolved, which is a file on the same stick.
//
// Deliberately NOT carried over in this version: a real per-beat grid,
// and waveform data (reading rekordbox's own waveform preview already
// works elsewhere in this project, but no rekordbox->Engine waveform
// format conversion exists yet). Rather than leave the player with an
// empty waveform for ever, the created tracks are handed to it the way
// Engine's own rekordbox import hands over its own: marked unanalysed,
// with the analysis columns empty, so the player runs its own analysis
// and draws the waveform with Denon's algorithms. The beatgrid is part
// of that analysis and is recomputed with it.
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
    //
    // rekordboxLibrarySequence: the sequence export.pdb carried when
    // `tracks` were read from it (issue #42). A library made from that
    // export IS an import of it, and Engine records the sequence it last
    // imported in Information.lastRekordBoxLibraryImportReadCounter; left
    // at libdjinterop's 0, a player offers on the very first insert to
    // import the rekordbox library over the one just created. Measured:
    // TESTRIG_2's export at 513, a library created from it at 0.
    static EngineLibraryCreationResult create(const std::string &directory,
                                               const std::vector<domain::Track> &tracks,
                                               EngineSchemaGeneration schemaGeneration,
                                               application::ProgressReporter &reporter =
                                                   application::NullProgressReporter::instance(),
                                               const application::CancellationToken &cancel =
                                                   application::CancellationToken::none(),
                                               std::optional<std::uint64_t> rekordboxLibrarySequence = std::nullopt);
};

}  // namespace seabass::infrastructure::engine
