// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PdbRowWriter::addTrack (the track import): track rows added to a copy
// of an export.pdb rekordbox 7.2.18 wrote (the T73 fixture: 11 tracks,
// ids up to 15, one artist, two albums without an artist, two keys, no
// genre, label or artwork). The file is read back with the app's own
// reader after every commit, and the new row's bytes are held against
// the encoder that pdb_row_encoding_test holds against rekordbox.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain/import_field_checks.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_encoding.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::pathToUtf8;
using namespace seabass::infrastructure::rekordbox;

namespace
{

constexpr size_t LenPage = 4096;

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::uint32_t u16(const std::string &b, size_t at)
{
    return static_cast<unsigned char>(b[at]) | (static_cast<unsigned char>(b[at + 1]) << 8);
}

std::uint32_t u32(const std::string &b, size_t at)
{
    return u16(b, at) | (u16(b, at + 2) << 16);
}

struct Row
{
    size_t at = 0;
    std::uint32_t index = 0;
    std::uint32_t page = 0;
};

// Every present row of the table of type `type`, by bytes and offsets.
std::vector<Row> rowsOf(const std::string &pdb, std::uint32_t type)
{
    std::vector<Row> rows;
    for (std::uint32_t t = 0; t < u32(pdb, 8); ++t) {
        const size_t entry = 28 + 16 * t;
        if (u32(pdb, entry) != type) {
            continue;
        }
        const std::uint32_t last = u32(pdb, entry + 12);
        for (std::uint32_t page = u32(pdb, entry + 8);; page = u32(pdb, LenPage * page + 12)) {
            const size_t base = LenPage * page;
            assert(base + LenPage <= pdb.size() && u32(pdb, base + 8) == type && u32(pdb, base + 4) == page);
            const bool dataPage = (static_cast<unsigned char>(pdb[base + 27]) & 0x40) == 0;
            const std::uint32_t offsets = u32(pdb, base + 24) & 0x1FFF;
            for (std::uint32_t r = 0; dataPage && r < offsets; ++r) {
                const size_t group = base + LenPage - (r / 16) * 0x24;
                if ((u16(pdb, group - 4) >> (r % 16)) & 1) {
                    rows.push_back({base + 40 + u16(pdb, group - 6 - 2 * (r % 16)), r, page});
                }
            }
            if (page == last) {
                break;
            }
        }
    }
    return rows;
}

std::optional<Row> trackRow(const std::string &pdb, std::uint32_t id)
{
    for (const Row &row : rowsOf(pdb, 0)) {
        if (u32(pdb, row.at + 72) == id) {
            return row;
        }
    }
    return std::nullopt;
}

// What the app reads of a track, as one line.
std::string line(const seabass::domain::Track &t)
{
    return t.sourceId + "|" + t.title + "|" + t.artist + "|" + t.album + "|" + std::to_string(t.bpm) + "|" + t.key + "|"
           + std::to_string(t.durationSeconds) + "|" + t.comment + "|" + t.filePath + "|" + std::to_string(t.cues.size()) + "|"
           + std::to_string(t.playlists.size());
}

std::map<std::string, std::string> readLibrary(const fs::path &stick)
{
    KaitaiRekordboxReader reader(pathToUtf8(stick / "PIONEER"));
    std::map<std::string, std::string> tracks;
    for (const auto &t : reader.readAll()) {
        assert(tracks.emplace(t.sourceId, line(t)).second && "one row per id");
    }
    return tracks;
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
        std::cerr << "usage: pdb_add_track_test <tests/fixtures/testshark_t73>\n";
        return 2;
    }
    const fs::path fixture = seabass::pathFromUtf8(argv[1]);
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_pdb_add_track_test";
    fs::remove_all(scratch);
    const auto stickAt = [&](const std::string &name) {
        const fs::path stick = scratch / name;
        fs::create_directories(stick / "PIONEER" / "rekordbox");
        fs::copy_file(fixture / "stick" / "PIONEER" / "rekordbox" / "export.pdb", stick / "PIONEER" / "rekordbox" / "export.pdb");
        return stick;
    };
    const auto pdbOf = [](const fs::path &stick) { return stick / "PIONEER" / "rekordbox" / "export.pdb"; };
    const fs::path stick = stickAt("stick");
    const std::string original = readFile(pdbOf(stick));
    const auto before = readLibrary(stick);
    assert(before.size() == 11 && rowsOf(original, 0).size() == 11);
    assert(rowsOf(original, 2).size() == 1 && rowsOf(original, 3).size() == 2 && rowsOf(original, 5).size() == 2);
    assert(rowsOf(original, 1).empty() && rowsOf(original, 4).empty() && rowsOf(original, 13).empty());

