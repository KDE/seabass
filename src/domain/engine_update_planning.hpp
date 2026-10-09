// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "domain/rekordbox_baseline.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// What Sync after Rekordbox Export proposes for one stick: bring Engine in
// line with what rekordbox changed, decided per item by a three-way merge
// of the recorded baseline (B), rekordbox now (R) and Engine now (E). See
// docs/sync-after-rekordbox-export-plan.md, "The three-way rule" and "The
// fallback without a baseline".
//
//   R equals E                 nothing
//   B equals E, R differs      rekordbox changed it: apply to Engine, checked
//   B equals E, R lacks it,    the export dropped what Seabass wrote:
//     B says Seabass wrote it  restore onto rekordbox, checked
//   B equals R, E differs      Engine changed it: listed as Engine's own, kept
//   all three differ           conflict, unchecked, with the reason
//   no B for the item          the fallback: additions checked, anything
//                              destructive or unattributed a conflict
//
// Pure domain: the planner never reads a file. What only the stick can
// say (does a file exist, how many Engine playlists share a path, each
// Engine row's pdbImportKey) arrives in EngineUpdateInput as data.

// Why a row is in the proposal. Tests assert this; the page shows
// reasonText.
enum class EngineUpdateReason {
    None,
    // B equals E and R differs (the item is rekordbox's change).
    RekordboxAdded,
    RekordboxRemoved,
    RekordboxRenamed,
    RekordboxMoved,    // a member's place in a playlist
    RekordboxChanged,  // a rating or a comment
    // A track rekordbox lists in a playlist replaces another copy of the
    // same song in Engine's copy of it (sameSong()).
    SameSongCopy,
    // B equals E, R lacks a value B holds with Seabass origin: the export
    // regenerated the file and dropped Seabass's write. Goes back.
    ExportDropped,
    // B holds a value R lacks and nothing recorded who wrote it.
    OriginUnknown,
    // B equals R, E differs.
    EngineOwn,
    // B, R and E all differ.
    BothChanged,
    // No B for the item (the fallback).
    NoBaselineAddition,      // checked
    NoBaselineImportedRow,   // an Engine row with pdbImportKey != 0 rekordbox does not list: conflict
    NoBaselineEngineOnly,    // an Engine row with pdbImportKey 0, an Engine-only playlist: kept
    NoBaselineEngineMember,  // a member only Engine's playlist holds: conflict
    NoBaselineOrder,         // the two orders of one playlist differ: conflict
    NoBaselineValuesDiffer,  // a rating or a comment: conflict
    NoBaselineRenameGuess,   // an Engine-only playlist that looks like this one renamed: conflict
    // Refusals and things to fix elsewhere first.
    EnginePathAmbiguous,  // enginePlaylistCountAtPath != 1
    DuplicateEngineRows,  // two Engine rows for one file: Clean Up first
    FileNotOnStick,       // rekordbox lists a file the stick does not have
};

// What every row carries.
struct EngineUpdateItemHeader
{
    // The item this row is about (rekordbox_baseline.hpp's itemKey
    // grammar). Rows that share a key are one item and are ticked
    // together: a member moved within a playlist is a remove and an add,
    // and a copy swapped for another is the remove that rides with an add.
    std::string key;
    bool checkedByDefault = false;
    bool conflict = false;
    EngineUpdateReason reason = EngineUpdateReason::None;
    std::string reasonText;
    // Keys of rows this one cannot be applied without: a new track's
    // membership needs the track, a playlist's membership needs the
    // playlist's create or rename and its parents'. Unticking one of
    // these unticks this row.
    std::vector<std::string> dependsOn;
    // engineUpdateStateHash of rekordbox's state of the item: what a
    // declined item is recorded with in RekordboxBaseline::declined, and
    // what that record is compared against on the next plan.
    std::string rekordboxState;
};

struct TrackToAdd
{
    EngineUpdateItemHeader header;
    Track rekordbox;  // the row to copy; rating, comment and cues ride along
    std::string stickRelativePath;
    std::string pathKey;
};

struct TrackToRemove
{
    EngineUpdateItemHeader header;
    Track engine;
    std::string pathKey;
    int playlistCount = 0;  // how many Engine playlist entries hold it
};

struct PlaylistCreate
{
    EngineUpdateItemHeader header;
    std::uint32_t pdbId = 0;
    std::string path;  // "Folder/List", as rekordbox has it now
    bool folder = false;
};

struct PlaylistRename
{
    EngineUpdateItemHeader header;
    std::uint32_t pdbId = 0;
    // Engine's path at apply time: after the renames of its parents that
    // come before it in the proposal (parents first), so a writer applying
    // them in order finds it there.
    std::string fromPath;
    std::string toPath;
};

struct PlaylistDelete
{
    EngineUpdateItemHeader header;
    std::uint32_t pdbId = 0;
    std::string path;  // Engine's path at apply time, after the renames
    bool folder = false;
    int engineMembers = 0;
};

// One entry of an Engine playlist. Proposals list a playlist's removes
// before its adds, adds in rekordbox's order.
struct MembershipEdit
{
    enum class Kind { Add, Remove };

    EngineUpdateItemHeader header;
    Kind kind = Kind::Add;
    std::uint32_t pdbId = 0;
    std::string playlistPath;  // Engine's path at apply time: rekordbox's path now
    std::string pathKey;
    // Add: rekordbox's row (Engine's may not exist yet; the writer finds
    // it by file at apply time). Remove: Engine's row.
    Track track;
    // Add only: the member to insert after, by pathKey; empty for the
    // start of the playlist. The member before this one in rekordbox's
    // playlist that Engine holds or that an earlier add of this proposal
    // puts there. When that earlier add is left unticked the anchor is
    // missing at apply time; the writer then appends.
    std::string afterPathKey;
};

