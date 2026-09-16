#include <chrono>
#include <thread>
// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <stdexcept>

#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

namespace seabass::infrastructure::onelibrary
{

namespace fs = std::filesystem;
using domain::CuePoint;

namespace
{

// Converts an absolute (or mixed-separator, e.g. "H:\/Contents/...")
// track file path into the slash-normalized, stick-root-relative form
// exportLibrary.db's content.path column uses, confirmed against a
// real stick's data during development, e.g.
// "/Contents/Artist/Track/01_Artist-Track.mp3".
std::string toContentPath(const std::string &stickRoot, const std::string &trackPath)
{
    // Trailing whitespace is not part of a filename. rekordbox's pdb
    // stores strings in fixed-length fields and pads them, so a path read
    // back from export.pdb can arrive space-padded -- and an unpadded
    // lookup then finds no content row and writes nothing, silently.
    // Windows does not permit a trailing space in a filename anyway, so
    // there is no file this could wrongly match.
    std::string filePath = trackPath;
    while (!filePath.empty() && (filePath.back() == ' ' || filePath.back() == '\t')) {
        filePath.pop_back();
    }

    std::error_code ec;
    fs::path rel = fs::relative(fs::path(filePath), fs::path(stickRoot), ec);
    std::string relStr = ec ? filePath : rel.generic_string();
    std::replace(relStr.begin(), relStr.end(), '\\', '/');
    if (relStr.empty() || relStr[0] != '/') {
        relStr = "/" + relStr;
    }
    return relStr;
}

}  // namespace

namespace
{

// Every content row at `contentPath`, oldest first. A real
// exportLibrary.db can list one file under more than one row (193 files on
// RV2), and the reader shows whichever it meets, so a write meant for the
// file has to reach all of them.
std::vector<int64_t> contentIdsAt(SqlCipherDb &db, const std::string &contentPath)
{
    std::vector<int64_t> ids;
    SqlCipherStatement find(db, "SELECT content_id FROM content WHERE path = ? ORDER BY content_id");
    find.bindText(1, contentPath);
    while (find.step()) {
        ids.push_back(find.columnInt64(0));
    }
    if (ids.empty()) {
        throw std::runtime_error("onelibrary: no content row for path " + contentPath);
    }
    return ids;
}

}  // namespace

std::string OneLibraryCueWriter::dbPathFor(const std::string &pioneerRoot)
{
    return (fs::path(pioneerRoot) / "rekordbox" / "exportLibrary.db").string();
}

bool OneLibraryCueWriter::existsFor(const std::string &pioneerRoot)
{
    std::error_code ec;
    return fs::is_regular_file(dbPathFor(pioneerRoot), ec);
}

OneLibraryCueWriter::OneLibraryCueWriter(std::string pioneerRoot, std::optional<std::string> realStickRoot)
    : m_pioneerRoot(std::move(pioneerRoot))
{
    m_stickRoot = realStickRoot ? std::move(*realStickRoot) : fs::path(m_pioneerRoot).parent_path().string();
    m_dbPath = dbPathFor(m_pioneerRoot);
    std::error_code ec;
    m_originalFileSize = fs::file_size(m_dbPath, ec);
    m_originalMtime = fs::last_write_time(m_dbPath, ec);
    if (ec) {
        throw std::runtime_error("onelibrary: " + m_dbPath + " does not exist, check existsFor() first");
    }
    m_originalChecksum = computeChecksum();
}

std::uint32_t OneLibraryCueWriter::computeChecksum() const
{
    std::ifstream in(m_dbPath, std::ios::binary);
    if (!in) {
        throw std::runtime_error("onelibrary: " + m_dbPath + " could not be opened to checksum it");
    }
    unsigned long crc = crc32(0L, Z_NULL, 0);
    std::array<char, 65536> buffer;
    while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
        crc = crc32(crc, reinterpret_cast<const Bytef *>(buffer.data()), static_cast<uInt>(in.gcount()));
    }
    return static_cast<std::uint32_t>(crc);
}

