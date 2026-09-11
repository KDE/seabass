#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace seabass::domain
{

// A hot cue or memory cue found on a track, normalized across the rekordbox
// and Engine library formats.
struct CuePoint
{
    enum class Kind { Memory, Hot };

    Kind kind = Kind::Memory;
    int hotCueNumber = 0;  // meaningful only when kind == Hot
    double positionMs = 0.0;
    std::string color;  // adapter-specific color representation (e.g. "#RRGGBB" or a named id)
    std::string comment;
    // True if this is a loop -- the player repeats between positionMs
    // ("loop-in") and loopEndMs ("loop-out") rather than sitting at a
    // single point. Orthogonal to kind: both rekordbox's memory-cue list
    // and its hot-cue list can hold a loop (confirmed via
    // specs/rekordbox_anlz.ksy's cue_extended_entry.type), matching
    // Engine's own separate hot-loop slots (djinterop::track::loops()).
    // On the hardware, a hot cue slot is either a cue or a loop, never
    // both -- callers that write hot cues/loops are expected to enforce
    // that themselves (see AddCueController). Appended after the
    // original fields (rather than inserted earlier) so existing
    // 5-positional-argument aggregate-inits across the test suite keep
    // mapping kind/hotCueNumber/positionMs/color/comment correctly and
    // just default isLoop/loopEndMs, instead of every one of them
    // needing an update for an unrelated field.
    bool isLoop = false;
    double loopEndMs = 0.0;  // meaningful only when isLoop
};

// One playlist a track belongs to, with its position within that specific
// playlist (0-based; -1 if the reader couldn't determine it), lets
// callers sort a playlist-filtered view back into its original order.
struct PlaylistMembership
{
    std::string name;  // full path, e.g. "Techno/Peak Time"
    int position = -1;
};

// One format's row for a file: which format wrote it, and its row id
// there. Not "which library" -- see Track::catalogRows.
struct CatalogRowRef
{
    std::string format;
    std::string sourceId;
};

// A track as read from either a rekordbox USB export or an Engine Library,
// normalized to a common shape. This is the shared intermediate
// representation the application layer's use cases operate on.
struct Track
{
    std::string sourceId;  // adapter-specific unique id (e.g. rekordbox track id, engine track id)
    // "rekordbox", "engine" or "onelibrary" -- which catalog this copy
    // was read from -- or "disk" for a file no catalog references at all.
    std::string format;

    // True for the "disk" case above: an audio file found on the stick
    // that no catalog mentions, built from its own tags rather than from
    // a database row (see application::findUnreferencedFiles). It has no
    // row, so sourceId is its path, and rating/comment/playCount are
    // always empty -- there is nowhere for them to have been stored.
    //
    // A bool rather than a `format == "disk"` test because two rules in
    // DuplicateCleanupPlanner turn on it and one of them ends in a file
    // being deleted: such a copy may only be the survivor when every
    // copy in the group is one too (keeping it over a catalogued copy
    // would leave that catalog pointing at a file we then deleted), and
    // "removing" it means deleting the file itself rather than dropping
    // a row. A misspelt format string would fail both silently.
    bool isUnreferenced = false;
    std::string title;
    std::string artist;
    std::string filename;
    std::string filePath;     // best-effort resolved absolute path to the audio file on disk, empty if unresolved
    std::string artworkPath;  // best-effort resolved path to a cover art image file, empty if unavailable
    // Non-empty (e.g. "TIDAL") if this track is a streaming-service link
    // rather than a local file, Engine only, set via a raw-SQL read of
    // Track.streamingSource (libdjinterop's public API doesn't expose
    // it). filePath is never meaningfully resolvable for these: it
    // points at a streaming-cache path on the computer that manages
    // playback, never at anything present on the stick itself. Callers
    // must never treat a track with this set as a real local file.
    // Never play it, merge it, sync it, or clean it up.
    std::string streamingSource;
    std::uint64_t fileSizeBytes = 0;  // best-effort size of the file at filePath on disk, 0 if unresolved/unreadable
    int bitrate = 0;  // kbps, 0 if unknown, used as the primary "which copy is higher quality" signal
    double durationSeconds = 0.0;

    // True when durationSeconds was not read but computed from bitrate
    // and stream size -- application::FileMetadata's own flag, carried
    // into the domain because the decision that needs it is here.
    // DuplicateTrackFinder groups on duration within a 2-second
    // tolerance and a guess can be seconds out, so a group holding an
    // estimate might not be one track at all; DuplicateCleanupPlanner
    // therefore proposes no file deletion at all from such a group.
    // Catalog rows carry a stored length and leave this false.
    bool durationIsEstimated = false;
    double bpm = 0.0;
    std::string key;  // human-readable, e.g. "Fm" or "F#m", empty if unknown
    std::vector<CuePoint> cues;

    // Normalized to 0-5 stars (Engine's own 0-100 scale is divided down by
    // readers before this is set), nullopt when unrated rather than 0 --
    // both formats use "no rating stored" and "explicitly rated at 0
    // stars" interchangeably at the storage level, and nullopt is the
    // more useful distinction for statistics ("how many tracks has this
    // DJ actually rated" vs. "how many are literally 0 stars").
    std::optional<int> rating;
    std::string comment;  // the DJ's own free-text comment field, empty if none
    // Album title, empty if the format did not record one or the track
    // has none. Held in a normalized table in both formats -- rekordbox's
    // export.pdb keeps an album_row per title with track rows carrying an
    // album_id, Engine stores it on the track -- so a reader has to
    // resolve it rather than read it off the row.
    std::string album;

    // Every row that points at this same file, in each format that has
    // one, this row included -- set by
    // application::collapseCatalogRows(), empty on anything that has not
    // been through it (every reader leaves it so, and an unreferenced
    // file has no rows in any format).
    //
    // DeviceLibrary (export.pdb), OneLibrary (exportLibrary.db) and
    // Engine (m.db) are not three libraries. They are ONE library
    // written three times, for three hardware products, and they are
    // meant to hold the same tracks; keeping them that way is what this
    // whole project is for. So a track appearing in all three is not
    // duplication, it is the normal, correct state -- what duplicates is
    // a FILE, when a re-export writes the same recording to disk again
    // and every format then lists both.
    //
    // Code that treats a row as a copy therefore sees one file as three
    // duplicates of itself. On a real stick that is 4369 rows for 1564
    // files, 1451 of them written in more than one format.
    //
    // The corollary is the other half of this field: removing a copy
    // means removing its row from every format listed here, because they
    // are supposed to keep saying the same thing. Leaving one behind
    // does not just orphan a row, it makes the formats disagree.
    std::vector<CatalogRowRef> catalogRows;

    // Every playlist this track belongs to. Best-effort: populated where
    // the reader supports it, empty otherwise.
    std::vector<PlaylistMembership> playlists;

    // Engagement signals used to prioritize which tracks are worth setting
    // cue points on. The two formats track different things, rekordbox
    // keeps a running play count, Engine (via libdjinterop) only exposes
    // the timestamp of the most recent play, so both are optional and
    // independent; a given Track will typically have at most one set,
    // depending on which format it came from.
    std::optional<int> playCount;
    std::optional<std::chrono::system_clock::time_point> lastPlayedAt;

    // When the authored fields above were last written, in seconds since
    // the epoch, 0 when the source cannot say.
    //
    // The last step of domain::metadata_merge's rule needs to know which
    // of two disagreeing copies is the more recent, and no catalog
    // records a per-track edit time. What a source can offer is the
    // granularity it actually has: the local metadata store knows
    // exactly, per row, because it wrote the row; a stick knows only
    // when its catalog files were last written, which is per stick.
    // Coarse but honest beats precise and invented. 0 never wins a
    // comparison, so a source that does not set this cannot overwrite
    // anything on the strength of a date it does not have.
    std::int64_t metadataModifiedAt = 0;
};

}  // namespace seabass::domain
