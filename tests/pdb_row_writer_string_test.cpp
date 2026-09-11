// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/little_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;
using Pdb = rekordbox_pdb_t;

namespace
{

constexpr uint32_t LenPage = 512;

// Appends a device_sql_string's on-disk bytes for `text`, in the given
// encoding, matching specs/rekordbox_pdb.ksy's device_sql_string/
// device_sql_short_ascii/device_sql_long_ascii/device_sql_long_utf16le
// exactly (this is the fixture-building inverse of
// pdb_row_writer.cpp's own readDeviceSqlStringSpan()/
// overwriteDeviceSqlStringInPlace(), built independently here so the
// test doesn't just check the writer against itself).
enum class StringKind { ShortAscii, LongAscii, LongUtf16Le };

void appendDeviceSqlString(std::string &buf, const std::string &text, StringKind kind)
{
    if (kind == StringKind::ShortAscii) {
        uint8_t lengthAndKind = static_cast<uint8_t>(((text.size() + 1) << 1) | 1);
        buf.push_back(static_cast<char>(lengthAndKind));
        buf += text;
    } else if (kind == StringKind::LongAscii) {
        uint16_t length = static_cast<uint16_t>(4 + text.size());
        buf.push_back(static_cast<char>(0x40));
        buf.push_back(static_cast<char>(length & 0xFF));
        buf.push_back(static_cast<char>((length >> 8) & 0xFF));
        buf.push_back(static_cast<char>(0x00));
        buf += text;
    } else {
        uint16_t length = static_cast<uint16_t>(4 + text.size() * 2);
        buf.push_back(static_cast<char>(0x90));
        buf.push_back(static_cast<char>(length & 0xFF));
        buf.push_back(static_cast<char>((length >> 8) & 0xFF));
        buf.push_back(static_cast<char>(0x00));
        for (char c : text) {
            buf.push_back(c);
            buf.push_back('\0');
        }
    }
}

// A minimal, structurally real export.pdb with one row each in the
// tracks, artists, and playlist_tree tables -- unlike
// pdb_row_writer_test.cpp's fixture, every row here carries real
// device_sql_string data (one of each on-disk encoding), since that's
// what this file's tests actually exercise. Layout mirrors that file's
// own derivation from specs/rekordbox_pdb.ksy; see pdb_row_writer.cpp's
// TrackOfsStringsOffset/ArtistNameOffsetNear/PlaylistTreeNameOffset
// comments for the byte-offset math this fixture is built against.
std::string buildSyntheticPdb()
{
    std::string buf(static_cast<size_t>(LenPage) * 4, '\0');

    writeU32LE(buf, 4, LenPage);
    writeU32LE(buf, 8, 3);   // num_tables
    writeU32LE(buf, 20, 5);  // sequence
    writeU32LE(buf, 28, static_cast<uint32_t>(Pdb::PAGE_TYPE_TRACKS));
    writeU32LE(buf, 36, 1);
    writeU32LE(buf, 40, 1);
    writeU32LE(buf, 44, static_cast<uint32_t>(Pdb::PAGE_TYPE_ARTISTS));
    writeU32LE(buf, 52, 2);
    writeU32LE(buf, 56, 2);
    writeU32LE(buf, 60, static_cast<uint32_t>(Pdb::PAGE_TYPE_PLAYLIST_TREE));
    writeU32LE(buf, 68, 3);
    writeU32LE(buf, 72, 3);

    auto packRowCounts = [](std::string &b, size_t pageStart, uint16_t numRowOffsets, uint16_t numRows) {
        uint32_t packed = (static_cast<uint32_t>(numRowOffsets) & 0x1FFF) |
                           ((static_cast<uint32_t>(numRows) & 0x7FF) << 13);
        b[pageStart + 24] = static_cast<char>(packed & 0xFF);
        b[pageStart + 25] = static_cast<char>((packed >> 8) & 0xFF);
        b[pageStart + 26] = static_cast<char>((packed >> 16) & 0xFF);
    };
    auto writePageHeader = [&](size_t pageStart, uint32_t pageIndex, Pdb::page_type_t type) {
        writeU32LE(buf, pageStart + 4, pageIndex);
        writeU32LE(buf, pageStart + 8, static_cast<uint32_t>(type));
        writeU32LE(buf, pageStart + 12, pageIndex);
        writeU32LE(buf, pageStart + 16, 5);
        buf[pageStart + 27] = static_cast<char>(0x24);
        packRowCounts(buf, pageStart, 1, 1);
    };
    auto setRowPresent = [&](size_t pageStart, uint16_t presentMask) {
        writeU16LE(buf, pageStart + LenPage - 4, presentMask);
    };
    auto setRowOfs = [&](size_t pageStart, uint16_t ofsFromHeap) { writeU16LE(buf, pageStart + LenPage - 6, ofsFromHeap); };

    constexpr size_t HeapStart = 40;

    // --- Page 1: tracks, one row (id=100) ---
    constexpr size_t TrackFixedSize = 136;  // through the end of ofs_strings[20], see pdb_row_writer.cpp
    constexpr size_t TrackIdFieldOffset = 72;
    constexpr size_t TrackOfsStringsOffset = 94;
    size_t page1 = LenPage * 1;
    writePageHeader(page1, 1, Pdb::PAGE_TYPE_TRACKS);
    setRowPresent(page1, 0b1);
    setRowOfs(page1, 0);

    size_t trackRowStart = page1 + HeapStart;
    writeU32LE(buf, trackRowStart + TrackIdFieldOffset, 100);

    std::string titleBytes, commentBytes, filenameBytes, filePathBytes;
    appendDeviceSqlString(titleBytes, "Real Title", StringKind::ShortAscii);
    appendDeviceSqlString(commentBytes, "Hi", StringKind::LongUtf16Le);
    appendDeviceSqlString(filenameBytes, "real.mp3", StringKind::LongAscii);
    appendDeviceSqlString(filePathBytes, "", StringKind::ShortAscii);  // zero text capacity, on purpose

    size_t ofsTitle = TrackFixedSize;
    size_t ofsComment = ofsTitle + titleBytes.size();
    size_t ofsFilename = ofsComment + commentBytes.size();
    size_t ofsFilePath = ofsFilename + filenameBytes.size();

    auto writeOfsString = [&](int index, uint16_t offset) {
        writeU16LE(buf, trackRowStart + TrackOfsStringsOffset + static_cast<size_t>(index) * 2, offset);
    };
    writeOfsString(17, static_cast<uint16_t>(ofsTitle));
    writeOfsString(16, static_cast<uint16_t>(ofsComment));
    writeOfsString(19, static_cast<uint16_t>(ofsFilename));
    writeOfsString(20, static_cast<uint16_t>(ofsFilePath));

    buf.replace(trackRowStart + ofsTitle, titleBytes.size(), titleBytes);
    buf.replace(trackRowStart + ofsComment, commentBytes.size(), commentBytes);
    buf.replace(trackRowStart + ofsFilename, filenameBytes.size(), filenameBytes);
    buf.replace(trackRowStart + ofsFilePath, filePathBytes.size(), filePathBytes);

    // --- Page 2: artists, one row (id=5), near-offset name form ---
    size_t page2 = LenPage * 2;
    writePageHeader(page2, 2, Pdb::PAGE_TYPE_ARTISTS);
    setRowPresent(page2, 0b1);
    setRowOfs(page2, 0);

    size_t artistRowStart = page2 + HeapStart;
    writeU16LE(buf, artistRowStart + 0, 0x0060);  // subtype, far-name bit (0x04) not set
    writeU32LE(buf, artistRowStart + 4, 5);        // id
    buf[artistRowStart + 8] = static_cast<char>(0x03);
    buf[artistRowStart + 9] = static_cast<char>(10);  // ofs_name_near

    std::string artistNameBytes;
    appendDeviceSqlString(artistNameBytes, "Real Artist", StringKind::ShortAscii);
    buf.replace(artistRowStart + 10, artistNameBytes.size(), artistNameBytes);

    // --- Page 3: playlist_tree, one row (id=9) ---
    size_t page3 = LenPage * 3;
    writePageHeader(page3, 3, Pdb::PAGE_TYPE_PLAYLIST_TREE);
    setRowPresent(page3, 0b1);
    setRowOfs(page3, 0);

    size_t playlistRowStart = page3 + HeapStart;
    writeU32LE(buf, playlistRowStart + 12, 9);  // id (parent_id=0, unnamed=0, sort_order=0 before it)

    std::string playlistNameBytes;
    appendDeviceSqlString(playlistNameBytes, "Real Playlist", StringKind::ShortAscii);
    buf.replace(playlistRowStart + 20, playlistNameBytes.size(), playlistNameBytes);

    return buf;
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readTrackTitle(const fs::path &path, uint32_t trackId)
{
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        std::string result;
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *t = dynamic_cast<Pdb::track_row_t *>(row->body());
                    if (t && t->id() == trackId) {
                        result = sqlText(t->title());
                    }
                }
            }
        });
        return result;
    }
    return {};
}

