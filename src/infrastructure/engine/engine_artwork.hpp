// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

namespace seabass::infrastructure::engine
{

// Where a track's cover art actually is, and whether a player can find it.
//
// Engine stores art as files, never in the database. An AlbumArt row that
// a player can use holds a hash as a blob, and the image sits at
// "Artwork/<that hash, base64url>.jpg" inside the library -- self-contained,
// so it survives the stick being plugged into anything.
//
// Engine's own "import rekordbox library" writes something else into the
// same column: the text "image://fileart//<absolute path>" naming the
// rekordbox JPEG as the *importing computer* saw it, e.g.
// "/media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg". That path exists on
// no player and on no other machine, so the art silently never appears --
// found on a real stick where 1174 of 1271 tracks were in this state while
// the 95 with cached art showed up fine.
enum class ArtworkStorage {
    // A hash, with its file present under Artwork/: what a player reads.
    Cached,
    // A hash whose file is missing: the row is fine, the image is gone.
    CachedFileMissing,
    // A hash whose file is there and holds nothing a player can draw --
    // empty, truncated, or not an image at all. What an unclean unplug
    // leaves: the directory entry survives, its data does not. Worth
    // saying apart from "missing", because the two read differently to
    // anyone looking at the stick: this one looks fine in a file manager.
    CachedFileUnreadable,
    // "image://fileart//<absolute path>" from an import. Unusable on a
    // player, whatever this machine can resolve.
    ImportedPath,
    // The track points at an AlbumArt row whose hash is NULL or empty:
    // art was asked for and there is nothing to find it by. Never
    // repairable -- there is no image to copy and no name to write.
    RowWithoutHash,
    // No art row, or an empty one.
    None,
};

struct ArtworkEntry
{
    std::int64_t trackId = 0;
    std::string title;
    std::string artist;
    ArtworkStorage storage = ArtworkStorage::None;
    // The raw AlbumArt.hash, as text when it is text.
    std::string reference;
    // The image this stick holds for it, found by anchoring on the
    // "PIONEER/Artwork/..." tail of an imported path, or -- for a row
    // whose own cached file is gone or empty -- by asking the rekordbox
    // catalog beside it what art it holds for the same audio file. Empty
    // when this stick has no such file, and then the entry cannot be
    // repaired.
    std::string imageOnStick;
    // The track's audio file, resolved against the Engine library, which
    // is how the rekordbox side is asked about the same track.
    std::string trackFile;
    // A copy exists somewhere that is not a plain file on this stick --
    // the audio file's own tags, or a stick backup on this computer. The
    // repair asks for the bytes when it gets there; the scan only asks
    // whether they exist, so the count it shows is a promise it can keep.
    bool otherSource = false;
};

struct ArtworkAudit
{
    int tracksWithArt = 0;
    int readableByAPlayer = 0;
    // Tracks whose art a player cannot find, worst first: the repairable
    // ones (imageOnStick set) before the ones with nothing to copy.
    std::vector<ArtworkEntry> unreadable;
    // Set when the database could not be read at all.
    std::string error;

    int repairable() const;
};

// Reads <engineLibraryPath>/Database2/m.db read-only. Never writes.
// Art the rekordbox catalog on the same stick holds, keyed by the audio
// file it belongs to (lowercased absolute path, so a FAT stick's casing
// cannot hide a match). Engine and rekordbox keep separate image files --
// different encodings, different bytes -- so this is not a shared store
// but a second source to rebuild from, and the only one that is on the
// stick itself when the Engine copy is gone.
using ArtworkSourceByTrackFile = std::unordered_map<std::string, std::string>;

std::string artworkSourceKey(const std::string &trackFile);

// The other places a lost cover can come back from, asked about one
// faulty entry at a time and never for a whole library. Passed in rather
// than called here: one of them reads tags (TagLib) and another reads
// stick backups (the zip reader), and neither belongs to this file's job
// of reading an Engine database.
using ArtworkSourceProbe = std::function<bool(const ArtworkEntry &)>;
using ArtworkSourceReader = std::function<std::string(const ArtworkEntry &)>;

ArtworkAudit auditArtwork(const std::string &engineLibraryPath, const ArtworkSourceByTrackFile &sources = {},
                          const ArtworkSourceProbe &hasOtherSource = {});

// How Engine spells a hash as a file name under Artwork/: base64url,
// unpadded. Exposed for the test, which checks it against the encoding
// every other implementation agrees on; nothing else calls it yet.
std::string artworkFileName(std::span<const std::uint8_t> hash);

// Which storage a raw AlbumArt.hash value is.
ArtworkStorage classifyArtworkReference(std::string_view reference);

// The "PIONEER/Artwork/..." tail of an imported reference, joined onto
// this stick. Empty when the reference carries no such tail, and also
// when the join itself is impossible: the tail is raw bytes out of a
// database column, and on Windows operator/ throws on bytes that are not
// valid UTF-8 (the Engine reader catches the same throw per row, for the
// same reason -- one bad row must not cost the whole scan).
std::string imageOnStickFor(std::string_view reference, const std::string &stickRoot);

struct ArtworkRepair
{
    int repaired = 0;
    // Files written under Artwork/, so a failed save can take them back out.
    std::vector<std::string> filesWritten;
    // Entries whose image was copied in but whose Track row was no longer
    // there to point at it (deleted between the audit and the save). Kept
    // apart from `repaired` so a caller can say which happened rather than
    // reporting "none of the images could be read".
    int tracksNoLongerThere = 0;
    // Entries skipped because the file is not an image this can promise a
    // player will read: the name a repair writes carries the format, so
    // only JPEG and PNG, decided on the bytes rather than the extension.
    int notAnImage = 0;
    std::string error;
};

// Gives each entry Engine's own storage: copies its image into Artwork/
// under the hash of its bytes, adds the AlbumArt row, and points the track
// at it. Entries with no imageOnStick are skipped. One transaction.
//
// `databaseFile` is the m.db to write. It is a parameter rather than
// <engineLibraryPath>/Database2/m.db because a save may have redirected
// writes to that database into a scratch copy it commits at the end: a
// change that writes the live file behind that redirect has its rows
// overwritten when the scratch lands. The images still go under
// <engineLibraryPath>/Artwork, which nothing redirects. Empty means the
// library's own, for a caller outside a save.
//
// `beforeWrite` is called with each file this is about to create, before
// it exists, so a save can protect it and take it back out if the save
// then fails (SaveContext::protectForThisChange). It may throw, which
// fails the repair with the transaction rolled back.
ArtworkRepair repairArtwork(const std::string &engineLibraryPath, const std::vector<ArtworkEntry> &entries,
                            const std::function<void(const std::string &)> &beforeWrite = {},
                            const std::string &databaseFile = {},
                            const ArtworkSourceReader &readOtherSource = {});

}  // namespace seabass::infrastructure::engine