    PdbRowWriter::NewTrack full;
    full.pathOnStick = "/Contents/An Artist/An Album/01 Tagged.flac";
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
    full.analyzePath = "/PIONEER/USBANLZ/P0AA/0000BEEF/ANLZ0000.DAT";
    full.artworkPath = "/PIONEER/Artwork/00001/a1.jpg";
    full.bpm = 127.97;
    full.durationSeconds = 301;
    full.trackNumber = 3;
    full.discNumber = 1;
    full.year = 1997;
    full.bitrate = 0;
    full.sampleDepth = 24;
    full.sampleRateHz = 48000;
    full.fileSizeBytes = 123456789;
    full.date = "2026-11-24";

    // 1. One tagged track: its id above the highest, the row readable by
    //    the app, rekordbox's eleven rows as they were.
    std::uint32_t fullId = 0;
    {
        PdbRowWriter writer(pathToUtf8(pdbOf(stick)));
        fullId = writer.addTrack(full);
        // rekordbox's highest present row is 15, but the table holds a row
        // marked deleted with id 16, and a deleted playlist entry names it.
        assert(fullId == 17 && "above 16, the highest id the table has ever held");
        assert(readFile(pdbOf(stick)) == original && "nothing reaches the file before commit()");
        assert(writer.commit());
    }
    const std::string afterOne = readFile(pdbOf(stick));
    {
        const auto after = readLibrary(stick);
        assert(after.size() == 12);
        for (const auto &[id, text] : before) {
            assert(after.at(id) == text && "rekordbox's rows read as before");
        }
        assert(after.at("17")
               == "17|T\xc3\xa4gged|An Artist|An Album|127.970000|Am|301.000000|from the tag|"
                      + pathToUtf8(stick / "Contents" / "An Artist" / "An Album" / "01 Tagged.flac") + "|0|0");
    }
    std::cout << "case 1 (a tagged track: id 17, read back by the app, rekordbox's rows untouched) OK\n";

