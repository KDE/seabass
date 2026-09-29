// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Cover art a player can actually find.
//
// Engine stores art as files named by a hash. Its own "import rekordbox
// library" instead writes "image://fileart//<absolute path>" naming the
// importing computer's copy of the rekordbox JPEG -- a path no player has,
// so the art silently never appears. Found on a real stick: 1174 of 1271
// tracks in that state, while the 95 with cached art showed up fine.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <sqlite3.h>

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::engine;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;

namespace
{

// The first three bytes are what says "JPEG" to anything that looks.
std::string jpeg(const std::string &payload)
{
    return std::string("\xFF\xD8\xFF", 3) + payload;
}

std::string png(const std::string &payload)
{
    return std::string("\x89PNG\r\n\x1a\n", 8) + payload;
}

void write(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

void exec(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::cerr << "sql failed: " << (error ? error : "?") << " for " << sql << "\n";
        assert(false);
    }
}

// A stick holding an Engine library, with rekordbox artwork beside it.
struct Fixture
{
    fs::path stick;
    fs::path library;
    explicit Fixture(const fs::path &root) : stick(root), library(root / "Engine Library")
    {
        fs::remove_all(stick);
        fs::create_directories(library / "Database2");
        fs::create_directories(library / "Artwork");
        // A real JPEG start-of-image, because the repair decides the name
        // it writes from the bytes rather than from the source's extension.
        write(stick / "PIONEER" / "Artwork" / "00001" / "a5_m.jpg", jpeg("FOR-TRACK-3"));
        sqlite3 *db = nullptr;
        assert(sqlite3_open(pathToUtf8(library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        exec(db, "CREATE TABLE AlbumArt (id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT, albumArt BLOB);");
        exec(db, "CREATE TABLE Track (id INTEGER PRIMARY KEY, title TEXT, artist TEXT, albumArtId INTEGER);");
        sqlite3_close(db);
    }
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(stick, ec);
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;

    sqlite3 *open()
    {
        sqlite3 *db = nullptr;
        assert(sqlite3_open(pathToUtf8(library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        return db;
    }
};

std::string hashOf(sqlite3 *db, std::int64_t trackId)
{
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT a.hash FROM Track t JOIN AlbumArt a ON a.id=t.albumArtId WHERE t.id=?;", -1, &stmt,
                       nullptr);
    sqlite3_bind_int64(stmt, 1, trackId);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.assign(static_cast<const char *>(sqlite3_column_blob(stmt, 0)),
                   static_cast<size_t>(sqlite3_column_bytes(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return out;
}

}  // namespace

// Run with a real "Engine Library" path to report on it instead of
// testing: the same audit the page runs, read-only, for checking this
// against a stick by hand.
int main(int argc, char **argv)
{
    if (argc > 1) {
        const ArtworkAudit audit = auditArtwork(argv[1]);
        if (!audit.error.empty()) {
            std::cerr << audit.error << "\n";
            return 1;
        }
        std::cout << "tracks with art:       " << audit.tracksWithArt << "\n"
                  << "a player can show:     " << audit.readableByAPlayer << "\n"
                  << "a player cannot show:  " << audit.unreadable.size() << "\n"
                  << "of those, repairable:  " << audit.repairable() << "\n";
        for (size_t i = 0; i < audit.unreadable.size() && i < 3; ++i) {
            const ArtworkEntry &entry = audit.unreadable[i];
            std::cout << "  e.g. id=" << entry.trackId << " " << entry.title << " by " << entry.artist << "\n"
                      << "       " << entry.reference << "\n"
                      << "       image here: " << (entry.imageOnStick.empty() ? "none" : entry.imageOnStick) << "\n";
        }
        return 0;
    }
    // 1. How Engine spells a hash as a file name: base64url, unpadded, and
    //    every tail length. Checked against the encoder every other
    //    implementation agrees on.
    {
        std::vector<std::uint8_t> counting(20);
        for (std::uint8_t i = 0; i < 20; ++i) {
            counting[i] = i;
        }
        assert(artworkFileName(counting) == "AAECAwQFBgcICQoLDA0ODxAREhM");
        assert(artworkFileName(std::vector<std::uint8_t>(20, 0xFF)) == "__________________________8");
        assert(artworkFileName(std::vector<std::uint8_t>{0xFB, 0xEF, 0xBE}) == "----");
        assert(artworkFileName(std::vector<std::uint8_t>{0xFB}) == "-w");
        assert(artworkFileName(std::vector<std::uint8_t>{0xFB, 0xEF}) == "--8");
        std::cout << "case 1 (a hash becomes the file name Engine uses) OK\n";
    }

    // 2. What each kind of reference is, and where an imported one's image
    //    is on *this* stick -- the volume label in the path is the machine
    //    that did the import, so only the tail is worth anything.
    {
        assert(classifyArtworkReference("", ReferenceType::Blob) == ArtworkStorage::None);
        assert(classifyArtworkReference("", ReferenceType::Text) == ArtworkStorage::None);
        for (const ReferenceType type : {ReferenceType::Blob, ReferenceType::Text}) {
            assert(classifyArtworkReference("image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg", type)
                   == ArtworkStorage::ImportedPath);
        }
        assert(classifyArtworkReference(std::string("\x01\x02\x03", 3), ReferenceType::Blob) == ArtworkStorage::Cached);
        // Hex text is the hash of an image kept in the row, never a file
        // name to spell out.
        assert(classifyArtworkReference("af2f6f87c56583adb67003735089017e2eb03572", ReferenceType::Text)
               == ArtworkStorage::InDatabase);
        const std::string here =
            imageOnStickFor("image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg", "/media/OTHER");
        assert(here == pathToUtf8((fs::path("/media/OTHER") / "PIONEER/Artwork/00001/a5_m.jpg").make_preferred()));
        assert(imageOnStickFor("image://fileart//somewhere/else.jpg", "/media/OTHER").empty());
        // The reference is the path the IMPORTING computer wrote, and that
        // machine is often a Windows one: "...\PIONEER\Artwork\...".
        // Looking only for the forward-slash spelling found nothing on a
        // stick imported there -- every track unrepairable, and the page
        // saying none of the images were on a stick that had all of them.
        // A backslash inside a forward-slash reference is part of the file
        // name (legal on Linux and on exFAT mounted there), not a
        // separator: rewriting it would look up a file that does not exist
        // and call the track unrepairable.
        const std::string literalBackslash =
            imageOnStickFor("image://fileart//media/W/PIONEER/Artwork/00001/a\\b.jpg", "/media/OTHER");
        assert(literalBackslash == pathToUtf8((fs::path("/media/OTHER") / "PIONEER/Artwork/00001/a\\b.jpg").make_preferred()));
        const std::string fromWindows =
            imageOnStickFor("image://fileart//E:\\PIONEER\\Artwork\\00001\\a5_m.jpg", "/media/OTHER");
        assert(fromWindows == pathToUtf8((fs::path("/media/OTHER") / "PIONEER/Artwork/00001/a5_m.jpg").make_preferred()));
        std::cout << "case 2 (an imported path is recognised, either spelling, and re-anchored here) OK\n";
    }

    // 3. The audit: one track a player can read, one whose cached file is
    //    gone, one imported path whose image is here, one whose is not.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_audit");
        sqlite3 *db = fixture.open();
        std::vector<std::uint8_t> cached(20, 0x11);
        const std::string cachedName = artworkFileName(cached);
        write(fixture.library / "Artwork" / (cachedName + ".jpg"), jpeg("CACHED-IMAGE"));
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (1, x'"
                 "1111111111111111111111111111111111111111');");
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (2, x'"
                 "2222222222222222222222222222222222222222');");  // no file for this one
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(3, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg');");
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(4, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00009/gone.jpg');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Readable', 'A', 1);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'File gone', 'B', 2);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (3, 'Imported', 'C', 3);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (4, 'Imported, no image', 'D', 4);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (5, 'No art at all', 'E', NULL);");
        sqlite3_close(db);

        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        assert(audit.error.empty());
        assert(audit.tracksWithArt == 4);  // the fifth asked for none
        assert(audit.readableByAPlayer == 1);
        assert(audit.unreadable.size() == 3);
        assert(audit.repairable() == 1);  // only the imported one whose image is here
        // Repairable first, so a page can offer them without sorting.
        assert(audit.unreadable[0].trackId == 3);
        assert(audit.unreadable[0].storage == ArtworkStorage::ImportedPath);
        assert(!audit.unreadable[0].imageOnStick.empty());
        std::cout << "case 3 (the audit counts what a player can read, and what can be repaired) OK\n";

        // 4. The repair gives that track Engine's own storage, and the
        //    audit then counts it as readable. The one with no image on
        //    this stick is left exactly as it was.
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable);
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        assert(repair.filesWritten.size() == 1);
        assert(fs::is_regular_file(repair.filesWritten[0]));

        const ArtworkAudit after = auditArtwork(pathToUtf8(fixture.library));
        assert(after.readableByAPlayer == 2);
        assert(after.repairable() == 0);
        assert(after.unreadable.size() == 2);  // the cached-file-gone one, and the one with no image

        db = fixture.open();
        const std::string newHash = hashOf(db, 3);
        assert(newHash.rfind("image://", 0) != 0);  // no longer a path
        assert(newHash.size() == 20);
        assert(hashOf(db, 4).rfind("image://", 0) == 0);  // untouched
        sqlite3_close(db);
        std::cout << "case 4 (repairing writes Engine's own storage, and leaves the rest alone) OK\n";
    }

    // 5. The name a repair writes carries the format, and the audit reads
    //    it back byte for byte -- so the format comes from the bytes, not
    //    from what the source file happened to be called. A source named
    //    .JPG used to be written as <hash>.JPG, which the audit (looking
    //    for ".jpg", ".jpeg", ".png") could never find again: the track
    //    stayed unreadable AND became unrepairable, notice up, button off.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_extension");
        write(fixture.stick / "PIONEER" / "Artwork" / "00001" / "SHOUTING.JPG", jpeg("UPPER"));
        write(fixture.stick / "PIONEER" / "Artwork" / "00001" / "quiet.png", png("PORTABLE"));
        write(fixture.stick / "PIONEER" / "Artwork" / "00001" / "notes.txt", "NOT AN IMAGE AT ALL");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(1, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/SHOUTING.JPG');");
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(2, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/quiet.png');");
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(3, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/notes.txt');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Shouty', 'A', 1);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'Portable', 'B', 2);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (3, 'Text file', 'C', 3);");
        sqlite3_close(db);

        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        // The text file is on the stick, so it used to count as repairable
        // and the page offered it. The audit reads the first bytes now, so
        // the count it shows and the button it offers are what a repair
        // will actually do -- and one odd file among a thousand good ones
        // cannot stop the save.
        assert(audit.repairable() == 2);
        assert(audit.unreadable.size() == 3);
        assert(audit.unreadable[2].trackId == 3);  // the text file, listed but not offered
        assert(audit.unreadable[2].imageOnStick.empty());

        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable);
        assert(repair.error.empty());
        assert(repair.repaired == 2);   // the JPEG and the PNG
        assert(repair.notAnImage == 0); // the text file was never offered
        assert(repair.filesWritten.size() == 2);
        for (const std::string &written : repair.filesWritten) {
            const fs::path ext = pathFromUtf8(written).extension();
            assert(ext == ".jpg" || ext == ".png");
        }
        const ArtworkAudit after = auditArtwork(pathToUtf8(fixture.library));
        assert(after.readableByAPlayer == 2);
        assert(after.unreadable.size() == 1);  // only the text file is left
        assert(after.unreadable[0].trackId == 3);

        // And handed one anyway -- the file changed under the page between
        // the scan and the save -- the repair refuses it by name rather
        // than writing a <hash>.txt no player will open.
        ArtworkEntry forced = after.unreadable[0];
        forced.imageOnStick = pathToUtf8(fixture.stick / "PIONEER" / "Artwork" / "00001" / "notes.txt");
        const ArtworkRepair refused = repairArtwork(pathToUtf8(fixture.library), {forced});
        assert(refused.error.empty());
        assert(refused.repaired == 0);
        assert(refused.notAnImage == 1);
        assert(refused.filesWritten.empty());
        std::cout << "case 5 (the written name says what the bytes are, and a non-image is never offered) OK\n";
    }

    // 6. A track that goes between the audit and the save. The image is
    //    copied and the AlbumArt row written, but the UPDATE matches
    //    nothing -- which used to leave `repaired` at 0 and fail the whole
    //    save with "none of the images could be read", after all the work
    //    had in fact been done.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_vanished");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(1, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (7, 'Here for now', 'A', 1);");
        sqlite3_close(db);

        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        assert(audit.repairable() == 1);
        db = fixture.open();
        exec(db, "DELETE FROM Track WHERE id = 7;");
        sqlite3_close(db);

        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable);
        assert(repair.error.empty());
        assert(repair.repaired == 0);
        assert(repair.tracksNoLongerThere == 1);
        assert(repair.filesWritten.size() == 1);
        std::cout << "case 6 (a track that went between the audit and the save is counted apart) OK\n";
    }

    // 7. beforeWrite throws (SaveContext::protectForThisChange does, when
    //    it cannot copy a file aside). The repair fails with that reason,
    //    the transaction is rolled back, and nothing is left pointing at
    //    art that was never written.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_protect_throws");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(1, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Protected', 'A', 1);");
        sqlite3_close(db);

        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        assert(audit.repairable() == 1);
        const ArtworkRepair repair =
            repairArtwork(pathToUtf8(fixture.library), audit.unreadable, [](const std::string &) {
                throw std::runtime_error("not enough temporary space to protect it");
            });
        assert(!repair.error.empty());
        assert(repair.error.find("not enough temporary space") != std::string::npos);
        assert(repair.repaired == 0);
        db = fixture.open();
        assert(hashOf(db, 1).rfind("image://", 0) == 0);  // still the imported path
        sqlite3_close(db);
        std::cout << "case 7 (a throw from beforeWrite fails the repair and rolls it back) OK\n";
    }

    // 8. The database is a parameter: a save may have redirected writes to
    //    m.db into a scratch copy it commits at the end, and a repair that
    //    wrote the live file behind that redirect had every row it wrote
    //    overwritten when the scratch landed. The images are not
    //    redirected -- they go under the library's own Artwork/.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_redirect");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES "
                 "(1, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Redirected', 'A', 1);");
        sqlite3_close(db);

        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        const fs::path scratch = fixture.stick / "scratch-m.db";
        fs::copy_file(fixture.library / "Database2" / "m.db", scratch);
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable, {}, pathToUtf8(scratch));
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        assert(repair.filesWritten.size() == 1);
        // The image went into the library, as always.
        assert(pathFromUtf8(repair.filesWritten[0]).parent_path() == fixture.library / "Artwork");

