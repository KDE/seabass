// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

namespace seabass::infrastructure::onelibrary
{

// Best-effort writer for Rekordbox's newer "OneLibrary" / "Device Library
// Plus" format (exportLibrary.db, alongside export.pdb on the same
// stick), cue points (writeCuesForPath) and, for cleanup, whole track
// rows (removeTrackByPath). See docs/onelibrary-format.md for the
// reverse-engineering this is based on, including the parts that are
// genuinely unverified (the exact meaning of a couple of secondary
// fields) and should be re-checked once real hardware-written example
// data is available.
//
// Deliberately NOT an application::CueWriter: that interface identifies
// a track by its format's own sourceId, but OneLibrary's content_id is a
// *separate* id space from export.pdb's track id (confirmed empirically
// during development, the same file's export.pdb id and OneLibrary
// content_id can differ). The one identifier both databases actually
// share is the track's file path, so that's what this takes instead.
//
// Deliberately does NOT do its own backup: callers should back up
// exportLibrary.db through the same infrastructure::backup::BackupStore
// (and add the resulting record to whatever backup list drives that
// operation's Undo) that already covers export.pdb/m.db for the same
// operation, see cleanup_controller.cpp's call site. Keeping every
// backup for one user-facing operation under one BackupStore/manifest is
// what makes Undo actually cover everything touched.
// Thrown when a save's rows are committed but the write-ahead log could
// not be folded. The distinction matters: the changes DID land, so a save
// loop must not report them as still pending and invite a second apply.
class OneLibraryLogNotFolded : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class OneLibraryCueWriter
{
public:
    // pioneerRoot: where to find/open exportLibrary.db
    // (pioneerRoot/rekordbox/exportLibrary.db) -- normally the stick's
    // "PIONEER" folder (same argument RekordboxCleanupWriter/
    // RekordboxCueWriter take), but callers may point this at a
    // relocated copy of the database instead (see realStickRoot below).
    //
    // realStickRoot: the stick's actual root directory, used to convert
    // each track's absolute file path into the stick-root-relative form
    // content.path values use, e.g. "/Contents/Artist/Track.mp3".
    // Defaults to pioneerRoot's own parent directory, which is correct
    // whenever pioneerRoot really is the stick's PIONEER folder. Only
    // needs to be passed explicitly when pioneerRoot points somewhere
    // else -- e.g. a fast local scratch copy of exportLibrary.db built to
    // avoid many slow round trips against real removable media -- where
    // pioneerRoot's own parent is no longer the stick at all, but the
    // file paths being looked up still are.
    explicit OneLibraryCueWriter(std::string pioneerRoot, std::optional<std::string> realStickRoot = std::nullopt);

    // True if exportLibrary.db exists for this stick. OneLibrary isn't
    // present on every export (only newer hardware/newer rekordbox
    // exports create it), so callers should skip the write entirely
    // rather than treat a missing file as an error.
    static bool existsFor(const std::string &pioneerRoot);
    static std::string dbPathFor(const std::string &pioneerRoot);

    // Replaces the complete cue set for the track at this (absolute)
    // file path, same "pass the whole set, not a delta" contract as
    // application::CueWriter::writeHotCues(). Looks the track up by
    // content.path (converted from filePath, relative to the stick
    // root); throws if no matching row is found, if the file changed
    // since this writer was constructed (staleness guard, see .cpp),
    // or if the post-commit read-back doesn't match what was written.
    // Callers should treat any exception here as failure of a
    // *secondary*, best-effort write, never roll back a primary
    // export.pdb/m.db write that already succeeded because of it.
    void writeCuesForPath(const std::string &filePath, const std::vector<domain::CuePoint> &cues);

    // Removes a track's row entirely, along with its dependent cue/
    // playlist-membership rows, for cleanup call sites that just
    // deleted this file (and its row) from the primary catalog
    // (rekordbox/Engine) and need to remove the now-orphaned OneLibrary
    // row too, rather than leave it pointing at a file that no longer
    // exists. No FK/cascade enforcement exists in this schema (see
    // docs/onelibrary-format.md), so dependents are deleted explicitly,
    // in dependency order, inside one transaction. Same contract as
    // writeCuesForPath(): looks the track up by content.path (converted
    // from filePath), throws if no matching row is found, if the file
    // changed since this writer was constructed (staleness guard), or if
    // the post-commit read-back doesn't show the row actually gone.
    // Same "secondary, best-effort write" caution applies to callers.
    //
    // Deliberately does NOT reassign playlist membership to any other
    // row -- there's no replacement in scope here (a genuinely orphaned
    // row, see removeTrackByPathReplacingWith() below for the case where
    // there is one). Callers that DO have a survivor to reassign to must
    // use that instead: using this method there would silently drop the
    // doomed row's playlist membership instead of preserving it, exactly
    // the failure application::LibraryCleanupWriter::
    // removeTrackReplacingWith()'s own contract exists to prevent for
    // every other format's writer -- confirmed as a real, already-shipped
    // bug in this codebase's own OneLibrary-mirror call sites before this
    // second method was added to fix it.
    void removeTrackByPath(const std::string &filePath);