    // 2. The row's own bytes: the encoder's, for these fields, with the
    //    lookup rows it names made and found; then the room rekordbox
    //    leaves; and its number on the page inside it.
    {
        const auto row = trackRow(afterOne, 17);
        assert(row);
        // Four artists in the order they were asked for, after
        // rekordbox's one; the album under its album artist.
        const auto artists = rowsOf(afterOne, 2);
        assert(artists.size() == 5);
        const auto albums = rowsOf(afterOne, 3);
        assert(albums.size() == 3);
        const Row &album = albums.back();
        assert(u32(afterOne, album.at + 12) == 3 && u32(afterOne, album.at + 8) == 5 && "album 3, of artist 5");
        assert(afterOne.compare(album.at, 31, encodePdbAlbumRow(album.index, 3, 5, "An Album")) == 0);
        for (std::uint32_t i = 1; i < 5; ++i) {
            const char *names[] = {"", "An Artist", "A Remixer", "A Composer", "An Album Artist"};
            const std::string expected = encodePdbArtistRow(artists[i].index, i + 1, names[i]);
            assert(afterOne.compare(artists[i].at, expected.size(), expected) == 0);
        }
        assert(rowsOf(afterOne, 1).size() == 1 && rowsOf(afterOne, 4).size() == 1 && rowsOf(afterOne, 13).size() == 1);
        assert(afterOne.compare(rowsOf(afterOne, 1)[0].at, 11, encodePdbNameRow(1, "Techno")) == 0);
        assert(afterOne.compare(rowsOf(afterOne, 4)[0].at, 12, encodePdbNameRow(1, "A Label")) == 0);
        assert(afterOne.compare(rowsOf(afterOne, 13)[0].at, 34, encodePdbNameRow(1, "/PIONEER/Artwork/00001/a1.jpg")) == 0);
        assert(rowsOf(afterOne, 5).size() == 3);
        assert(afterOne.compare(rowsOf(afterOne, 5)[2].at, 11, encodePdbKeyRow(3, "Am")) == 0);

        PdbTrackRowFields f;
        f.rowIndex = row->index;
        f.sampleRate = 48000;
        f.composerId = 4;
        f.fileSize = 123456789;
        f.artworkId = 1;
        f.keyId = 3;
        f.labelId = 1;
        f.remixerId = 3;
        f.bitrate = 0;
        f.trackNumber = 3;
        f.tempo = 12797;
        f.genreId = 1;
        f.albumId = 3;
        f.artistId = 2;
        f.id = 17;
        f.discNumber = 1;
        f.year = 1997;
        f.sampleDepth = 24;
        f.duration = 301;
        f.fileType = 5;
        f.dateAdded = "2026-11-24";
        f.analyzePath = "/PIONEER/USBANLZ/P0AA/0000BEEF/ANLZ0000.DAT";
        f.analyzeDate = "2026-11-24";
        f.comment = "from the tag";
        f.title = "T\xc3\xa4gged";
        f.filename = "01 Tagged.flac";
        f.filePath = "/Contents/An Artist/An Album/01 Tagged.flac";
        const std::string expected = encodePdbTrackRow(f);
        assert(afterOne.compare(row->at, expected.size(), expected) == 0);
        // What rekordbox's collection would have filled: none.
        assert(u32(afterOne, row->at + 20) == 0 && u32(afterOne, row->at + 24) == 0 && u32(afterOne, row->at + 4) == 0xC0700);
        // The room after it: at least 48 bytes, all zero, inside the
        // page's used part.
        const size_t base = LenPage * row->page;
        const size_t usedEnd = base + 40 + u16(afterOne, base + 30);
        const size_t rowEnd = row->at + expected.size();
        assert(usedEnd >= rowEnd + 48 && usedEnd < rowEnd + 52 && (usedEnd - row->at) % 4 == 0);
        assert(afterOne.substr(rowEnd, usedEnd - rowEnd) == std::string(usedEnd - rowEnd, '\0'));
        // It went onto the table's last page as rekordbox left it (page
        // 52, three rows present of seven), as row 7.
        assert(row->page == 52 && row->index == 7 && u16(afterOne, row->at + 2) == 7 * 0x20);
    }
    std::cout << "case 2 (the row is the encoder's bytes with 48 bytes of room; lookup rows made in order) OK\n";

