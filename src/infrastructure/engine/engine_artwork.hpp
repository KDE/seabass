// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"

struct sqlite3;
struct sqlite3_blob;

namespace seabass::infrastructure::engine
{

// Where a track's cover art actually is, and whether a player can find it.
//
// Engine has kept art two ways. Current libraries store it as files: an
// AlbumArt row holds a hash as a blob, and the image sits at
// "Artwork/<that hash, base64url>.jpg" inside the library. Older ones
// (schema 3.0.1 and earlier, and libraries Engine DJ 4.5.0 migrated from
// them) keep the image itself in AlbumArt.albumArt, beside a hash written
// as hex text. Both are self-contained, so they survive the stick being
// plugged into anything.
//
// Engine's own "import rekordbox library" writes something else into the
// hash column: the text "image://fileart//<absolute path>" naming the
// rekordbox JPEG as the *importing device* saw it, e.g.
// "/media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg". A path from
// another computer exists nowhere else, and the art silently never
// appears. A path under "/media/<this stick's label>/" is different: it
// is what a Denon player wrote, since Engine OS mounts every stick at
// /media/<label>, and every Engine OS player resolves it again for as
// long as the stick keeps its label. Engine DJ on a computer does not
// (/Volumes/<label> on a Mac, /media/<user>/<label> here). WHALESHARK2
// had 1467 of 1564 tracks that way and the Prime 4 showed every cover;
// the audit used to call them all missing.
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
    // "image://fileart//<absolute path>" from an import on another
    // computer. Unusable on a player, whatever this machine can resolve.
    ImportedPath,
    // "image://fileart//media/<this stick's label>/..." from an import on
    // a Denon player, with the image on this stick: every Engine OS
    // player shows it, Engine DJ on a computer does not. Not a fault.
    // Repairable all the same, into Engine's own storage, for a library
    // that should show its covers everywhere.
    ImportedPathOnPlayer,
    // The track points at an AlbumArt row whose hash is NULL or empty:
    // art was asked for and there is nothing to find it by. Never
    // repairable -- there is no image to copy and no name to write.
    RowWithoutHash,
    // The image is in the row itself, in AlbumArt.albumArt: readable by a
    // player whatever the row's hash or id.
    InDatabase,
    // A row that keeps its image in the database, and whose image is empty
    // (NULL or no bytes). Filled in when the track is alone on the row; a
    // track sharing it gets a database row of its own. Bytes that are no
    // picture are InDatabaseLeftAlone instead.
    InDatabaseUnreadable,
    // A row holding bytes in albumArt that are neither JPEG nor PNG: an
    // image in a format a player may not show (GIF, WebP, AVIF...) or
    // bytes that are no picture at all. Seabass cannot tell a real cover
    // from damage there, so it leaves the row alone: never written over,
    // never pointed away from, and not counted among the faults.
    InDatabaseLeftAlone,
    // A row holding a value in albumArt that could not be read to see what
    // it is. Counted apart and left alone until a later check can read it.
    InDatabaseUnchecked,
    // No art row, or Engine's own empty "no cover" row.
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
    // How the library kept its covers when it was audited (the database or
    // files), for a track whose own row names neither. Taken from the
    // audit, so every repair of one save writes in the same storage.
    std::optional<bool> libraryCoversInDatabase;
};

