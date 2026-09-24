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

#include "infrastructure/paths/utf8_path.hpp"
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
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        PdbRowWriter::TrackTextOverride text;
        text.title = "Obfuscated Title Much Longer Than Original";  // longer than the 10-byte short_ascii capacity
        text.comment = "Fake";                                      // longer than the 2-char utf16le capacity
        text.filename = "fake.mp3";                                 // exactly fits the 8-byte long_ascii capacity
        text.filePath = "x";                                        // longer than the 0-byte capacity
        bool overwrote = writer.overwriteTrackText(100, text);
        assert(overwrote);
        // Two of the four: the title and the comment. The filename fits
        // exactly and must not be counted, or the number would be
        // "fields written" under another name. Nor the file path, whose
        // capacity is zero: a field with no room held nothing and lost
        // nothing, and an empty comment is the ordinary case on a real
        // track, so counting those would report one cut per track for
        // text that was never there.
        //
        // Nothing here is wrong: preserving the byte span is the
        // contract. The count exists so a finished export can say how
        // many of its fields were too small to carry a whole
        // placeholder.
        assert(writer.truncatedTextFields() == 2);
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

    // And a write that fits counts nothing, or the number above would be
    // "fields written" with a different name on it.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        PdbRowWriter::TrackTextOverride text;
        text.title = "Short";      // inside the 10-byte capacity
        text.comment = "A";        // inside the 2-code-unit capacity
        text.filename = "ok.mp3";  // inside the 8-byte capacity
        text.filePath = "";        // nothing into a zero-byte field is not a cut
        assert(writer.overwriteTrackText(100, text));
        assert(writer.truncatedTextFields() == 0);
        assert(writer.commit());
        std::cout << "case 2b (a write that fits is not counted as cut short) OK\n";
    }

    // overwriteTrackText: text shorter than capacity is space-padded,
    // and round-trips back out with that padding (the writer's contract
    // is "fits exactly," not "trims trailing spaces on read").
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
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
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
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
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
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

    // overwriteTrackExtraText: a slot that is not in use points back into
    // the row's fixed header, and must be left alone. Here ISRC (slot 0)
    // points 8 bytes into track 100's header, where the byte reads like a
    // 17-character string header -- as real header bytes can. Writing
    // "a string" there used to fill 17 header bytes with text and spaces
    // (on a real row: sample rate, file size, artist and album ids) while
    // the page still parsed, so nothing downstream noticed.
    {
        const size_t trackRowStart = static_cast<size_t>(LenPage) * 1 + 40;
        const size_t trackFixedSize = 136;
        std::string withStraySlot = pristine;
        writeU16LE(withStraySlot, trackRowStart + 94 + 0 * 2, 8);          // ISRC slot -> header byte 8
        withStraySlot[trackRowStart + 8] = static_cast<char>(0x25);       // short ASCII, 17 characters
        writeFile(pdbPath, withStraySlot);

        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        PdbRowWriter::TrackExtraTextOverride extra;
        extra.isrc = "ISRC-SCRUBBED";
        extra.texter = "Texter";
        extra.message = "Message";
        extra.mixName = "Mix";
        assert(writer.overwriteTrackExtraText(100, extra));
        assert(writer.commit());

        std::string after;
        {
            std::ifstream in(pdbPath, std::ios::binary);
            std::ostringstream oss;
            oss << in.rdbuf();
            after = oss.str();
        }
        assert(after.size() == withStraySlot.size());
        // Track 100's whole fixed header, string offset table included.
        assert(after.compare(trackRowStart, trackFixedSize, withStraySlot, trackRowStart, trackFixedSize) == 0);
        auto texts = readTrackTexts(pdbPath, 100);
        assert(texts.title == "Real Title");
        assert(texts.filename == "real.mp3");
        std::cout << "case 6 (overwriteTrackExtraText: a slot pointing into the header is left alone) OK\n";
    }

    // ---- Non-ASCII must be refused, not transliterated ----------------
    //
    // RED ON PURPOSE until the guard lands. This case is written against
    // the behaviour the writer SHOULD have; on master it fails, and the
    // failure is the bug.
    //
    // fitAsciiToCapacity() says in its own comment that "anonymized
    // placeholder text is always plain ASCII, so byte-level truncation/
    // padding never splits a multi-byte character". That is true today
    // and nothing enforces it. Two things go wrong the moment it stops
    // being true, and neither fails anywhere:
    //
    //   - substr(0, capacityBytes) is a byte-level truncation, which is
    //     exactly what splits a multi-byte character.
    //   - the UTF-16 branch writes each BYTE as a code unit with a zero
    //     high byte. So "é" (C3 A9) is written as "Ã©": two valid UTF-16
    //     units, in a field that reparses cleanly, with every length and
    //     checksum correct.
    //
    // The comment field in this fixture is device_sql_long_utf16le, so
    // it is that branch. "Cé" is two characters and fits the two-unit
    // capacity, so this is about the encoding rather than about
    // truncation.
    //
    // Refusing is the fix, not re-encoding: the field has a fixed byte
    // capacity, so a correct UTF-16 write still has to decide what to
    // drop, and that decision belongs to the caller that knows what the
    // text is. A writer that silently transliterates is how a stick ends
    // up looking right and not being right, which this file has already
    // produced once.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        PdbRowWriter::TrackTextOverride text;
        text.comment = "C\xC3\xA9";  // "Cé", two characters, three bytes
        const bool overwrote = writer.overwriteTrackText(100, text);
        assert(!overwrote && "non-ASCII must be refused rather than written as Latin-1");
        assert(!writer.commit() || readTrackTexts(pdbPath, 100).comment == "Hi");
        assert(readTrackTexts(pdbPath, 100).comment == "Hi" && "and the row is left exactly as it was");
        std::cout << "case 7 (non-ASCII is refused, not transliterated into the UTF-16 field) OK\n";
    }

    // The other half of the rule, and the one that is easy to lose:
    // refuse what the field cannot represent, and NOTHING else. ASCII
    // arriving through a UTF-16 field is the ordinary case -- it is what
    // every anonymized placeholder is -- and must stay fine.
    //
    // docs/write-path-rules.md records a refusal that was itself the
    // bug, failing 635 of 1118 tracks on an everyday situation. This is
    // the case that stops this guard becoming the next one.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        PdbRowWriter::TrackTextOverride text;
        text.comment = "Ok";  // ASCII, into the utf16le field
        assert(writer.overwriteTrackText(100, text));
        assert(writer.commit());
        assert(readTrackTexts(pdbPath, 100).comment == "Ok");
        std::cout << "case 8 (ASCII through a UTF-16 field is still written) OK\n";
    }

    // A refusal must leave the row exactly as it was, not half of it.
    // overwriteTrackText() writes four fields, and its return says only
    // whether the ROW was found -- so a call that wrote the title and
    // then refused the comment would report the same thing as a clean
    // one, with the row changed.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        PdbRowWriter::TrackTextOverride text;
        text.title = "Safe Title";          // fine on its own
        text.comment = "C\xC3\xA9";          // and this one is not
        assert(!writer.overwriteTrackText(100, text));
        writer.commit();
        const auto after = readTrackTexts(pdbPath, 100);
        assert(after.title == "Real Title" && "the field that WOULD have been written is untouched");
        assert(after.comment == "Hi");
        std::cout << "case 9 (one unrepresentable field refuses the whole row, nothing half-written) OK\n";
    }

    // The same on the other tables, which have their own entry points.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        assert(!writer.overwriteArtistName(5, "Caf\xC3\xA9"));
        assert(!writer.overwritePlaylistName(9, "Caf\xC3\xA9"));
        // Still accepts what it can represent, on the same writer.
        assert(writer.overwriteArtistName(5, "Artist Z"));
        assert(writer.commit());
        assert(readArtistName(pdbPath, 5) == "Artist Z");
        assert(readPlaylistName(pdbPath, 9) == "Real Playlist" && "the refused one kept its name");
        std::cout << "case 10 (artist and playlist names refuse non-ASCII and still take ASCII) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