void OneLibraryCueWriter::checkNotStale() const
{
    std::error_code statEc;
    auto currentSize = fs::file_size(m_dbPath, statEc);
    auto currentMtime = fs::last_write_time(m_dbPath, statEc);
    bool statMismatch = statEc || currentSize != m_originalFileSize || currentMtime != m_originalMtime;
    bool checksumMismatch = computeChecksum() != m_originalChecksum;
    if (statMismatch || checksumMismatch) {
        throw std::runtime_error("onelibrary: " + m_dbPath +
                                  " changed since this writer was opened, refusing to write a stale copy");
    }
}

void OneLibraryCueWriter::refreshStalenessBaseline()
{
    std::error_code refreshEc;
    m_originalFileSize = fs::file_size(m_dbPath, refreshEc);
    m_originalMtime = fs::last_write_time(m_dbPath, refreshEc);
    m_originalChecksum = computeChecksum();
}

SqlCipherDb &OneLibraryCueWriter::writeConnection()
{
    if (!m_lib) {
        m_lib = std::make_unique<SqlCipherLibrary>();
    }
    if (!m_writeDb) {
        m_writeDb = std::make_unique<SqlCipherDb>(*m_lib, m_dbPath, /*readOnly=*/false);
        m_writeDb->exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    }
    return *m_writeDb;
}

SqlCipherDb &OneLibraryCueWriter::verifyConnection()
{
    if (!m_lib) {
        m_lib = std::make_unique<SqlCipherLibrary>();
    }
    if (!m_verifyDb) {
        m_verifyDb = std::make_unique<SqlCipherDb>(*m_lib, m_dbPath, /*readOnly=*/true);
        m_verifyDb->exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    }
    return *m_verifyDb;
}