struct TrackTexts
{
    std::string title, comment, filename, filePath;
};

// sqlText() trims the format's field padding, which is right for every
// consumer -- the padding is filler, not data. The writer's contract is
// the opposite: a shorter string must be padded out so the field keeps
// its capacity. So this reads the field WITHOUT trimming, which is the
// only way to assert padding once the normal reader stops showing it.
std::string rawText(Pdb::device_sql_string_t *s)
{
    auto *body = s->body();
    if (auto *a = dynamic_cast<Pdb::device_sql_short_ascii_t *>(body)) {
        return a->text();
    }
    if (auto *a = dynamic_cast<Pdb::device_sql_long_ascii_t *>(body)) {
        return a->text();
    }
    if (auto *a = dynamic_cast<Pdb::device_sql_long_utf16le_t *>(body)) {
        return a->text();
    }
    return "";
}

TrackTexts readRawTrackTexts(const fs::path &path, uint32_t trackId);

TrackTexts readTrackTexts(const fs::path &path, uint32_t trackId)
{
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    TrackTexts out;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *t = dynamic_cast<Pdb::track_row_t *>(row->body());
                    if (t && t->id() == trackId) {
                        out.title = sqlText(t->title());
                        out.comment = sqlText(t->comment());
                        out.filename = sqlText(t->filename());
                        out.filePath = sqlText(t->file_path());
                    }
                }
            }
        });
    }
    return out;
}