    // 3. Lookup rows are shared, rekordbox's own included; an album is
    //    told apart by its album artist; an untagged file names none.
    {
        PdbRowWriter writer(pathToUtf8(pdbOf(stick)));
        PdbRowWriter::NewTrack second = full;
        second.pathOnStick = "/Contents/An Artist/An Album/02 Second.mp3";
        second.title = "Second";
        second.analyzePath = "/PIONEER/USBANLZ/P0AB/0000BEEF/ANLZ0000.DAT";
        second.bitrate = 320;
        // A tempo whose hundredths a plain cut gets wrong: 64.07 times
        // 100 is 6406.99... as a double.
        second.bpm = 64.07;
        assert(writer.addTrack(second) == 18);
        PdbRowWriter::NewTrack other = full;
        other.pathOnStick = "/Contents/An Artist/An Album/03 Other.m4a";
        other.title = "Other";
        other.albumArtist = "Somebody Else";
        other.analyzePath.clear();
        other.artworkPath.clear();
        assert(writer.addTrack(other) == 19);
        PdbRowWriter::NewTrack theirs;
        theirs.pathOnStick = "/Contents/Seabass Test/T73/new.aiff";
        theirs.title = "Theirs";
        theirs.artist = "Seabass Test";
        theirs.album = "T73";
        theirs.key = "Bm";
        theirs.date = "2026-11-24";
        assert(writer.addTrack(theirs) == 20);
        PdbRowWriter::NewTrack bare;
        bare.pathOnStick = "/Contents/UnknownArtist/UnknownAlbum/bare.wav";
        bare.title = "bare";
        bare.date = "2026-11-24";
        assert(writer.addTrack(bare) == 21);
        assert(writer.commit());
        const std::string pdb = readFile(pdbOf(stick));
        assert(rowsOf(pdb, 2).size() == 6 && "one artist more: Somebody Else");
        assert(rowsOf(pdb, 3).size() == 4 && "one album more: An Album under Somebody Else");
        assert(rowsOf(pdb, 1).size() == 1 && rowsOf(pdb, 4).size() == 1 && rowsOf(pdb, 5).size() == 3
               && rowsOf(pdb, 13).size() == 1);
        const auto at = [&](std::uint32_t id) { return trackRow(pdb, id).value().at; };
        assert(u32(pdb, at(18) + 64) == 3 && u32(pdb, at(19) + 64) == 4 && "the same album name, two albums");
        assert(u32(pdb, at(18) + 28) == 1 && u32(pdb, at(19) + 28) == 0 && "the cover shared, and none");
        assert(u16(pdb, at(18) + 90) == 1 && u16(pdb, at(19) + 90) == 4 && u16(pdb, at(20) + 90) == 12
               && u16(pdb, at(21) + 90) == 11 && "mp3, m4a, aiff, wav");
        assert(u32(pdb, at(18) + 48) == 320 && u32(pdb, at(18) + 56) == 6407);
        // The analysis is dated when there is one: string 15, after
        // string 14's path, is the day on 18 and empty on 19.
        assert(pdb.substr(at(18) + u16(pdb, at(18) + 94 + 2 * 15), 11) == std::string("\x17") + "2026-11-24");
        assert(pdb[at(19) + u16(pdb, at(19) + 94 + 2 * 14)] == '\x03' && pdb[at(19) + u16(pdb, at(19) + 94 + 2 * 15)] == '\x03');
        // rekordbox's artist 1, its album 2 ("T73", no artist), its key 1.
        assert(u32(pdb, at(20) + 68) == 1 && u32(pdb, at(20) + 64) == 2 && u32(pdb, at(20) + 32) == 1);
        for (const size_t field : {12, 28, 32, 40, 44, 60, 64, 68}) {
            assert(u32(pdb, at(21) + field) == 0 && "an untagged file names no lookup row");
        }
        const auto after = readLibrary(stick);
        assert(after.size() == 16);
        for (const auto &[id, text] : before) {
            assert(after.at(id) == text);
        }
        assert(after.at("20").rfind("20|Theirs|Seabass Test|T73|0.000000|Bm|", 0) == 0);
        assert(after.at("21").rfind("21|bare|||0.000000||0.000000||", 0) == 0);
    }
    std::cout << "case 3 (lookup rows shared, rekordbox's reused, an album told apart by its artist, a bare file) OK\n";

