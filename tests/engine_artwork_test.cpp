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

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <sqlite3.h>

#include "infrastructure/engine/engine_artwork.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::engine;

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
        std::cerr << "sql failed: " << (error ? error : "?") << " -- " << sql << "\n";
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
        assert(sqlite3_open((library / "Database2" / "m.db").string().c_str(), &db) == SQLITE_OK);
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
        assert(sqlite3_open((library / "Database2" / "m.db").string().c_str(), &db) == SQLITE_OK);
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
            std::cout << "  e.g. id=" << entry.trackId << " " << entry.title << " -- " << entry.artist << "\n"
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
        assert(classifyArtworkReference("") == ArtworkStorage::None);
        assert(classifyArtworkReference("image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg")
               == ArtworkStorage::ImportedPath);
        assert(classifyArtworkReference(std::string("\x01\x02\x03", 3)) == ArtworkStorage::Cached);
        const std::string here =
            imageOnStickFor("image://fileart//media/WHALESHARK2/PIONEER/Artwork/00001/a5_m.jpg", "/media/OTHER");
        assert(here == (fs::path("/media/OTHER") / "PIONEER/Artwork/00001/a5_m.jpg").make_preferred().string());
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
        assert(literalBackslash == (fs::path("/media/OTHER") / "PIONEER/Artwork/00001/a\\b.jpg").make_preferred().string());
        const std::string fromWindows =
            imageOnStickFor("image://fileart//E:\\PIONEER\\Artwork\\00001\\a5_m.jpg", "/media/OTHER");
        assert(fromWindows == (fs::path("/media/OTHER") / "PIONEER/Artwork/00001/a5_m.jpg").make_preferred().string());
        std::cout << "case 2 (an imported path is recognised, either spelling, and re-anchored here) OK\n";
    }

    // 3. The audit: one track a player can read, one whose cached file is
    //    gone, one imported path whose image is here, one whose is not.
    {
        Fixture fixture(seabass::testing::scratchRoot() / "seabass_engine_artwork_audit");
        sqlite3 *db = fixture.open();
        std::vector<std::uint8_t> cached(20, 0x11);
        const std::string cachedName = artworkFileName(cached);
        write(fixture.library / "Artwork" / (cachedName + ".jpg"), "CACHED-IMAGE");
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

        const ArtworkAudit audit = auditArtwork(fixture.library.string());
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
        const ArtworkRepair repair = repairArtwork(fixture.library.string(), audit.unreadable);
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        assert(repair.filesWritten.size() == 1);
        assert(fs::is_regular_file(repair.filesWritten[0]));

        const ArtworkAudit after = auditArtwork(fixture.library.string());
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

        const ArtworkAudit audit = auditArtwork(fixture.library.string());
        // The text file is on the stick, so it used to count as repairable
        // and the page offered it. The audit reads the first bytes now, so
        // the count it shows and the button it offers are what a repair
        // will actually do -- and one odd file among a thousand good ones
        // cannot stop the save.
        assert(audit.repairable() == 2);
        assert(audit.unreadable.size() == 3);
        assert(audit.unreadable[2].trackId == 3);  // the text file, listed but not offered
        assert(audit.unreadable[2].imageOnStick.empty());

        const ArtworkRepair repair = repairArtwork(fixture.library.string(), audit.unreadable);
        assert(repair.error.empty());
        assert(repair.repaired == 2);   // the JPEG and the PNG
        assert(repair.notAnImage == 0); // the text file was never offered
        assert(repair.filesWritten.size() == 2);
        for (const std::string &written : repair.filesWritten) {
            const std::string ext = fs::path(written).extension().string();
            assert(ext == ".jpg" || ext == ".png");
        }
        const ArtworkAudit after = auditArtwork(fixture.library.string());
        assert(after.readableByAPlayer == 2);
        assert(after.unreadable.size() == 1);  // only the text file is left
        assert(after.unreadable[0].trackId == 3);

        // And handed one anyway -- the file changed under the page between
        // the scan and the save -- the repair refuses it by name rather
        // than writing a <hash>.txt no player will open.
        ArtworkEntry forced = after.unreadable[0];
        forced.imageOnStick = (fixture.stick / "PIONEER" / "Artwork" / "00001" / "notes.txt").string();
        const ArtworkRepair refused = repairArtwork(fixture.library.string(), {forced});
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

        const ArtworkAudit audit = auditArtwork(fixture.library.string());
        assert(audit.repairable() == 1);
        db = fixture.open();
        exec(db, "DELETE FROM Track WHERE id = 7;");
        sqlite3_close(db);

        const ArtworkRepair repair = repairArtwork(fixture.library.string(), audit.unreadable);
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

        const ArtworkAudit audit = auditArtwork(fixture.library.string());
        assert(audit.repairable() == 1);
        const ArtworkRepair repair =
            repairArtwork(fixture.library.string(), audit.unreadable, [](const std::string &) {
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

        const ArtworkAudit audit = auditArtwork(fixture.library.string());
        const fs::path scratch = fixture.stick / "scratch-m.db";
        fs::copy_file(fixture.library / "Database2" / "m.db", scratch);
        const ArtworkRepair repair = repairArtwork(fixture.library.string(), audit.unreadable, {}, scratch.string());
        assert(repair.error.empty());
        assert(repair.repaired == 1);
        assert(repair.filesWritten.size() == 1);
        // The image went into the library, as always.
        assert(fs::path(repair.filesWritten[0]).parent_path() == fixture.library / "Artwork");

        sqlite3 *scratchDb = nullptr;
        assert(sqlite3_open(scratch.string().c_str(), &scratchDb) == SQLITE_OK);
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

        const ArtworkAudit audit = auditArtwork(fixture.library.string());
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

    std::cout << "engine_artwork_test: all cases passed\n";
    return 0;
}