// A rating or a comment, either direction. Toward rekordbox only as a
// restore (ExportDropped), and only ratings: export.pdb has no room for
// comments.
struct MetadataEdit
{
    enum class Field { Rating, Comment };
    enum class Direction { ToEngine, ToRekordbox };

    EngineUpdateItemHeader header;
    Field field = Field::Rating;
    Direction direction = Direction::ToEngine;
    Track rekordbox;
    Track engine;
    std::string pathKey;
    std::optional<int> rating;  // the value to write, nullopt clears it
    std::string comment;
};

// Cues, either direction, as a SyncPlan that SyncPlanChange writes
// unchanged.
// TODO(step 3 of docs/sync-after-rekordbox-export-plan.md): the planner
// fills cuesToEngine and cuesToRekordbox; step 2 leaves both empty.
struct CueEdit
{
    EngineUpdateItemHeader header;
    SyncPlan plan;
};

// Any one write of the proposal, for a conflict's choices.
using EngineUpdateEdit =
    std::variant<TrackToAdd, TrackToRemove, PlaylistCreate, PlaylistRename, PlaylistDelete, MembershipEdit, MetadataEdit>;

// Something the planner cannot decide. Each choice is what staging it
// writes; an empty choice writes nothing (Engine keeps what it has).
// Both empty: nothing here can resolve it (two Engine rows for one file,
// an ambiguous Engine path), the reason says what to do instead.
struct EngineUpdateConflict
{
    EngineUpdateItemHeader header;
    std::string pathKey;  // for a track's items; empty for a playlist's
    std::string rekordboxSide;  // one line each, for the two buttons
    std::string engineSide;
    std::vector<EngineUpdateEdit> rekordboxChoice;
    std::vector<EngineUpdateEdit> engineChoice;
};

// Shown, never written.
struct EngineOwnItem
{
    EngineUpdateItemHeader header;
    Track engine;              // the track's items
    std::string playlistPath;  // the playlist's items
};

struct EngineUpdateProposal
{
    bool hasBaseline = false;
    std::uint64_t baselineSequence = 0;
    std::uint64_t currentSequence = 0;

    // In the staging order of the plan's "Writers": creates (parents
    // first), adds, renames (parents first), memberships, metadata, cues
    // to Engine, deletes (deepest first), removals, restores.
    std::vector<PlaylistCreate> playlistsToCreate;
    std::vector<TrackToAdd> tracksToAdd;
    std::vector<PlaylistRename> playlistsToRename;
    std::vector<MembershipEdit> membership;
    std::vector<MetadataEdit> metadataToEngine;
    std::vector<CueEdit> cuesToEngine;  // step 3
    std::vector<PlaylistDelete> playlistsToDelete;
    std::vector<TrackToRemove> tracksToRemove;
    std::vector<MetadataEdit> restoresToRekordbox;
    std::vector<CueEdit> cuesToRekordbox;  // step 3

    std::vector<EngineUpdateConflict> conflicts;
    std::vector<EngineOwnItem> engineOwnKept;
    // Adds refused because the file is not on the stick (FileNotOnStick),
    // never checked, said so.
    std::vector<TrackToAdd> notAdded;
    // Keys left out because the user declined them and rekordbox has not
    // changed them since, or because a row they depend on was left out.
    std::vector<std::string> declinedSuppressed;

    // Nothing to write and nothing to decide. Engine's own, the refusals
    // and the suppressed keys are information and do not count.
    bool empty() const;
    // Something to write or decide, and all of it cues: the page links to
    // Sync Cue Points.
    bool onlyCues() const;
};

// One Engine playlist or folder as the stick has it.
struct EnginePlaylistInfo
{
    std::string path;  // "Folder/List", as the Engine reader spells it
    bool folder = false;
    // enginePlaylistCountAtPath(path): more than one is ambiguous and
    // nothing at that path is edited.
    int countAtPath = 1;
};

struct EngineUpdateInput
{
    std::vector<Track> rekordbox;  // export.pdb's tracks with playlists and positions
    std::vector<Track> engine;     // rows with streamingSource are ignored
    std::vector<PlaylistInfo> rekordboxPlaylists;
    std::vector<EnginePlaylistInfo> enginePlaylists;
    std::optional<RekordboxBaseline> baseline;  // nullopt: the fallback for everything
    std::uint64_t currentSequence = 0;          // export.pdb's sequence now
    // Engine's Track.pdbImportKey by Engine sourceId. A row missing here is
    // treated as imported (not 0): unsure means conflict.
    std::map<std::string, std::int64_t> enginePdbImportKey;
    // As for baselineFrom: Track::filePath to the stick-relative path, and
    // that to the comparison key. Unset means identity.
    std::function<std::string(const std::string &)> stickRelativeOf;
    std::function<std::string(const std::string &)> pathKeyOf;
    // Whether the stick has the file at this stick-relative path. Unset
    // means it does.
    std::function<bool(const std::string &)> fileExists;
};

class EngineUpdatePlanner
{
public:
    static EngineUpdateProposal plan(const EngineUpdateInput &input);
};

// The hash recorded for a declined item (FNV-1a 64, 16 hex digits) of a
// canonical spelling of rekordbox's state of it. Stable across runs and
// builds, unlike std::hash.
std::string engineUpdateStateHash(std::string_view canonicalState);

}  // namespace seabass::domain