struct ArtworkAudit
{
    int tracksWithArt = 0;
    int readableByAPlayer = 0;
    // Tracks whose art a player cannot find, worst first: the repairable
    // ones (imageOnStick set) before the ones with nothing to copy.
    std::vector<ArtworkEntry> unreadable;
    // Tracks whose art a Denon player finds and Engine DJ on a computer
    // does not (ImportedPathOnPlayer). Counted among readableByAPlayer,
    // not among the faults; each has imageOnStick set.
    std::vector<ArtworkEntry> playerOnly;
    // Tracks whose row keeps bytes in the database that Seabass leaves
    // alone (InDatabaseLeftAlone). Apart from the faults: a library of GIF
    // covers is not a broken one.
    std::vector<ArtworkEntry> leftAlone;
    // Tracks whose row's value could not be read to check it
    // (InDatabaseUnchecked): neither a fault nor left alone, and said.
    std::vector<ArtworkEntry> unchecked;
    // Whether the library keeps its covers in the database, by the rule a
    // repair writes by (keepsCoversInDatabase in engine_artwork.cpp).
    bool coversInDatabase = false;
    // The label this stick goes by, the one the playerOnly links name: a
    // player shows those covers only while the stick keeps it.
    std::string stickLabel;
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

//
// `cancel` is checked before every row, so a stop lands within one row's
// work (a stat or two, or one probe of the other sources) rather than
// after the whole table: throws application::OperationCancelled, with the
// database already closed.
//
// `progress` hears two phases, each with a total set up front and a tick
// per row: the covers kept inside the database ("Reading cover images"),
// then the tracks ("Checking cover art"). On a USB stick either can take
// long enough that a bar which does not move looks like a hang.
ArtworkAudit auditArtwork(const std::string &engineLibraryPath, const ArtworkSourceByTrackFile &sources = {},
                          const ArtworkSourceProbe &hasOtherSource = {},
                          const application::CancellationToken &cancel = application::CancellationToken::none(),
                          application::ProgressReporter &progress = application::NullProgressReporter::instance());

// How Engine spells a hash as a file name under Artwork/: base64url,
// unpadded. Exposed for the test, which checks it against the encoding
// every other implementation agrees on; nothing else calls it yet.
std::string artworkFileName(std::span<const std::uint8_t> hash);

// The file repairArtwork() writes under the library's Artwork/ for the
// image in `imageFile` when the library keeps its covers as files: the
// image's content hash as artworkFileName() spells it, with
// extensionForImage()'s extension. Known before the write, so a change
// can declare it for Undo (BackupTarget::removeOnRestoreIfAbsent). Empty
// when the file cannot be read or is neither a JPEG nor a PNG, which
// repairArtwork() writes nothing for.
std::string artworkFileForImage(const std::string &engineLibraryPath, const std::string &imageFile);

// How SQLite stored an AlbumArt.hash value, which is what tells the two
// kinds of storage apart: a blob names a file, text names the image in
// the row beside it.
enum class ReferenceType { Blob, Text };

// The file a player reads for a row's hash: artworkFileName() under
// `artworkDirectory` (the library's Artwork/), as ".jpg", ".jpeg" or
// ".png", and holding a JPEG or PNG. Empty when there is none. `anyFile`,
// when given, is set if a file by one of those names exists at all.
// With `checkBytes` false the file only has to be there: a stat, not an
// open, for a reader that shows covers and leaves judging them to the
// audit.
std::string cachedArtworkFile(const std::string &artworkDirectory, std::span<const std::uint8_t> hash,
                              bool *anyFile = nullptr, bool checkBytes = true);

// The first bytes (up to 12) of the image an AlbumArt row keeps in the
// database, read without loading the rest of it. Empty when there is none;
// no value when it could not be read, which says nothing about it.
std::optional<std::string> albumArtImageHead(sqlite3 *handle, std::int64_t albumArtId);

// The same for many rows, through one blob handle moved from row to row.
class AlbumArtImageHeads
{
public:
    explicit AlbumArtImageHeads(sqlite3 *handle) : m_handle(handle) {}
    ~AlbumArtImageHeads();
    AlbumArtImageHeads(const AlbumArtImageHeads &) = delete;
    AlbumArtImageHeads &operator=(const AlbumArtImageHeads &) = delete;