void OneLibraryCueWriter::writeCuesForPath(const std::string &filePath, const std::vector<CuePoint> &cues)
{
    // Staleness guard, adapted from PdbRowWriter's own convention to
    // this format: PdbRowWriter reads a whole file into memory once and
    // must re-check nothing else wrote it before a later blind
    // overwrite; here the write goes through a real SQL transaction
    // (SQLite's own file locking already prevents two writers from
    // racing *during* it), so the equivalent check is "does the file on
    // disk still look like the one this writer was constructed
    // against", refusing if something rewrote it out from under us in
    // between construction and this call.
    checkNotStale();

    std::string contentPath = toContentPath(m_stickRoot, filePath);
    SqlCipherDb &db = writeConnection();
    // Every row listing this file gets the same cues; see contentIdsAt().
    const std::vector<int64_t> contentIds = contentIdsAt(db, contentPath);

    // The colour index of every cue each row has now, by slot and
    // position: the domain model carries no OneLibrary colour, so a
    // whole-set rewrite used to reset every cue to colour 0. A cue that
    // keeps its slot and position keeps its colour.
    std::map<int64_t, std::map<std::pair<int64_t, int64_t>, int64_t>> coloursByContentId;
    for (int64_t contentId : contentIds) {
        SqlCipherStatement colours(db, "SELECT kind, inUsec, colorTableIndex FROM cue WHERE content_id = ?");
        colours.bindInt64(1, contentId);
        while (colours.step()) {
            coloursByContentId[contentId][{colours.columnInt64(0), colours.columnInt64(1)}] = colours.columnInt64(2);
        }
    }

    db.exec("BEGIN IMMEDIATE;");
    try {
        for (int64_t contentId : contentIds) {
            const auto &colorByKindAndPosition = coloursByContentId[contentId];
            {
                // Must run before the DELETE FROM cue below. It looks
                // up cue_ids by content_id via a subquery against the
                // still-present cue rows; deleting cue first would leave
                // this a silent no-op and orphan hotCueBankList_cue rows.
                SqlCipherStatement delBank(db,
                                            "DELETE FROM hotCueBankList_cue WHERE cue_id IN "
                                            "(SELECT cue_id FROM cue WHERE content_id = ?)");
                delBank.bindInt64(1, contentId);
                delBank.run();
            }
            {
                SqlCipherStatement del(db, "DELETE FROM cue WHERE content_id = ?");
                del.bindInt64(1, contentId);
                del.run();
            }
            for (const auto &cue : cues) {
                    // kind: 0 for a memory cue, otherwise the hot cue slot
                    // number, matching the DOCUMENTED convention of
                    // master.db's structurally-equivalent djmdCue.Kind
                    // ("0 if memory cue, otherwise the number of Hot Cue").
                    // Device Library Plus's own schema is described by prior
                    // reverse-engineering as "similar to the main Rekordbox
                    // database", and this stick's own OneLibrary cue table
                    // is empty (never populated), so this specific mapping
                    // is a well-reasoned inference, not independently
                    // confirmed against real Device Library Plus data,
                    // see docs/onelibrary-format.md.
                    int kind = cue.kind == CuePoint::Kind::Hot ? cue.hotCueNumber : 0;
                    int64_t inUsec = static_cast<int64_t>(cue.positionMs * 1000.0);
                    // A loop keeps its out point and is flagged as one; a cue
                    // point has out == in, matching export.pdb's convention.
                    const int64_t outUsec = cue.isLoop ? static_cast<int64_t>(cue.loopEndMs * 1000.0) : inUsec;

                    SqlCipherStatement insert(db,
                                               "INSERT INTO cue (content_id, kind, colorTableIndex, cueComment, "
                                               "isActiveLoop, inUsec, outUsec) VALUES (?, ?, ?, ?, ?, ?, ?)");
                    insert.bindInt64(1, contentId);
                    insert.bindInt64(2, kind);
                    // colorTableIndex: no verified RGB/hex -> index mapping
                    // exists anywhere this was cross-checked against (see
                    // docs/onelibrary-format.md), so a cue that was here
                    // before keeps the index it had, and a new one gets 0 (a
                    // defined, inert default) rather than a fabricated guess.
                    auto knownColour = colorByKindAndPosition.find({static_cast<int64_t>(kind), inUsec});
                    insert.bindInt64(3, knownColour == colorByKindAndPosition.end() ? 0 : knownColour->second);
                    if (cue.comment.empty()) {
                        insert.bindNull(4);
                    } else {
                        insert.bindText(4, cue.comment);
                    }
                    insert.bindInt64(5, cue.isLoop ? 1 : 0);
                    insert.bindInt64(6, inUsec);
                    insert.bindInt64(7, outUsec);
                    insert.run();
            }
        }
        db.exec("COMMIT;");
    } catch (...) {
        // Best-effort: never let a rollback failure mask (or replace,
        // via a second throw) the original error that triggered it.
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }

    // Correctness verification: SQLite's transaction already guarantees
    // the commit was durable, but not that this code wrote what it
    // meant to, re-open fresh and read the rows back, mirroring
    // PdbRowWriter::commit()'s "re-parse the edited result before
    // trusting it" check, adapted to "re-read the committed result
    // before trusting it" for a SQL store.
    SqlCipherDb &verifyDb = verifyConnection();
    for (int64_t contentId : contentIds) {
        SqlCipherStatement row(verifyDb, "SELECT count(*) FROM content WHERE content_id = ?");
        row.bindInt64(1, contentId);
        row.step();
        if (row.columnInt64(0) != 1) {
            throw std::runtime_error("onelibrary: post-write verification failed, content row vanished");
        }
        SqlCipherStatement verify(verifyDb, "SELECT count(*) FROM cue WHERE content_id = ?");
        verify.bindInt64(1, contentId);
        verify.step();
        auto actualCount = static_cast<size_t>(verify.columnInt64(0));
        if (actualCount != cues.size()) {
            throw std::runtime_error("onelibrary: post-write verification failed, expected " +
                                      std::to_string(cues.size()) + " cue row(s), found " +
                                      std::to_string(actualCount));
        }
    }

    // Refresh the staleness baseline to the file's new (post-write) state.
    // Without this, reusing one writer instance across several
    // writeCuesForPath() calls (one write per track) would have every
    // call after the first refuse itself, since the file legitimately
    // changed size/mtime due to this writer's *own* prior write.
    refreshStalenessBaseline();
}

