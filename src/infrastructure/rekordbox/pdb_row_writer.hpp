// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace seabass::infrastructure::rekordbox
{

// Mutates a rekordbox export.pdb file's row-presence bits and select
// fixed-size row fields -- see specs/rekordbox_pdb.ksy for the format.
//
// Clearing a row's presence bit is the format's own real deletion
// mechanism, not a workaround: the spec's own doc comment on that bit
// says "Will be false if the row has been deleted." Rows are never
// moved or reflowed on delete (the spec describes real deletions
// leaving heap "gaps"), so every edit here is a precise, bounded
// overwrite of a handful of already-existing bytes -- never a
// structural change to the file.
//
// All edits happen against an in-memory copy of the file, read once at
// construction; the real file on disk is never opened for writing at
// all until commit(), which writes the edited copy to a temp file next
// to the original and atomically renames it into place. If commit() is
// never called (an exception, a caller deciding to abort, a crash), the
// original file is completely untouched.
//
// Hardening beyond the core edit path:
//  - the constructor rejects a file that doesn't even look like a real
//    export.pdb (implausible len_page/num_tables) before trusting any
//    offset computed from it;
//  - every byte read/written goes through little_endian.hpp's
//    bounds-checked accessors, so a corrupt/truncated file fails with a
//    clear exception instead of undefined behavior;
//  - commit() refuses (leaving the real file untouched) if the file on
//    disk has changed size or mtime since construction -- something
//    else touched it in the meantime, so our in-memory copy is stale;
//  - commit() re-parses the fully edited buffer with the real parser
//    before ever writing it anywhere, refusing if that throws -- a bug
//    in this class should never reach disk;
//  - the temp file is fsync()'d (FlushFileBuffers on Windows) before
//    the rename, so the edit is actually durable on the physical medium
//    the moment commit() returns true, not just sitting in a write-back
//    cache a pulled USB stick could lose.
// appendPlaylistEntries() or removePlaylistEntries() could not place the
// playlist's rows the way rekordbox would; nothing was changed.
class PdbPageFull : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class PdbRowWriter
{
public:
    // exportExt.pdb is the same container with a different table-type
    // enum and its rows parsed through body_ext() rather than body().
    // The kaitai parser takes that as a construction flag, so every
    // parse this class makes has to agree with the file it opened;
    // getting it wrong does not fail loudly, it simply finds no rows.
    enum class Format
    {
        Export,     // export.pdb
        ExportExt,  // exportExt.pdb: My Tags and their categories
    };

    explicit PdbRowWriter(std::string pdbPath, Format format = Format::Export);

    // True if a present track row with this id exists.
    bool trackExists(uint32_t trackId) const;

    // Clears the presence bit for the track row with this id in the
    // tracks table. Returns false if no such (present) row is found.
    bool removeTrack(uint32_t trackId);

    // Overwrites an existing playlist_entry row's track_id field in
    // place, leaving its entry_index/playlist_id untouched. Returns
    // false if no (playlistId, oldTrackId) row is found.
    bool repointPlaylistEntry(uint32_t playlistId, uint32_t oldTrackId, uint32_t newTrackId);

    // Copies key/tempo/artwork_id directly from donorTrackId's row onto
    // targetTrackId's row, for whichever of copyKey/copyTempo/
    // copyArtwork is true -- used to fill in a Clean Up survivor's
    // missing bpm/key/artwork from another copy in its duplicate group
    // (see domain::DuplicateCleanupPlan). Returns how many fields were
    // actually copied. Throws if either track id doesn't exist.
    size_t copyTrackFieldsIfMissing(uint32_t donorTrackId, uint32_t targetTrackId, bool copyKey, bool copyTempo,
                                     bool copyArtwork);

    // Overwrites a track row's rating (0 to 5 stars) in place. One
    // byte, already present in every track row, so this is a bounded
    // field overwrite like copyTrackFieldsIfMissing rather than
    // anything structural. Returns false if no track with this id
    // exists; throws if the rating is outside 0..5.
    bool setTrackRating(uint32_t trackId, int rating);

    // Overwrites a track row's play count in place: a u2 in the row's
    // fixed part, the same one-field overwrite as setTrackRating. Clamped
    // to the field's 0..65535 rather than wrapped, so a large merged count
    // reads as large rather than as small. Returns false if no track row
    // with this id exists.
    bool setTrackPlayCount(uint32_t trackId, int playCount);

    // A track's free-text fields, to be written into the row's existing
    // device_sql_string spans in place -- see overwriteTrackText()'s own
    // doc comment for how each is fit into its field's fixed byte budget.
    struct TrackTextOverride
    {
        std::string title;
        std::string comment;
        std::string filename;
        std::string filePath;
    };

    // Overwrites title/comment/filename/file_path on the track row with
    // this id, each re-encoded into the *exact* on-disk byte span its
    // current value already occupies: the containing device_sql_string's
    // own length/kind header is never touched, so (like every other edit
    // in this class) the row is never resized or reflowed. Text longer
    // than the available span is truncated to fit; shorter text is
    // right-padded with ASCII spaces. All four fields are always
    // overwritten -- pass a field's current value if you don't want it
    // to change. Returns false if no track with this id exists.
    bool overwriteTrackText(uint32_t trackId, const TrackTextOverride &text);

    // The track row's other free-text slots. Kept apart from
    // TrackTextOverride because those four are the ones a caller
    // normally wants to set, while these exist only to be emptied --
    // adding them to that struct would silently blank them for every
    // existing caller.
    //
    // mix_name in particular held "Extended Mix", "Original Mix" and the
    // like on every track of a real 1,161-track export that had been
    // through every other scrub in this class.
    struct TrackExtraTextOverride
    {
        std::string isrc;
        std::string texter;
        std::string message;
        std::string mixName;
    };

    // Same byte-length-preserving overwrite as overwriteTrackText(), on
    // ofs_strings slots 0, 1, 5 and 12. Returns false if no track with
    // this id exists.
    bool overwriteTrackExtraText(uint32_t trackId, const TrackExtraTextOverride &text);

    // Zeroes every byte of every data page that no *present* row
    // occupies, and returns how many bytes that was.
    //
    // Overwriting a row leaves the old bytes where they were: rekordbox
    // marks the row not-present and writes the new one elsewhere in the
    // page, so a pdb carries the text of everything it has ever held. On
    // a real 1,161-track export one title appeared eleven times where
    // three would do, and 1,255 readable fragments of the real library
    // survived a scrub that had correctly rewritten every live row.
    //
    // Nothing reads this space -- it is free by the format's own
    // accounting -- so clearing it costs nothing and is the only way to
    // stop a copied pdb carrying its history. Call it after every edit:
    // it works from where rows are now, so anything written afterwards
    // would be missed.
    //
    // Bounds come from the format rather than from guesswork: the heap
    // starts at the page's heap_pos and ends where the row-index groups
    // begin (len_page - num_row_groups * 0x24), and a row's extent runs
    // to the next row's start. That last part is an upper bound rather
    // than the true length, so this under-clears rather than over-clears
    // -- it can leave a few bytes of a dead row's tail inside a live
    // row's extent, and can never truncate a live row.
    int zeroUnusedSpace();

    // Overwrites an artist_row's name field the same way (near/far
    // offset per specs/rekordbox_pdb.ksy's artist_row -- see the .cpp).
    // Returns false if no artist with this id exists.
    bool overwriteArtistName(uint32_t artistId, const std::string &text);

    // Overwrites a playlist_tree_row's name field the same way. Returns
    // false if no playlist/folder with this id exists.
    bool overwritePlaylistName(uint32_t playlistId, const std::string &text);

    // The pdb's other name tables. Tracks, artists and playlists have a
    // method each above because a caller picks which one by id; these
    // three have no such caller -- every row in them is a name and every
    // one of them has to go -- so one method covers all three.
    //
    // They existed unscrubbed for as long as this writer has: a rekordbox
    // export anonymized by every method above still shipped the album
    // title, genre and record label of all 1,161 tracks, because nothing
    // here could reach them and the reader-based checks only ever sampled
    // title, artist and path. Found by the byte sweep in
    // anonymization_byte_sweep.hpp.
    //
    // keys and colors are deliberately not here: "Am" and "Red" say
    // nothing about whose library this is.
    enum class NameTable
    {
        Genres,
        Albums,
        Labels,
        Artists,
        Playlists,
    };

    // Every My Tag and tag category name in an exportExt.pdb, replaced
    // with placeholder(index), byte length preserved like the rest.
    // Returns how many rows were rewritten; refuses (returns 0) on a
    // writer opened as Format::Export, since the rows it would look for
    // cannot be there.
    //
    // Categories and tags are rewritten alike and the caller cannot tell
    // them apart from the index. That is deliberate: which of the two a
    // row is says nothing a fixture needs, and a placeholder that
    // announced "CATEGORY" would leak the structure it was hiding.
    // `rowsLeftAlone` receives the number of present tag rows this could
    // NOT rewrite -- an offset that leaves its page, a field with no
    // capacity, a placeholder the field cannot represent.
    //
    // Required, with no default, and that is the point. A row skipped
    // here keeps the name it already had, and in an anonymiser that is a
    // My Tag a DJ typed; the return value counts rows REWRITTEN, so 27
    // of 28 reads as a positive number while one real name goes out with
    // the export. A comment telling callers to look at it is a note, not
    // a guarantee (docs/write-path-rules.md), and it had already failed
    // to be one: tools/anonymize_export_ext.cpp took the default, said
    // "rewrote N tag name(s)", and regenerated the committed fixture
    // with whatever it could not touch left in.
    //
    // Taking it away means every caller has to decide, and a new one
    // cannot fail to by doing nothing.
    int overwriteAllTagNames(const std::function<std::string(size_t index)> &placeholder, int *rowsLeftAlone);

    // Replaces the name in every present row of `table` with
    // placeholder(index), and returns how many rows were rewritten.
    // Like the overwrites above this preserves each field's on-disk byte
    // length, so a placeholder longer than the name it replaces is
    // truncated to fit.
    //
    // `rowsLeftAlone` is set to how many present rows it did NOT rewrite,
    // for the reasons overwriteAllTagNames() gives -- a name offset that
    // leaves its page, a field with no capacity, a placeholder the field
    // cannot represent -- and it is required for the reason given there.
    // Each of those rows keeps its real album, genre, label, artist or
    // playlist name. The bound that skips the first of them was added
    // here without it, so a row it refused went into the export real and
    // was counted nowhere; the anonymizer's own report said nothing, and
    // only the verifier's byte sweep, if it knew the name, could object.
    int overwriteAllNames(NameTable table, const std::function<std::string(size_t index)> &placeholder,
                          int *rowsLeftAlone);

    // For every playlist_entry row currently pointing at oldTrackId:
    // if that same playlist already has an entry for newTrackId,
    // removes the oldTrackId entry (avoids a duplicate); otherwise
    // repoints it to newTrackId in place, so the playlist keeps its
    // membership/position instead of silently losing the track. Walks
    // every playlist_entries page exactly once. Returns the number of
    // rows affected.
    size_t reassignPlaylistMemberships(uint32_t oldTrackId, uint32_t newTrackId);

    // Takes trackId out of playlistId: clears the presence bit of every
    // playlist_entry row of that pair, with rekordbox's deletion
    // bookkeeping. The other entries keep their entry_index, leaving a
    // gap; removePlaylistEntries() is how rekordbox takes a track out of
    // a playlist. Returns how many rows were cleared (0 when the track
    // was not in the playlist).
    size_t removePlaylistEntry(uint32_t playlistId, uint32_t trackId);

    // Takes trackIds out of playlistId the way rekordbox does (#62's
    // reference export): every entry of the playlist is deleted and the
    // ones staying are added again in their order, entry_index 1 on, so
    // the numbering stays contiguous. The deleted rows' pages are listed
    // on the table's index page (see commit()). Needs room for the
    // survivors like appendPlaylistEntries(), and throws PdbPageFull
    // without changing anything when there is none. Returns how many
    // entries went (0 when none of the tracks was in the playlist,
    // nothing written).
    size_t removePlaylistEntries(uint32_t playlistId, const std::set<uint32_t> &trackIds);

    // Adds trackIds to the end of playlistId (#62): one playlist_entry row
    // each, entry_index continuing from the playlist's highest, laid out
    // the way rekordbox itself appends, measured on four rekordbox-written
    // exports and a reference export made for this (issue #62's comments):
    //  - the row goes at heap offset used_size; offsets are row index * 12
    //    and the heap is never compacted;
    //  - its offset takes the next index slot, a new group of sixteen
    //    starting with zeroed present and transaction flags;
    //  - num_row_offsets and num_rows grow by one, used_size by 12, and
    //    free_size shrinks by 12 + 2 (+ 4 for a new group): free_size is
    //    len_page - 40 - used_size - (groups * 4 + num_row_offsets * 2);
    //  - one transaction per row, so the page ends naming the last row
    //    added: transaction (1, that row), its transaction flag the only
    //    one set;
    //  - rows that do not all fit on the table's last page go on a new
    //    page, all of them: the table's empty_candidate page becomes the
    //    last page and points at next_unused_page, which becomes the new
    //    candidate, and next_unused_page moves one on. A candidate past
    //    the end of the file grows it;
    //  - the page's sequence and the header's, as commit() always does.
    // Throws PdbPageFull, changing nothing, when the last page does not
    // lead to the empty candidate or the candidate is not an empty page.
    // A track already in the playlist is not added twice. Returns how
    // many rows were added.
    size_t appendPlaylistEntries(uint32_t playlistId, const std::vector<uint32_t> &trackIds);

    // The ids of the track rows whose file_path is `pathOnStick` as
    // export.pdb spells it ("/Contents/Artist/track.mp3"), space padding
    // ignored. More than one when the file was imported twice.
    std::vector<uint32_t> trackIdsWithFilePath(const std::string &pathOnStick) const;

    // The playlist tree as export.pdb holds it now: every playlist and
    // folder, with its parent (0 for the top level) and its place there.
    struct PlaylistTreeNode
    {
        uint32_t id = 0;
        uint32_t parentId = 0;
        uint32_t sortOrder = 0;
        bool isFolder = false;
        std::string name;
    };
    std::vector<PlaylistTreeNode> playlistTree() const;

    // Adds a playlist (or a folder) named `name` under parentId (0: the
    // top level; otherwise a folder), at `position` among its siblings
    // (default: last). Done the way rekordbox does it (#62's reference
    // export): every row of that level is deleted and written again,
    // sort_order 0 on, the new one among them; its id is one above any
    // the tree has had, so a removed playlist's id is never reused. The
    // name is encoded as rekordbox encodes it (short ASCII up to 126
    // bytes, long ASCII, or UTF-16). Throws std::invalid_argument for an
    // empty name, a '/' in it, a name its level already has (playlists
    // are matched by path across the libraries) or a parent that is not
    // a folder, PdbPageFull when the
    // rows cannot be placed; nothing changes then. Returns the new id.
    uint32_t createPlaylist(uint32_t parentId, const std::string &name, bool isFolder,
                            std::optional<size_t> position = std::nullopt);

    // Deletes a playlist, or a folder with everything in it: the tree
    // rows of the node and its descendants, every entry of each playlist
    // among them, and the rest of its level written again, sort_order 0
    // on, as rekordbox did when deleting Q1 and the folder F1. Tracks
    // stay: rekordbox also deleted the tracks no other playlist held, and
    // Seabass does not delete music with a playlist. Returns how
    // many playlists and folders went (0 when there is no such id).
    size_t deletePlaylist(uint32_t id);

    // Puts playlistId's entries in the order of trackIds, which must hold
    // exactly the tracks it has (std::invalid_argument otherwise): the
    // entries are deleted and added again, entry_index 1 on, as rekordbox
    // did moving a05 to the top of Q1. Returns false, writing nothing,
    // when the order is already that.
    bool reorderPlaylist(uint32_t playlistId, const std::vector<uint32_t> &trackIds);

    // Bumps the sequence number for every page touched this session
    // (page.sequence <- the header's current sequence; then the header's
    // own sequence is incremented -- matching the order the format's own
    // doc comment describes), lists every playlist page rows were deleted
    // from on its table's index page as rekordbox does, re-parses the result to confirm it's
    // structurally valid, then atomically replaces the file at pdbPath
    // with the edited copy (fsync'd/flushed before the rename). Returns
    // false, leaving the original file completely untouched, if nothing
    // was ever successfully edited, the file on disk changed since
    // construction, the edited buffer fails to re-parse, or the
    // write/rename failed.
    bool commit();

    // How many fields this writer had to cut a placeholder short to fit.
    //
    // Truncation is the contract, not a fault: a row cannot grow without
    // reflowing its page, so every overwrite here preserves the byte
    // length, and anonymizationPlaceholder() puts its hash in front of
    // the readable word precisely because the tail is what gets eaten.
    // What a finished export could not say, until this existed, is how
    // many of its fields were too small to carry a whole placeholder --
    // which is the difference between "the hash is in there" and "the
    // hash is in there and so is half a word of the original".
    //
    // Counted per field, across every row this writer touched. The
    // blank-out slots (ISRC, texter, message, mix name) are not counted:
    // empty text never truncates.
    std::size_t truncatedTextFields() const { return m_truncatedTextFields; }

private:
    std::size_t m_truncatedTextFields = 0;
    Format m_format = Format::Export;
    std::string m_pdbPath;
    std::string m_buffer;
    std::set<uint32_t> m_editedPageIndices;
    // Rows this session deleted, by page, as row indices: commit() records
    // them as the page's last transaction, the way rekordbox does, unless
    // the page was also appended to (appendPlaylistEntries writes its own).
    std::map<uint32_t, std::set<uint32_t>> m_deletedRowsByPage;
    std::set<uint32_t> m_appendedPages;
    // Where placeRows() puts new rows of one table: how many on its last
    // page, or how many on each new page.
    struct RowPlacement
    {
        size_t tableEntry = 0;
        uint32_t tableType = 0;
        uint32_t lastPage = 0;
        size_t onLastPage = 0;
        std::vector<size_t> perNewPage;
    };
    // One playlist_tree row to write: sort_order is its place in the level.
    struct TreeRowSlot
    {
        uint32_t id = 0;
        uint32_t parentId = 0;
        bool isFolder = false;
        std::string nameBytes;
    };
    RowPlacement placeRows(uint32_t tableType, const std::vector<std::string> &rows) const;
    static size_t rowsFitting(uint32_t numRowOffsets, uint16_t freeSize, const std::vector<std::string> &rows, size_t from);
    void writeRows(const RowPlacement &placement, const std::vector<std::string> &rows);
    uint32_t startPage(size_t tableEntry, uint32_t tableType);
    void appendRowsToPage(uint32_t pageIndex, const std::vector<std::string> &rows, size_t &next, size_t count);
    void rewritePlaylistEntries(uint32_t playlistId, const std::vector<uint32_t> &trackIds);
    void rewriteTreeLevel(uint32_t parentId, const std::vector<TreeRowSlot> &level);
    static std::vector<TreeRowSlot> siblingsOf(const std::string &buffer, uint32_t parentId);
    static std::string treeRowBytes(const TreeRowSlot &slot, uint32_t sortOrder);
    // Lists a playlist or playlist-tree page this session deleted rows
    // from on its table's index page, as rekordbox does; called by
    // commit().
    void listDeletionInTableIndex(uint32_t pageIndex);
    // Clears one row's presence bit with rekordbox's bookkeeping (#62):
    // num_rows one lower and page_flags' 0x10 ("this page has deleted
    // rows") set, as on every page rekordbox deleted from. A row already
    // absent is left alone. `presentFlagsOffset` and `bit` locate it.
    void deleteRowAt(uint32_t pageIndex, size_t presentFlagsOffset, uint16_t bit);
    std::uintmax_t m_originalFileSize = 0;
    std::filesystem::file_time_type m_originalMtime;
    // A whole-file CRC32 of m_buffer as originally read (before any
    // edits mutate it in place), checked at commit() against a *fresh*
    // read of the real file -- filesystem size/mtime alone are a known-
    // insufficient staleness signal on Windows: an in-process write that
    // doesn't change the file's length leaves both blind there (proven
    // empirically -- same finding, same fix, as
    // OneLibraryCueWriter::checkNotStale(), which this class's own
    // staleness convention was originally the model for). Kept alongside
    // size/mtime as a cheap pre-filter, not a replacement.
    std::uint32_t m_originalChecksum = 0;
};

}  // namespace seabass::infrastructure::rekordbox