TrackTexts readRawTrackTexts(const fs::path &path, uint32_t trackId)
{
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    TrackTexts out;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *t = dynamic_cast<Pdb::track_row_t *>(row->body());
                    if (t && t->id() == trackId) {
                        out.title = rawText(t->title());
                        out.comment = rawText(t->comment());
                        out.filename = rawText(t->filename());
                        out.filePath = rawText(t->file_path());
                    }
                }
            }
        });
    }
    return out;
}

std::string readArtistName(const fs::path &path, uint32_t artistId)
{
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    std::string result;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_ARTISTS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *a = dynamic_cast<Pdb::artist_row_t *>(row->body());
                    if (a && a->id() == artistId) {
                        result = sqlText(a->name());
                    }
                }
            }
        });
    }
    return result;
}

std::string readRawArtistName(const fs::path &path, uint32_t artistId)
{
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    std::string result;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_ARTISTS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *a = dynamic_cast<Pdb::artist_row_t *>(row->body());
                    if (a && a->id() == artistId) {
                        result = rawText(a->name());
                    }
                }
            }
        });
    }
    return result;
}

std::string readPlaylistName(const fs::path &path, uint32_t playlistId)
{
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    std::string result;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_PLAYLIST_TREE) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *p = dynamic_cast<Pdb::playlist_tree_row_t *>(row->body());
                    if (p && p->id() == playlistId) {
                        result = sqlText(p->name());
                    }
                }
            }
        });
    }
    return result;
}

