// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
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
    explicit PdbRowWriter(std::string pdbPath);

    // True if a present track row with this id exists.
    bool trackExists(uint32_t trackId) const;

    // Clears the presence bit for the track row with this id in the
    // tracks table. Returns false if no such (present) row is found.
    bool removeTrack(uint32_t trackId);

    // Clears the presence bit for the playlist_entry row matching
    // (playlistId, trackId). Returns false if not found.
    bool removePlaylistEntry(uint32_t playlistId, uint32_t trackId);

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

    // Overwrites an artist_row's name field the same way (near/far
    // offset per specs/rekordbox_pdb.ksy's artist_row -- see the .cpp).
    // Returns false if no artist with this id exists.
    bool overwriteArtistName(uint32_t artistId, const std::string &text);

    // Overwrites a playlist_tree_row's name field the same way. Returns
    // false if no playlist/folder with this id exists.
    bool overwritePlaylistName(uint32_t playlistId, const std::string &text);

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