        sqlite3 *scratchDb = nullptr;
        assert(sqlite3_open(pathToUtf8(scratch).c_str(), &scratchDb) == SQLITE_OK);
        assert(hashOf(scratchDb, 1).rfind("image://", 0) != 0);  // the scratch got the row
        sqlite3_close(scratchDb);
        db = fixture.open();
        assert(hashOf(db, 1).rfind("image://", 0) == 0);  // the live file was left alone
        sqlite3_close(db);
        std::cout << "case 8 (the repair writes the database it is given, not the library's) OK\n";
    }

    // 9. Art asked for with nothing to find it by: the track points at an
    //    AlbumArt row whose hash is NULL. That is not "no art" -- it is art
    //    a player cannot show, and counting it as no art left it out of
    //    every figure the page shows. A track with no albumArtId at all
    //    still counts as nothing to report.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_null_hash");
        sqlite3 *db = fixture.open();
        // Engine's own "this track has no cover": the row libdjinterop
        // seeds at id 1 in every schema it writes, with an empty hash.
        // Most tracks on most sticks point at it, so reporting it would
        // put a permanent, unfixable warning on every healthy library.
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (1, '');");
        // A row of its own with nothing in it: art asked for, nothing to
        // find it by. Two of these are in the committed fixture.
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (5, NULL);");
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (6, x'');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'No cover', 'A', 1);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'No cover either', 'B', 0);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (3, 'No art at all', 'C', NULL);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (4, 'Null row', 'D', 5);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (5, 'Empty row', 'E', 6);");
        sqlite3_close(db);

        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        assert(audit.error.empty());
        assert(audit.tracksWithArt == 2);  // the three with no cover asked for none
        assert(audit.readableByAPlayer == 0);
        assert(audit.unreadable.size() == 2);
        assert(audit.unreadable[0].trackId == 4);
        assert(audit.unreadable[1].trackId == 5);
        assert(audit.unreadable[0].storage == ArtworkStorage::RowWithoutHash);
        assert(audit.unreadable[1].storage == ArtworkStorage::RowWithoutHash);
        assert(audit.repairable() == 0);  // there is no image and no name to write
        std::cout << "case 9 (Engine's no-cover row is no art; a row of its own with nothing in it is a fault) OK\n";
    }

    // 10. The fault this was written for: a stick pulled mid-write leaves
    //     the artwork file's name and loses its data. The row is right,
    //     the file is there, and it holds nothing -- which a player draws
    //     as no cover at all. It has to be found (an existing file is not
    //     the question; an image is), and it has to be fixable from the
    //     rekordbox art on the same stick, which is the copy the user
    //     still has with them when the Engine one is gone.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_emptied");
        fs::create_directories(fixture.stick / "Contents");
        write(fixture.stick / "Contents" / "song.mp3", "AUDIO");
        write(fixture.stick / "PIONEER" / "Artwork" / "00002" / "cover.jpg", jpeg("REKORDBOX-COVER"));
        sqlite3 *db = nullptr;
        assert(sqlite3_open(pathToUtf8(fixture.library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        exec(db, "DROP TABLE Track;");
        exec(db, "CREATE TABLE Track (id INTEGER PRIMARY KEY, title TEXT, artist TEXT, albumArtId INTEGER, path TEXT);");
        std::vector<std::uint8_t> lost(20, 0x44);
        const std::string lostName = artworkFileName(lost);
        write(fixture.library / "Artwork" / (lostName + ".jpg"), "");  // the name survived, the data did not
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (1, x'"
                 "4444444444444444444444444444444444444444');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId, path) VALUES "
                 "(1, 'Emptied', 'A', 1, '../Contents/song.mp3');");
        sqlite3_close(db);

        // Without the rekordbox side: found, named as its own kind of
        // fault, and not repairable -- never counted as readable.
        const ArtworkAudit alone = auditArtwork(pathToUtf8(fixture.library));
        assert(alone.error.empty());
        assert(alone.readableByAPlayer == 0);
        assert(alone.unreadable.size() == 1);
        assert(alone.unreadable[0].storage == ArtworkStorage::CachedFileUnreadable);
        assert(alone.repairable() == 0);

        // With it: the same fault, now repairable from the art beside it.
        ArtworkSourceByTrackFile sources;
        sources.emplace(artworkSourceKey(pathToUtf8(fixture.stick / "Contents" / "song.mp3")),
                        pathToUtf8(fixture.stick / "PIONEER" / "Artwork" / "00002" / "cover.jpg"));
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), sources);
        assert(audit.repairable() == 1);
        assert(!audit.unreadable[0].imageOnStick.empty());

        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable);
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        // The empty file is not what the row points at any more, and what
        // it does point at has bytes in it.
        const ArtworkAudit after = auditArtwork(pathToUtf8(fixture.library), sources);
        assert(after.readableByAPlayer == 1);
        assert(after.unreadable.empty());
        std::cout << "case 10 (an emptied artwork file is a fault, and the rekordbox art beside it fixes it) OK\n";
    }

    // 11. Neither library has a copy any more, but the track does: the
    //     art both of them originally took from the file's own tags is
    //     still in the file. A row with no hash at all is fixed the same
    //     way -- it names a track, and a track has a file.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_embedded");
        fs::create_directories(fixture.stick / "Contents");
        write(fixture.stick / "Contents" / "tagged.mp3", "AUDIO-WITH-A-PICTURE-INSIDE");
        write(fixture.stick / "Contents" / "bare.mp3", "AUDIO-WITH-NOTHING-INSIDE");
        sqlite3 *db = nullptr;
        assert(sqlite3_open(pathToUtf8(fixture.library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        exec(db, "DROP TABLE Track;");
        exec(db, "CREATE TABLE Track (id INTEGER PRIMARY KEY, title TEXT, artist TEXT, albumArtId INTEGER, path TEXT);");
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (7, NULL);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId, path) VALUES "
                 "(1, 'Has a picture', 'A', 7, '../Contents/tagged.mp3');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId, path) VALUES "
                 "(2, 'Has none anywhere', 'B', 7, '../Contents/bare.mp3');");
        sqlite3_close(db);

        const std::string tagged = pathToUtf8(fixture.stick / "Contents" / "tagged.mp3");
        const auto probe = [&tagged](const ArtworkEntry &entry) { return entry.trackFile == tagged; };
        const auto reader = [&tagged](const ArtworkEntry &entry) {
            return entry.trackFile == tagged ? jpeg("EMBEDDED-COVER") : std::string();
        };

        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(audit.error.empty());
        assert(audit.unreadable.size() == 2);
        assert(audit.unreadable[0].storage == ArtworkStorage::RowWithoutHash);
        // Only the one whose file really carries a picture is promised.
        assert(audit.repairable() == 1);
        assert(audit.unreadable[0].otherSource);
        assert(!audit.unreadable[1].otherSource);

        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable, {}, {}, reader);
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        assert(repair.filesWritten.size() == 1);

        const ArtworkAudit after = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(after.readableByAPlayer == 1);
        assert(after.unreadable.size() == 1);  // the one with no art anywhere stays a fault
        assert(after.repairable() == 0);
        std::cout << "case 11 (a track's own tags are the last source, and a hash-less row is fixed from them too) OK\n";
    }

    // 12. A stop lands at the next row, not after the table: the page the
    //     audit runs for waits for it before it may leave. The probe stops
    //     the audit on the first faulty row it is asked about; the other
    //     two are never probed, and the audit throws rather than hand back
    //     a partial count as if it were the whole library.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_cancel");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (7, NULL);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'One', 'A', 7);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'Two', 'B', 7);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (3, 'Three', 'C', 7);");
        sqlite3_close(db);

        seabass::application::CancellationToken cancel;
        int probed = 0;
        const auto probe = [&cancel, &probed](const ArtworkEntry &) {
            ++probed;
            cancel.cancel();
            return false;
        };
        bool stopped = false;
        try {
            (void)auditArtwork(pathToUtf8(fixture.library), {}, probe, cancel);
        } catch (const seabass::application::OperationCancelled &) {
            stopped = true;
        }
        assert(stopped && "a cancelled audit says so, it does not return a partial count");
        assert(probed == 1 && "no row is read after the stop");

        // And the database is closed on the way out: a write can have it.
        db = fixture.open();
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (4, 'Four', 'D', 7);");
        sqlite3_close(db);
        std::cout << "case 12 (a stop lands at the next row, and the database is let go) OK\n";
    }

    // 13. An older library keeps each cover inside its row, in
    //     AlbumArt.albumArt, beside a hash written as hex text (39 or 40
    //     characters: leading zeros dropped). A player reads those, so the
    //     audit counts them readable, whatever the hash says and even at
    //     id 1. A row whose image is empty is a fault of its own, repaired
    //     in place (case 14) and never by re-pointing the track, which
    //     would move it off the storage its library reads; a row holding
    //     bytes that are no picture is left alone (case 23). And the empty
    //     "no cover" row is told by its content, not by sitting at id 1.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_in_database");
        fs::create_directories(fixture.stick / "Contents");
        sqlite3 *db = fixture.open();
        exec(db, "DROP TABLE Track;");
        exec(db, "CREATE TABLE Track (id INTEGER PRIMARY KEY, title TEXT, artist TEXT, albumArtId INTEGER, path TEXT);");
        const std::string pngImage = png("A-PNG-KEPT-IN-THE-ROW");
        const std::string jpegImage = jpeg("A-JPEG-KEPT-IN-THE-ROW");
        const auto insertRow = [db](int id, const char *hash, const std::string *image) {
            sqlite3_stmt *insert = nullptr;
            assert(sqlite3_prepare_v2(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (?, ?, ?);", -1, &insert,
                                      nullptr)
                   == SQLITE_OK);
            sqlite3_bind_int(insert, 1, id);
            sqlite3_bind_text(insert, 2, hash, -1, SQLITE_TRANSIENT);
            if (image != nullptr) {
                sqlite3_bind_blob(insert, 3, image->data(), static_cast<int>(image->size()), SQLITE_TRANSIENT);
            } else {
                sqlite3_bind_null(insert, 3);
            }
            assert(sqlite3_step(insert) == SQLITE_DONE);
            sqlite3_finalize(insert);
        };
        const std::string emptyImage;
        const std::string notAPicture = "NOT A PICTURE";
        insertRow(1, "af2f6f87c56583adb67003735089017e2eb03572", &pngImage);
        insertRow(2, "551c96558e2eb05ea31f3735b129f242b720c15", &jpegImage);  // 39 characters
        insertRow(3, "934a576ac3a4a0ab6d66478652b9bb8b7ac68b82", &emptyImage);
        insertRow(4, "8998055a7787a03a8e8de2fa607a11f37b4c674a", &notAPicture);
        insertRow(5, "", nullptr);  // Engine's "no cover" row, not at id 1
        for (int track = 1; track <= 6; ++track) {
            const int row = track <= 2 ? track : track - 1;  // tracks 2 and 3 share row 2
            exec(db, "INSERT INTO Track (id, title, artist, albumArtId, path) VALUES (" + std::to_string(track)
                         + ", 'T', 'A', " + std::to_string(row) + ", '../Contents/t" + std::to_string(track)
                         + ".mp3');");
        }
        sqlite3_close(db);

        // Every track's tags carry a cover: the case in which a repair
        // used to be offered for all of them.
        const auto probe = [](const ArtworkEntry &) { return true; };
        const auto reader = [](const ArtworkEntry &) { return jpeg("FROM-THE-TAGS"); };
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(audit.error.empty());
        assert(audit.tracksWithArt == 5);  // track 6 asked for none
        assert(audit.readableByAPlayer == 3);
        // Row 3 is positively empty: a fault, repaired in place. Row 4 holds
        // bytes that are no picture: left alone, and no fault.
        assert(audit.unreadable.size() == 1);
        assert(audit.unreadable[0].trackId == 4);
        assert(audit.unreadable[0].storage == ArtworkStorage::InDatabaseUnreadable);
        assert(audit.unreadable[0].otherSource);
        assert(audit.leftAlone.size() == 1 && audit.leftAlone[0].trackId == 5);
        assert(audit.repairable() == 1);

        // A repair handed these tracks anyway, as the old audit did, leaves
        // every one on its row: nothing written, nothing re-pointed.
        std::vector<ArtworkEntry> forced;
        for (std::int64_t track = 1; track <= 5; ++track) {
            ArtworkEntry entry;
            entry.trackId = track;
            entry.storage = ArtworkStorage::CachedFileMissing;
            entry.otherSource = true;
            forced.push_back(entry);
        }
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), forced, {}, {}, reader);
        assert(repair.error.empty());
        assert(repair.repaired == 0);
        assert(repair.keptInDatabase == 5);
        assert(repair.filesWritten.empty());
        db = fixture.open();
        sqlite3_stmt *rows = nullptr;
        assert(sqlite3_prepare_v2(db, "SELECT t.id, t.albumArtId, a.albumArt FROM Track t JOIN AlbumArt a "
                                      "ON a.id = t.albumArtId ORDER BY t.id;",
                                  -1, &rows, nullptr)
               == SQLITE_OK);
        const std::vector<std::string> expected = {pngImage, jpegImage, jpegImage, emptyImage, notAPicture, ""};
        int seen = 0;
        while (sqlite3_step(rows) == SQLITE_ROW) {
            const int track = sqlite3_column_int(rows, 0);
            assert(sqlite3_column_int(rows, 1) == (track <= 2 ? track : track - 1));
            const void *blob = sqlite3_column_blob(rows, 2);
            const std::string image =
                blob ? std::string(static_cast<const char *>(blob), static_cast<size_t>(sqlite3_column_bytes(rows, 2)))
                     : std::string();
            assert(image == expected[static_cast<size_t>(track - 1)]);
            ++seen;
        }
        sqlite3_finalize(rows);
        sqlite3_stmt *count = nullptr;
        assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM AlbumArt;", -1, &count, nullptr) == SQLITE_OK);
        assert(sqlite3_step(count) == SQLITE_ROW);
        assert(sqlite3_column_int(count, 0) == 5);  // no row added
        sqlite3_finalize(count);
        sqlite3_close(db);
        assert(seen == 6);

        const ArtworkAudit after = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(after.readableByAPlayer == 3);
        std::cout << "case 13 (covers kept in the database are readable, and never re-pointed) OK\n";
    }

    // 14. A row that keeps its image in the database and holds none a
    //     player can draw is repaired in place: the rescued image goes
    //     into that row's albumArt, and its hash and id stay, so every
    //     track pointing at it keeps pointing at it. Only JPEG or PNG is
    //     written, and an image that reads already is never overwritten.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_in_place");
        fs::create_directories(fixture.stick / "Contents");
        sqlite3 *db = fixture.open();
        exec(db, "DROP TABLE Track;");
        exec(db, "CREATE TABLE Track (id INTEGER PRIMARY KEY, title TEXT, artist TEXT, albumArtId INTEGER, path TEXT);");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (1, 'af2f6f87c56583adb67003735089017e2eb03572', x'"
                 "89504E470D0A1A0A00');");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (3, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', x'');");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (4, '8998055a7787a03a8e8de2fa607a11f37b4c674a', "
                 "x'4E4F5420412050494354555245');");
        for (const auto &[track, row] : {std::pair{1, 1}, std::pair{4, 3}, std::pair{5, 4}, std::pair{7, 3}}) {
            exec(db, "INSERT INTO Track (id, title, artist, albumArtId, path) VALUES (" + std::to_string(track)
                         + ", 'T', 'A', " + std::to_string(row) + ", '../Contents/t" + std::to_string(track) + ".mp3');");
        }
        sqlite3_close(db);
        const std::string rescued = jpeg("RESCUED-FROM-THE-TAGS");
        const auto probe = [](const ArtworkEntry &) { return true; };
        const auto reader = [&rescued](const ArtworkEntry &) { return rescued; };
        const auto imageOf = [&fixture](int row) {
            sqlite3 *handle = fixture.open();
            sqlite3_stmt *stmt = nullptr;
            sqlite3_prepare_v2(handle, "SELECT albumArt, hash FROM AlbumArt WHERE id = ?;", -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, row);
            std::string image;
            std::string hash;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                if (const void *blob = sqlite3_column_blob(stmt, 0)) {
                    image.assign(static_cast<const char *>(blob), static_cast<size_t>(sqlite3_column_bytes(stmt, 0)));
                }
                hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            }
            sqlite3_finalize(stmt);
            sqlite3_close(handle);
            return std::pair{image, hash};
        };

        const std::string junk = imageOf(4).first;
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(audit.unreadable.size() == 2);   // tracks 4 and 7, on the empty row
        assert(audit.leftAlone.size() == 1);    // track 5: its row holds bytes that are no picture
        assert(audit.repairable() == 2);
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable, {}, {}, reader);
        assert(repair.error.empty());
        // Track 4 shares row 3 with track 7, so it gets a row of its own,
        // in the database, where this library keeps its covers (row 1);
        // track 7, then alone on row 3, is repaired in place. Row 4 is not
        // touched.
        assert(repair.repaired == 2);
        assert(repair.filesWritten.empty());
        assert(imageOf(3) == (std::pair{rescued, std::string("934a576ac3a4a0ab6d66478652b9bb8b7ac68b82")}));
        assert(imageOf(4).first == junk);
        const ArtworkAudit after = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(after.readableByAPlayer == 3);
        assert(after.unreadable.empty());
        assert(after.leftAlone.size() == 1);
        db = fixture.open();
        sqlite3_stmt *count = nullptr;
        assert(sqlite3_prepare_v2(db, "SELECT albumArtId FROM Track ORDER BY id;", -1, &count, nullptr) == SQLITE_OK);
        std::vector<int> rows;
        while (sqlite3_step(count) == SQLITE_ROW) {
            rows.push_back(sqlite3_column_int(count, 0));
        }
        assert(rows.size() == 4 && rows[0] == 1 && rows[1] != 3 && rows[2] == 4 && rows[3] == 3);
        sqlite3_finalize(count);
        sqlite3_close(db);

        // Handed a track whose row reads already, the repair leaves it.
        ArtworkEntry forced;
        forced.trackId = 1;
        forced.storage = ArtworkStorage::InDatabaseUnreadable;
        forced.otherSource = true;
        const std::string before = imageOf(1).first;
        const ArtworkRepair refused = repairArtwork(pathToUtf8(fixture.library), {forced}, {}, {}, reader);
        assert(refused.repaired == 0 && refused.keptInDatabase == 1);
        assert(imageOf(1).first == before);
        // And a rescue that is not a JPEG or PNG is never written.
        exec(db = fixture.open(), "UPDATE AlbumArt SET albumArt = x'' WHERE id = 4;");
        sqlite3_close(db);
        forced.trackId = 5;
        const ArtworkRepair notAnImage = repairArtwork(pathToUtf8(fixture.library), {forced}, {}, {},
                                                       [](const ArtworkEntry &) { return std::string("GIF89a..."); });
        assert(notAnImage.repaired == 0 && notAnImage.notAnImage == 1);
        assert(imageOf(4).first.empty());
        std::cout << "case 14 (an unreadable image kept in the database is replaced in place) OK\n";
    }

    // 15. What covers take up on the stick. An older library's images are
    //     inside its database, and the copies the reader writes out to
    //     show them are on this computer: counting those copies measured
    //     the wrong disk and left the images in m.db uncounted.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_stick_bytes");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (1, 'af2f6f87c56583adb67003735089017e2eb03572', "
                 "zeroblob(3000));");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (2, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', "
                 "zeroblob(500));");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (3, '', NULL);");
        // Stored as text, with a NUL inside: four bytes, which length()
        // alone counts as two characters.
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (4, 'x', CAST(x'41420043' AS TEXT));");
        sqlite3_close(db);
        const fs::path onStick = fixture.stick / "PIONEER" / "Artwork" / "00001" / "a5_m.jpg";  // written by Fixture
        const fs::path localCopy = seabass::infrastructure::paths::localEngineArtworkDir() / "some-library" / "c.png";
        write(localCopy, std::string(70000, 'x'));
        std::vector<seabass::domain::Track> tracks(3);
        tracks[0].artworkPath = pathToUtf8(onStick);
        tracks[1].artworkPath = pathToUtf8(onStick);  // counted once
        tracks[2].artworkPath = pathToUtf8(localCopy);
        // A file-kind row whose Artwork/ file is there, which no track names:
        // what a failed artwork stage leaves. Counted from the database.
        db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (5, x'6666666666666666666666666666666666666666', NULL);");
        sqlite3_close(db);
        const fs::path fileCover = fixture.library / "Artwork" / (artworkFileName(std::vector<std::uint8_t>(20, 0x66)) + ".jpg");
        write(fileCover, jpeg(std::string(1234, 'f')));
        const std::uint64_t expected = fs::file_size(onStick) + 3000 + 500 + 4 + fs::file_size(fileCover);
        assert(artworkBytesOnStick(tracks, pathToUtf8(fixture.library)) == expected);
        assert(artworkBytesOnStick(tracks, {}) == fs::file_size(onStick));
        fs::remove_all(localCopy.parent_path());

        // A database that cannot be read gives no figure, not a smaller one.
        Fixture broken(seabass::testing::scratchRoot() / "seabass_engine_artwork_stick_bytes_broken");
        write(broken.library / "Database2" / "m.db", std::string(4096, 'x'));
        assert(!artworkBytesOnStick(tracks, pathToUtf8(broken.library)).has_value());
        std::cout << "case 15 (covers are counted where they take up the stick, database included) OK\n";
    }

    // 16. Engine 2.x and 3.x seed the "no cover" row as AlbumArt (1, NULL,
    //     NULL), libdjinterop as (1, '', NULL). Both are Engine's way of
    //     saying a track has no art: never a fault, never repaired. The
    //     committed fixture one_stick_two_catalogs has twelve tracks there.
    //     A row of its own with nothing in it (the other fixture's AlbumArt
    //     469, case 9) stays a fault.
    {
        const fs::path stick = seabass::testing::scratchRoot() / "seabass_engine_artwork_seed_row";
        std::error_code ec;
        fs::remove_all(stick, ec);
        fs::create_directories(stick);
        fs::copy(pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "one_stick_two_catalogs" / "engine",
                 stick / "Engine Library", fs::copy_options::recursive);
        const fs::path database = stick / "Engine Library" / "Database2" / "m.db";
        const auto tracksOnRowOne = [&database] {
            sqlite3 *db = nullptr;
            assert(sqlite3_open(pathToUtf8(database).c_str(), &db) == SQLITE_OK);
            sqlite3_stmt *stmt = nullptr;
            assert(sqlite3_prepare_v2(db, "SELECT id FROM Track WHERE albumArtId = 1 ORDER BY id;", -1, &stmt, nullptr)
                   == SQLITE_OK);
            std::vector<std::int64_t> ids;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                ids.push_back(sqlite3_column_int64(stmt, 0));
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return ids;
        };
        const std::vector<std::int64_t> artless = tracksOnRowOne();
        assert(artless.size() == 12);
        const auto probe = [](const ArtworkEntry &) { return true; };
        const ArtworkAudit audit = auditArtwork(pathToUtf8(stick / "Engine Library"), {}, probe);
        assert(audit.error.empty());
        for (const ArtworkEntry &entry : audit.unreadable) {
            assert(std::find(artless.begin(), artless.end(), entry.trackId) == artless.end());
        }
        const ArtworkRepair repair = repairArtwork(pathToUtf8(stick / "Engine Library"), audit.unreadable, {}, {},
                                                   [](const ArtworkEntry &) { return jpeg("FROM-THE-TAGS"); });
        assert(repair.error.empty());
        assert(tracksOnRowOne() == artless);  // none of them was given a row of its own

        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_null_seed");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (1, NULL, NULL);");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (2, NULL, NULL);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Seed', 'A', 1);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'Lost its hash', 'B', 2);");
        sqlite3_close(db);
        const ArtworkAudit seeded = auditArtwork(pathToUtf8(fixture.library));
        assert(seeded.tracksWithArt == 1);
        assert(seeded.unreadable.size() == 1 && seeded.unreadable[0].trackId == 2);
        assert(seeded.unreadable[0].storage == ArtworkStorage::RowWithoutHash);
        fs::remove_all(stick, ec);
        std::cout << "case 16 (both seeds of Engine's no-cover row are no art, a row of its own is a fault) OK\n";
    }

    // 17. A schema whose AlbumArt has no albumArt column keeps no images in
    //     the database: its art is audited as files, not refused.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_no_image_column");
        sqlite3 *db = fixture.open();
        exec(db, "DROP TABLE AlbumArt;");
        exec(db, "CREATE TABLE AlbumArt (id INTEGER PRIMARY KEY, hash BLOB);");
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (2, x'7777777777777777777777777777777777777777');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'File', 'A', 2);");
        sqlite3_close(db);
        write(fixture.library / "Artwork" / (artworkFileName(std::vector<std::uint8_t>(20, 0x77)) + ".jpg"), jpeg("F"));
        db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (3, x'8888888888888888888888888888888888888888');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'File gone', 'B', 3);");
        sqlite3_close(db);
        const auto probe = [](const ArtworkEntry &) { return true; };
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(audit.error.empty());
        assert(audit.readableByAPlayer == 1);
        assert(audit.repairable() == 1);
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable, {}, {},
                                                   [](const ArtworkEntry &) { return jpeg("TAGS"); });
        assert(repair.error.empty());
        assert(repair.repaired == 1);

        // A text hash there names no image in a row that cannot hold one:
        // never a fault of the in-place kind, and a repair handed one
        // anyway does not fail the save.
        db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash) VALUES (40, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (30, 'Text hash', 'C', 40);");
        sqlite3_close(db);
        const ArtworkAudit withText = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(withText.error.empty());
        for (const ArtworkEntry &entry : withText.unreadable) {
            assert(entry.storage != ArtworkStorage::InDatabaseUnreadable);
        }
        const ArtworkRepair rebuilt = repairArtwork(pathToUtf8(fixture.library), withText.unreadable, {}, {},
                                                    [](const ArtworkEntry &) { return jpeg("TAGS-TOO"); });
        assert(rebuilt.error.empty() && rebuilt.repaired == 1);  // the text-hash track, given a file
        ArtworkEntry inPlace;
        inPlace.trackId = 30;
        inPlace.storage = ArtworkStorage::InDatabaseUnreadable;
        inPlace.otherSource = true;
        const ArtworkRepair refused = repairArtwork(pathToUtf8(fixture.library), {inPlace}, {}, {},
                                                    [](const ArtworkEntry &) { return jpeg("TAGS"); });
        assert(refused.error.empty());
        assert(refused.repaired == 0 && refused.noLongerInDatabase == 1);
        std::cout << "case 17 (no albumArt column: the art is audited as files) OK\n";
    }

    // 18. Bytes in albumArt that are no image: a row whose blob hash names
    //     a file that is there still reads (a player finds the file); any
    //     other such row, an imported path's or a text hash's, is left
    //     alone, and a repair handed one anyway writes nothing.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_junk_in_row");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (2, x'7777777777777777777777777777777777777777', "
                 "x'4A554E4B');");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES "
                 "(3, 'image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg', x'4A554E4B');");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (4, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', "
                 "x'4A554E4B');");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (5, '8998055a7787a03a8e8de2fa607a11f37b4c674a', x'');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'File', 'A', 2);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (3, 'Imported', 'B', 3);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (4, 'In the row', 'C', 4);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (5, 'Empty', 'D', 5);");
        sqlite3_close(db);
        write(fixture.library / "Artwork" / (artworkFileName(std::vector<std::uint8_t>(20, 0x77)) + ".jpg"), jpeg("F"));
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        assert(audit.readableByAPlayer == 1);
        assert(audit.leftAlone.size() == 2);
        assert(audit.leftAlone[0].trackId == 3 && audit.leftAlone[1].trackId == 4);
        assert(audit.unreadable.size() == 1 && audit.unreadable[0].trackId == 5);
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), {audit.leftAlone[0]});
        assert(repair.error.empty() && repair.repaired == 0);

        // A row that stopped keeping its image in the database between the
        // audit and the save is counted, not dropped silently.
        ArtworkEntry moved = audit.unreadable[0];
        moved.otherSource = true;
        db = fixture.open();
        exec(db, "UPDATE AlbumArt SET hash = x'9999999999999999999999999999999999999999' WHERE id = 5;");
        sqlite3_close(db);
        const ArtworkRepair gone = repairArtwork(pathToUtf8(fixture.library), {moved}, {}, {},
                                                 [](const ArtworkEntry &) { return jpeg("TAGS"); });
        assert(gone.repaired == 0 && gone.noLongerInDatabase == 1);
        std::cout << "case 18 (junk in a row leaves it alone unless its file is there) OK\n";
    }

    // 19. A save committing while the audit opens the database is waited
    //     out, not reported as a database that cannot be read.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_busy");
        sqlite3 *db = fixture.open();
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (2, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', x'"
                 "FFD8FF00');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'T', 'A', 2);");
        exec(db, "BEGIN EXCLUSIVE;");
        std::thread release([db] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        });
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library));
        release.join();
        sqlite3_close(db);
        if (!audit.error.empty()) {
            std::cerr << audit.error << "\n";
        }
        assert(audit.error.empty());
        assert(audit.readableByAPlayer == 1);
        std::cout << "case 19 (a save committing is waited out) OK\n";
    }

    // 20. A real image in a format players may not show (GIF, WebP) is
    //     left alone: reported apart from the faults, and never overwritten
    //     in place nor left behind by pointing the track elsewhere,
    //     whatever source is offered.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_other_formats");
        sqlite3 *db = fixture.open();
        const std::string gif = std::string("GIF89a") + std::string(40, 'g');
        const std::string webp = std::string("RIFF\x20\x00\x00\x00WEBPVP8 ", 16) + std::string(40, 'w');
        const auto insertRow = [db](int id, const std::string &image) {
            sqlite3_stmt *insert = nullptr;
            assert(sqlite3_prepare_v2(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (?, ?, ?);", -1, &insert,
                                      nullptr)
                   == SQLITE_OK);
            sqlite3_bind_int(insert, 1, id);
            sqlite3_bind_text(insert, 2, ("abcdef" + std::to_string(id)).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(insert, 3, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
            assert(sqlite3_step(insert) == SQLITE_DONE);
            sqlite3_finalize(insert);
        };
        insertRow(2, gif);
        insertRow(3, webp);
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Gif', 'A', 2);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'WebP', 'B', 3);");
        sqlite3_close(db);
        const auto probe = [](const ArtworkEntry &) { return true; };
        const auto reader = [](const ArtworkEntry &) { return jpeg("FROM-THE-TAGS"); };
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(audit.error.empty());
        assert(audit.unreadable.empty());  // a library of such covers is not a broken one
        assert(audit.leftAlone.size() == 2);
        for (const ArtworkEntry &entry : audit.leftAlone) {
            assert(entry.storage == ArtworkStorage::InDatabaseLeftAlone);
        }
        std::vector<ArtworkEntry> forced;
        for (const ArtworkStorage storage : {ArtworkStorage::InDatabaseUnreadable, ArtworkStorage::CachedFileMissing}) {
            for (std::int64_t track : {1, 2}) {
                ArtworkEntry entry;
                entry.trackId = track;
                entry.storage = storage;
                entry.otherSource = true;
                forced.push_back(entry);
            }
        }
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), forced, {}, {}, reader);
        assert(repair.error.empty());
        assert(repair.repaired == 0);
        assert(repair.filesWritten.empty());
        db = fixture.open();
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(db, "SELECT t.albumArtId, a.albumArt FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId "
                                      "ORDER BY t.id;",
                                  -1, &stmt, nullptr)
               == SQLITE_OK);
        const std::vector<std::pair<int, std::string>> expected = {{2, gif}, {3, webp}};
        for (const auto &[row, image] : expected) {
            assert(sqlite3_step(stmt) == SQLITE_ROW);
            assert(sqlite3_column_int(stmt, 0) == row);
            assert(std::string(static_cast<const char *>(sqlite3_column_blob(stmt, 1)),
                               static_cast<size_t>(sqlite3_column_bytes(stmt, 1)))
                   == image);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        std::cout << "case 20 (a GIF or WebP kept in the database is never written over) OK\n";
    }

    // 21. A library whose schema predates 3.0.2 keeps its covers in the
    //     database, so a repair there gives a track a row of that kind: a
    //     text hash and the image in albumArt, and no file under Artwork/,
    //     which its players do not read. The hash is spelled the way
    //     Engine's own rows appear to be: lowercase hex of a SHA-1 of the
    //     image, without leading zeros (inferred from one untouched JPEG in
    //     a 4.5.0 library). From 3.0.2 on, a repair writes a file as before.
    {
        const auto schema = [](Fixture &fixture, int minor, int patch) {
            sqlite3 *db = fixture.open();
            exec(db, "CREATE TABLE Information (id INTEGER PRIMARY KEY, uuid TEXT, schemaVersionMajor INTEGER, "
                     "schemaVersionMinor INTEGER, schemaVersionPatch INTEGER);");
            exec(db, "INSERT INTO Information VALUES (1, 'u', 3, " + std::to_string(minor) + ", " + std::to_string(patch)
                         + ");");
            exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (1, NULL, NULL), (5, NULL, NULL);");
            exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Asked', 'A', 5);");
            exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (2, 'Also', 'B', 5);");
            sqlite3_close(db);
        };
        const std::string tags = jpeg("ZERO-0");  // SHA-1 027faee4c127397d313b9b26b711d09a7c0eae79
        const auto reader = [&tags](const ArtworkEntry &) { return tags; };
        const auto probe = [](const ArtworkEntry &) { return true; };

        Fixture older(seabass::testing::scratchRoot() / "seabass_engine_artwork_schema_301");
        schema(older, 0, 1);
        const ArtworkAudit audit = auditArtwork(pathToUtf8(older.library), {}, probe);
        assert(audit.repairable() == 2);
        const ArtworkRepair repair = repairArtwork(pathToUtf8(older.library), audit.unreadable, {}, {}, reader);
        assert(repair.error.empty());
        assert(repair.repaired == 2);
        assert(repair.filesWritten.empty());
        assert(fs::is_empty(older.library / "Artwork"));
        sqlite3 *db = older.open();
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(db, "SELECT DISTINCT a.id, typeof(a.hash), a.hash, a.albumArt FROM Track t "
                                      "JOIN AlbumArt a ON a.id = t.albumArtId;",
                                  -1, &stmt, nullptr)
               == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1))) == "text");
        assert(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)))
               == "27faee4c127397d313b9b26b711d09a7c0eae79");
        assert(std::string(static_cast<const char *>(sqlite3_column_blob(stmt, 3)),
                           static_cast<size_t>(sqlite3_column_bytes(stmt, 3)))
               == tags);
        assert(sqlite3_step(stmt) == SQLITE_DONE);  // both tracks share the one new row
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        const ArtworkAudit after = auditArtwork(pathToUtf8(older.library), {}, probe);
        assert(after.readableByAPlayer == 2 && after.unreadable.empty());

        Fixture newer(seabass::testing::scratchRoot() / "seabass_engine_artwork_schema_302");
        schema(newer, 0, 2);
        const ArtworkAudit newerAudit = auditArtwork(pathToUtf8(newer.library), {}, probe);
        const ArtworkRepair newerRepair = repairArtwork(pathToUtf8(newer.library), newerAudit.unreadable, {}, {}, reader);
        assert(newerRepair.error.empty() && newerRepair.repaired == 2);
        assert(newerRepair.filesWritten.size() == 1);
        db = newer.open();
        assert(hashOf(db, 1).size() == 20);  // a blob hash naming the file
        sqlite3_close(db);
        std::cout << "case 21 (a library before schema 3.0.2 is repaired in its own storage) OK\n";
    }

    // 22. Two tracks on one row with no usable image, whose own tags hold
    //     different covers: writing the first one's into the shared row
    //     would give the second the wrong cover. The first gets a row of
    //     its own, in the library's storage; the second, alone on the row
    //     by then, has it repaired in place, as a track alone on its row
    //     always does.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_shared_row");
        sqlite3 *db = fixture.open();
        exec(db, "CREATE TABLE Information (id INTEGER PRIMARY KEY, uuid TEXT, schemaVersionMajor INTEGER, "
                 "schemaVersionMinor INTEGER, schemaVersionPatch INTEGER);");
        exec(db, "INSERT INTO Information VALUES (1, 'u', 3, 0, 1);");
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (1, NULL, NULL), "
                 "(3, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', x''), (4, '8998055a7787a03a8e8de2fa607a11f37b4c674a', x'');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'First', 'A', 3), (2, 'Second', 'B', 3), "
                 "(3, 'Alone', 'C', 4);");
        sqlite3_close(db);
        const auto coverOf = [](const ArtworkEntry &entry) { return jpeg("TAGS-OF-TRACK-" + std::to_string(entry.trackId)); };
        const auto probe = [](const ArtworkEntry &) { return true; };
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(audit.repairable() == 3);
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), audit.unreadable, {}, {}, coverOf);
        assert(repair.error.empty());
        assert(repair.repaired == 3);
        assert(repair.filesWritten.empty());
        db = fixture.open();
        const auto imageFor = [db](int track) {
            sqlite3_stmt *stmt = nullptr;
            sqlite3_prepare_v2(db, "SELECT a.id, a.albumArt FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId "
                                   "WHERE t.id = ?;",
                               -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, track);
            assert(sqlite3_step(stmt) == SQLITE_ROW);
            const auto result = std::pair{sqlite3_column_int(stmt, 0),
                                          std::string(static_cast<const char *>(sqlite3_column_blob(stmt, 1)),
                                                      static_cast<size_t>(sqlite3_column_bytes(stmt, 1)))};
            sqlite3_finalize(stmt);
            return result;
        };
        const auto first = imageFor(1);
        const auto second = imageFor(2);
        const auto alone = imageFor(3);
        assert(first.second == jpeg("TAGS-OF-TRACK-1") && first.first != 3);
        assert(second.second == jpeg("TAGS-OF-TRACK-2") && second.first == 3);
        assert(alone.first == 4 && alone.second == jpeg("TAGS-OF-TRACK-3"));  // in place
        sqlite3_close(db);
        std::cout << "case 22 (tracks sharing an empty row get rows of their own) OK\n";
    }

    // 23. The in-place rule fails closed. Only a value that is positively
    //     empty is written into; anything else that is not JPEG or PNG (an
    //     AVIF, bytes that are no picture) is left alone, never written
    //     over and never pointed away from, and a library of such covers
    //     audits without faults. A check that cannot be read gives the
    //     entry up, counted, and writes nothing.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_fail_closed");
        sqlite3 *db = fixture.open();
        const std::string avif = std::string("\0\0\0\x20" "ftypavif", 12) + std::string(40, 'a');
        const std::string noise = "\x13\x37 random bytes that are no picture";
        const auto insertRow = [db](int id, const char *hashSql, const std::string &image) {
            sqlite3_stmt *insert = nullptr;
            const std::string sql = std::string("INSERT INTO AlbumArt (id, hash, albumArt) VALUES (?, ") + hashSql + ", ?);";
            assert(sqlite3_prepare_v2(db, sql.c_str(), -1, &insert, nullptr) == SQLITE_OK);
            sqlite3_bind_int(insert, 1, id);
            sqlite3_bind_blob(insert, 2, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
            assert(sqlite3_step(insert) == SQLITE_DONE);
            sqlite3_finalize(insert);
        };
        insertRow(2, "'934a576ac3a4a0ab6d66478652b9bb8b7ac68b82'", avif);
        insertRow(3, "'8998055a7787a03a8e8de2fa607a11f37b4c674a'", noise);
        insertRow(4, "x'5555555555555555555555555555555555555555'", noise);  // its file is not there
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Avif', 'A', 2), (2, 'Noise', 'B', 3), "
                 "(3, 'Noise beside a file hash', 'C', 4);");
        sqlite3_close(db);
        const auto probe = [](const ArtworkEntry &) { return true; };
        const auto reader = [](const ArtworkEntry &) { return jpeg("FROM-THE-TAGS"); };
        const ArtworkAudit audit = auditArtwork(pathToUtf8(fixture.library), {}, probe);
        assert(audit.error.empty());
        assert(audit.unreadable.empty());
        assert(audit.leftAlone.size() == 3);
        std::vector<ArtworkEntry> forced;
        for (const ArtworkStorage storage :
             {ArtworkStorage::InDatabaseUnreadable, ArtworkStorage::CachedFileMissing, ArtworkStorage::ImportedPath}) {
            for (std::int64_t track = 1; track <= 3; ++track) {
                ArtworkEntry entry;
                entry.trackId = track;
                entry.storage = storage;
                entry.otherSource = true;
                forced.push_back(entry);
            }
        }
        const ArtworkRepair repair = repairArtwork(pathToUtf8(fixture.library), forced, {}, {}, reader);
        assert(repair.error.empty());
        assert(repair.repaired == 0);
        assert(repair.filesWritten.empty());
        db = fixture.open();
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(db, "SELECT t.albumArtId, a.albumArt FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId "
                                      "ORDER BY t.id;",
                                  -1, &stmt, nullptr)
               == SQLITE_OK);
        for (const auto &[row, image] : std::vector<std::pair<int, std::string>>{{2, avif}, {3, noise}, {4, noise}}) {
            assert(sqlite3_step(stmt) == SQLITE_ROW);
            assert(sqlite3_column_int(stmt, 0) == row);
            assert(std::string(static_cast<const char *>(sqlite3_column_blob(stmt, 1)),
                               static_cast<size_t>(sqlite3_column_bytes(stmt, 1)))
                   == image);
        }
        sqlite3_finalize(stmt);

        // A read that fails: the Track table turned into a view that raises
        // an error for one of its rows. Nothing is written, and the entries
        // it cost are counted with a reason.
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (5, '27faee4c127397d313b9b26b711d09a7c0eae79', x''), "
                 "(6, 'd394737b55f3cd783d37b513dc278b4d3d32b27f', x'');");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (4, 'Empty', 'D', 5), (5, 'Empty too', 'E', 6);");
        exec(db, "ALTER TABLE Track RENAME TO TrackData;");
        exec(db, "CREATE VIEW Track AS SELECT id, title, artist, "
                 "CASE WHEN id = 5 THEN abs(-9223372036854775807 - 1) ELSE albumArtId END AS albumArtId FROM TrackData;");
        sqlite3_close(db);
        std::vector<ArtworkEntry> unreadable;
        for (std::int64_t track : {4, 5}) {
            ArtworkEntry entry;
            entry.trackId = track;
            entry.storage = ArtworkStorage::InDatabaseUnreadable;
            entry.otherSource = true;
            unreadable.push_back(entry);
        }
        const ArtworkRepair failed = repairArtwork(pathToUtf8(fixture.library), unreadable, {}, {}, reader);
        assert(failed.error.empty());
        assert(failed.repaired == 0);
        assert(failed.failedReads >= 1);
        assert(!failed.failureReasons.empty());
        db = fixture.open();
        assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM AlbumArt WHERE id IN (5, 6) AND length(albumArt) = 0;", -1,
                                  &stmt, nullptr)
               == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 2);  // both still empty
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        std::cout << "case 23 (the in-place rule fails closed) OK\n";
    }

    // 24. The storage a repair writes in is read from the library's own
    //     rows first: a text hash beside image bytes means covers in the
    //     database (as in libraries Engine DJ 4.5.0 migrated, which still
    //     say schema 3.0.2), a blob hash means files (as this project's
    //     own library creator writes them); only a library with neither
    //     is decided by its schema.
    {
        const auto library = [](Fixture &fixture, int patch, const std::string &existingRow) {
            sqlite3 *db = fixture.open();
            exec(db, "CREATE TABLE Information (id INTEGER PRIMARY KEY, uuid TEXT, schemaVersionMajor INTEGER, "
                     "schemaVersionMinor INTEGER, schemaVersionPatch INTEGER);");
            exec(db, "INSERT INTO Information VALUES (1, 'u', 3, 0, " + std::to_string(patch) + ");");
            exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES " + existingRow + ", (5, NULL, NULL);");
            exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Has one', 'A', 2), "
                     "(2, 'Asked', 'B', 5);");
            sqlite3_close(db);
        };
        const auto probe = [](const ArtworkEntry &) { return true; };
        const auto reader = [](const ArtworkEntry &) { return jpeg("ZERO-0"); };

        Fixture migrated(seabass::testing::scratchRoot() / "seabass_engine_artwork_rows_say_database");
        library(migrated, 2, "(2, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', x'FFD8FF00')");
        const ArtworkAudit audit = auditArtwork(pathToUtf8(migrated.library), {}, probe);
        assert(audit.coversInDatabase);
        const ArtworkRepair repair = repairArtwork(pathToUtf8(migrated.library), audit.unreadable, {}, {}, reader);
        assert(repair.error.empty() && repair.repaired == 1 && repair.filesWritten.empty());
        sqlite3 *db = migrated.open();
        assert(hashOf(db, 2) == "27faee4c127397d313b9b26b711d09a7c0eae79");
        sqlite3_close(db);

        Fixture ours(seabass::testing::scratchRoot() / "seabass_engine_artwork_rows_say_files");
        library(ours, 1, "(2, x'7777777777777777777777777777777777777777', NULL)");
        write(ours.library / "Artwork" / (artworkFileName(std::vector<std::uint8_t>(20, 0x77)) + ".jpg"), jpeg("F"));
        const ArtworkAudit oursAudit = auditArtwork(pathToUtf8(ours.library), {}, probe);
        assert(!oursAudit.coversInDatabase);
        const ArtworkRepair oursRepair = repairArtwork(pathToUtf8(ours.library), oursAudit.unreadable, {}, {}, reader);
        assert(oursRepair.error.empty() && oursRepair.repaired == 1 && oursRepair.filesWritten.size() == 1);
        db = ours.open();
        assert(hashOf(db, 2).size() == 20);
        sqlite3_close(db);
        std::cout << "case 24 (the library's own rows decide the storage a repair writes in) OK\n";
    }

    // 25. A save repairs one track at a time, and the storage it writes in
    //     must not drift as it goes. A track keeps the kind of row it
    //     points at (a text hash: the database; a blob hash: a file); only
    //     a track with no usable row takes the library's kind, which the
    //     audit decided once, by counting rows of each kind (text rows
    //     whose image is gone count too) and the schema only when there
    //     are none.
    {
        const auto probe = [](const ArtworkEntry &) { return true; };
        const auto reader = [](const ArtworkEntry &entry) { return jpeg("TAGS-" + std::to_string(entry.trackId)); };
        const auto schema = [](sqlite3 *db, int patch) {
            exec(db, "CREATE TABLE Information (id INTEGER PRIMARY KEY, uuid TEXT, schemaVersionMajor INTEGER, "
                     "schemaVersionMinor INTEGER, schemaVersionPatch INTEGER);");
            exec(db, "INSERT INTO Information VALUES (1, 'u', 3, 0, " + std::to_string(patch) + ");");
        };
        const auto oneAtATime = [&](Fixture &fixture, const ArtworkAudit &audit) {
            ArtworkRepair total;
            for (ArtworkEntry entry : audit.unreadable) {
                entry.otherSource = true;  // the imported one's image is on no stick here: take the tags
                const ArtworkRepair one = repairArtwork(pathToUtf8(fixture.library), {entry}, {}, {}, reader);
                assert(one.error.empty());
                total.repaired += one.repaired;
                total.filesWritten.insert(total.filesWritten.end(), one.filesWritten.begin(), one.filesWritten.end());
            }
            return total;
        };
        const auto textRows = [](Fixture &fixture) {
            sqlite3 *db = fixture.open();
            sqlite3_stmt *stmt = nullptr;
            sqlite3_prepare_v2(db,
                               "SELECT count(*) FROM AlbumArt WHERE typeof(hash) = 'text' AND hash != '' AND "
                               "substr(hash, 1, 8) != 'image://';",
                               -1, &stmt, nullptr);
            sqlite3_step(stmt);
            const int count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return count;
        };

        // A file library with one empty text row, repaired first: the
        // others are not turned into database rows by it.
        Fixture files(seabass::testing::scratchRoot() / "seabass_engine_artwork_stable_files");
        sqlite3 *db = files.open();
        schema(db, 2);
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES "
                 "(2, x'2222222222222222222222222222222222222222', NULL), "
                 "(3, x'3333333333333333333333333333333333333333', NULL), "
                 "(4, x'4444444444444444444444444444444444444444', NULL), "
                 "(5, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', x''), "
                 "(6, 'image://fileart//media/X/PIONEER/Artwork/00001/gone.jpg', NULL);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'Text row', 'A', 5), "
                 "(2, 'File gone', 'B', 2), (3, 'File gone too', 'C', 3), (4, 'Imported', 'D', 6);");
        sqlite3_close(db);
        write(files.library / "Artwork" / (artworkFileName(std::vector<std::uint8_t>(20, 0x44)) + ".jpg"), jpeg("F"));
        const ArtworkAudit filesAudit = auditArtwork(pathToUtf8(files.library), {}, probe);
        assert(!filesAudit.coversInDatabase);
        assert(filesAudit.unreadable.size() == 4);
        const ArtworkRepair filesRepair = oneAtATime(files, filesAudit);
        assert(filesRepair.repaired == 4);
        assert(filesRepair.filesWritten.size() == 3);  // tracks 2, 3 and 4 as files
        assert(textRows(files) == 1);                   // only the row that was text to begin with

        // A database library whose images are all gone, two tracks sharing
        // a row: the database it stays.
        Fixture emptied(seabass::testing::scratchRoot() / "seabass_engine_artwork_stable_database");
        db = emptied.open();
        schema(db, 2);
        exec(db, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (2, '934a576ac3a4a0ab6d66478652b9bb8b7ac68b82', x''), "
                 "(3, '8998055a7787a03a8e8de2fa607a11f37b4c674a', NULL);");
        exec(db, "INSERT INTO Track (id, title, artist, albumArtId) VALUES (1, 'A', 'A', 2), (2, 'B', 'B', 2), "
                 "(3, 'C', 'C', 3);");
        sqlite3_close(db);
        const ArtworkAudit emptiedAudit = auditArtwork(pathToUtf8(emptied.library), {}, probe);
        assert(emptiedAudit.coversInDatabase);
        const ArtworkRepair emptiedRepair = oneAtATime(emptied, emptiedAudit);
        assert(emptiedRepair.repaired == 3);
        assert(emptiedRepair.filesWritten.empty());
        assert(fs::is_empty(emptied.library / "Artwork"));
        std::cout << "case 25 (a save keeps each track's kind of storage, and the library's is decided once) OK\n";
    }

    std::cout << "engine_artwork_test: all cases passed\n";
    return 0;
}