void OneLibraryCueWriter::removeTrackByPath(const std::string &filePath)
{
    // Same staleness guard as writeCuesForPath(), see its own comment
    // for the reasoning.
    checkNotStale();

    std::string contentPath = toContentPath(m_stickRoot, filePath);
    SqlCipherDb &db = writeConnection();
    {

        // Every row listing the file; see contentIdsAt().
        const std::vector<int64_t> contentIds = contentIdsAt(db, contentPath);

        db.exec("BEGIN IMMEDIATE;");
        try {
            // No FK/cascade enforcement exists anywhere in this schema
            // (no declared constraints confirmed, and PRAGMA
            // foreign_keys is never set for this connection). Every
            // dependent table is deleted explicitly, in dependency
            // order, exactly like writeCuesForPath()'s own
            // hotCueBankList_cue -> cue ordering, extended one level up
            // to content itself.
            for (int64_t contentId : contentIds) {
                {
                    SqlCipherStatement delBank(db,
                                                "DELETE FROM hotCueBankList_cue WHERE cue_id IN "
                                                "(SELECT cue_id FROM cue WHERE content_id = ?)");
                    delBank.bindInt64(1, contentId);
                    delBank.run();
                }
                {
                    SqlCipherStatement delCue(db, "DELETE FROM cue WHERE content_id = ?");
                    delCue.bindInt64(1, contentId);
                    delCue.run();
                }
                {
                    SqlCipherStatement delPlaylist(db, "DELETE FROM playlist_content WHERE content_id = ?");
                    delPlaylist.bindInt64(1, contentId);
                    delPlaylist.run();
                }
                {
                    SqlCipherStatement delContent(db, "DELETE FROM content WHERE content_id = ?");
                    delContent.bindInt64(1, contentId);
                    delContent.run();
                }
            }
            db.exec("COMMIT;");
        } catch (...) {
            try {
                db.exec("ROLLBACK;");
            } catch (...) {
            }
            throw;
        }
    }  // db closed here

    // Correctness verification: re-open fresh and confirm the row (and
    // its dependents) are actually gone, same "re-read the committed
    // result before trusting it" convention as writeCuesForPath().
    SqlCipherDb &verifyDb = verifyConnection();
    SqlCipherStatement verify(verifyDb, "SELECT count(*) FROM content WHERE path = ?");
    verify.bindText(1, contentPath);
    verify.step();
    if (verify.columnInt64(0) != 0) {
        throw std::runtime_error("onelibrary: post-removal verification failed, content row still present");
    }

    // Refresh the staleness baseline, see writeCuesForPath()'s own
    // comment for why this matters when one writer instance is reused
    // across several calls.
    refreshStalenessBaseline();
}

bool OneLibraryCueWriter::hasTrackAtPath(const std::string &filePath)
{
    checkNotStale();
    SqlCipherStatement find(writeConnection(), "SELECT 1 FROM content WHERE path = ? LIMIT 1");
    find.bindText(1, toContentPath(m_stickRoot, filePath));
    return find.step();
}

void OneLibraryCueWriter::removeTrackByPathReplacingWith(const std::string &doomedFilePath,
                                                           const std::string &survivorFilePath)
{
    // Same staleness guard as writeCuesForPath(), see its own comment
    // for the reasoning.
    checkNotStale();

    std::string doomedContentPath = toContentPath(m_stickRoot, doomedFilePath);
    std::string survivorContentPath = toContentPath(m_stickRoot, survivorFilePath);
    SqlCipherDb &db = writeConnection();

    const std::vector<int64_t> doomedIds = contentIdsAt(db, doomedContentPath);
    // Any one of the survivor's rows will do to repoint playlists at.
    const int64_t survivorId = contentIdsAt(db, survivorContentPath).front();

    for (int64_t doomedId : doomedIds) {
        removeTrackByIdReplacingWith(doomedId, survivorId);
    }
}