    // 4. Refused, the library as it was: every reason in turn, then the
    //    same track spelled right goes in.
    {
        PdbRowWriter writer(pathToUtf8(pdbOf(stick)));
        PdbRowWriter::NewTrack good = full;
        good.pathOnStick = "/Contents/An Artist/An Album/04 Fresh.flac";
        good.title = "Fresh";
        good.artist = "Never Made Unless Added";
        good.analyzePath = "/PIONEER/USBANLZ/P0AC/0000BEEF/ANLZ0000.DAT";
        // Refused, and for the reason under test: `why` is in the words
        // of the refusal.
        const auto refused = [&](const std::function<void(PdbRowWriter::NewTrack &)> &spoil, const std::string &why) {
            PdbRowWriter::NewTrack t = good;
            spoil(t);
            try {
                (void)writer.addTrack(t);
            } catch (const std::invalid_argument &e) {
                if (std::string(e.what()).find(why) != std::string::npos) {
                    return true;
                }
                std::cerr << "refused, but with: " << e.what() << " (wanted: " << why << ")\n";
            } catch (const std::exception &e) {
                std::cerr << "threw: " << e.what() << "\n";
            }
            return false;
        };
        assert(refused([](auto &t) { t.title.clear(); }, "needs a title"));
        for (const char *date : {"", "24-11-2026", "2026-11-24 10:00", "2026/11/24", "2026-1-24", "2026-11/24", "2026-x1-24",
                                 "2026-11-2x", "x026-11-24", "2026-13-01", "2026-00-10", "2026-01-32", "2026-01-00",
                                 "2026-02-29", "2026-02-30", "2026-04-31", "2100-02-29"}) {
            assert(refused([&](auto &t) { t.date = date; }, "is not a date"));
        }
        assert(seabass::domain::isCalendarDate("2024-02-29") && seabass::domain::isCalendarDate("2000-02-29")
               && seabass::domain::isCalendarDate("2026-12-31") && "leap days and the last day are days");
        for (const char *path : {"", "/", "Contents/a.flac", "/Contents/../a.flac", "/Contents/a/..", "/Contents\\a.flac",
                                 "/Contents//a.flac", "/Contents/./a.flac", "/Contents/a.flac/", "//Contents/a.flac"}) {
            assert(refused([&](auto &t) { t.pathOnStick = path; }, "is not a path from the stick's root"));
        }
        for (const char *path : {"/Contents/a.ogg", "/Contents/a", "/Contents/a.flac/b", "/Contents/a.flac.txt",
                                 "/Contents/a.mp4"}) {
            assert(refused([&](auto &t) { t.pathOnStick = path; }, "no file type known"));
        }
        // Texts a row cannot hold and find again: a blank at the end (the
        // reader takes it off, so the name would never match itself), a
        // NUL, bytes that are not UTF-8.
        for (const char *text : {"DJ X ", "DJ X\t", "caf\xc3", "caf\xc3" "A", "\xed\xa0\x80", "\xc0\xaf", "\xf5\x80\x80\x80"}) {
            for (std::string PdbRowWriter::NewTrack::*field :
                 {&PdbRowWriter::NewTrack::title, &PdbRowWriter::NewTrack::artist, &PdbRowWriter::NewTrack::album,
                  &PdbRowWriter::NewTrack::albumArtist, &PdbRowWriter::NewTrack::genre, &PdbRowWriter::NewTrack::label,
                  &PdbRowWriter::NewTrack::remixer, &PdbRowWriter::NewTrack::composer, &PdbRowWriter::NewTrack::key,
                  &PdbRowWriter::NewTrack::comment}) {
                assert(refused([&](auto &t) { t.*field = text; }, "is not UTF-8, or ends in a blank"));
            }
        }
        assert(refused([](auto &t) { t.artist = std::string("a\0b", 3); }, "is not UTF-8, or ends in a blank"));
        // A row has to fit a page with its room: a comment of 2500
        // accented letters is 5 KB of UTF-16. Asked without a library
        // too, for a caller that has not copied the file yet.
        assert(refused([](auto &t) {
            for (int i = 0; i < 2500; ++i) {
                t.comment += "\xc3\xa9";
            }
        }, "do not fit a page"));
        assert(PdbRowWriter::newTrackRefusal(good).empty());
        {
            PdbRowWriter::NewTrack t = good;
            t.title.clear();
            assert(PdbRowWriter::newTrackRefusal(t).find("needs a title") != std::string::npos);
            t = good;
            t.comment.assign(3000, 'c');
            assert(PdbRowWriter::newTrackRefusal(t).empty() && "3 KB of plain text is a long string, and fits");
            t.comment.assign(3900, 'c');
            assert(PdbRowWriter::newTrackRefusal(t).find("do not fit a page") != std::string::npos);
        }
        // A title past what a string's 16-bit length holds.
        assert(throws<std::invalid_argument>([&] {
            PdbRowWriter::NewTrack t = good;
            t.title.clear();
            for (int i = 0; i < 40000; ++i) {
                t.title += "\xc3\xa9";
            }
            (void)writer.addTrack(t);
        }));
        // A file a row already names: rekordbox's, in its spelling and in
        // another letter case, and one of this test's.
        assert(refused([](auto &t) { t.pathOnStick = "/Contents/Seabass Test/T73 Base/T73-B07-click-44k-114bpm.flac"; }, "already names"));
        assert(refused([](auto &t) { t.pathOnStick = "/CONTENTS/SEABASS TEST/T73 Base/T73-B07-click-44k-114bpm.FLAC"; }, "already names"));
        assert(refused([](auto &t) { t.pathOnStick = "/Contents/An Artist/An Album/01 TAGGED.flac"; }, "already names"));
        assert(refused([](auto &t) { t.bpm = -1.0; }, "tempo") && refused([](auto &t) { t.bpm = 1000.0; }, "tempo"));
        assert(refused([](auto &t) { t.bpm = std::numeric_limits<double>::quiet_NaN(); }, "tempo"));
        assert(refused([](auto &t) { t.durationSeconds = -1; }, "does not fit") && refused([](auto &t) { t.durationSeconds = 65536; }, "does not fit"));
        assert(refused([](auto &t) { t.discNumber = -1; }, "does not fit") && refused([](auto &t) { t.discNumber = 65536; }, "does not fit"));
        assert(refused([](auto &t) { t.year = -1; }, "does not fit") && refused([](auto &t) { t.year = 65536; }, "does not fit"));
        assert(refused([](auto &t) { t.sampleDepth = -1; }, "does not fit") && refused([](auto &t) { t.sampleDepth = 65536; }, "does not fit"));
        assert(refused([](auto &t) { t.trackNumber = -1; }, "does not fit") && refused([](auto &t) { t.bitrate = -1; }, "does not fit"));
        assert(refused([](auto &t) { t.sampleRateHz = -1; }, "does not fit"));
        assert(refused([](auto &t) { t.fileSizeBytes = 0x100000000ull; }, "too large"));
        // An analysis file another row names (rekordbox's, and this
        // test's), and paths that are not under /PIONEER/.
        assert(refused([](auto &t) { t.analyzePath = "/PIONEER/USBANLZ/P01E/00009ECC/ANLZ0000.DAT"; }, "already names the analysis file"));
        // The same file as the stick's filesystem sees it, in another case.
        assert(refused([](auto &t) { t.analyzePath = "/PIONEER/USBANLZ/p01e/00009ecc/anlz0000.dat"; }, "already names the analysis file"));
        assert(refused([](auto &t) { t.analyzePath = "/PIONEER/USBANLZ/P0AA/0000BEEF/ANLZ0000.DAT"; }, "already names the analysis file"));
        assert(refused([](auto &t) { t.analyzePath = "USBANLZ/P0AC/0000BEEF/ANLZ0000.DAT"; }, "under the stick's /PIONEER/"));
        assert(refused([](auto &t) { t.artworkPath = "/Contents/cover.jpg"; }, "under the stick's /PIONEER/"));
        // Nothing of all that is in the buffer: committing writes a file
        // that reads as before, with no artist made for a refused track.
        (void)writer.commit();
        assert(rowsOf(readFile(pdbOf(stick)), 2).size() == 6 && readLibrary(stick).size() == 16);
    }
    {
        PdbRowWriter writer(pathToUtf8(pdbOf(stick)));
        PdbRowWriter::NewTrack good = full;
        good.pathOnStick = "/Contents/An Artist/An Album/04 Fresh.FLAC";
        good.title = "Fresh";
        good.analyzePath = "/PIONEER/USBANLZ/P0AC/0000BEEF/ANLZ0000.DAT";
        // The largest numbers the fields hold are held.
        good.durationSeconds = 65535;
        good.fileSizeBytes = 0xFFFFFFFFull;
        assert(writer.addTrack(good) == 22 && writer.commit());
        const std::string pdb = readFile(pdbOf(stick));
        const size_t at = trackRow(pdb, 22).value().at;
        assert(u16(pdb, at + 84) == 65535 && u32(pdb, at + 16) == 0xFFFFFFFFu && u16(pdb, at + 90) == 5);
    }
    std::cout << "case 4 (every refusal leaves the library as it was; the limits themselves are held) OK\n";

