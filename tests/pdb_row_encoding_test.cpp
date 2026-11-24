// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The export.pdb row encoders (pdb_row_encoding.hpp) against rows
// rekordbox itself wrote: every present row of the track table and its
// lookup tables in rekordbox's own exports is read into fields by a
// decoder of this test's own (bytes and offsets, nothing shared with the
// encoder or the app's parser), encoded again, and must be the bytes
// that are on the page.
//
// The exports: the T73 fixture (rekordbox 7.2.18, two exports onto one
// stick, FLAC, AIFF and MP3) and #62's playlist reference (the same
// rekordbox, tone tracks with genres, labels and artwork).

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_row_encoding.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::rekordbox;

namespace
{

std::uint32_t u16(const std::string &b, size_t at)
{
    return static_cast<unsigned char>(b[at]) | (static_cast<unsigned char>(b[at + 1]) << 8);
}

std::uint32_t u32(const std::string &b, size_t at)
{
    return u16(b, at) | (u16(b, at + 2) << 16);
}

std::string utf8Of(std::uint32_t cp)
{
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

// A DeviceSQL string at `at`: its text as UTF-8 and how many bytes it
// takes. `kind` is 's' short, 'l' long ASCII, 'u' UTF-16.
struct Text
{
    std::string text;
    size_t bytes = 0;
    char kind = '?';
};

Text textAt(const std::string &b, size_t at)
{
    Text t;
    const auto head = static_cast<unsigned char>(b[at]);
    if (head & 1) {
        t.kind = 's';
        t.bytes = head >> 1;
        t.text = b.substr(at + 1, t.bytes - 1);
        return t;
    }
    t.bytes = u16(b, at + 1);
    assert((head == 0x40 || head == 0x90) && b[at + 3] == '\0' && t.bytes >= 4);
    if (head == 0x40) {
        t.kind = 'l';
        t.text = b.substr(at + 4, t.bytes - 4);
        return t;
    }
    t.kind = 'u';
    for (size_t i = at + 4; i + 1 < at + t.bytes; i += 2) {
        std::uint32_t cp = u16(b, i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < at + t.bytes) {
            const std::uint32_t low = u16(b, i + 2);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            i += 2;
        }
        t.text += utf8Of(cp);
    }
    return t;
}

// One present row: where its bytes start in the file, and its number on
// its page.
struct Row
{
    size_t at = 0;
    std::uint32_t index = 0;
};

// Every present row of the table of type `type`, page by page from the
// table's first to its last.
std::vector<Row> rowsOf(const std::string &pdb, std::uint32_t type)
{
    std::vector<Row> rows;
    const size_t lenPage = u32(pdb, 4);
    const std::uint32_t tables = u32(pdb, 8);
    for (std::uint32_t t = 0; t < tables; ++t) {
        const size_t entry = 28 + 16 * t;
        if (u32(pdb, entry) != type) {
            continue;
        }
        const std::uint32_t last = u32(pdb, entry + 12);
        for (std::uint32_t page = u32(pdb, entry + 8);; page = u32(pdb, lenPage * page + 12)) {
            const size_t base = lenPage * page;
            assert(base + lenPage <= pdb.size() && u32(pdb, base + 8) == type);
            const bool dataPage = (static_cast<unsigned char>(pdb[base + 27]) & 0x40) == 0;
            const std::uint32_t offsets = u32(pdb, base + 24) & 0x1FFF;
            for (std::uint32_t r = 0; dataPage && r < offsets; ++r) {
                const size_t group = base + lenPage - (r / 16) * 0x24;
                if ((u16(pdb, group - 4) >> (r % 16)) & 1) {
                    rows.push_back({base + 40 + u16(pdb, group - 6 - 2 * (r % 16)), r});
                }
            }
            if (page == last) {
                break;
            }
        }
    }
    return rows;
}

std::string hex(const std::string &bytes)
{
    static constexpr char Digits[] = "0123456789abcdef";
    std::string out;
    for (const char c : bytes) {
        out += Digits[static_cast<unsigned char>(c) >> 4];
        out += Digits[static_cast<unsigned char>(c) & 0xF];
        out += ' ';
    }
    return out;
}

// How many bytes rekordbox has where the encoder puts a zero: the bytes
// before a string that starts on a multiple of four are whatever its
// buffer held ("Wa" in one row of a real library), zero in every row of
// the fixtures.
int strayPadding = 0;

// Whether `encoded` is what the file holds at `at`, zeros of the encoder
// aside (counted in strayPadding); says so when not.
bool same(const std::string &pdb, size_t at, const std::string &encoded, const char *what, std::uint32_t id)
{
    bool equal = at + encoded.size() <= pdb.size();
    for (size_t i = 0; equal && i < encoded.size(); ++i) {
        if (pdb[at + i] == encoded[i]) {
            continue;
        }
        if (encoded[i] == '\0') {
            ++strayPadding;
        } else {
            equal = false;
        }
    }
    if (equal) {
        return true;
    }
    std::cerr << what << " id " << id << " differs\n  rekordbox: " << hex(pdb.substr(at, encoded.size()))
              << "\n  encoded:   " << hex(encoded) << "\n";
    return false;
}

struct Counts
{
    int tracks = 0;
    int artists = 0;
    int albums = 0;
    int genres = 0;
    int labels = 0;
    int keys = 0;
    int artwork = 0;
    std::map<char, int> stringKinds;
    int longNames = 0;
};

Counts check(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string pdb = buffer.str();
    assert(pdb.size() > 4096 && u32(pdb, 4) == 4096);
    Counts counts;

    for (const Row &row : rowsOf(pdb, 0)) {
        const size_t at = row.at;
        assert(u16(pdb, at) == 0x24);
        PdbTrackRowFields f;
        assert(u16(pdb, at + 2) == row.index * 0x20 && "index_shift is the row's number on its page");
        f.rowIndex = row.index;
        f.bitmask = u32(pdb, at + 4);
        f.sampleRate = u32(pdb, at + 8);
        f.composerId = u32(pdb, at + 12);
        f.fileSize = u32(pdb, at + 16);
        f.masterContentId = u32(pdb, at + 20);
        f.masterDbId = u32(pdb, at + 24);
        f.artworkId = u32(pdb, at + 28);
        f.keyId = u32(pdb, at + 32);
        f.originalArtistId = u32(pdb, at + 36);
        f.labelId = u32(pdb, at + 40);
        f.remixerId = u32(pdb, at + 44);
        f.bitrate = u32(pdb, at + 48);
        f.trackNumber = u32(pdb, at + 52);
        f.tempo = u32(pdb, at + 56);
        f.genreId = u32(pdb, at + 60);
        f.albumId = u32(pdb, at + 64);
        f.artistId = u32(pdb, at + 68);
        f.id = u32(pdb, at + 72);
        f.discNumber = static_cast<std::uint16_t>(u16(pdb, at + 76));
        f.playCount = static_cast<std::uint16_t>(u16(pdb, at + 78));
        f.year = static_cast<std::uint16_t>(u16(pdb, at + 80));
        f.sampleDepth = static_cast<std::uint16_t>(u16(pdb, at + 82));
        f.duration = static_cast<std::uint16_t>(u16(pdb, at + 84));
        f.unknown86 = static_cast<std::uint16_t>(u16(pdb, at + 86));
        f.colorId = static_cast<std::uint8_t>(pdb[at + 88]);
        f.rating = static_cast<std::uint8_t>(pdb[at + 89]);
        f.fileType = static_cast<std::uint16_t>(u16(pdb, at + 90));
        f.unknown92 = static_cast<std::uint16_t>(u16(pdb, at + 92));
        std::string *const strings[21] = {
            &f.isrc,        &f.texter,      &f.informationUpdateCount, &f.analysisDataUpdateCount, &f.cueUpdateCount,
            &f.message,     &f.kuvoPublic,  &f.autoloadHotcues,        &f.unknownString8,          &f.unknownString9,
            &f.dateAdded,   &f.releaseDate, &f.mixName,                &f.unknownString13,         &f.analyzePath,
            &f.analyzeDate, &f.comment,     &f.title,                  &f.unknownString18,         &f.filename,
            &f.filePath,
        };
        for (int i = 0; i < 21; ++i) {
            const Text text = textAt(pdb, at + u16(pdb, at + 94 + 2 * i));
            *strings[i] = text.text;
            ++counts.stringKinds[text.kind];
        }
        assert(same(pdb, at, encodePdbTrackRow(f), "track", f.id));
        // What an imported row is given for the fields nobody could name:
        // the one value every row of rekordbox's has.
        assert(f.unknown86 == PdbTrackRowFields().unknown86 && f.unknown92 == PdbTrackRowFields().unknown92);
        assert(f.kuvoPublic == "ON" && f.autoloadHotcues == "ON");
        assert((f.bitmask & 0xFFFF) == 0x0700 && "the low half of the bitmask is the same on every row");
        ++counts.tracks;
    }
    for (const Row &row : rowsOf(pdb, 2)) {
        const size_t at = row.at;
        assert(u16(pdb, at) == 0x60 && pdb[at + 8] == 3);
        const Text name = textAt(pdb, at + static_cast<unsigned char>(pdb[at + 9]));
        assert(same(pdb, at, encodePdbArtistRow(row.index, u32(pdb, at + 4), name.text), "artist", u32(pdb, at + 4)));
        counts.longNames += name.kind != 's';
        ++counts.artists;
    }
    for (const Row &row : rowsOf(pdb, 3)) {
        const size_t at = row.at;
        assert(u16(pdb, at) == 0x80 && pdb[at + 20] == 3);
        const Text name = textAt(pdb, at + static_cast<unsigned char>(pdb[at + 21]));
        assert(same(pdb, at, encodePdbAlbumRow(row.index, u32(pdb, at + 12), u32(pdb, at + 8), name.text), "album",
                    u32(pdb, at + 12)));
        counts.longNames += name.kind != 's';
        ++counts.albums;
    }
    const auto nameRows = [&](std::uint32_t type, const char *what, int &count) {
        for (const Row &row : rowsOf(pdb, type)) {
            assert(same(pdb, row.at, encodePdbNameRow(u32(pdb, row.at), textAt(pdb, row.at + 4).text), what, u32(pdb, row.at)));
            ++count;
        }
    };
    nameRows(1, "genre", counts.genres);
    nameRows(4, "label", counts.labels);
    nameRows(13, "artwork", counts.artwork);
    for (const Row &row : rowsOf(pdb, 5)) {
        assert(u32(pdb, row.at) == u32(pdb, row.at + 4));
        assert(same(pdb, row.at, encodePdbKeyRow(u32(pdb, row.at), textAt(pdb, row.at + 8).text), "key", u32(pdb, row.at)));
        ++counts.keys;
    }
    std::cout << seabass::pathToUtf8(file.filename()) << ": " << counts.tracks << " tracks, " << counts.artists << " artists, "
              << counts.albums << " albums, " << counts.genres << " genres, " << counts.labels << " labels, " << counts.keys
              << " keys, " << counts.artwork << " artwork rows, strings short/long/UTF-16 " << counts.stringKinds['s'] << "/"
              << counts.stringKinds['l'] << "/" << counts.stringKinds['u'] << ", " << counts.longNames
              << " artist or album names not short\n";
    return counts;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: pdb_row_encoding_test <tests/fixtures> [more export.pdb files]\n";
        return 2;
    }
    const fs::path fixtures = seabass::pathFromUtf8(argv[1]);
    const Counts t73 = check(fixtures / "testshark_t73" / "stick" / "PIONEER" / "rekordbox" / "export.pdb");
    assert(t73.tracks == 11 && t73.artists == 1 && t73.albums == 2 && t73.keys == 2);
    const Counts tones = check(fixtures / "pdb_playlist_tree" / "0-base.pdb");
    assert(tones.tracks > 0 && tones.artists > 0 && tones.albums > 0);
    // Not vacuous: between them the two exports hold every kind of row.
    assert(t73.genres + tones.genres > 0 && t73.labels + tones.labels > 0 && t73.artwork + tones.artwork > 0);
    assert(strayPadding == 0 && "the fixtures' rows are the encoder's bytes, every one");
    // Not held against the two anonymized libraries of the fixtures:
    // Seabass's anonymizer writes its placeholders into the string a row
    // already has, so they hold plain ASCII in UTF-16 strings, which
    // rekordbox writes as short ones. The UTF-16 artist, album, file and
    // path strings they would add are covered by a real library run by
    // hand (1161 tracks, 833 artists, 1044 albums: every row the same).
    // Any other export given on the command line, for a developer with a
    // real library at hand (none is in the repository).
    for (int i = 2; i < argc; ++i) {
        (void)check(seabass::pathFromUtf8(argv[i]));
    }
    if (strayPadding > 0) {
        std::cout << strayPadding << " padding bytes of rekordbox's are not zero in the files given\n";
    }

    // The strings as rekordbox spells them, at their edges.
    assert(encodeDeviceSqlString("") == std::string("\x03"));
    assert(encodeDeviceSqlString(std::string(126, 'a')).size() == 127 && "the longest short string");
    assert(encodeDeviceSqlString(std::string(127, 'a')).substr(0, 4) == std::string("\x40\x83\x00\x00", 4));
    assert(encodeDeviceSqlString("\xc3\xa9") == std::string("\x90\x06\x00\x00\xe9\x00", 6));
    // Past the two-byte plane: a surrogate pair (U+1F3B5).
    assert(encodeDeviceSqlString("\xf0\x9f\x8e\xb5") == std::string("\x90\x08\x00\x00\x3c\xd8\xb5\xdf", 8));
    // The highest plane: the lead byte's own bits count (U+100000).
    assert(encodeDeviceSqlString("\xf4\x80\x80\x80") == std::string("\x90\x08\x00\x00\xc0\xdb\x00\xdc", 8));

    // Every number of a track row at its own place: a row whose fields
    // all differ, read back offset by offset (rekordbox's rows have 0 in
    // several of them, where two swapped fields would look the same).
    {
        PdbTrackRowFields f;
        f.rowIndex = 5;
        f.bitmask = 101;
        f.sampleRate = 102;
        f.composerId = 103;
        f.fileSize = 104;
        f.masterContentId = 105;
        f.masterDbId = 106;
        f.artworkId = 107;
        f.keyId = 108;
        f.originalArtistId = 109;
        f.labelId = 110;
        f.remixerId = 111;
        f.bitrate = 112;
        f.trackNumber = 113;
        f.tempo = 114;
        f.genreId = 115;
        f.albumId = 116;
        f.artistId = 117;
        f.id = 118;
        f.discNumber = 119;
        f.playCount = 120;
        f.year = 121;
        f.sampleDepth = 122;
        f.duration = 123;
        f.unknown86 = 124;
        f.colorId = 125;
        f.rating = 126;
        f.fileType = 127;
        f.unknown92 = 128;
        const std::string row = encodePdbTrackRow(f);
        assert(u16(row, 0) == 0x24 && u16(row, 2) == 5 * 0x20);
        std::uint32_t expected = 101;
        for (size_t at = 4; at < 76; at += 4) {
            assert(u32(row, at) == expected++);
        }
        for (size_t at = 76; at < 88; at += 2) {
            assert(u16(row, at) == expected++);
        }
        assert(static_cast<unsigned char>(row[88]) == 125 && static_cast<unsigned char>(row[89]) == 126);
        assert(u16(row, 90) == 127 && u16(row, 92) == 128);
        assert(u16(row, 94) == 136 && "the strings start after their 21 offsets");
    }

    // A name that is not a short string sits two zero bytes further.
    assert(encodePdbArtistRow(3, 7, "\xc3\xa9")
           == std::string("\x60\x00\x60\x00\x07\x00\x00\x00\x03\x0c\x00\x00\x90\x06\x00\x00\xe9\x00", 18));
    assert(encodePdbAlbumRow(1, 9, 4, "\xc3\xa9")
           == std::string("\x80\x00\x20\x00\x00\x00\x00\x00\x04\x00\x00\x00\x09\x00\x00\x00\x00\x00\x00\x00\x03\x18\x00\x00"
                          "\x90\x06\x00\x00\xe9\x00",
                          30));
    std::cout << "pdb_row_encoding_test: all cases passed\n";
    return 0;
}