    std::optional<std::string> of(std::int64_t albumArtId);

private:
    sqlite3 *m_handle;
    struct sqlite3_blob *m_blob = nullptr;
};

// Which storage a raw AlbumArt.hash value is.
ArtworkStorage classifyArtworkReference(std::string_view reference, ReferenceType type);

// The "PIONEER/Artwork/..." tail of an imported reference, joined onto
// this stick. Empty when the reference carries no such tail, and also
// when the join itself is impossible: the tail is raw bytes out of a
// database column, and on Windows operator/ throws on bytes that are not
// valid UTF-8 (the Engine reader catches the same throw per row, for the
// same reason -- one bad row must not cost the whole scan).
std::string imageOnStickFor(std::string_view reference, const std::string &stickRoot);
// Whether an imported reference names this stick as a Denon player mounts
// it ("image://fileart//media/<label>/..."), the label compared without
// case. Exposed for the test.
bool importedPathIsThisStickOnAPlayer(std::string_view reference, const std::string &label);
// Whether two stick labels are one name to a player: compared without
// case. The one rule for every comparison of labels in the artwork code
// and in the restore that calls it.
bool sameStickLabel(std::string_view a, std::string_view b);

// The label a stick is known by where the audit asks (see volumeLabelOf in
// engine_artwork.cpp): what decides ImportedPathOnPlayer, and so what the
// Cover Art page names.
std::string stickLabelForArtwork(const std::string &stickRoot);

// A player's links follow the stick's name, so a library copied onto a
// stick with another label shows none of them: every
// "image://fileart//media/WHALESHARK/..." row names a mount that stick
// never gets. WS_NEW, made from WHALESHARK's backup, had 1361 such rows
// and showed 186 covers.
//
// Rewrites the label in every AlbumArt hash that is such a link to
// `oldLabel` (compared without case, as importedPathIsThisStickOnAPlayer
// does) to `newLabel`, in one transaction on `databaseFile` (an m.db).
// Every other byte of the hash stays, and so does its storage class:
// Engine's import writes these as blobs holding text, and a blob stays a
// blob. Rows holding an image, rows naming a file by hash, and links to
// any other label are not touched; neither is a row already spelled
// `newLabel`. Returns how many rows were rewritten, or -1 with *error
// set (error must not be null). A database that is not there is an
// error, never created.
int relabelImportedArtworkLinks(const std::string &databaseFile, const std::string &oldLabel,
                                const std::string &newLabel, std::string *error);

// What making a library's player links label-proof did.
struct LabelProofArtwork
{
    // Tracks that pointed at a link to the old label.
    int linkedTracks = 0;
    // Of those, the ones whose cover is now in the library's own storage.
    int copied = 0;
    // And the ones still on a link, renamed to the new label: the image
    // was not on the stick, was no image, or there was no room.
    int renamed = 0;
    // What the images to copy take up, and whether the stick lacked it,
    // in which case nothing was copied and every link was renamed.
    std::uint64_t bytesNeeded = 0;
    bool noRoom = false;
    // Set when the database could not be read or written. A failed copy
    // is rolled back whole; a failed rename after a copy that landed
    // leaves the copied covers in place.
    std::string error;
};

// For a library put on a stick whose label is not the one its player
// links name (a restore or a clone onto a stick called something else):
// every track on a link to `oldLabel` whose image this stick holds gets it
// copied into the library's own storage by repairArtwork(), so the cover
// no longer depends on any label. Only those tracks are handed to the
// repair; no other row is looked at. What cannot be copied, and every
// link when the images need more than `freeBytes` less `marginBytes`
// (0 free means unknown, which never refuses), is renamed to `newLabel`
// by relabelImportedArtworkLinks(), so a player finds it on this stick
// for as long as it keeps that name. Two transactions on the library's
// own m.db, the copy first.
LabelProofArtwork makeArtworkLinksLabelProof(const std::string &engineLibraryPath, const std::string &oldLabel,
                                             const std::string &newLabel, std::uint64_t freeBytes,
                                             std::uint64_t marginBytes);

// How many AlbumArt rows are such links to `label`, read-only. -1 with
// *error set when the database cannot be read (error must not be null).
int countImportedArtworkLinks(const std::string &databaseFile, const std::string &label, std::string *error);

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
    // Entries left alone because the track's row keeps its art in the
    // database: re-pointing it would drop that image.
    int keptInDatabase = 0;
    // InDatabaseUnreadable entries whose row no longer keeps its image in
    // the database when the repair gets to them: nothing is written.
    int noLongerInDatabase = 0;
    // Entries given up because a check of the database could not be read:
    // nothing is written for them, whatever the check would have said.
    int failedReads = 0;
    std::vector<std::string> failureReasons;
    std::string error;
};

// The extension an Engine artwork file must carry, decided on the bytes
// rather than on the name the source had.
//
// Engine looks for "<hash>.jpg", ".jpeg" or ".png" exactly, so a source
// called a5_m.JPG written as <hash>.JPG is a file no player finds and no
// audit can match -- that one left a track counted unreadable AND
// unrepairable, the notice up and the button disabled. An extension also
// says nothing about what is in the file. Empty when the bytes are
// neither JPEG nor PNG, which means: do not name this for a player,
// because we cannot promise it reads it.
//
// Shared with the library creator, which used to keep a second opinion
// and take the extension from the source's own name.
//
// Inline because it is a pure test on a handful of bytes, and because
// several targets compile the library creator without linking this
// file's object.
inline std::string extensionForImage(std::string_view bytes)
{
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xD8 && static_cast<unsigned char>(bytes[2]) == 0xFF) {
        return ".jpg";
    }
    static constexpr std::string_view PngMagic("\x89PNG\r\n\x1a\n", 8);
    if (bytes.size() >= PngMagic.size() && bytes.substr(0, PngMagic.size()) == PngMagic) {
        return ".png";
    }
    return {};
}

// What the covers of these tracks take up on the stick: each distinct
// image file once, plus, for `engineLibraryPath` when given, the covers
// that Engine library keeps: images in its database and files its rows
// name under Artwork/. When that database cannot be read, the rest is
// still counted and engineUnreadable says the Engine part is missing.
struct ArtworkBytes
{
    std::uint64_t bytes = 0;
    bool engineUnreadable = false;
};
ArtworkBytes artworkBytesOnStick(const std::vector<domain::Track> &tracks, const std::string &engineLibraryPath);

// Gives each entry the library's own storage and points the track at it.
// From schema 3.0.2 on: the image copied into Artwork/ under the hash of
// its bytes, and a row with that hash. Before 3.0.2, where the library
// keeps its covers in the database: a row with a text hash and the image
// in it, and no file. Entries with no source are skipped, and so is any
// track whose current row keeps its art in the database. An
// InDatabaseUnreadable entry alone on its row gets the image written into
// that row, whose hash and id stay; on a row other tracks share, it gets a
// row of its own instead. A row whose image reads, or is a real image in
// another format, is never overwritten. One transaction.
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