    // 5. More tracks than the last page has room for: new pages are
    //    started, for the lookup tables too, and everything reads back.
    //    Titles and paths of every string kind (short, long, UTF-16).
    {
        PdbRowWriter writer(pathToUtf8(pdbOf(stick)));
        const std::string longName(140, 'n');
        for (int i = 0; i < 120; ++i) {
            PdbRowWriter::NewTrack t;
            const std::string n = std::to_string(i);
            t.pathOnStick = "/Contents/Many/" + (i % 3 == 0 ? longName : std::string("m")) + n + ".mp3";
            t.title = (i % 3 == 1 ? "Caf\xc3\xa9 " : "Many ") + n;
            t.artist = "Artist " + n + (i % 5 == 0 ? " \xc3\xa9" : "");
            t.album = "Album " + std::to_string(i / 2);
            t.albumArtist = t.artist;
            t.genre = "Genre " + std::to_string(i % 7);
            t.label = "Label " + std::to_string(i % 11);
            t.key = std::to_string(i % 12 + 1) + "A";
            t.artworkPath = "/PIONEER/Artwork/00001/a" + std::to_string(100 + i) + ".jpg";
            t.analyzePath = "/PIONEER/USBANLZ/P1" + std::to_string(100 + i) + "/0000BEEF/ANLZ0000.DAT";
            t.bpm = 100.0 + i;
            t.durationSeconds = 200 + i;
            t.date = "2026-11-24";
            assert(writer.addTrack(t) == static_cast<std::uint32_t>(23 + i));
        }
        assert(writer.commit());
        const std::string pdb = readFile(pdbOf(stick));
        assert(pdb.size() % LenPage == 0 && pdb.size() > afterOne.size() + 10 * LenPage && "pages were added");
        assert(rowsOf(pdb, 0).size() == 17 + 120);
        assert(rowsOf(pdb, 2).size() == 6 + 120 && rowsOf(pdb, 3).size() == 4 + 120 && rowsOf(pdb, 13).size() == 1 + 120);
        assert(rowsOf(pdb, 1).size() == 1 + 7 && rowsOf(pdb, 4).size() == 1 + 11 && rowsOf(pdb, 5).size() == 3 + 12);
        // Every row carries its own number on its page.
        std::map<std::uint32_t, int> perPage;
        for (const Row &row : rowsOf(pdb, 0)) {
            assert(u16(pdb, row.at + 2) == row.index * 0x20);
            ++perPage[row.page];
        }
        assert(perPage.size() > 10 && "the track table grew by pages");
        for (const std::uint32_t type : {2u, 3u}) {
            for (const Row &row : rowsOf(pdb, type)) {
                assert(u16(pdb, row.at + 2) == (row.index * 0x20 & 0xFFFF));
            }
        }
        const auto after = readLibrary(stick);
        assert(after.size() == 17 + 120);
        for (const auto &[id, text] : before) {
            assert(after.at(id) == text);
        }
        assert(after.at("23").rfind("23|Many 0|Artist 0 \xc3\xa9|Album 0|100.000000|1A|200.000000||"
                                        + pathToUtf8(stick / "Contents" / "Many" / (longName + "0.mp3")),
                                    0)
               == 0);
        assert(after.at("24").rfind("24|Caf\xc3\xa9 1|Artist 1|Album 0|101.000000|2A|201.000000||", 0) == 0);
        assert(after.at("142").rfind("142|Many 119|Artist 119|Album 59|219.000000|12A|319.000000||", 0) == 0);
    }
    std::cout << "case 5 (120 tracks: new pages for every table, all read back, rekordbox's rows untouched) OK\n";