void OneLibraryCueWriter::removeTrackByIdReplacingWith(int64_t doomedContentId, int64_t survivorContentId)
{
    checkNotStale();
    if (doomedContentId == survivorContentId) {
        throw std::runtime_error("onelibrary: refusing to replace content row id=" + std::to_string(doomedContentId)
                                 + " with itself");
    }

    auto rowExists = [](SqlCipherDb &db, int64_t contentId) {
        SqlCipherStatement count(db, "SELECT count(*) FROM content WHERE content_id = ?");
        count.bindInt64(1, contentId);
        count.step();
        return count.columnInt64(0) == 1;
    };

    SqlCipherDb &db = writeConnection();
    if (!rowExists(db, doomedContentId)) {
        throw std::runtime_error("onelibrary: no content row id=" + std::to_string(doomedContentId));
    }
    if (!rowExists(db, survivorContentId)) {
        throw std::runtime_error("onelibrary: no content row id=" + std::to_string(survivorContentId));
    }
    {
        db.exec("BEGIN IMMEDIATE;");
        try {
            {
                SqlCipherStatement delBank(db,
                                            "DELETE FROM hotCueBankList_cue WHERE cue_id IN "
                                            "(SELECT cue_id FROM cue WHERE content_id = ?)");
                delBank.bindInt64(1, doomedContentId);
                delBank.run();
            }
            {
                SqlCipherStatement delCue(db, "DELETE FROM cue WHERE content_id = ?");
                delCue.bindInt64(1, doomedContentId);
                delCue.run();
            }
            {
                // Reassign the doomed row's playlist memberships onto the
                // survivor first (matches PdbRowWriter::
                // reassignPlaylistMemberships()'s own dedup rule exactly):
                // a playlist the survivor is already in is left alone --
                // its doomed-side entry is just dropped below, not turned
                // into a second row for the same (playlist, survivor)
                // pair. Everything else genuinely moves.
                SqlCipherStatement reassign(db,
                                             "UPDATE playlist_content SET content_id = ? WHERE content_id = ? "
                                             "AND playlist_id NOT IN "
                                             "(SELECT playlist_id FROM playlist_content WHERE content_id = ?)");
                reassign.bindInt64(1, survivorContentId);
                reassign.bindInt64(2, doomedContentId);
                reassign.bindInt64(3, survivorContentId);
                reassign.run();
            }
            {
                // Whatever's left under doomedContentId at this point is exactly
                // the memberships the UPDATE above skipped (survivor
                // already had them) -- safe to drop outright now.
                SqlCipherStatement delPlaylist(db, "DELETE FROM playlist_content WHERE content_id = ?");
                delPlaylist.bindInt64(1, doomedContentId);
                delPlaylist.run();
            }
            {
                SqlCipherStatement delContent(db, "DELETE FROM content WHERE content_id = ?");
                delContent.bindInt64(1, doomedContentId);
                delContent.run();
            }
            db.exec("COMMIT;");
        } catch (...) {
            try {
                db.exec("ROLLBACK;");
            } catch (...) {
            }
            throw;
        }
    }

    // Correctness verification: re-open fresh and confirm the doomed row
    // is gone AND the survivor's own row still exists (a broken
    // survivor-lookup above would otherwise silently produce a no-op
    // reassignment followed by a real deletion, losing memberships
    // instead of moving them). By id, never by path: a path can name
    // more than one row, and counting by it failed a removal that had
    // worked.
    SqlCipherDb &verifyDb = verifyConnection();
    if (rowExists(verifyDb, doomedContentId)) {
        throw std::runtime_error("onelibrary: post-removal verification failed, content row still present");
    }
    if (!rowExists(verifyDb, survivorContentId)) {
        throw std::runtime_error("onelibrary: post-removal verification failed, survivor content row missing");
    }

    refreshStalenessBaseline();
}

