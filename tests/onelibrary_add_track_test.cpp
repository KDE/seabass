// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// OneLibraryCueWriter::addTrack (the track import): a content row made
// from a file alone, on a copy of an exportLibrary.db rekordbox 7.2.18
// wrote (the T73 fixture: 11 tracks, content ids up to 15, one artist,
// two albums without an artist, two keys, no genre, label or image).
// The new row is held against rekordbox's own rows column by column.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::pathToUtf8;
using namespace seabass::infrastructure::onelibrary;

namespace
{

// One query's rows (their first column) as text, NULL as "<null>", over
// a connection of its own that is closed again before the writer goes on.
std::vector<std::vector<std::string>> query(const std::string &dbFile, const std::string &sql)
{
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbFile, true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement statement(db, sql);
    std::vector<std::vector<std::string>> rows;
    while (statement.step()) {
        std::vector<std::string> row;
        row.push_back(statement.columnIsNull(0) ? "<null>" : statement.columnText(0));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string one(const std::string &dbFile, const std::string &sql)
{
    const auto rows = query(dbFile, sql);
    assert(rows.size() == 1 && rows[0].size() == 1);
    return rows[0][0];
}

template <typename Exception, typename Call>
bool throws(Call &&call)
{
    try {
        call();
    } catch (const Exception &) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: onelibrary_add_track_test <tests/fixtures/testshark_t73>\n";
        return 2;
    }
    const fs::path fixture = seabass::pathFromUtf8(argv[1]);
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_onelibrary_add_track_test";
    fs::remove_all(scratch);
    // A copy first: SQLite rewrites -wal and -shm beside whatever it opens.
    const auto stickAt = [&](const std::string &name) {
        const fs::path stick = scratch / name;
        fs::create_directories(stick / "PIONEER" / "rekordbox");
        fs::copy_file(fixture / "stick" / "PIONEER" / "rekordbox" / "exportLibrary.db",
                      stick / "PIONEER" / "rekordbox" / "exportLibrary.db");
        return stick;
    };
    const fs::path stick = stickAt("stick");
    const std::string db = pathToUtf8(stick / "PIONEER" / "rekordbox" / "exportLibrary.db");
    const auto file = [&](const fs::path &root, const char *name) {
        return pathToUtf8(root / "Contents" / "An Artist" / "An Album" / name);
    };

    // The fixture as committed.
    assert(one(db, "SELECT count(*) FROM content") == "11" && one(db, "SELECT max(content_id) FROM content") == "15");
    assert(one(db, "SELECT numberOfContents FROM property") == "11");
    assert(one(db, "SELECT count(*) FROM artist") == "1" && one(db, "SELECT count(*) FROM album") == "2");
    assert(one(db, "SELECT count(*) FROM genre") == "0" && one(db, "SELECT count(*) FROM label") == "0");
    assert(one(db, "SELECT count(*) FROM image") == "0" && one(db, "SELECT count(*) FROM key") == "2");

    auto writer = std::make_unique<OneLibraryCueWriter>(pathToUtf8(stick / "PIONEER"));
    OneLibraryCueWriter &w = *writer;

    OneLibraryCueWriter::NewTrack full;
    full.filePath = file(stick, "01 Tagged.flac");
    full.title = "T\xc3\xa4gged";
    full.artist = "An Artist";
    full.album = "An Album";
    full.albumArtist = "An Album Artist";
    full.genre = "Techno";
    full.label = "A Label";
    full.remixer = "A Remixer";
    full.composer = "A Composer";
    full.key = "Am";
    full.comment = "from the tag";
    full.analysisDataFilePath = "/PIONEER/USBANLZ/P0AA/0000BEEF/ANLZ0000.DAT";
    full.imagePath = "/PIONEER/Artwork/00001/a1.jpg";
    full.bpm = 127.97;
    full.lengthSeconds = 301;
    full.trackNumber = 3;
    full.discNumber = 1;
    full.year = 1997;
    full.bitrate = 0;
    full.bitDepth = 24;
    full.sampleRateHz = 48000;
    full.fileSizeBytes = 123456789;
    full.date = "2026-11-24";

    // 1. The row, its id above the highest, every lookup made.
    const int64_t id = w.addTrack(full);
    assert(id == 16 && "one above the highest id; the gaps at 3, 4, 7 and 8 are not filled");
    const std::string where = " FROM content WHERE content_id = 16";
    assert(one(db, "SELECT title" + where) == "T\xc3\xa4gged");
    assert(one(db, "SELECT path" + where) == "/Contents/An Artist/An Album/01 Tagged.flac");
    assert(one(db, "SELECT fileName" + where) == "01 Tagged.flac");
    assert(one(db, "SELECT bpmx100" + where) == "12797" && one(db, "SELECT length" + where) == "301");
    assert(one(db, "SELECT trackNo" + where) == "3" && one(db, "SELECT discNo" + where) == "1");
    assert(one(db, "SELECT releaseYear" + where) == "1997" && one(db, "SELECT djComment" + where) == "from the tag");
    assert(one(db, "SELECT fileType" + where) == "5" && one(db, "SELECT fileSize" + where) == "123456789");
    assert(one(db, "SELECT bitrate" + where) == "0" && one(db, "SELECT bitDepth" + where) == "24");
    assert(one(db, "SELECT samplingRate" + where) == "48000");
    assert(one(db, "SELECT dateCreated || '/' || dateAdded" + where) == "2026-11-24/2026-11-24");
    assert(one(db, "SELECT analysisDataFilePath" + where) == "/PIONEER/USBANLZ/P0AA/0000BEEF/ANLZ0000.DAT");
    assert(one(db, "SELECT a.name FROM content c JOIN artist a ON a.artist_id = c.artist_id_artist WHERE c.content_id = 16")
           == "An Artist");
    assert(one(db, "SELECT a.name FROM content c JOIN artist a ON a.artist_id = c.artist_id_remixer WHERE c.content_id = 16")
           == "A Remixer");
    assert(one(db, "SELECT a.name FROM content c JOIN artist a ON a.artist_id = c.artist_id_composer WHERE c.content_id = 16")
           == "A Composer");
    assert(one(db, "SELECT al.name || '/' || a.name FROM content c JOIN album al ON al.album_id = c.album_id "
                   "JOIN artist a ON a.artist_id = al.artist_id WHERE c.content_id = 16")
           == "An Album/An Album Artist");
    assert(one(db, "SELECT g.name FROM content c JOIN genre g ON g.genre_id = c.genre_id WHERE c.content_id = 16") == "Techno");
    assert(one(db, "SELECT l.name FROM content c JOIN label l ON l.label_id = c.label_id WHERE c.content_id = 16") == "A Label");
    assert(one(db, "SELECT k.name FROM content c JOIN key k ON k.key_id = c.key_id WHERE c.content_id = 16") == "Am");
    assert(one(db, "SELECT i.path FROM content c JOIN image i ON i.image_id = c.image_id WHERE c.content_id = 16")
           == "/PIONEER/Artwork/00001/a1.jpg");
    // Four artists made (artist, remixer, composer, album artist) beside
    // rekordbox's one; ids go on from the highest.
    assert(one(db, "SELECT count(*) || '/' || max(artist_id) FROM artist") == "5/5");
    assert(one(db, "SELECT count(*) FROM artist WHERE nameForSearch IS NULL") == "5" && "as rekordbox's own artist rows");
    assert(one(db, "SELECT isComplation || '/' || typeof(nameForSearch) || '/' || typeof(image_id) FROM album "
                   "WHERE name = 'An Album'")
           == "0/null/null");
    assert(one(db, "SELECT count(DISTINCT typeof(nameForSearch) || typeof(image_id) || isComplation) FROM album") == "1"
           && "as rekordbox's own album rows");
    assert(one(db, "SELECT count(*) FROM key") == "3" && one(db, "SELECT numberOfContents FROM property") == "12");
    assert(one(db, "SELECT count(*) FROM content") == "12");
    std::cout << "case 1 (a tagged file: the row, id 16, every lookup row made) OK\n";

    // 2. Column by column against a row rekordbox wrote (content 13, a
    //    FLAC of its second export). Same SQLite type in every column
    //    but the ones the import cannot have a value for, each named.
    {
        const auto columns = query(db, "SELECT name FROM pragma_table_info('content')");
        assert(columns.size() == 46);
        const std::map<std::string, std::string> differs = {
            // rekordbox's row has no genre, label or image; ours has.
            {"genre_id", "integer"}, {"label_id", "integer"}, {"image_id", "integer"},
            {"artist_id_remixer", "integer"}, {"artist_id_composer", "integer"},
            // Counters rekordbox moves as it writes cues and analysis:
            // NULL, as on its own rows that were never analysed.
            {"cueUpdateCount", "null"}, {"analysisDataUpdateCount", "null"}, {"informationUpdateCount", "null"},
        };
        for (const auto &column : columns) {
            const std::string name = column[0];
            const std::string ours = one(db, "SELECT typeof(\"" + name + "\") FROM content WHERE content_id = 16");
            const std::string theirs = one(db, "SELECT typeof(\"" + name + "\") FROM content WHERE content_id = 13");
            const auto known = differs.find(name);
            if (known != differs.end()) {
                assert(ours == known->second);
            } else if (ours != theirs) {
                std::cerr << name << ": " << ours << " here, " << theirs << " in rekordbox's row\n";
                assert(false && "a column typed differently from rekordbox's own row");
            }
        }
        // And the same value wherever rekordbox's rows all agree on one.
        for (const char *constant : {"titleForSearch", "subtitle", "artist_id_lyricist", "color_id", "releaseDate",
                                     "isrc", "djPlayCount", "isHotCueAutoLoadOn", "isKuvoDeliverStatusOn",
                                     "kuvoDeliveryComment", "hasModified"}) {
            const std::string name = constant;
            assert(one(db, "SELECT count(DISTINCT coalesce(\"" + name + "\", '<null>')) FROM content WHERE content_id < 16") == "1"
                   && "rekordbox's eleven rows agree");
            assert(one(db, "SELECT count(DISTINCT coalesce(\"" + name + "\", '<null>')) FROM content") == "1");
        }
        // No stars: the import has no rating to give (one of
        // rekordbox's rows here has four).
        assert(one(db, "SELECT rating" + where) == "0");
        // What only rekordbox's collection knows: none here.
        assert(one(db, "SELECT masterDbId || '/' || masterContentId || '/' || contentLink || '/' || analysedBits" + where)
               == "0/0/0/0");
    }
    std::cout << "case 2 (typed and filled as rekordbox's own row, column by column) OK\n";

    // 3. A second track of the same artist, album, genre, label, key:
    //    no lookup row is made twice. The same album name under another
    //    album artist is another album.
    {
        OneLibraryCueWriter::NewTrack second = full;
        second.filePath = file(stick, "02 Second.mp3");
        second.title = "Second";
        second.imagePath = "/PIONEER/Artwork/00001/a2.jpg";
        second.bitrate = 320;
        assert(w.addTrack(second) == 17);
        assert(one(db, "SELECT count(*) FROM artist") == "5" && one(db, "SELECT count(*) FROM album") == "3");
        assert(one(db, "SELECT count(*) FROM genre") == "1" && one(db, "SELECT count(*) FROM label") == "1");
        assert(one(db, "SELECT count(*) FROM key") == "3" && one(db, "SELECT count(*) FROM image") == "2");
        assert(one(db, "SELECT count(DISTINCT album_id) FROM content WHERE content_id IN (16, 17)") == "1");
        assert(one(db, "SELECT fileType || '/' || bitrate FROM content WHERE content_id = 17") == "1/320");

        OneLibraryCueWriter::NewTrack other = full;
        other.filePath = file(stick, "03 Other.m4a");
        other.title = "Other";
        other.albumArtist = "Somebody Else";
        other.imagePath.clear();
        assert(w.addTrack(other) == 18);
        assert(one(db, "SELECT count(*) FROM album WHERE name = 'An Album'") == "2");
        assert(one(db, "SELECT count(DISTINCT album_id) FROM content WHERE content_id IN (16, 18)") == "2");
        assert(one(db, "SELECT fileType || '/' || typeof(image_id) FROM content WHERE content_id = 18") == "4/null");

        // rekordbox's own lookup rows are found, not doubled: its artist,
        // its album without an artist, its key.
        OneLibraryCueWriter::NewTrack theirs;
        theirs.filePath = pathToUtf8(stick / "Contents" / "Seabass Test" / "T73" / "new.aiff");
        theirs.title = "Theirs";
        theirs.artist = "Seabass Test";
        theirs.album = "T73";
        theirs.key = "Bm";
        theirs.date = "2026-11-24";
        assert(w.addTrack(theirs) == 19);
        assert(one(db, "SELECT artist_id_artist || '/' || album_id || '/' || key_id || '/' || fileType FROM content "
                       "WHERE content_id = 19")
               == "1/2/1/12");
        assert(one(db, "SELECT count(*) FROM artist") == "6" && one(db, "SELECT count(*) FROM album") == "4");
        assert(one(db, "SELECT numberOfContents FROM property") == "15");
    }
    std::cout << "case 3 (lookup rows shared, an album told apart by its artist, rekordbox's rows reused) OK\n";

    // 4. A file with nothing but a name: no lookup row is named or made.
    {
        const std::string lookups = one(db, "SELECT (SELECT count(*) FROM artist) + (SELECT count(*) FROM album) + "
                                            "(SELECT count(*) FROM genre) + (SELECT count(*) FROM label) + "
                                            "(SELECT count(*) FROM key) + (SELECT count(*) FROM image)");
        OneLibraryCueWriter::NewTrack bare;
        bare.filePath = pathToUtf8(stick / "Contents" / "UnknownArtist" / "UnknownAlbum" / "bare.wav");
        bare.title = "bare";
        bare.date = "2026-11-24";
        assert(w.addTrack(bare) == 20);
        assert(one(db, "SELECT typeof(artist_id_artist) || typeof(album_id) || typeof(genre_id) || typeof(label_id) || "
                       "typeof(key_id) || typeof(image_id) || typeof(artist_id_remixer) || typeof(artist_id_composer) "
                       "FROM content WHERE content_id = 20")
               == "nullnullnullnullnullnullnullnull");
        assert(one(db, "SELECT fileType || '/' || bpmx100 || '/' || analysisDataFilePath || '/' || djComment FROM content "
                       "WHERE content_id = 20")
               == "11/0//");
        assert(one(db, "SELECT (SELECT count(*) FROM artist) + (SELECT count(*) FROM album) + "
                       "(SELECT count(*) FROM genre) + (SELECT count(*) FROM label) + "
                       "(SELECT count(*) FROM key) + (SELECT count(*) FROM image)")
               == lookups);
    }
    std::cout << "case 4 (an untagged file: no lookup row named, none made) OK\n";

    // 5. Refused, nothing written: a file a row already names (in
    //    another letter case too, as the stick's filesystem compares),
    //    no title, a date in another spelling, a type with no number.
    {
        const std::string before = one(db, "SELECT count(*) || '/' || (SELECT count(*) FROM artist) || '/' || "
                                           "(SELECT numberOfContents FROM property) FROM content");
        OneLibraryCueWriter::NewTrack again = full;
        again.artist = "Never Made";
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(again); }));
        again.filePath = file(stick, "01 TAGGED.FLAC");
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(again); }));
        // rekordbox's own row for that file.
        again.filePath = pathToUtf8(stick / "Contents" / "Seabass Test" / "T73 Base" / "T73-B07-click-44k-114bpm.flac");
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(again); }));
        OneLibraryCueWriter::NewTrack bad = full;
        bad.artist = "Never Made";
        bad.filePath = file(stick, "04 Fresh.flac");
        bad.title.clear();
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }));
        bad.title = "Fresh";
        for (const char *date : {"", "24-11-2026", "2026-11-24 10:00", "2026/11/24", "2026-1-24", "2026-11/24",
                                 "2026-x1-24", "2026-11-2x", "x026-11-24"}) {
            bad.date = date;
            assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }));
        }
        bad.date = "2026-11-24";
        for (const char *name : {"05 Fresh.ogg", "06 Fresh", "07 Fresh.flac.txt"}) {
            bad.filePath = file(stick, name);
            assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }));
        }
        assert(one(db, "SELECT count(*) || '/' || (SELECT count(*) FROM artist) || '/' || "
                       "(SELECT numberOfContents FROM property) FROM content")
               == before);
        // And the same track, spelled right, goes in.
        bad.filePath = file(stick, "04 Fresh.FLAC");
        assert(w.addTrack(bad) == 21);
        assert(one(db, "SELECT fileType FROM content WHERE content_id = 21") == "5");
    }
    std::cout << "case 5 (a named file, no title, a misspelled date, an unknown type: refused, nothing written) OK\n";

    // 6. Read back as Seabass reads a stick.
    {
        writer->finishWriting();
        writer.reset();
        OneLibraryReader reader(pathToUtf8(stick / "PIONEER"));
        const auto tracks = reader.readAll();
        assert(tracks.size() == 17);
        const seabass::domain::Track *found = nullptr;
        for (const auto &t : tracks) {
            if (t.title == "T\xc3\xa4gged") {
                assert(found == nullptr);
                found = &t;
            }
        }
        assert(found != nullptr);
        assert(found->artist == "An Artist" && found->album == "An Album");
        assert(found->bpm > 127.96 && found->bpm < 127.98);
        assert(found->filePath == full.filePath);
    }
    std::cout << "case 6 (the reader lists the new rows with rekordbox's) OK\n";

    // 7. A content table with a column this build has never seen: not
    //    guessed at. And one that lost a column.
    //    Each alone: one more column, one fewer, and the same number
    //    with one under another name.
    int schema = 0;
    for (const char *change : {"ALTER TABLE content ADD COLUMN somethingNew integer;",
                               "ALTER TABLE content DROP COLUMN isrc;",
                               "ALTER TABLE content RENAME COLUMN isrc TO isrcCode;"}) {
        const fs::path other = stickAt("schema" + std::to_string(++schema));
        const std::string otherDb = pathToUtf8(other / "PIONEER" / "rekordbox" / "exportLibrary.db");
        {
            SqlCipherLibrary lib;
            SqlCipherDb raw(lib, otherDb, false);
            raw.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
            raw.exec(change);
        }
        OneLibraryCueWriter strange(pathToUtf8(other / "PIONEER"));
        OneLibraryCueWriter::NewTrack t = full;
        t.filePath = file(other, "01 Tagged.flac");
        assert(throws<std::runtime_error>([&] { (void)strange.addTrack(t); }));
        strange.finishWriting();
        assert(one(otherDb, "SELECT count(*) || '/' || (SELECT count(*) FROM artist) FROM content") == "11/1");
    }
    std::cout << "case 7 (an unknown content schema is refused) OK\n";

    // 8. A failure after the lookup rows were made takes them back: the
    //    property table gone, so the last statement of the transaction
    //    fails.
    {
        const fs::path broken = stickAt("broken");
        const std::string brokenDb = pathToUtf8(broken / "PIONEER" / "rekordbox" / "exportLibrary.db");
        {
            SqlCipherLibrary lib;
            SqlCipherDb raw(lib, brokenDb, false);
            raw.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
            raw.exec("DROP TABLE property;");
        }
        OneLibraryCueWriter half(pathToUtf8(broken / "PIONEER"));
        OneLibraryCueWriter::NewTrack t = full;
        t.filePath = file(broken, "01 Tagged.flac");
        assert(throws<std::runtime_error>([&] { (void)half.addTrack(t); }));
        half.finishWriting();
        assert(one(brokenDb, "SELECT count(*) || '/' || (SELECT count(*) FROM artist) || '/' || "
                             "(SELECT count(*) FROM genre) || '/' || (SELECT count(*) FROM image) FROM content")
               == "11/1/0/0");
    }
    std::cout << "case 8 (a failed insert leaves no row and no lookup row) OK\n";

    fs::remove_all(scratch);
    std::cout << "onelibrary_add_track_test: all cases passed\n";
    return 0;
}
