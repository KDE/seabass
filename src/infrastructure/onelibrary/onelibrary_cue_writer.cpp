// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

#include <chrono>
#include <thread>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

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
    fs::path rel = fs::relative(pathFromUtf8(filePath), pathFromUtf8(stickRoot), ec);
    std::string relStr = ec ? filePath : pathToGenericUtf8(rel);
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
        throw OneLibraryRowMissing("onelibrary: no content row for path " + contentPath);
    }
    return ids;
}

// The analysis files the rows `contentIds` name, each once, in the
// catalog's own spelling ("/PIONEER/USBANLZ/.../ANLZ0000.DAT"). Rows of
// one file usually name one analysis file, but nothing makes them, so a
// write meant for the file reaches every one they name. Empty for a
// database whose content table has no analysisDataFilePath column.
std::vector<std::string> analysisFilesOf(SqlCipherDb &db, const std::vector<int64_t> &contentIds, bool hasColumn)
{
    std::vector<std::string> files;
    if (!hasColumn) {
        return files;
    }
    for (int64_t contentId : contentIds) {
        SqlCipherStatement find(db, "SELECT analysisDataFilePath FROM content WHERE content_id = ?");
        find.bindInt64(1, contentId);
        if (find.step()) {
            std::string file = find.columnText(0);
            if (!file.empty() && std::find(files.begin(), files.end(), file) == files.end()) {
                files.push_back(std::move(file));
            }
        }
    }
    return files;
}

}  // namespace

namespace
{

constexpr const char *CueTableColumns = "SELECT content_id, cue_id, kind, inUsec, outUsec, isActiveLoop FROM cue";

domain::CueTableEntry cueTableEntry(SqlCipherStatement &select)
{
    domain::CueTableEntry entry;
    entry.cueId = select.columnInt64(1);
    entry.kind = select.columnInt64(2);
    if (entry.understood()) {
        CuePoint &cue = entry.cue;
        cue.kind = entry.kind == 0 ? CuePoint::Kind::Memory : CuePoint::Kind::Hot;
        cue.hotCueNumber = static_cast<int>(entry.kind);
        cue.positionMs = static_cast<double>(select.columnInt64(3)) / 1000.0;
        const int64_t out = select.columnInt64(4);
        cue.isLoop = (!select.columnIsNull(5) && select.columnInt64(5) != 0) || out > select.columnInt64(3);
        cue.loopEndMs = cue.isLoop ? static_cast<double>(out) / 1000.0 : 0.0;
    }
    return entry;
}

}  // namespace

std::vector<domain::CueTableEntry> readCueTable(const SqlCipherDb &db, int64_t contentId)
{
    std::vector<domain::CueTableEntry> entries;
    SqlCipherStatement select(db, std::string(CueTableColumns) + " WHERE content_id = ? ORDER BY cue_id");
    select.bindInt64(1, contentId);
    while (select.step()) {
        entries.push_back(cueTableEntry(select));
    }
    return entries;
}

OneLibraryCueTables::OneLibraryCueTables(const std::string &pioneerRoot)
{
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement select(db, std::string(CueTableColumns) + " ORDER BY content_id, cue_id");
    while (select.step()) {
        m_tables[select.columnInt64(0)].push_back(cueTableEntry(select));
    }
}

const std::vector<domain::CueTableEntry> &OneLibraryCueTables::of(int64_t contentId) const
{
    static const std::vector<domain::CueTableEntry> none;
    const auto found = m_tables.find(contentId);
    return found == m_tables.end() ? none : found->second;
}

std::string OneLibraryCueWriter::dbPathFor(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "exportLibrary.db");
}

bool OneLibraryCueWriter::existsFor(const std::string &pioneerRoot)
{
    std::error_code ec;
    return fs::is_regular_file(pathFromUtf8(dbPathFor(pioneerRoot)), ec);
}

OneLibraryCueWriter::OneLibraryCueWriter(std::string pioneerRoot, std::optional<std::string> realStickRoot)
    : m_pioneerRoot(std::move(pioneerRoot))
{
    // The analysis files are the stick's own, whichever copy of the
    // database this writer was pointed at: pioneerRoot itself when it sits
    // on the stick (whatever the folder is called), the stick's PIONEER
    // folder when the database is a copy somewhere else.
    m_anlzRoot = m_pioneerRoot;
    const auto folder = [](const std::string &path) {
        fs::path normal = pathFromUtf8(path).lexically_normal();
        return normal.has_filename() ? normal : normal.parent_path();  // "/stick/" names "/stick"
    };
    if (realStickRoot && folder(m_pioneerRoot).parent_path() != folder(*realStickRoot)) {
        m_anlzRoot = pathToUtf8(pathFromUtf8(*realStickRoot) / "PIONEER");
    }
    m_stickRoot = realStickRoot ? std::move(*realStickRoot) : pathToUtf8(pathFromUtf8(m_pioneerRoot).parent_path());
    m_dbPath = dbPathFor(m_pioneerRoot);
    m_dbFile = pathFromUtf8(m_dbPath);
    std::error_code ec;
    m_originalFileSize = fs::file_size(m_dbFile, ec);
    m_originalMtime = fs::last_write_time(m_dbFile, ec);
    if (ec) {
        throw std::runtime_error("onelibrary: " + m_dbPath + " does not exist, check existsFor() first");
    }
    m_originalChecksum = computeChecksum();
}

