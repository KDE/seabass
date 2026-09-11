#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::onelibrary;
using namespace seabass::domain;
namespace fs = std::filesystem;

namespace
{

// Builds a minimal-but-real exportLibrary.db fixture: SQLCipher-encrypted
// with the real, derived, universal OneLibrary key, containing just
// enough schema (content, cue, hotCueBankList_cue) for OneLibraryCueWriter
// to operate on. Column names/types match a real stick's `.schema` output
// exactly (captured during development, see docs/onelibrary-format.md),
// trimmed to what these tests exercise.
void createFixture(const std::string &pioneerRoot)
{
    fs::create_directories(fs::path(pioneerRoot) / "rekordbox");
    std::string dbPath = OneLibraryCueWriter::dbPathFor(pioneerRoot);

    std::string key = deriveOneLibraryKey();
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbPath, /*readOnly=*/false);
    db.exec("PRAGMA key = '" + key + "';");
    db.exec(
        "CREATE TABLE content(content_id integer primary key, title varchar, path varchar, "
        "bpmx100 integer, key_id integer, image_id integer);");
    db.exec(
        "CREATE TABLE cue(cue_id integer primary key, content_id integer, kind integer, "
        "colorTableIndex integer, cueComment varchar, isActiveLoop integer, inUsec integer, "
        "outUsec integer);");
    db.exec("CREATE TABLE hotCueBankList_cue(hotCueBankList_id integer, cue_id integer, sequenceNo integer);");
    db.exec("CREATE TABLE playlist_content(content_id integer, playlist_id integer, sequenceNo integer);");
    db.exec("CREATE TABLE playlist(playlist_id integer primary key, name varchar, playlist_id_parent integer);");
    db.exec("INSERT INTO content (content_id, title, path) VALUES (1, 'Test Track', '/Contents/Test Track.mp3');");
}

