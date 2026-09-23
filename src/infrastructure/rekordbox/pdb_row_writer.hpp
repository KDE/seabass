// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <functional>
#include <string>

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
    int overwriteAllNames(NameTable table, const std::function<std::string(size_t index)> &placeholder);

    // For every playlist_entry row currently pointing at oldTrackId:
    // if that same playlist already has an entry for newTrackId,
    // removes the oldTrackId entry (avoids a duplicate); otherwise
    // repoints it to newTrackId in place, so the playlist keeps its
    // membership/position instead of silently losing the track. Walks
    // every playlist_entries page exactly once. Returns the number of
    // rows affected.
    size_t reassignPlaylistMemberships(uint32_t oldTrackId, uint32_t newTrackId);

    // Bumps the sequence number for every page touched this session
    // (page.sequence <- the header's current sequence; then the header's
    // own sequence is incremented -- matching the order the format's own
    // doc comment describes), re-parses the result to confirm it's
    // structurally valid, then atomically replaces the file at pdbPath
    // with the edited copy (fsync'd/flushed before the rename). Returns
    // false, leaving the original file completely untouched, if nothing
    // was ever successfully edited, the file on disk changed since
    // construction, the edited buffer fails to re-parse, or the
    // write/rename failed.
    bool commit();

private:
    Format m_format = Format::Export;
    std::string m_pdbPath;
    std::string m_buffer;
    std::set<uint32_t> m_editedPageIndices;
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