std::uint32_t OneLibraryCueWriter::computeChecksum() const
{
    std::ifstream in(m_dbFile, std::ios::binary);
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
    auto currentSize = fs::file_size(m_dbFile, statEc);
    auto currentMtime = fs::last_write_time(m_dbFile, statEc);
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
    m_originalFileSize = fs::file_size(m_dbFile, refreshEc);
    m_originalMtime = fs::last_write_time(m_dbFile, refreshEc);
    m_originalChecksum = computeChecksum();
}

SqlCipherDb &OneLibraryCueWriter::writeConnection()
{
    if (m_finished) {
        // Reopening here would recreate the -wal and -shm finishWriting()
        // just removed, and the baseline this writer holds no longer
        // describes the file, so the next write would fail with a
        // staleness error that blames the wrong thing.
        throw std::runtime_error("onelibrary: this writer has finished for the save; a new one is needed to write again");
    }
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
    if (m_finished) {
        // Read-only or not, opening a WAL database recreates the -shm and
        // -wal that finishWriting() just removed.
        throw std::runtime_error("onelibrary: this writer has finished for the save; a new one is needed to read back");
    }
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

    // The cues a player shows first: the analysis file each row names,
    // through the writer DeviceLibrary's cues go through, with its
    // read-back and its put-back on failure. Before the table, and put
    // back if the table then fails, so the call lands whole or not at all
    // for a caller that has no backup of its own to restore from.
    //
    // OnlyIfChanged: when DeviceLibrary lists this file it names the same
    // analysis file, and a save mirroring a DeviceLibrary write here finds
    // the cues already in it. Rewriting 167 KB per track to change nothing
    // is what that would otherwise cost on a stick, and it would back the
    // file up for nothing too: the hook runs only for a file that changes.
    std::vector<std::pair<std::string, std::string>> written;  // path, bytes before
    const auto putBack = [&written]() {
        for (const auto &[path, bytes] : written) {
            if (!infrastructure::writeFileDurablyAtomic(path, bytes)) {
                std::cerr << "warning: onelibrary: " << path
                          << " could NOT be put back after a failed cue write and needs restoring from the backup\n";
            }
        }
    };
    try {
        rekordbox::RekordboxCueWriter fileWriter(m_anlzRoot);
        for (const std::string &analysisFile : analysisFilesOf(db, contentIds, hasAnalysisPathColumn(db))) {
            std::error_code ec;
            if (!fs::is_regular_file(pathFromUtf8(rekordbox::extAnlzPath(m_anlzRoot, analysisFile)), ec)) {
                // Nothing analysed the track, or the file was lost. The
                // table below is all there is to write, and no player is
                // known to read it, so say so rather than pass in silence.
                std::cerr << "warning: onelibrary: " << contentPath << " names analysis file " << analysisFile
                          << ", which is not on the stick; its cues went into the cue table only\n";
                continue;
            }
            fileWriter.writeCuesToAnalysisFile(
                analysisFile, cues, rekordbox::RekordboxCueWriter::Rewrite::OnlyIfChanged,
                [this, &written](const std::vector<std::string> &files) {
                    for (const std::string &file : files) {
                        if (m_beforeCueFileWrite) {
                            m_beforeCueFileWrite(file);
                        }
                        std::ifstream in(pathFromUtf8(file), std::ios::binary);
                        if (!in) {
                            throw std::runtime_error("onelibrary: " + file + " could not be read before writing it");
                        }
                        written.push_back(
                            {file, std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())});
                    }
                });
        }
        writeCueRows(db, contentIds, cues);
    } catch (...) {
        putBack();
        throw;
    }
}