    // Same removal as removeTrackByPath(), except the doomed row's
    // playlist memberships are reassigned to survivorFilePath's row
    // first, not dropped -- for callers that have a real survivor in
    // scope (a duplicate-consolidation cleanup, not a genuine orphan).
    // Matches PdbRowWriter::reassignPlaylistMemberships()'s own
    // dedup rule exactly: if the survivor is already in a playlist the
    // doomed row was also in, that membership is just dropped (not
    // duplicated) rather than inserting a second row for the same
    // (playlist, survivor) pair.
    void removeTrackByPathReplacingWith(const std::string &doomedFilePath, const std::string &survivorFilePath);

    // True if a content row lists this (absolute) file path. For a caller
    // whose row may already have gone earlier in the same save -- a
    // rekordbox repair mirrors its row removal here -- and that must tell
    // "already done" from "never there" before removing it.
    bool hasTrackAtPath(const std::string &filePath);

    // The same removal for one content row, named by id. What a caller
    // holding row ids (Clean Up, whose plan was read from this database)
    // must use: a path can name more than one row, so resolving an id
    // back through its path picks whichever row SQLite returns first.
    void removeTrackByIdReplacingWith(int64_t doomedContentId, int64_t survivorContentId);

    // Writes the two authored fields that are not cues: the rating in
    // stars (0 to 5, the scale domain::Track uses) and the DJ's own
    // comment. Either may be absent, and an absent one is left alone
    // rather than cleared.
    //
    // Unlike export.pdb, which keeps a comment in a fixed byte span it
    // cannot grow, this is a plain SQL column: any comment fits. See
    // docs/metadata-backup-plan.md for the per-format table that falls
    // out of that difference.
    void writeAnnotationForPath(const std::string &filePath, const std::optional<int> &stars,
                                 const std::optional<std::string> &comment);

    // Sets a content row's play count (djPlayCount). Clean Up's survivor
    // gets its duplicate copies' counts added up. Read back before it is
    // trusted, like the annotation above.
    void writePlayCountForPath(const std::string &filePath, int playCount);

    // Fills in a Clean Up survivor's missing bpm/key/artwork from
    // another copy in its duplicate group (see domain::
    // DuplicateCleanupPlan). Copies the donor row's own already-valid
    // bpmx100/key_id/image_id column values directly onto the target
    // row -- key_id/image_id are references into the key/image tables,
    // so this reuses whichever row the donor already points at rather
    // than re-deriving a lookup from a parsed key string or artwork
    // file path. Each of copyBpm/copyKey/copyArtwork independently
    // opts that one field in; throws if either path has no matching
    // content row, or if the file changed since this writer was
    // constructed (same staleness guard as writeCuesForPath()).

    void propagateMissingFieldsForPath(const std::string &donorFilePath, const std::string &targetFilePath,
                                        bool copyBpm, bool copyKey, bool copyArtwork);

    // Folds the write-ahead log back into exportLibrary.db and closes the
    // connections, so the library a save leaves behind is one file.
    //
    // exportLibrary.db runs in WAL mode, and until this existed nothing
    // here ever checkpointed: the rows reached the database only because
    // SQLite folds the log when the last connection closes. That is
    // SQLite's guarantee, not ours, and it is one a future writer that
    // holds a connection open past the save would quietly withdraw --
    // leaving committed cues in a sidecar that every reader of the
    // database alone cannot see, with no error anywhere.
    //
    // A no-op when this writer never opened anything.
    // Throws OneLibraryLogNotFolded when the library is not one file
    // afterwards -- on any save, cancelled or not. Never an error type: the
    // rows are committed, and a save reported as unapplied gets applied
    // twice.
    void finishWriting();

