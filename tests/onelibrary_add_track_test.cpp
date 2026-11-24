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
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
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
    // A file on the stick, made on the way: a row is only added for one
    // that is there.
    const auto touch = [](const fs::path &path) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << "audio";
        return pathToUtf8(path);
    };
    const auto file = [&](const fs::path &root, const char *name) {
        return touch(root / "Contents" / "An Artist" / "An Album" / name);
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
        second.analysisDataFilePath = "/PIONEER/USBANLZ/P0AB/0000BEEF/ANLZ0000.DAT";
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
        other.analysisDataFilePath.clear();
        assert(w.addTrack(other) == 18);
        assert(one(db, "SELECT count(*) FROM album WHERE name = 'An Album'") == "2");
        assert(one(db, "SELECT count(DISTINCT album_id) FROM content WHERE content_id IN (16, 18)") == "2");
        assert(one(db, "SELECT fileType || '/' || typeof(image_id) FROM content WHERE content_id = 18") == "4/null");

        // rekordbox's own lookup rows are found, not doubled: its artist,
        // its album without an artist, its key.
        OneLibraryCueWriter::NewTrack theirs;
        theirs.filePath = touch(stick / "Contents" / "Seabass Test" / "T73" / "new.aiff");
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
        bare.filePath = touch(stick / "Contents" / "UnknownArtist" / "UnknownAlbum" / "bare.wav");
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
        again.filePath = touch(stick / "Contents" / "Seabass Test" / "T73 Base" / "T73-B07-click-44k-114bpm.flac");
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(again); }));
        OneLibraryCueWriter::NewTrack bad = full;
        bad.artist = "Never Made";
        bad.filePath = file(stick, "04 Fresh.flac");
        // Its own analysis file, so that each refusal below is for the
        // one thing wrong with it.
        bad.analysisDataFilePath = "/PIONEER/USBANLZ/P0AC/0000BEEF/ANLZ0000.DAT";
        bad.title.clear();
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }));
        bad.title = "Fresh";
        for (const char *date : {"", "24-11-2026", "2026-11-24 10:00", "2026/11/24", "2026-1-24", "2026-11/24",
                                 "2026-x1-24", "2026-11-2x", "x026-11-24"}) {
            bad.date = date;
            assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }));
        }
        for (const char *date : {"2026-13-01", "2026-00-10", "2026-01-32", "2026-01-00", "2026-02-29", "2026-02-30",
                                 "2026-04-31", "2100-02-29"}) {
            bad.date = date;
            assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }) && "not a day on a calendar");
        }
        bad.date = "2026-11-24";
        // Numbers the row cannot hold are not clamped into it.
        {
            const auto refused = [&](const std::function<void(OneLibraryCueWriter::NewTrack &)> &spoil) {
                OneLibraryCueWriter::NewTrack t = bad;
                spoil(t);
                return throws<std::invalid_argument>([&] { (void)w.addTrack(t); });
            };
            assert(refused([](auto &t) { t.bpm = -1.0; }) && refused([](auto &t) { t.bpm = 1000.0; }));
            assert(refused([](auto &t) { t.bpm = std::numeric_limits<double>::quiet_NaN(); }));
            assert(refused([](auto &t) { t.bpm = std::numeric_limits<double>::infinity(); }));
            assert(refused([](auto &t) { t.lengthSeconds = -1; }) && refused([](auto &t) { t.trackNumber = -1; }));
            assert(refused([](auto &t) { t.discNumber = -1; }) && refused([](auto &t) { t.year = -1; }));
            assert(refused([](auto &t) { t.bitrate = -1; }) && refused([](auto &t) { t.bitDepth = -1; }));
            assert(refused([](auto &t) { t.sampleRateHz = -1; }));
            assert(refused([](auto &t) { t.fileSizeBytes = std::numeric_limits<std::uint64_t>::max(); }));
            // An analysis file another row names (rekordbox's, and one of
            // this test's), and paths that are not under /PIONEER/.
            assert(refused([](auto &t) { t.analysisDataFilePath = "/PIONEER/USBANLZ/P01E/00009ECC/ANLZ0000.DAT"; }));
            assert(refused([](auto &t) { t.analysisDataFilePath = "/PIONEER/USBANLZ/P0AA/0000BEEF/ANLZ0000.DAT"; }));
            assert(refused([](auto &t) { t.analysisDataFilePath = "USBANLZ/P0AA/0000BEEF/ANLZ0000.DAT"; }));
            assert(refused([](auto &t) { t.imagePath = "/Contents/cover.jpg"; }));
            // The same analysis file as the stick's filesystem sees it.
            assert(refused([](auto &t) { t.analysisDataFilePath = "/PIONEER/USBANLZ/p01e/00009ecc/anlz0000.dat"; }));
            // Texts a row cannot hold and find again: a blank at the end,
            // a NUL, bytes that are not UTF-8.
            for (const char *text : {"DJ X ", "DJ X\t", "caf\xc3", "caf\xc3" "A", "\xed\xa0\x80", "\xc0\xaf"}) {
                for (std::string OneLibraryCueWriter::NewTrack::*field :
                     {&OneLibraryCueWriter::NewTrack::title, &OneLibraryCueWriter::NewTrack::artist,
                      &OneLibraryCueWriter::NewTrack::album, &OneLibraryCueWriter::NewTrack::albumArtist,
                      &OneLibraryCueWriter::NewTrack::genre, &OneLibraryCueWriter::NewTrack::label,
                      &OneLibraryCueWriter::NewTrack::remixer, &OneLibraryCueWriter::NewTrack::composer,
                      &OneLibraryCueWriter::NewTrack::key, &OneLibraryCueWriter::NewTrack::comment}) {
                    assert(refused([&](auto &t) { t.*field = text; }));
                }
            }
            assert(refused([](auto &t) { t.artist = std::string("a\0b", 3); }));
        }
        // A file that is not there, and one that is not on this stick.
        bad.filePath = pathToUtf8(stick / "Contents" / "An Artist" / "An Album" / "never copied.flac");
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }));
        bad.filePath = touch(scratch / "beside the stick.flac");
        assert(throws<std::invalid_argument>([&] { (void)w.addTrack(bad); }));
        for (const char *name : {"05 Fresh.ogg", "06 Fresh", "07 Fresh.flac.txt", "08 Fresh.mp4"}) {
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
    std::cout << "case 5 (a named file, a missing one, one off the stick, no title, impossible dates and numbers, a shared "
                 "analysis file, an unknown type: refused, nothing written) OK\n";

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
    for (const char *change :
         {"ALTER TABLE artist ADD COLUMN sortName varchar;", "ALTER TABLE album ADD COLUMN year integer;",
          "ALTER TABLE genre ADD COLUMN x integer;", "ALTER TABLE label ADD COLUMN x integer;",
          "ALTER TABLE key ADD COLUMN x integer;", "ALTER TABLE image ADD COLUMN x integer;",
          "ALTER TABLE property DROP COLUMN numberOfContents;", "UPDATE property SET numberOfContents = NULL;",
          "INSERT INTO property (numberOfContents) VALUES (3);", "INSERT INTO property (numberOfContents) VALUES (NULL);",
          "DELETE FROM property;"}) {
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
        if (!throws<std::runtime_error>([&] { (void)strange.addTrack(t); })) {
            std::cerr << "not refused after: " << change << "\n";
            assert(false);
        }
        strange.finishWriting();
        assert(one(otherDb, "SELECT count(*) || '/' || (SELECT count(*) FROM artist) FROM content") == "11/1");
    }
    std::cout << "case 7 (an unknown content or lookup schema, and a property table without its one count, are refused) OK\n";

    // 8. A failure after the lookup rows were made takes them back:
    //    something in the database refuses the content row itself.
    {
        const fs::path broken = stickAt("broken");
        const std::string brokenDb = pathToUtf8(broken / "PIONEER" / "rekordbox" / "exportLibrary.db");
        {
            SqlCipherLibrary lib;
            SqlCipherDb raw(lib, brokenDb, false);
            raw.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
            raw.exec("CREATE TRIGGER refuse BEFORE INSERT ON content BEGIN SELECT RAISE(ABORT, 'refused'); END;");
        }
        OneLibraryCueWriter half(pathToUtf8(broken / "PIONEER"));
        OneLibraryCueWriter::NewTrack t = full;
        t.filePath = file(broken, "01 Tagged.flac");
        assert(throws<std::runtime_error>([&] { (void)half.addTrack(t); }));
        half.finishWriting();
        assert(one(brokenDb, "SELECT count(*) || '/' || (SELECT count(*) FROM artist) || '/' || "
                             "(SELECT count(*) FROM genre) || '/' || (SELECT count(*) FROM image) || '/' || "
                             "(SELECT numberOfContents FROM property) FROM content")
               == "11/1/0/0/11");
    }
    std::cout << "case 8 (a failed insert leaves no row and no lookup row) OK\n";

    // 9. An id something still names is not given out again: the
    //    player's history, a My Tag, a recommendation, a cue or a
    //    playlist row left behind by a removed track (rekordbox's rows
    //    end at 15).
    {
        int n = 0;
        for (const char *leftover :
             {"INSERT INTO history_content (history_id, content_id, sequenceNo) VALUES (1, 40, 1);",
              "INSERT INTO myTag_content (myTag_id, content_id) VALUES (1, 40);",
              "INSERT INTO recommendedLike (content_id_1, content_id_2, rating, createdDate) VALUES (40, 2, 1, 0);",
              "INSERT INTO recommendedLike (content_id_1, content_id_2, rating, createdDate) VALUES (2, 40, 1, 0);",
              "INSERT INTO cue (cue_id, content_id, kind) VALUES (900, 40, 0);",
              "INSERT INTO playlist_content (playlist_id, content_id, sequenceNo) VALUES (9, 40, 9);",
              "DELETE FROM content WHERE content_id = 15;"}) {
            const fs::path other = stickAt("leftover" + std::to_string(++n));
            const std::string otherDb = pathToUtf8(other / "PIONEER" / "rekordbox" / "exportLibrary.db");
            {
                SqlCipherLibrary lib;
                SqlCipherDb raw(lib, otherDb, false);
                raw.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
                raw.exec(leftover);
            }
            OneLibraryCueWriter after(pathToUtf8(other / "PIONEER"));
            OneLibraryCueWriter::NewTrack t = full;
            t.filePath = file(other, "01 Tagged.flac");
            // The last: content 15 removed with its playlist row left,
            // as this class's own removal never leaves it; 15 is named.
            const int64_t expected = n == 7 ? 16 : 41;
            const int64_t got = after.addTrack(t);
            if (got != expected) {
                std::cerr << "id " << got << " after: " << leftover << "\n";
                assert(false);
            }
            after.finishWriting();
        }
    }
    std::cout << "case 9 (an id the history, a tag, a recommendation, a cue or a playlist still names is not reused) OK\n";

    // 10. A file two rows already name in two other spellings is named:
    //     not a third row.
    {
        const fs::path other = stickAt("spellings");
        const std::string otherDb = pathToUtf8(other / "PIONEER" / "rekordbox" / "exportLibrary.db");
        {
            SqlCipherLibrary lib;
            SqlCipherDb raw(lib, otherDb, false);
            raw.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
            raw.exec("UPDATE content SET path = '/Contents/AN ARTIST/An Album/01 Tagged.flac' WHERE content_id = 1;");
            raw.exec("UPDATE content SET path = '/Contents/an artist/An Album/01 Tagged.flac' WHERE content_id = 2;");
        }
        OneLibraryCueWriter twice(pathToUtf8(other / "PIONEER"));
        OneLibraryCueWriter::NewTrack t = full;
        t.filePath = file(other, "01 Tagged.flac");
        assert(throws<std::invalid_argument>([&] { (void)twice.addTrack(t); }));
        twice.finishWriting();
        assert(one(otherDb, "SELECT count(*) FROM content") == "11");
    }
    std::cout << "case 10 (a file named by two rows in two other spellings is not given a third) OK\n";

    // 11. A row that reads back other than it was written is taken out
    //     again, the count with it, and the call fails. Something in the
    //     database rewrites the title as it lands.
    {
        const fs::path other = stickAt("readback");
        const std::string otherDb = pathToUtf8(other / "PIONEER" / "rekordbox" / "exportLibrary.db");
        {
            SqlCipherLibrary lib;
            SqlCipherDb raw(lib, otherDb, false);
            raw.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
            raw.exec("CREATE TRIGGER rewrite AFTER INSERT ON content BEGIN "
                     "UPDATE content SET fileSize = 1 WHERE content_id = NEW.content_id; END;");
        }
        OneLibraryCueWriter spoiled(pathToUtf8(other / "PIONEER"));
        OneLibraryCueWriter::NewTrack t = full;
        t.filePath = file(other, "01 Tagged.flac");
        bool failed = false;
        try {
            (void)spoiled.addTrack(t);
        } catch (const std::runtime_error &e) {
            failed = std::string(e.what()).find("taken out again") != std::string::npos;
        }
        assert(failed);
        // And the writer goes on working: its baseline is the file as the
        // removal left it.
        assert(!spoiled.hasTrackAtPath(t.filePath));
        spoiled.finishWriting();
        assert(one(otherDb, "SELECT count(*) || '/' || (SELECT numberOfContents FROM property) FROM content") == "11/11");
        // With every lookup row it had made: rekordbox's one artist, two
        // albums and two keys are what is left.
        assert(one(otherDb, "SELECT (SELECT count(*) FROM artist) || '/' || (SELECT count(*) FROM album) || '/' || "
                            "(SELECT count(*) FROM genre) || '/' || (SELECT count(*) FROM label) || '/' || "
                            "(SELECT count(*) FROM key) || '/' || (SELECT count(*) FROM image)")
               == "1/2/0/0/2/0");
    }
    std::cout << "case 11 (a row that does not read back as written is removed again, with its lookup rows) OK\n";

    fs::remove_all(scratch);
    std::cout << "onelibrary_add_track_test: all cases passed\n";
    return 0;
}