std::string readRawPlaylistName(const fs::path &path, uint32_t playlistId)
{
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    std::string result;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_PLAYLIST_TREE) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *p = dynamic_cast<Pdb::playlist_tree_row_t *>(row->body());
                    if (p && p->id() == playlistId) {
                        result = rawText(p->name());
                    }
                }
            }
        });
    }
    return result;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_pdb_row_writer_string_test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path pdbPath = root / "export.pdb";

    std::string pristine = buildSyntheticPdb();

    // Self-check: the fixture's real string data reads back as designed
    // (one row of each encoding) before any mutation is tested.
    {
        writeFile(pdbPath, pristine);
        auto texts = readTrackTexts(pdbPath, 100);
        assert(texts.title == "Real Title");
        assert(texts.comment == "Hi");
        assert(texts.filename == "real.mp3");
        assert(texts.filePath.empty());
        assert(readArtistName(pdbPath, 5) == "Real Artist");
        assert(readPlaylistName(pdbPath, 9) == "Real Playlist");
        std::cout << "case 1 (synthetic fixture's real string data reads back as designed) OK\n";
    }

    // overwriteTrackText: shorter text is truncated to fit, since the
    // field's own length/kind header (and therefore its on-disk byte
    // span) is never touched.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        PdbRowWriter::TrackTextOverride text;
        text.title = "Obfuscated Title Much Longer Than Original";  // longer than the 10-byte short_ascii capacity
        text.comment = "Fake";                                      // longer than the 2-char utf16le capacity
        text.filename = "fake.mp3";                                 // exactly fits the 8-byte long_ascii capacity
        text.filePath = "x";                                        // longer than the 0-byte capacity
        bool overwrote = writer.overwriteTrackText(100, text);
        assert(overwrote);
        bool committed = writer.commit();
        assert(committed);

        auto texts = readTrackTexts(pdbPath, 100);
        assert(texts.title == "Obfuscated");  // truncated to the original 10-byte capacity
        assert(texts.comment == "Fa");        // truncated to the original 2-code-unit capacity
        assert(texts.filename == "fake.mp3");
        assert(texts.filePath.empty());  // zero capacity -> stays empty no matter what's passed

        std::string after;
        {
            std::ifstream in(pdbPath, std::ios::binary);
            std::ostringstream oss;
            oss << in.rdbuf();
            after = oss.str();
        }
        assert(after.size() == pristine.size());  // never resized/reflowed
        std::cout << "case 2 (overwriteTrackText: truncates to each field's existing byte capacity) OK\n";
    }

    // overwriteTrackText: text shorter than capacity is space-padded,
    // and round-trips back out with that padding (the writer's contract
    // is "fits exactly," not "trims trailing spaces on read").
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        PdbRowWriter::TrackTextOverride text;
        text.title = "Hi";
        text.comment = "H";
        text.filename = "x";
        text.filePath = "";
        bool overwrote = writer.overwriteTrackText(100, text);
        assert(overwrote);
        bool committed = writer.commit();
        assert(committed);

        auto texts = readRawTrackTexts(pdbPath, 100);
        // Original field capacities: title=10 (len("Real Title")),
        // comment=2 (len("Hi")), filename=8 (len("real.mp3")) -- padding
        // computed rather than hand-counted in a literal, to avoid an
        // off-by-one in the test itself.
        assert(texts.title == "Hi" + std::string(10 - 2, ' '));
        assert(texts.comment == "H" + std::string(2 - 1, ' '));
        assert(texts.filename == "x" + std::string(8 - 1, ' '));
        assert(texts.filePath.empty());
        std::cout << "case 3 (overwriteTrackText: shorter text is space-padded to the existing capacity) OK\n";
    }

    // overwriteTrackText: unknown track id is a no-op, never marks
    // anything dirty.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        PdbRowWriter::TrackTextOverride text;
        text.title = "x";
        assert(!writer.overwriteTrackText(999999, text));
        assert(!writer.commit());
        std::cout << "case 4 (overwriteTrackText: unknown track id is a no-op) OK\n";
    }

    // overwriteArtistName / overwritePlaylistName: same fit-to-capacity
    // behavior, on their own tables, leaving the track row untouched.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        bool overwroteArtist = writer.overwriteArtistName(5, "Artist Z");
        assert(overwroteArtist);
        bool overwrotePlaylist = writer.overwritePlaylistName(9, "Set 1");
        assert(overwrotePlaylist);
        assert(!writer.overwriteArtistName(999999, "nope"));
        assert(!writer.overwritePlaylistName(999999, "nope"));
        bool committed = writer.commit();
        assert(committed);

        // Original capacities: artist name=11 (len("Real Artist")),
        // playlist name=13 (len("Real Playlist")).
        assert(readRawArtistName(pdbPath, 5) == "Artist Z" + std::string(11 - 8, ' '));
        assert(readRawPlaylistName(pdbPath, 9) == "Set 1" + std::string(13 - 5, ' '));
        assert(readTrackTitle(pdbPath, 100) == "Real Title");     // track row untouched
        std::cout << "case 5 (overwriteArtistName/overwritePlaylistName: fit to capacity, other tables untouched) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