    // 6. An id something still names is not given out again: a track
    //    row marked deleted, a playlist entry of a track that is gone.
    {
        const fs::path other = stickAt("removed");
        PdbRowWriter writer(pathToUtf8(pdbOf(other)));
        assert(writer.removeTrack(15));
        PdbRowWriter::NewTrack t = full;
        assert(writer.addTrack(t) == 17 && "15 and 16 were tracks once");
        assert(writer.commit());
        assert(readLibrary(other).size() == 11);
    }
    {
        const fs::path other = stickAt("listed");
        PdbRowWriter writer(pathToUtf8(pdbOf(other)));
        // Playlist 9 is rekordbox's "T73 Order".
        assert(writer.appendPlaylistEntries(9, {40}) == 1);
        PdbRowWriter::NewTrack t = full;
        assert(writer.addTrack(t) == 41);
    }
    std::cout << "case 6 (the id of a deleted row, and one a playlist entry names, are not reused) OK\n";

    // 7. A row that cannot be placed leaves nothing of its track behind:
    //    the track table's empty candidate is made a page in use, so a
    //    new track page cannot be started, while the artist's row fits.
    {
        const fs::path other = stickAt("full");
        std::string pdb = readFile(pdbOf(other));
        for (std::uint32_t t = 0; t < u32(pdb, 8); ++t) {
            if (u32(pdb, 28 + 16 * t) == 0) {
                pdb[28 + 16 * t + 4] = 2;  // empty_candidate: page 2, a track page
                pdb[28 + 16 * t + 5] = 0;
            }
        }
        std::ofstream(pdbOf(other), std::ios::binary | std::ios::trunc) << pdb;
        PdbRowWriter writer(pathToUtf8(pdbOf(other)));
        int added = 0;
        bool full52 = false;
        for (int i = 0; i < 10 && !full52; ++i) {
            PdbRowWriter::NewTrack t = full;
            t.pathOnStick = "/Contents/Full/" + std::to_string(i) + ".flac";
            t.artist = "Artist of " + std::to_string(i);
            t.analyzePath.clear();
            try {
                (void)writer.addTrack(t);
                ++added;
            } catch (const PdbPageFull &) {
                full52 = true;
            }
        }
        assert(full52 && added >= 1 && added < 10);
        assert(writer.commit());
        const std::string after = readFile(pdbOf(other));
        assert(rowsOf(after, 0).size() == 11u + static_cast<size_t>(added));
        // One artist per track that went in (plus rekordbox's one and the
        // three other names of the first), none for the one that did not.
        assert(rowsOf(after, 2).size() == 1u + 3u + static_cast<size_t>(added));
        assert(readLibrary(other).size() == 11u + static_cast<size_t>(added));
    }
    // 7b. And when nothing else was added, the file is not touched at
    //     all: no page stamped for a row that was taken back. The last
    //     track page is made to look full, so the first track fails.
    {
        const fs::path other = stickAt("untouched");
        std::string pdb = readFile(pdbOf(other));
        for (std::uint32_t t = 0; t < u32(pdb, 8); ++t) {
            if (u32(pdb, 28 + 16 * t) == 0) {
                pdb[28 + 16 * t + 4] = 2;
                pdb[28 + 16 * t + 5] = 0;
            }
        }
        pdb[LenPage * 52 + 28] = 0;  // free_size of page 52: none
        pdb[LenPage * 52 + 29] = 0;
        std::ofstream(pdbOf(other), std::ios::binary | std::ios::trunc) << pdb;
        {
            PdbRowWriter writer(pathToUtf8(pdbOf(other)));
            assert(throws<PdbPageFull>([&] { (void)writer.addTrack(full); }));
            (void)writer.commit();
        }
        assert(readFile(pdbOf(other)) == pdb && "not a byte of a track that did not go in");
    }
    std::cout << "case 7 (a track row that cannot be placed takes its lookup rows back, and leaves the file alone) OK\n";