    // Whether this writer opened its write connection -- i.e. wrote, or was
    // about to. A save that only created the writer to ask existsFor() or
    // hasTrackAtPath() has nothing of its own in the log.
    bool hasWritten() const { return m_writeDb != nullptr; }

    // For a database that was just put back from a backup together with
    // its -wal: open, checkpoint, close, and say how many bytes of log are
    // still beside it (0 when it is one file again). A rolled-back save
    // destroys its writers before restoring files, so nothing else folds
    // the log the restore brought back.
    // nullopt when it cannot tell (the database is gone, or the log cannot
    // be measured); 0 when the database is one file again; otherwise the
    // bytes still in the log. Collapsing all three to 0 made "nothing left"
    // and "could not look" indistinguishable to the caller that logs it.
    static std::optional<std::uint64_t> foldLogOf(const std::string &dbPath);

private:
    // The two connections this writer works through, opened on first use
    // and then kept.
    //
    // Every method used to open its own read-write connection and then a
    // second, read-only one to verify the commit, so each call paid two
    // SQLCipher key derivations. That derivation, not the disk, is what
    // makes a OneLibrary write cost 235 ms against a stick, and it is the
    // single largest per-item cost in a save (see
    // docs/write-path-performance.md).
    //
    // The verification keeps its own separate connection rather than
    // re-reading through the one that just wrote. Reading the committed
    // result back through a different connection is the property that
    // check exists for; what it does not need is a fresh key derivation
    // every time to do it.
    //
    // Safe to hold across calls because every method checks the staleness
    // guard first and throws if the file changed underneath, so a held
    // connection is never used against a database this writer no longer
    // recognises. They close with the writer, which a save owns for its
    // own duration.
    SqlCipherDb &writeConnection();
    SqlCipherDb &verifyConnection();

    // Set once finishWriting() has folded and closed: the baseline this
    // writer holds no longer describes the file.
    bool m_finished = false;
    std::unique_ptr<SqlCipherLibrary> m_lib;
    std::unique_ptr<SqlCipherDb> m_writeDb;
    std::unique_ptr<SqlCipherDb> m_verifyDb;

    // Throws if the file no longer looks like the one this writer was
    // constructed against (or last wrote itself) -- shared by every
    // write method below rather than duplicated four times.
    //
    // Checks a whole-file CRC32 (via zlib, already a project dependency)
    // against the baseline captured at construction/last write, alongside
    // the cheaper size/mtime check as a fast pre-filter. The CRC32 is the
    // authoritative signal: size/mtime alone are known-insufficient (a
    // real Windows test found size unchanged for an external row INSERT
    // that lands in an already-allocated page -- realistic for any edit
    // smaller than a full page on a database this size -- and mtime,
    // read via a path-based fs::last_write_time() stat, unreliable
    // in-process on Windows for a mechanism not yet fully isolated,
    // plausibly cached directory-entry metadata vs. a handle-based
    // query). A whole-file byte read has no equivalent platform-metadata
    // ambiguity -- it's the same "read the actual bytes and compare" this
    // project's own PdbRowWriter already relies on for the equivalent
    // rekordbox-format guard.
    //
    // NOTE on a rejected approach: `PRAGMA data_version` was tried first
    // and is wrong for this call shape -- it's a *per-connection* counter
    // that starts fresh at 1 for any newly-opened connection, so a helper
    // that opens-queries-closes a connection every call (as this class
    // does throughout) can never see it change, regardless of platform.
    // Verified empirically (real sqlite3, three separate external
    // writes, value never moved off 1) before removing it -- it would
    // only work by holding one connection open for this writer's entire
    // lifetime, a real structural change this class doesn't make.
    void checkNotStale() const;

    // Refreshes the staleness baseline to the file's new (post-write)
    // state -- without this, reusing one writer instance across several
    // write calls would have every call after the first refuse itself,
    // since the file legitimately changed due to this writer's *own*
    // prior write.
    void refreshStalenessBaseline();

    // Whole-file CRC32, read in one shot. exportLibrary.db-sized files
    // are small enough (hundreds of KB to a few MB on real sticks) that
    // this is cheap next to the SQL round trip each write already does.
    std::uint32_t computeChecksum() const;

    std::string m_pioneerRoot;
    std::string m_stickRoot;
    std::string m_dbPath;
    std::uintmax_t m_originalFileSize = 0;
    std::filesystem::file_time_type m_originalMtime;
    std::uint32_t m_originalChecksum = 0;
};

}  // namespace seabass::infrastructure::onelibrary