void OneLibraryCueWriter::writeAnnotationForPath(const std::string &filePath, const std::optional<int> &stars,
                                                   const std::optional<std::string> &comment)
{
    if (!stars && !comment) {
        return;
    }

    checkNotStale();

    const std::string contentPath = toContentPath(m_stickRoot, filePath);
    SqlCipherDb &db = writeConnection();

    // Every row listing the file; see contentIdsAt().
    const std::vector<int64_t> contentIds = contentIdsAt(db, contentPath);

    db.exec("BEGIN IMMEDIATE;");
    try {
      for (int64_t contentId : contentIds) {
        if (stars) {
            // Stars, 0 to 5, the same scale rekordbox uses and the same
            // one domain::Track carries. Not Engine's 0-100: measured on
            // the committed fixture, whose one rated track reads 3 here
            // and three stars everywhere else.
            SqlCipherStatement set(db, "UPDATE content SET rating = ? WHERE content_id = ?");
            set.bindInt64(1, *stars);
            set.bindInt64(2, contentId);
            set.run();
        }
        if (comment) {
            // djComment, not "comment" -- the schema has no such column,
            // and a scrub that looked for one silently did nothing for a
            // while (see onelibrary_anonymizer.cpp's note).
            SqlCipherStatement set(db, "UPDATE content SET djComment = ? WHERE content_id = ?");
            set.bindText(1, *comment);
            set.bindInt64(2, contentId);
            set.run();
        }
      }
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }

    // Re-read the committed result through the separate verify
    // connection before trusting it -- the same convention every other
    // write in this class follows, and it costs no extra open because
    // that connection is already held. Without it a rating that did not
    // land would still advance the staleness baseline below, so this
    // writer would record the file as its own successful change and the
    // page would tell the DJ the rating went back.
    SqlCipherDb &verifyDb = verifyConnection();
    for (int64_t contentId : contentIds) {
    if (stars) {
        SqlCipherStatement verify(verifyDb, "SELECT rating FROM content WHERE content_id = ?");
        verify.bindInt64(1, contentId);
        verify.step();
        if (verify.columnIsNull(0) || verify.columnInt64(0) != *stars) {
            throw std::runtime_error("onelibrary: post-write verification failed, rating did not land for "
                                     + contentPath);
        }
    }
    if (comment) {
        SqlCipherStatement verify(verifyDb, "SELECT djComment FROM content WHERE content_id = ?");
        verify.bindInt64(1, contentId);
        verify.step();
        if (verify.columnText(0) != *comment) {
            throw std::runtime_error("onelibrary: post-write verification failed, comment did not land for "
                                     + contentPath);
        }
    }
    }

    refreshStalenessBaseline();
}

void OneLibraryCueWriter::writePlayCountForPath(const std::string &filePath, int playCount)
{
    checkNotStale();

    const std::string contentPath = toContentPath(m_stickRoot, filePath);
    SqlCipherDb &db = writeConnection();

    // Every row listing the file; see contentIdsAt().
    const std::vector<int64_t> contentIds = contentIdsAt(db, contentPath);

    const int64_t count = std::max(0, playCount);
    db.exec("BEGIN IMMEDIATE;");
    try {
        for (int64_t contentId : contentIds) {
            SqlCipherStatement set(db, "UPDATE content SET djPlayCount = ? WHERE content_id = ?");
            set.bindInt64(1, count);
            set.bindInt64(2, contentId);
            set.run();
        }
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }

    // Read back through the verify connection before the staleness
    // baseline moves, for the reason writeAnnotationForPath gives.
    for (int64_t contentId : contentIds) {
        SqlCipherStatement verify(verifyConnection(), "SELECT djPlayCount FROM content WHERE content_id = ?");
        verify.bindInt64(1, contentId);
        verify.step();
        if (verify.columnIsNull(0) || verify.columnInt64(0) != count) {
            throw std::runtime_error("onelibrary: post-write verification failed, play count did not land for "
                                     + contentPath);
        }
    }

    refreshStalenessBaseline();
}