    // 7c. A table that holds an id at the top of the range gets no row
    //     after it (a deleted row's bytes can be anything: here the
    //     deleted track row of page 52, slot 2, is given 0xFFFFFFFF). And
    //     a page whose sizes do not add up to its rows gets no row.
    {
        const fs::path other = stickAt("wrap");
        std::string pdb = readFile(pdbOf(other));
        const size_t group = LenPage * 52 + LenPage;
        assert(((u16(pdb, group - 4) >> 2) & 1) == 0 && "slot 2 of page 52 is a deleted row");
        const size_t deleted = LenPage * 52 + 40 + u16(pdb, group - 6 - 2 * 2);
        assert(u32(pdb, deleted + 72) == 16);
        for (int i = 0; i < 4; ++i) {
            pdb[deleted + 72 + i] = '\xff';
        }
        std::ofstream(pdbOf(other), std::ios::binary | std::ios::trunc) << pdb;
        PdbRowWriter writer(pathToUtf8(pdbOf(other)));
        assert(throws<std::runtime_error>([&] { (void)writer.addTrack(full); }));
        (void)writer.commit();
        assert(readFile(pdbOf(other)) == pdb);
    }
    {
        const fs::path other = stickAt("sizes");
        std::string pdb = readFile(pdbOf(other));
        pdb[LenPage * 52 + 30] = static_cast<char>(pdb[LenPage * 52 + 30] - 4);  // used_size: four bytes fewer
        std::ofstream(pdbOf(other), std::ios::binary | std::ios::trunc) << pdb;
        PdbRowWriter writer(pathToUtf8(pdbOf(other)));
        PdbRowWriter::NewTrack bare;
        bare.pathOnStick = "/Contents/bare.flac";
        bare.title = "bare";
        bare.date = "2026-11-24";
        assert(throws<PdbPageFull>([&] { (void)writer.addTrack(bare); }));
    }
    std::cout << "case 7c (no id past the top of the range; no row onto a page that does not add up) OK\n";

    // 8. exportExt.pdb has no tracks.
    {
        const fs::path ext = scratch / "exportExt.pdb";
        fs::copy_file(fixture / "stick" / "PIONEER" / "rekordbox" / "exportExt.pdb", ext);
        PdbRowWriter writer(pathToUtf8(ext), PdbRowWriter::Format::ExportExt);
        assert(throws<std::logic_error>([&] { (void)writer.addTrack(full); }));
    }
    std::cout << "case 8 (exportExt.pdb takes no track) OK\n";

    fs::remove_all(scratch);
    std::cout << "pdb_add_track_test: all cases passed\n";
    return 0;
}