std::vector<CuePoint> sampleCues()
{
    CuePoint hot1{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"};
    CuePoint hot2{CuePoint::Kind::Hot, 2, 5500.5, "#00FF00", ""};
    CuePoint mem{CuePoint::Kind::Memory, 0, 12345.0, "#0000FF", "breakdown"};
    return {hot1, hot2, mem};
}

// Fresh empty scratch dir with a fixture in it, ready for one test case.
fs::path freshScratch()
{
    fs::path scratch = seabass::testing::scratchRoot() / "seabass_onelibrary_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    return scratch;
}

// Raw byte-level read of the database file, used by case 15 to establish
// that its external modification really did change the file's contents
// even though size and mtime both stayed put -- i.e. that the only signal
// left for the staleness guard is the bytes themselves.
std::string readWholeFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main()
{
    // Case 1: write cues, read them back through an entirely independent
    // second connection (not the writer's own success return), proves
    // a real round-trip, not just "the write call didn't throw".
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        assert(OneLibraryCueWriter::existsFor(pioneerRoot.string()));

        OneLibraryCueWriter writer(pioneerRoot.string());
        std::string filePath = (scratch / "Contents" / "Test Track.mp3").string();
        writer.writeCuesForPath(filePath, sampleCues());

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");

        SqlCipherStatement countStmt(db, "SELECT count(*) FROM cue WHERE content_id = 1");
        countStmt.step();
        assert(countStmt.columnInt64(0) == 3);

        SqlCipherStatement hotStmt(
            db, "SELECT inUsec, cueComment FROM cue WHERE content_id = 1 AND kind = 1");
        assert(hotStmt.step());
        assert(hotStmt.columnInt64(0) == 1000000);  // 1000.0ms -> 1,000,000us
        assert(hotStmt.columnText(1) == "drop");

        SqlCipherStatement memStmt(db, "SELECT inUsec FROM cue WHERE content_id = 1 AND kind = 0");
        assert(memStmt.step());
        assert(memStmt.columnInt64(0) == 12345000);  // 12345.0ms -> 12,345,000us

        std::cout << "case 1 (round-trip write/read via an independent reader) OK\n";
    }

    // Case 2: the staleness guard refuses a write if the file changed
    // since this writer was constructed (simulating another process/
    // connection touching it in between).
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        OneLibraryCueWriter writer(pioneerRoot.string());

        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("INSERT INTO content (content_id, title, path) VALUES (2, 'Other', '/Contents/Other.mp3');");
        }

        bool threw = false;
        try {
            writer.writeCuesForPath((scratch / "Contents" / "Test Track.mp3").string(), sampleCues());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 2 (staleness guard refuses a write after external modification) OK\n";
    }

    // Case 3: writing cues for a path with no matching content.path row
    // fails cleanly, and doesn't corrupt/touch anything else.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        OneLibraryCueWriter writer(pioneerRoot.string());

        bool threw = false;
        try {
            writer.writeCuesForPath((scratch / "Contents" / "Nonexistent.mp3").string(), sampleCues());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");
        SqlCipherStatement contentCount(db, "SELECT count(*) FROM content");
        contentCount.step();
        assert(contentCount.columnInt64(0) == 1);
        SqlCipherStatement cueCount(db, "SELECT count(*) FROM cue");
        cueCount.step();
        assert(cueCount.columnInt64(0) == 0);

        std::cout << "case 3 (write to unknown path fails cleanly, nothing corrupted) OK\n";
    }

    // Case 4: re-writing cues onto the same track replaces the whole set
    // (matching application::CueWriter's "pass the complete list" contract),
    // rather than appending to the previous write.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string filePath = (scratch / "Contents" / "Test Track.mp3").string();

        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            writer.writeCuesForPath(filePath, sampleCues());
        }
        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            std::vector<CuePoint> single = {CuePoint{CuePoint::Kind::Hot, 3, 2000.0, "#FFFFFF", "only one now"}};
            writer.writeCuesForPath(filePath, single);
        }

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");
        SqlCipherStatement countStmt(db, "SELECT count(*) FROM cue WHERE content_id = 1");
        countStmt.step();
        assert(countStmt.columnInt64(0) == 1);

        std::cout << "case 4 (re-write replaces the whole cue set, not appends) OK\n";
    }

    // Case 5: one writer instance, reused for several sequential
    // writeCuesForPath() calls across different tracks. The staleness
    // baseline must refresh after each successful write, or every call
    // after the first would spuriously refuse itself (see
    // onelibrary_cue_writer.cpp's refresh at the end of
    // writeCuesForPath()). Matches how local_cue_controller.cpp actually
    // uses this class (one writer, one write per restore candidate).
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("INSERT INTO content (content_id, title, path) VALUES (2, 'Second Track', '/Contents/Second Track.mp3');");
        }

        OneLibraryCueWriter writer(pioneerRoot.string());
        writer.writeCuesForPath((scratch / "Contents" / "Test Track.mp3").string(), sampleCues());
        // Would previously throw here (stale baseline from before the
        // first write above) if the baseline weren't refreshed.
        writer.writeCuesForPath((scratch / "Contents" / "Second Track.mp3").string(), sampleCues());

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");
        SqlCipherStatement c1(db, "SELECT count(*) FROM cue WHERE content_id = 1");
        c1.step();
        assert(c1.columnInt64(0) == 3);
        SqlCipherStatement c2(db, "SELECT count(*) FROM cue WHERE content_id = 2");
        c2.step();
        assert(c2.columnInt64(0) == 3);

        std::cout << "case 5 (one writer reused across sequential writes to different tracks) OK\n";
    }

    // Case 6: removeTrackByPath() deletes the content row and every
    // dependent row (cue, hotCueBankList_cue, playlist_content). No FK/
    // cascade enforcement exists in this schema (see
    // docs/onelibrary-format.md), so this is exactly the risk a blind
    // single-table DELETE would miss.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string filePath = (scratch / "Contents" / "Test Track.mp3").string();

        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            writer.writeCuesForPath(filePath, sampleCues());
        }
        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("INSERT INTO playlist_content (content_id, playlist_id, sequenceNo) VALUES (1, 99, 0);");
        }

        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            writer.removeTrackByPath(filePath);
        }

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");

        SqlCipherStatement contentCount(db, "SELECT count(*) FROM content WHERE content_id = 1");
        contentCount.step();
        assert(contentCount.columnInt64(0) == 0);
        SqlCipherStatement cueCount(db, "SELECT count(*) FROM cue WHERE content_id = 1");
        cueCount.step();
        assert(cueCount.columnInt64(0) == 0);
        SqlCipherStatement playlistCount(db, "SELECT count(*) FROM playlist_content WHERE content_id = 1");
        playlistCount.step();
        assert(playlistCount.columnInt64(0) == 0);
        // hotCueBankList_cue has no content_id column of its own. It's
        // only reachable via cue_id, which is exactly why the DELETE
        // must run before (not after) the cue rows it depends on are
        // gone. Confirm no bank rows reference cue_ids that no longer
        // exist in cue at all (a real orphan, since cue is now empty).
        SqlCipherStatement bankCount(db, "SELECT count(*) FROM hotCueBankList_cue");
        bankCount.step();
        assert(bankCount.columnInt64(0) == 0);

        std::cout << "case 6 (removeTrackByPath deletes content + every dependent row) OK\n";
    }

    // Case 7: removing a path with no matching content.path row fails
    // cleanly, same "nothing corrupted" contract as writeCuesForPath()'s
    // own unknown-path case.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        OneLibraryCueWriter writer(pioneerRoot.string());

        bool threw = false;
        try {
            writer.removeTrackByPath((scratch / "Contents" / "Nonexistent.mp3").string());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");
        SqlCipherStatement contentCount(db, "SELECT count(*) FROM content");
        contentCount.step();
        assert(contentCount.columnInt64(0) == 1);

        std::cout << "case 7 (removeTrackByPath on an unknown path fails cleanly) OK\n";
    }

    // Case 8: the staleness guard applies to removeTrackByPath() too -
    // refuses if the file changed since this writer was constructed.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        OneLibraryCueWriter writer(pioneerRoot.string());

        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("INSERT INTO content (content_id, title, path) VALUES (2, 'Other', '/Contents/Other.mp3');");
        }

        bool threw = false;
        try {
            writer.removeTrackByPath((scratch / "Contents" / "Test Track.mp3").string());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 8 (staleness guard refuses removeTrackByPath after external modification) OK\n";
    }

    // Case 9: the writer can be pointed at a relocated copy of the
    // database (e.g. a fast local scratch copy built to avoid slow
    // removable-media I/O) while content-path lookups still resolve
    // against the real stick root, passed explicitly. Regression test
    // for a real bug: sync's whole-file-replace path redirected this
    // writer at a scratch directory without this parameter, so every
    // content.path lookup was computed relative to the scratch
    // directory's own parent (e.g. /tmp) instead of the real stick,
    // silently failing every single write.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string realFilePath = (scratch / "Contents" / "Test Track.mp3").string();

        fs::path relocated = seabass::testing::scratchRoot() / "seabass_onelibrary_relocated_test";
        std::error_code ec;
        fs::remove_all(relocated, ec);
        fs::create_directories(relocated / "rekordbox");
        fs::copy_file(OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), relocated / "rekordbox" / "exportLibrary.db");

        // Without realStickRoot, this would derive the stick root as
        // relocated's own parent (seabass::testing::scratchRoot()) -- nothing
        // under there matches realFilePath, so the lookup would fail.
        // Passing it explicitly is what this test actually verifies.
        OneLibraryCueWriter writer(relocated.string(), scratch.string());
        writer.writeCuesForPath(realFilePath, sampleCues());

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(relocated.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");
        SqlCipherStatement countStmt(db, "SELECT count(*) FROM cue WHERE content_id = 1");
        countStmt.step();
        assert(countStmt.columnInt64(0) == 3);

        fs::remove_all(relocated, ec);
        std::cout << "case 9 (writer against a relocated db resolves content paths via the real stick root) OK\n";
    }

    // Case 10: removeTrackByPathReplacingWith() moves the doomed row's
    // playlist membership onto the survivor instead of dropping it.
    // Regression test for a real bug: every OneLibrary-mirror call site
    // in this codebase used to call plain removeTrackByPath() even when
    // a survivor was in scope, silently losing playlist membership on
    // every duplicate-cleanup/consistency-repair operation that touched
    // OneLibrary.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string doomedPath = (scratch / "Contents" / "Test Track.mp3").string();
        std::string survivorPath = (scratch / "Contents" / "Survivor Track.mp3").string();

        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec(
                "INSERT INTO content (content_id, title, path) VALUES (2, 'Survivor', "
                "'/Contents/Survivor Track.mp3');");
            // Doomed track (content_id 1, from createFixture) is in
            // playlist 99. Survivor isn't in any playlist yet.
            db.exec("INSERT INTO playlist_content (content_id, playlist_id, sequenceNo) VALUES (1, 99, 0);");
        }

        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            writer.removeTrackByPathReplacingWith(doomedPath, survivorPath);
        }

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");

        SqlCipherStatement doomedGone(db, "SELECT count(*) FROM content WHERE content_id = 1");
        doomedGone.step();
        assert(doomedGone.columnInt64(0) == 0);

        SqlCipherStatement survivorInPlaylist(db, "SELECT count(*) FROM playlist_content WHERE content_id = 2 AND playlist_id = 99");
        survivorInPlaylist.step();
        assert(survivorInPlaylist.columnInt64(0) == 1);

        SqlCipherStatement doomedPlaylistGone(db, "SELECT count(*) FROM playlist_content WHERE content_id = 1");
        doomedPlaylistGone.step();
        assert(doomedPlaylistGone.columnInt64(0) == 0);

        std::cout << "case 10 (removeTrackByPathReplacingWith reassigns playlist membership to the survivor) OK\n";
    }

    // Case 11: the dedup rule -- if the survivor is already in a
    // playlist the doomed track was also in, that membership is just
    // dropped, not duplicated into a second row for the same
    // (playlist, survivor) pair. Matches PdbRowWriter::
    // reassignPlaylistMemberships()'s own rule exactly.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string doomedPath = (scratch / "Contents" / "Test Track.mp3").string();
        std::string survivorPath = (scratch / "Contents" / "Survivor Track.mp3").string();

        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec(
                "INSERT INTO content (content_id, title, path) VALUES (2, 'Survivor', "
                "'/Contents/Survivor Track.mp3');");
            // Both doomed (content_id 1) and survivor (content_id 2)
            // are already in playlist 99.
            db.exec("INSERT INTO playlist_content (content_id, playlist_id, sequenceNo) VALUES (1, 99, 0);");
            db.exec("INSERT INTO playlist_content (content_id, playlist_id, sequenceNo) VALUES (2, 99, 1);");
        }

        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            writer.removeTrackByPathReplacingWith(doomedPath, survivorPath);
        }

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");

        // Exactly one row for (playlist 99, survivor) -- not duplicated.
        SqlCipherStatement survivorInPlaylist(db, "SELECT count(*) FROM playlist_content WHERE content_id = 2 AND playlist_id = 99");
        survivorInPlaylist.step();
        assert(survivorInPlaylist.columnInt64(0) == 1);

        SqlCipherStatement doomedPlaylistGone(db, "SELECT count(*) FROM playlist_content WHERE content_id = 1");
        doomedPlaylistGone.step();
        assert(doomedPlaylistGone.columnInt64(0) == 0);

        std::cout << "case 11 (removeTrackByPathReplacingWith dedups a playlist the survivor is already in) OK\n";
    }

    // Case 12: propagateMissingFieldsForPath copies the donor's raw
    // bpmx100/key_id/image_id values directly onto the target row, read
    // back through an independent connection.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string donorPath = (scratch / "Contents" / "Test Track.mp3").string();
        std::string targetPath = (scratch / "Contents" / "Target Track.mp3").string();

        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            // Donor (content_id 1, from createFixture) gets real values;
            // target (content_id 2) starts with none.
            db.exec("UPDATE content SET bpmx100 = 12800, key_id = 7, image_id = 42 WHERE content_id = 1;");
            db.exec(
                "INSERT INTO content (content_id, title, path) VALUES (2, 'Target', "
                "'/Contents/Target Track.mp3');");
        }

        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            writer.propagateMissingFieldsForPath(donorPath, targetPath, /*copyBpm=*/true, /*copyKey=*/true,
                                                  /*copyArtwork=*/true);
        }

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");

        SqlCipherStatement target(db, "SELECT bpmx100, key_id, image_id FROM content WHERE content_id = 2");
        assert(target.step());
        assert(target.columnInt64(0) == 12800);
        assert(target.columnInt64(1) == 7);
        assert(target.columnInt64(2) == 42);

        // Donor row itself is untouched.
        SqlCipherStatement donor(db, "SELECT bpmx100, key_id, image_id FROM content WHERE content_id = 1");
        assert(donor.step());
        assert(donor.columnInt64(0) == 12800);
        assert(donor.columnInt64(1) == 7);
        assert(donor.columnInt64(2) == 42);

        std::cout << "case 12 (propagateMissingFieldsForPath copies bpm/key/artwork from donor onto target) OK\n";
    }

    // Case 13: only one field flagged -- the others stay untouched
    // (NULL) on the target.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string donorPath = (scratch / "Contents" / "Test Track.mp3").string();
        std::string targetPath = (scratch / "Contents" / "Target Track.mp3").string();

        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("UPDATE content SET bpmx100 = 12800, key_id = 7, image_id = 42 WHERE content_id = 1;");
            db.exec(
                "INSERT INTO content (content_id, title, path) VALUES (2, 'Target', "
                "'/Contents/Target Track.mp3');");
        }

        {
            OneLibraryCueWriter writer(pioneerRoot.string());
            writer.propagateMissingFieldsForPath(donorPath, targetPath, /*copyBpm=*/false, /*copyKey=*/true,
                                                  /*copyArtwork=*/false);
        }

        std::string key = deriveOneLibraryKey();
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot.string()), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + key + "';");

        SqlCipherStatement target(db, "SELECT bpmx100, key_id, image_id FROM content WHERE content_id = 2");
        assert(target.step());
        assert(target.columnIsNull(0));
        assert(target.columnInt64(1) == 7);
        assert(target.columnIsNull(2));

        std::cout << "case 13 (propagateMissingFieldsForPath: only the flagged field is copied) OK\n";
    }

    // Case 14: unknown donor or target path throws, never touches the db.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string donorPath = (scratch / "Contents" / "Test Track.mp3").string();
        std::string missingPath = (scratch / "Contents" / "Nonexistent.mp3").string();

        OneLibraryCueWriter writer(pioneerRoot.string());
        bool threw = false;
        try {
            writer.propagateMissingFieldsForPath(missingPath, donorPath, true, true, true);
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            writer.propagateMissingFieldsForPath(donorPath, missingPath, true, true, true);
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        std::cout << "case 14 (propagateMissingFieldsForPath: unknown path throws) OK\n";
    }


    // Case 15: the staleness guard against a modification neither half of
    // its cheap size/mtime pre-filter can see -- the shape that defeated
    // both of the guard's earlier implementations.
    //
    // Cases 2 and 8 above already tamper externally, but they don't pin
    // down WHICH part of the guard did the catching: their INSERT bumps
    // the file's mtime, so a metadata-only guard passes them too. Verified
    // rather than assumed -- forcing checkNotStale()'s checksum comparison
    // to false and re-running left both of them still passing.
    //
    // So this case removes every signal except the file's actual bytes:
    //
    //   - The external write is a real SQLCipher UPDATE to a same-length
    //     value, so the page is rewritten in place and file_size() cannot
    //     move. It also leaves a VALID database behind, which matters --
    //     an earlier version of this case flipped a raw byte instead, and
    //     passed for entirely the wrong reason: the corrupted ciphertext
    //     made the writer throw on decryption, not on staleness. A test
    //     that throws for the wrong reason is worse than no test.
    //   - The mtime is then restored to its pre-tamper value, so the
    //     timestamp comparison is blind by construction rather than by
    //     luck. (On Windows it is often blind anyway: an in-process
    //     path-based fs::last_write_time() does not observe a write that
    //     just happened, since the directory entry only refreshes on
    //     handle close/reopen. Restoring it explicitly makes this case
    //     deterministic on every platform instead of relying on that.)
    //
    // Only reading the file's content can catch what's left, which is why
    // the assertion below checks the exception's MESSAGE rather than just
    // that something was thrown. An earlier guard here used
    // `PRAGMA data_version`, blind for its own reason: a per-connection
    // counter that only moves when that same connection observes another
    // connection's commit, so querying it on a freshly-opened connection
    // returns the same value forever.
    //
    // tests/pdb_row_writer_staleness_test.cpp covers the equivalent case
    // on the rekordbox side.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(pioneerRoot.string());
        std::string dbPath = OneLibraryCueWriter::dbPathFor(pioneerRoot.string());

        OneLibraryCueWriter writer(pioneerRoot.string());

        const auto originalMtime = fs::last_write_time(dbPath);
        const auto originalSize = fs::file_size(dbPath);
        const std::string originalBytes = readWholeFile(dbPath);

        // "Test Track" -> "Xest Track": same length, so the row and its
        // page keep their size and the file's length cannot change.
        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, dbPath, /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            db.exec("UPDATE content SET title = 'Xest Track' WHERE content_id = 1;");
        }
        fs::last_write_time(dbPath, originalMtime);

        const std::string tamperedBytes = readWholeFile(dbPath);
        assert(fs::file_size(dbPath) == originalSize);            // size blind
        assert(fs::last_write_time(dbPath) == originalMtime);     // mtime blind
        assert(tamperedBytes != originalBytes);                   // content genuinely changed

        std::string message;
        try {
            writer.writeCuesForPath((scratch / "Contents" / "Test Track.mp3").string(), sampleCues());
        } catch (const std::exception &e) {
            message = e.what();
        }
        // Checking the message, not merely that something threw: the
        // database is still perfectly readable here, so the only correct
        // reason to refuse is staleness.
        assert(message.find("changed since this writer was opened") != std::string::npos);

        // And the refusal has to be real, not merely reported -- the
        // external change must still be on disk, not overwritten by
        // anything the writer decided to flush anyway.
        assert(readWholeFile(dbPath) == tamperedBytes);

        std::cout << "case 15 (staleness guard refuses an external same-length write with size and mtime unchanged) OK\n";
    }

    // A loop keeps its out point through a write (the reader's side is
    // covered in onelibrary_reader_test), and a cue that keeps its slot
    // and position keeps the colour index the stick had for it, even
    // though the domain model does not carry one.
    {
        fs::path scratch = freshScratch();
        std::string pioneerRoot = (scratch / "PIONEER").string();
        createFixture(pioneerRoot);
        std::string filePath = (scratch / "Contents" / "Test Track.mp3").string();
        CuePoint loop{CuePoint::Kind::Hot, 2, 1000.0, "#FF0000", "loop"};
        loop.isLoop = true;
        loop.loopEndMs = 3000.0;
        CuePoint point{CuePoint::Kind::Memory, 0, 7000.0, "", ""};
        {
            OneLibraryCueWriter writer(pioneerRoot);
            writer.writeCuesForPath(filePath, {loop, point});
        }
        {
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/false);
            db.exec("PRAGMA key = '" + key + "';");
            SqlCipherStatement check(db, "SELECT kind, isActiveLoop, inUsec, outUsec FROM cue ORDER BY kind");
            assert(check.step());
            assert(check.columnInt64(0) == 0 && check.columnInt64(1) == 0 && check.columnInt64(2) == check.columnInt64(3));
            assert(check.step());
            assert(check.columnInt64(0) == 2 && check.columnInt64(1) == 1 && check.columnInt64(2) == 1000000
                   && check.columnInt64(3) == 3000000);
            // The stick's own colour on the loop, as rekordbox would have set it.
            db.exec("UPDATE cue SET colorTableIndex = 5 WHERE kind = 2;");
        }
        {
            // The same cues written again (an unrelated save touching
            // this track): the loop's colour index survives.
            OneLibraryCueWriter writer(pioneerRoot);
            writer.writeCuesForPath(filePath, {loop, point});
            std::string key = deriveOneLibraryKey();
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/true);
            db.exec("PRAGMA key = '" + key + "';");
            SqlCipherStatement check(db, "SELECT colorTableIndex FROM cue WHERE kind = 2");
            assert(check.step() && check.columnInt64(0) == 5);
        }
        std::cout << "case: loops round-trip and a kept cue keeps its colour index OK\n";
    }

    std::cout << "All onelibrary_cue_writer_test cases passed.\n";
    return 0;
}