void OneLibraryCueWriter::propagateMissingFieldsForPath(const std::string &donorFilePath,
                                                          const std::string &targetFilePath, bool copyBpm,
                                                          bool copyKey, bool copyArtwork)
{
    if (!copyBpm && !copyKey && !copyArtwork) {
        return;
    }

    checkNotStale();

    std::string donorContentPath = toContentPath(m_stickRoot, donorFilePath);
    std::string targetContentPath = toContentPath(m_stickRoot, targetFilePath);
    SqlCipherDb &db = writeConnection();

    // Every row listing the target gets the field; see contentIdsAt(). From
    // the donor, the first of its rows that has one: a file listed twice
    // can carry a value on one row and nothing on the other.
    const std::vector<int64_t> donorIds = contentIdsAt(db, donorContentPath);
    const std::vector<int64_t> targetIds = contentIdsAt(db, targetContentPath);
    auto donorWith = [&](const std::string &column) {
        for (int64_t id : donorIds) {
            SqlCipherStatement has(db, "SELECT " + column + " IS NOT NULL AND " + column
                                           + " != 0 FROM content WHERE content_id = ?");
            has.bindInt64(1, id);
            if (has.step() && has.columnInt64(0) != 0) {
                return id;
            }
        }
        return donorIds.front();
    };
    const int64_t bpmDonor = copyBpm ? donorWith("bpmx100") : -1;
    const int64_t keyDonor = copyKey ? donorWith("key_id") : -1;
    const int64_t artworkDonor = copyArtwork ? donorWith("image_id") : -1;

    db.exec("BEGIN IMMEDIATE;");
    try {
        auto copyColumn = [&](const std::string &column, int64_t donorId) {
            for (int64_t targetId : targetIds) {
                SqlCipherStatement copy(db, "UPDATE content SET " + column + " = (SELECT " + column
                                                + " FROM content WHERE content_id = ?) WHERE content_id = ?");
                copy.bindInt64(1, donorId);
                copy.bindInt64(2, targetId);
                copy.run();
            }
        };
        if (copyBpm) {
            copyColumn("bpmx100", bpmDonor);
        }
        if (copyKey) {
            copyColumn("key_id", keyDonor);
        }
        if (copyArtwork) {
            copyColumn("image_id", artworkDonor);
        }
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }

    refreshStalenessBaseline();
}

void OneLibraryCueWriter::finishWriting()
{
    // Never opened: nothing was mirrored in this save, so there is no log
    // of ours to fold and no sidecar of ours to remove.
    if (!m_writeDb) {
        return;
    }
    // TRUNCATE rather than PASSIVE: PASSIVE gives up silently when a
    // reader is in the way, which would leave exactly the stranded frames
    // this exists to prevent. The verify connection is this writer's own
    // and is closed first so it cannot be that reader.
    m_verifyDb.reset();

    // The pragma's answer is a ROW -- (busy, log, checkpointed) -- not an
    // error code: a blocked checkpoint returns busy=1 and SQLITE_OK, so
    // exec() would report success while leaving every frame in place.
    // Read the row, and give a reader that is merely mid-statement time to
    // finish rather than calling one moment's contention a failed save.
    m_writeDb->exec("PRAGMA busy_timeout = 5000;");
    bool folded = false;
    for (int attempt = 0; attempt < 3 && !folded; ++attempt) {
        SqlCipherStatement checkpoint(*m_writeDb, "PRAGMA wal_checkpoint(TRUNCATE);");
        folded = checkpoint.step() ? checkpoint.columnInt64(0) == 0 : false;
        if (!folded && attempt + 1 < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    // The -shm indexes the log and cannot be removed while any handle is
    // open, so the connections go before the files do.
    m_writeDb.reset();

    const fs::path wal = fs::path(m_dbPath + "-wal");
    const fs::path shm = fs::path(m_dbPath + "-shm");
    std::error_code ec;
    const bool walThere = fs::exists(wal, ec);
    if (ec) {
        // Cannot even tell whether a log is there -- the stick went away,
        // or came back read-only. That is the one state this must not
        // bless: a save reporting everything written while its rows may
        // be sitting in a file nobody can read.
        throw std::runtime_error("could not tell whether Device Library Plus still has a write-ahead log beside "
                                 "exportLibrary.db, so the save cannot say its rows are in the library");
    }
    const std::uintmax_t remaining = walThere ? fs::file_size(wal, ec) : 0;
    if (ec) {
        throw std::runtime_error("could not read the size of Device Library Plus's write-ahead log, so the save "
                                 "cannot say its rows are in the library");
    }
    if (remaining > 0) {
        throw std::runtime_error("Device Library Plus kept " + std::to_string(remaining)
                                 + " bytes in its write-ahead log after the save; those rows are not in "
                                   "exportLibrary.db and a player reading it would not see them");
    }
    fs::remove(wal, ec);
    fs::remove(shm, ec);
    // No refreshStalenessBaseline() here: the connections are closed and
    // this writer can never write again, so re-reading the whole database
    // off the stick buys nothing -- and computeChecksum() throws when the
    // file cannot be opened, which from inside a finish hook would report
    // a save that fully succeeded as failed.
}

}  // namespace seabass::infrastructure::onelibrary