void OneLibraryCueWriter::writeCueRows(SqlCipherDb &db, const std::vector<int64_t> &contentIds,
                                       const std::vector<CuePoint> &cues)
{
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

void OneLibraryCueWriter::removeCueRows(const std::vector<std::pair<int64_t, int64_t>> &pairs)
{
    // Each pair once: a pair named twice would be counted twice below.
    const std::set<std::pair<int64_t, int64_t>> contentAndCueIds(pairs.begin(), pairs.end());
    if (contentAndCueIds.empty()) {
        return;
    }
    checkNotStale();
    SqlCipherDb &db = writeConnection();
    // How many cue rows each content row has now, for the read-back.
    std::map<int64_t, int64_t> before;
    for (const auto &[contentId, cueId] : contentAndCueIds) {
        SqlCipherStatement owned(db, "SELECT count(*) FROM cue WHERE cue_id = ? AND content_id = ?");
        owned.bindInt64(1, cueId);
        owned.bindInt64(2, contentId);
        owned.step();
        if (owned.columnInt64(0) != 1) {
            throw OneLibraryRowMissing("onelibrary: cue " + std::to_string(cueId) + " is not a cue of content row "
                                       + std::to_string(contentId));
        }
        if (!before.contains(contentId)) {
            SqlCipherStatement count(db, "SELECT count(*) FROM cue WHERE content_id = ?");
            count.bindInt64(1, contentId);
            count.step();
            before[contentId] = count.columnInt64(0);
        }
    }

    db.exec("BEGIN IMMEDIATE;");
    try {
        for (const auto &[contentId, cueId] : contentAndCueIds) {
            // The bank links first, while the cue they name is there, as
            // writeCueRows() does.
            SqlCipherStatement delBank(db, "DELETE FROM hotCueBankList_cue WHERE cue_id = ?");
            delBank.bindInt64(1, cueId);
            delBank.run();
            SqlCipherStatement del(db, "DELETE FROM cue WHERE cue_id = ? AND content_id = ?");
            del.bindInt64(1, cueId);
            del.bindInt64(2, contentId);
            del.run();
        }
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }

    // Read back on the other connection: those rows and their links gone,
    // every other cue row of those content rows still there.
    SqlCipherDb &verifyDb = verifyConnection();
    std::map<int64_t, int64_t> removed;
    for (const auto &[contentId, cueId] : contentAndCueIds) {
        ++removed[contentId];
        SqlCipherStatement gone(verifyDb, "SELECT (SELECT count(*) FROM cue WHERE cue_id = ?) + "
                                          "(SELECT count(*) FROM hotCueBankList_cue WHERE cue_id = ?)");
        gone.bindInt64(1, cueId);
        gone.bindInt64(2, cueId);
        gone.step();
        if (gone.columnInt64(0) != 0) {
            throw std::runtime_error("onelibrary: post-write verification failed, cue " + std::to_string(cueId)
                                     + " or its bank link is still there");
        }
    }
    for (const auto &[contentId, count] : before) {
        SqlCipherStatement left(verifyDb, "SELECT count(*) FROM cue WHERE content_id = ?");
        left.bindInt64(1, contentId);
        left.step();
        if (left.columnInt64(0) != count - removed[contentId]) {
            throw std::runtime_error("onelibrary: post-write verification failed, content row "
                                     + std::to_string(contentId) + " lost cues it was meant to keep");
        }
    }
    refreshStalenessBaseline();
}

std::vector<domain::CueTableEntry> OneLibraryCueWriter::cueTableOf(int64_t contentId)
{
    return readCueTable(writeConnection(), contentId);
}

std::optional<std::vector<CuePoint>> OneLibraryCueWriter::analysisFileCuesOf(int64_t contentId)
{
    SqlCipherDb &db = writeConnection();
    const auto files = analysisFilesOf(db, {contentId}, hasAnalysisPathColumn(db));
    if (files.empty()) {
        return std::nullopt;
    }
    try {
        // Read by the scan's rule (domain::cueListsRead), so a row the
        // check would leave alone is left alone here too.
        domain::Track::CueListsCheck check = domain::Track::CueListsCheck::NotChecked;
        auto cues = rekordbox::readAnalysisFileCues(*rekordbox::anlzSourceForPioneerRoot(m_anlzRoot), files.front(),
                                                    &check);
        return cues && domain::cueListsRead(check) ? cues : std::nullopt;
    } catch (const rekordbox::AnalysisFileUnreadable &) {
        return std::nullopt;
    }
}

bool OneLibraryCueWriter::hasAnalysisPathColumn(SqlCipherDb &db)
{
    // Fixed for the database's life, so asked once per writer.
    if (!m_hasAnalysisPathColumn) {
        bool found = false;
        SqlCipherStatement columns(db, "PRAGMA table_info(content)");
        while (columns.step()) {
            found = found || columns.columnText(1) == "analysisDataFilePath";
        }
        m_hasAnalysisPathColumn = found;
    }
    return *m_hasAnalysisPathColumn;
}

std::vector<std::string> OneLibraryCueWriter::cueFilesForPath(const std::string &filePath)
{
    checkNotStale();
    SqlCipherDb &db = writeConnection();
    std::vector<std::string> files;
    try {
        for (const std::string &analysisFile :
             analysisFilesOf(db, contentIdsAt(db, toContentPath(m_stickRoot, filePath)), hasAnalysisPathColumn(db))) {
            for (std::string &file : rekordbox::rekordboxCueFilesFor(m_anlzRoot, analysisFile)) {
                std::error_code ec;
                if (fs::is_regular_file(pathFromUtf8(file), ec)) {
                    files.push_back(std::move(file));
                }
            }
        }
    } catch (const OneLibraryRowMissing &) {
        // No row, nothing writeCuesForPath() could write.
    }
    return files;
}

void OneLibraryCueWriter::setBeforeCueFileWrite(std::function<void(const std::string &file)> hook)
{
    m_beforeCueFileWrite = std::move(hook);
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

    // Both lookups throw OneLibraryRowMissing, and a caller cannot tell
    // from the exception which side was missing -- which matters, because
    // the two mean opposite things. A doomed copy this catalog does not
    // list is nothing to do; a SURVIVOR it does not list means the row
    // that would be removed has nothing to be repointed at, and removing
    // it anyway would take the track out of this catalog entirely. The
    // message names the side so a caller's log and refusal can too.
    std::vector<int64_t> doomedIds;
    try {
        doomedIds = contentIdsAt(db, doomedContentPath);
    } catch (const OneLibraryRowMissing &) {
        throw OneLibraryRowMissing("onelibrary: no content row for the copy being removed, path "
                                   + doomedContentPath);
    }
    // Any one of the survivor's rows will do to repoint playlists at.
    int64_t survivorId = 0;
    try {
        survivorId = contentIdsAt(db, survivorContentPath).front();
    } catch (const OneLibraryRowMissing &) {
        throw OneLibrarySurvivorMissing("onelibrary: the copy being kept has no content row, path "
                                        + survivorContentPath);
    }

    // The survivor's own row is in this list when both paths are the
    // same file, which is what two catalog rows for one file look like
    // from here. Skipped rather than refused, because exportLibrary.db
    // does list one file under several rows (193 files on RV2): the
    // OTHER rows at that path are real duplicates and removing them is
    // exactly the job. Only when there is nothing left but the
    // survivor's own row is there nothing to do.
    bool removedAny = false;
    for (int64_t doomedId : doomedIds) {
        if (doomedId == survivorId) {
            continue;
        }
        removeTrackByIdReplacingWith(doomedId, survivorId);
        removedAny = true;
    }
    if (!removedAny) {
        throw OneLibrarySameRow("onelibrary: the copy to remove and the copy to keep are the same content row, id="
                                + std::to_string(survivorId));
    }
}

void OneLibraryCueWriter::removeTrackByIdReplacingWith(int64_t doomedContentId, int64_t survivorContentId)
{
    checkNotStale();
    if (doomedContentId == survivorContentId) {
        throw OneLibrarySameRow("onelibrary: refusing to replace content row id=" + std::to_string(doomedContentId)
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
        throw OneLibraryRowMissing("onelibrary: no content row id=" + std::to_string(doomedContentId));
    }
    if (!rowExists(db, survivorContentId)) {
        // The row to remove is there and the one to put its playlists on
        // is not: the same question the path lookup above answers with
        // its own type, and for the same reason. A caller reading "not
        // listed" as a non-event must not swallow this one.
        throw OneLibrarySurvivorMissing("onelibrary: no content row id=" + std::to_string(survivorContentId)
                                        + " to replace with");
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

namespace
{

// The playlist (not folder) at a full path, walking parents the way the
// reader builds names. Nothing when no playlist has that path.
std::optional<int64_t> playlistIdAtPath(SqlCipherDb &db, const std::string &path)
{
    struct Node
    {
        std::string name;
        int64_t parent = 0;
        int attribute = 0;
    };
    std::map<int64_t, Node> nodes;
    {
        SqlCipherStatement read(db, "SELECT playlist_id, name, playlist_id_parent, attribute FROM playlist");
        while (read.step()) {
            nodes[read.columnInt64(0)] = Node{read.columnText(1), read.columnInt64(2),
                                              static_cast<int>(read.columnInt64(3))};
        }
    }
    for (const auto &[id, node] : nodes) {
        std::vector<std::string> parts;
        int64_t current = id;
        for (int guard = 0; current != 0 && guard < 64; ++guard) {
            const auto it = nodes.find(current);
            if (it == nodes.end()) {
                break;
            }
            parts.push_back(it->second.name);
            current = it->second.parent;
        }
        std::string spelled;
        for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
            spelled += (spelled.empty() ? "" : "/") + *part;
        }
        if (spelled == path) {
            return id;
        }
    }
    return std::nullopt;
}

int64_t membershipRows(SqlCipherDb &db, int64_t playlistId, const std::vector<int64_t> &contentIds)
{
    int64_t rows = 0;
    for (const int64_t contentId : contentIds) {
        SqlCipherStatement count(db, "SELECT count(*) FROM playlist_content WHERE playlist_id = ? AND content_id = ?");
        count.bindInt64(1, playlistId);
        count.bindInt64(2, contentId);
        count.step();
        rows += count.columnInt64(0);
    }
    return rows;
}

// sequenceNo 1, 2, 3... in the playlist's current order, as rekordbox
// leaves a playlist after taking an entry out.
void renumberPlaylistContent(SqlCipherDb &db, int64_t playlistId)
{
    std::vector<int64_t> rowids;
    {
        SqlCipherStatement read(db, "SELECT rowid FROM playlist_content WHERE playlist_id = ? ORDER BY sequenceNo, rowid");
        read.bindInt64(1, playlistId);
        while (read.step()) {
            rowids.push_back(read.columnInt64(0));
        }
    }
    for (size_t i = 0; i < rowids.size(); ++i) {
        SqlCipherStatement update(db, "UPDATE playlist_content SET sequenceNo = ? WHERE rowid = ?");
        update.bindInt64(1, static_cast<int64_t>(i + 1));
        update.bindInt64(2, rowids[i]);
        update.run();
    }
}

// One level of the tree in the order given, sequenceNo from 0.
void numberPlaylistLevel(SqlCipherDb &db, const std::vector<int64_t> &ids)
{
    for (size_t i = 0; i < ids.size(); ++i) {
        SqlCipherStatement update(db, "UPDATE playlist SET sequenceNo = ? WHERE playlist_id = ?");
        update.bindInt64(1, static_cast<int64_t>(i));
        update.bindInt64(2, ids[i]);
        update.run();
    }
}

struct PlaylistRow
{
    int64_t id = 0;
    int64_t parentId = 0;
    int64_t sequenceNo = 0;
    bool isFolder = false;
    std::string name;
};

std::vector<PlaylistRow> playlistRows(SqlCipherDb &db)
{
    std::vector<PlaylistRow> rows;
    SqlCipherStatement read(db, "SELECT playlist_id, coalesce(playlist_id_parent, 0), coalesce(sequenceNo, 0), "
                                "coalesce(attribute, 0), coalesce(name, '') FROM playlist ORDER BY playlist_id_parent, sequenceNo, playlist_id");
    while (read.step()) {
        rows.push_back({read.columnInt64(0), read.columnInt64(1), read.columnInt64(2), read.columnInt64(3) == 1, read.columnText(4)});
    }
    return rows;
}

std::vector<int64_t> levelIds(const std::vector<PlaylistRow> &rows, int64_t parentId)
{
    std::vector<int64_t> ids;
    for (const auto &r : rows) {
        if (r.parentId == parentId) {
            ids.push_back(r.id);
        }
    }
    return ids;
}

std::vector<int64_t> contentOrder(SqlCipherDb &db, int64_t playlistId)
{
    std::vector<int64_t> ids;
    SqlCipherStatement read(db, "SELECT content_id FROM playlist_content WHERE playlist_id = ? ORDER BY sequenceNo, rowid");
    read.bindInt64(1, playlistId);
    while (read.step()) {
        ids.push_back(read.columnInt64(0));
    }
    return ids;
}

}  // namespace

bool OneLibraryCueWriter::addToPlaylist(const std::string &playlistPath, const std::string &filePath)
{
    checkNotStale();
    const std::string contentPath = toContentPath(m_stickRoot, filePath);
    SqlCipherDb &db = writeConnection();
    const auto playlistId = playlistIdAtPath(db, playlistPath);
    if (!playlistId) {
        throw std::runtime_error("onelibrary: no playlist \"" + playlistPath + "\"");
    }
    const std::vector<int64_t> contentIds = contentIdsAt(db, contentPath);
    if (contentIds.empty()) {
        throw OneLibraryRowMissing("onelibrary: no content row lists " + filePath);
    }
    if (membershipRows(db, *playlistId, contentIds) > 0) {
        return false;
    }
    db.exec("BEGIN IMMEDIATE;");
    try {
        int64_t next = 1;
        {
            SqlCipherStatement last(db, "SELECT coalesce(max(sequenceNo), 0) FROM playlist_content WHERE playlist_id = ?");
            last.bindInt64(1, *playlistId);
            last.step();
            next = last.columnInt64(0) + 1;
        }
        SqlCipherStatement insert(db, "INSERT INTO playlist_content (playlist_id, content_id, sequenceNo) VALUES (?, ?, ?)");
        insert.bindInt64(1, *playlistId);
        insert.bindInt64(2, contentIds.front());
        insert.bindInt64(3, next);
        insert.run();
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
    if (membershipRows(verifyConnection(), *playlistId, contentIds) == 0) {
        throw std::runtime_error("onelibrary: post-write verification failed, " + filePath + " is not in \""
                                 + playlistPath + "\"");
    }
    refreshStalenessBaseline();
    return true;
}

bool OneLibraryCueWriter::removeFromPlaylist(const std::string &playlistPath, const std::string &filePath)
{
    checkNotStale();
    const std::string contentPath = toContentPath(m_stickRoot, filePath);
    SqlCipherDb &db = writeConnection();
    const auto playlistId = playlistIdAtPath(db, playlistPath);
    if (!playlistId) {
        throw std::runtime_error("onelibrary: no playlist \"" + playlistPath + "\"");
    }
    const std::vector<int64_t> contentIds = contentIdsAt(db, contentPath);
    if (contentIds.empty()) {
        throw OneLibraryRowMissing("onelibrary: no content row lists " + filePath);
    }
    if (membershipRows(db, *playlistId, contentIds) == 0) {
        return false;
    }
    db.exec("BEGIN IMMEDIATE;");
    try {
        for (const int64_t contentId : contentIds) {
            SqlCipherStatement del(db, "DELETE FROM playlist_content WHERE playlist_id = ? AND content_id = ?");
            del.bindInt64(1, *playlistId);
            del.bindInt64(2, contentId);
            del.run();
        }
        renumberPlaylistContent(db, *playlistId);
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
    if (membershipRows(verifyConnection(), *playlistId, contentIds) != 0) {
        throw std::runtime_error("onelibrary: post-write verification failed, " + filePath + " is still in \""
                                 + playlistPath + "\"");
    }
    refreshStalenessBaseline();
    return true;
}

std::vector<OneLibraryCueWriter::PlaylistNode> OneLibraryCueWriter::playlistTree()
{
    std::vector<PlaylistNode> nodes;
    for (const auto &r : playlistRows(verifyConnection())) {
        nodes.push_back({r.id, r.parentId, r.sequenceNo, r.isFolder, r.name});
    }
    return nodes;
}

int OneLibraryCueWriter::playlistCountAtPath(const std::string &path)
{
    const auto rows = playlistRows(verifyConnection());
    std::map<int64_t, const PlaylistRow *> byId;
    for (const auto &r : rows) {
        byId[r.id] = &r;
    }
    int count = 0;
    for (const auto &r : rows) {
        std::string spelled = r.name;
        int guard = 0;
        for (int64_t p = r.parentId; p != 0 && byId.count(p) && guard < 64; p = byId.at(p)->parentId, ++guard) {
            spelled = byId.at(p)->name + "/" + spelled;
        }
        count += spelled == path ? 1 : 0;
    }
    return count;
}

std::vector<std::pair<int64_t, int64_t>> OneLibraryCueWriter::playlistContent(const std::string &playlistPath)
{
    SqlCipherDb &db = verifyConnection();
    const auto id = playlistIdAtPath(db, playlistPath);
    if (!id) {
        throw std::runtime_error("onelibrary: no playlist \"" + playlistPath + "\"");
    }
    std::vector<std::pair<int64_t, int64_t>> rows;
    SqlCipherStatement read(db, "SELECT content_id, sequenceNo FROM playlist_content WHERE playlist_id = ? ORDER BY sequenceNo, rowid");
    read.bindInt64(1, *id);
    while (read.step()) {
        rows.emplace_back(read.columnInt64(0), read.columnInt64(1));
    }
    return rows;
}

bool OneLibraryCueWriter::isInAnyPlaylist(const std::string &filePath)
{
    SqlCipherDb &db = verifyConnection();
    for (const int64_t contentId : contentIdsAt(db, toContentPath(m_stickRoot, filePath))) {
        SqlCipherStatement count(db, "SELECT count(*) FROM playlist_content WHERE content_id = ?");
        count.bindInt64(1, contentId);
        count.step();
        if (count.columnInt64(0) > 0) {
            return true;
        }
    }
    return false;
}

int64_t OneLibraryCueWriter::playlistContentRowsWithoutPlaylist()
{
    SqlCipherStatement count(verifyConnection(),
                             "SELECT count(*) FROM playlist_content WHERE playlist_id NOT IN (SELECT playlist_id FROM playlist)");
    count.step();
    return count.columnInt64(0);
}

int64_t OneLibraryCueWriter::createPlaylist(const std::string &parentPath, const std::string &name, bool isFolder,
                                            std::optional<size_t> position)
{
    if (name.empty() || name.find('/') != std::string::npos) {
        throw std::invalid_argument("onelibrary: a playlist name must be non-empty and hold no '/'");
    }
    checkNotStale();
    SqlCipherDb &db = writeConnection();
    const auto rows = playlistRows(db);
    int64_t parentId = 0;
    if (!parentPath.empty()) {
        const auto parent = playlistIdAtPath(db, parentPath);
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto &r) { return parent && r.id == *parent; });
        if (row == rows.end() || !row->isFolder) {
            throw std::invalid_argument("onelibrary: no folder \"" + parentPath + "\"");
        }
        parentId = *parent;
    }
    for (const auto &r : rows) {
        if (r.parentId == parentId && r.name == name) {
            throw std::invalid_argument("onelibrary: \"" + name + "\" is already there");
        }
    }
    int64_t id = 1;
    for (const auto &r : rows) {
        id = std::max(id, r.id + 1);
    }
    std::vector<int64_t> level = levelIds(rows, parentId);
    level.insert(level.begin() + static_cast<std::ptrdiff_t>(std::min(position.value_or(level.size()), level.size())), id);
    db.exec("BEGIN IMMEDIATE;");
    try {
        SqlCipherStatement insert(db, "INSERT INTO playlist (playlist_id, sequenceNo, name, image_id, attribute, playlist_id_parent) "
                                      "VALUES (?, 0, ?, NULL, ?, ?)");
        insert.bindInt64(1, id);
        insert.bindText(2, name);
        insert.bindInt64(3, isFolder ? 1 : 0);
        insert.bindInt64(4, parentId);
        insert.run();
        numberPlaylistLevel(db, level);
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
    const std::string path = parentPath.empty() ? name : parentPath + "/" + name;
    if (playlistIdAtPath(verifyConnection(), path) != id) {
        throw std::runtime_error("onelibrary: post-write verification failed, \"" + path + "\" is not there");
    }
    refreshStalenessBaseline();
    return id;
}

size_t OneLibraryCueWriter::deletePlaylist(const std::string &path)
{
    checkNotStale();
    SqlCipherDb &db = writeConnection();
    const auto id = playlistIdAtPath(db, path);
    if (!id) {
        return 0;
    }
    const auto rows = playlistRows(db);
    std::set<int64_t> doomed{*id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto &r : rows) {
            if (doomed.count(r.parentId) && doomed.insert(r.id).second) {
                grew = true;
            }
        }
    }
    int64_t parentId = 0;
    for (const auto &r : rows) {
        if (r.id == *id) {
            parentId = r.parentId;
        }
    }
    std::vector<int64_t> level;
    for (const int64_t sibling : levelIds(rows, parentId)) {
        if (sibling != *id) {
            level.push_back(sibling);
        }
    }
    db.exec("BEGIN IMMEDIATE;");
    try {
        for (const int64_t gone : doomed) {
            SqlCipherStatement content(db, "DELETE FROM playlist_content WHERE playlist_id = ?");
            content.bindInt64(1, gone);
            content.run();
            SqlCipherStatement row(db, "DELETE FROM playlist WHERE playlist_id = ?");
            row.bindInt64(1, gone);
            row.run();
        }
        numberPlaylistLevel(db, level);
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
    if (playlistIdAtPath(verifyConnection(), path)) {
        throw std::runtime_error("onelibrary: post-write verification failed, \"" + path + "\" is still there");
    }
    refreshStalenessBaseline();
    return doomed.size();
}

bool OneLibraryCueWriter::reorderPlaylist(const std::string &playlistPath, const std::vector<std::string> &filePaths)
{
    checkNotStale();
    SqlCipherDb &db = writeConnection();
    const auto id = playlistIdAtPath(db, playlistPath);
    if (!id) {
        throw std::runtime_error("onelibrary: no playlist \"" + playlistPath + "\"");
    }
    const std::vector<int64_t> now = contentOrder(db, *id);
    std::vector<int64_t> wanted;
    for (const auto &file : filePaths) {
        const std::vector<int64_t> ids = contentIdsAt(db, toContentPath(m_stickRoot, file));
        const auto member = std::find_if(ids.begin(), ids.end(), [&](int64_t c) {
            return std::find(now.begin(), now.end(), c) != now.end();
        });
        if (member == ids.end()) {
            throw std::invalid_argument("onelibrary: " + file + " is not in \"" + playlistPath + "\"");
        }
        wanted.push_back(*member);
    }
    if (wanted == now) {
        return false;
    }
    std::vector<int64_t> a = now;
    std::vector<int64_t> b = wanted;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) {
        throw std::invalid_argument("onelibrary: a new order must hold the playlist's files, each as often as now");
    }
    db.exec("BEGIN IMMEDIATE;");
    try {
        SqlCipherStatement clear(db, "DELETE FROM playlist_content WHERE playlist_id = ?");
        clear.bindInt64(1, *id);
        clear.run();
        for (size_t i = 0; i < wanted.size(); ++i) {
            SqlCipherStatement insert(db, "INSERT INTO playlist_content (playlist_id, content_id, sequenceNo) VALUES (?, ?, ?)");
            insert.bindInt64(1, *id);
            insert.bindInt64(2, wanted[i]);
            insert.bindInt64(3, static_cast<int64_t>(i + 1));
            insert.run();
        }
        db.exec("COMMIT;");
    } catch (...) {
        try {
            db.exec("ROLLBACK;");
        } catch (...) {
        }
        throw;
    }
    if (contentOrder(verifyConnection(), *id) != wanted) {
        throw std::runtime_error("onelibrary: post-write verification failed, \"" + playlistPath + "\" is not in the new order");
    }
    refreshStalenessBaseline();
    return true;
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

std::optional<std::uint64_t> OneLibraryCueWriter::foldLogOf(const std::string &dbPath)
{
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(dbPath), ec) || ec) {
        return std::nullopt;  // no database to fold into, or it cannot be looked at
    }
    try {
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, dbPath, /*readOnly=*/false);
        db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
        try {
            db.exec("PRAGMA busy_timeout = 1000;");
            SqlCipherStatement checkpoint(db, "PRAGMA wal_checkpoint(TRUNCATE);");
            checkpoint.step();
        } catch (const std::exception &) {
        }
    } catch (const std::exception &) {
        // Cannot even open it: the measurement below still says what is left.
    }
    const fs::path wal = pathFromUtf8(dbPath + "-wal");
    const bool walThere = fs::exists(wal, ec);
    if (ec) {
        return std::nullopt;
    }
    if (!walThere) {
        return 0;  // one file again
    }
    const std::uintmax_t left = fs::file_size(wal, ec);
    return ec ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{left};
}

void OneLibraryCueWriter::finishWriting()
{
    // Never opened: nothing was mirrored in this save, so there is no log of
    // ours to fold and no sidecar of ours to remove.
    if (!m_writeDb) {
        return;
    }

    // One rule, deliberately: after this, either the library is one file or
    // the caller hears about it. Five review rounds went into trying to tell
    // a busy checkpoint from a broken one -- SqlCipherStatement reports every
    // non-ROW result the same way, so every attempt at classifying ended up
    // either failing good saves or warning about them. What matters is not
    // why the fold did not happen but whether it did.
    m_verifyDb.reset();
    // Advisory, and inside its own try: exec() throws on any non-OK rc,
    // and a throw here would reach runFinishHooks as an ERROR -- the one
    // outcome this function must never produce. 1 s, not 5: the pragma
    // makes each checkpoint attempt block on the busy handler already, so
    // three attempts at 5 s froze the GUI at "Finishing" for 15 s.
    try {
        m_writeDb->exec("PRAGMA busy_timeout = 1000;");
    } catch (const std::exception &) {
    }
    bool folded = false;
    for (int attempt = 0; attempt < 3 && !folded; ++attempt) {
        try {
            SqlCipherStatement checkpoint(*m_writeDb, "PRAGMA wal_checkpoint(TRUNCATE);");
            folded = checkpoint.step() && checkpoint.columnInt64(0) == 0;
        } catch (const std::exception &) {
            folded = false;  // busy is the common case and is what the retries are for
        }
        if (!folded && attempt + 1 < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // The last connection closing is itself a checkpoint: SQLite folds the
    // log and removes both sidecars. So this is where the answer is decided,
    // not in the loop above.
    m_writeDb.reset();
    m_finished = true;

    const fs::path wal = pathFromUtf8(m_dbPath + "-wal");
    const fs::path shm = pathFromUtf8(m_dbPath + "-shm");
    std::error_code ec;
    // The database itself first. fs::exists clears its error code for a
    // path that is simply not there, so on a stick that went away after
    // the last commit the -wal reads as "absent" -- which looked like the
    // clean outcome, and reported "Done" for rows that live only in a log
    // on a device that is gone.
    std::error_code dbEc;
    const bool dbThere = fs::is_regular_file(m_dbFile, dbEc) && !dbEc;
    const bool walThere = fs::exists(wal, ec);
    std::uintmax_t remaining = 0;
    if (!ec && walThere) {
        remaining = fs::file_size(wal, ec);
        if (ec) {
            remaining = 0;  // file_size's failure value is uintmax_t(-1), not a byte count
        }
    }
    if (dbThere && !ec && remaining == 0) {
        // The library is one file again. A -wal that is simply gone is the
        // ordinary outcome, not a fault: the close folded it.
        fs::remove(wal, ec);
        fs::remove(shm, ec);
        return;
    }

    // Frames left, or a log we cannot measure. Always the warning type,
    // never an error: the rows are committed either way, and reporting the
    // save as unapplied would have the user save again and apply the same
    // removals twice.
    throw OneLibraryLogNotFolded(
        !dbThere ? std::string("exportLibrary.db is no longer where Device Library Plus wrote it, so the save "
                               "cannot say its rows are in the library (did the stick go away?)")
        : ec     ? std::string("could not tell whether Device Library Plus still has a write-ahead log beside "
                               "exportLibrary.db, so the save cannot say its rows are in the library")
                 : "Device Library Plus kept " + std::to_string(remaining)
                       + " bytes in its write-ahead log; those rows are not in exportLibrary.db and a player "
                         "reading it would not see them");
}

}  // namespace seabass::infrastructure::onelibrary
