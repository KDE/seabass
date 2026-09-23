// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_anlz.h"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/little_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_library_anonymizer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;
using Pdb = rekordbox_pdb_t;
using Anlz = rekordbox_anlz_t;

namespace
{

constexpr uint32_t LenPage = 1024;  // large enough to hold 3 real string-bearing track rows in one page

enum class StringKind { ShortAscii };

void appendDeviceSqlString(std::string &buf, const std::string &text)
{
    auto lengthAndKind = static_cast<uint8_t>(((text.size() + 1) << 1) | 1);
    buf.push_back(static_cast<char>(lengthAndKind));
    buf += text;
}

// A minimal, structurally real export.pdb with 3 track rows (two
// sharing one artist), 1 playlist with entries for all 3 tracks, and 2
// artist rows -- enough to exercise shared-artist renaming (once, not
// once per track) and a playlist, all in one fixture.
// narrowStrings: every text field one or two bytes wide, which is what a
// real export's short fields look like next to a placeholder. Used by
// the truncation case; every other case wants the ordinary widths.
std::string buildSyntheticPdb(bool narrowStrings = false)
{
    std::string buf(static_cast<size_t>(LenPage) * 5, '\0');

    writeU32LE(buf, 4, LenPage);
    writeU32LE(buf, 8, 4);   // num_tables
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
    writeU32LE(buf, 76, static_cast<uint32_t>(Pdb::PAGE_TYPE_PLAYLIST_ENTRIES));
    writeU32LE(buf, 84, 4);
    writeU32LE(buf, 88, 4);

    auto packRowCounts = [](std::string &b, size_t pageStart, uint16_t n) {
        uint32_t packed = (static_cast<uint32_t>(n) & 0x1FFF) | ((static_cast<uint32_t>(n) & 0x7FF) << 13);
        b[pageStart + 24] = static_cast<char>(packed & 0xFF);
        b[pageStart + 25] = static_cast<char>((packed >> 8) & 0xFF);
        b[pageStart + 26] = static_cast<char>((packed >> 16) & 0xFF);
    };
    auto writePageHeader = [&](size_t pageStart, uint32_t pageIndex, Pdb::page_type_t type, uint16_t numRows) {
        writeU32LE(buf, pageStart + 4, pageIndex);
        writeU32LE(buf, pageStart + 8, static_cast<uint32_t>(type));
        writeU32LE(buf, pageStart + 12, pageIndex);
        writeU32LE(buf, pageStart + 16, 5);
        buf[pageStart + 27] = static_cast<char>(0x24);
        packRowCounts(buf, pageStart, numRows);
    };
    auto setRowPresent = [&](size_t pageStart, uint16_t presentMask) {
        writeU16LE(buf, pageStart + LenPage - 4, presentMask);
    };
    auto setRowOfs = [&](size_t pageStart, int rowIndex, uint16_t ofsFromHeap) {
        writeU16LE(buf, pageStart + LenPage - (6 + 2 * static_cast<size_t>(rowIndex)), ofsFromHeap);
    };

    constexpr size_t HeapStart = 40;
    constexpr size_t TrackFixedSize = 136;
    constexpr size_t TrackIdFieldOffset = 72;
    constexpr size_t TrackArtistIdFieldOffset = 68;
    constexpr size_t TrackOfsStringsOffset = 94;

    // --- Page 1: 3 track rows, ids 100/101/102 (100,101 share artist 5; 102 is artist 6) ---
    size_t page1 = LenPage * 1;
    writePageHeader(page1, 1, Pdb::PAGE_TYPE_TRACKS, 3);
    setRowPresent(page1, 0b111);

    auto writeTrackRow = [&](size_t rowStart, uint32_t id, uint32_t artistId, const std::string &analyzePath) {
        writeU32LE(buf, rowStart + TrackIdFieldOffset, id);
        writeU32LE(buf, rowStart + TrackArtistIdFieldOffset, artistId);

        std::string titleBytes, commentBytes, filenameBytes, filePathBytes, analyzePathBytes;
        appendDeviceSqlString(titleBytes, narrowStrings ? "T" : "Real Title " + std::to_string(id));
        appendDeviceSqlString(commentBytes, narrowStrings ? "C" : "Real Comment " + std::to_string(id));
        appendDeviceSqlString(filenameBytes, narrowStrings ? "r.mp3" : "real" + std::to_string(id) + ".mp3");
        appendDeviceSqlString(filePathBytes,
                              narrowStrings ? "/r.mp3" : "/Contents/real" + std::to_string(id) + ".mp3");
        appendDeviceSqlString(analyzePathBytes, analyzePath);

        size_t ofsAnalyze = TrackFixedSize;
        size_t ofsComment = ofsAnalyze + analyzePathBytes.size();
        size_t ofsTitle = ofsComment + commentBytes.size();
        size_t ofsFilename = ofsTitle + titleBytes.size();
        size_t ofsFilePath = ofsFilename + filenameBytes.size();

        auto writeOfs = [&](int index, uint16_t offset) {
            writeU16LE(buf, rowStart + TrackOfsStringsOffset + static_cast<size_t>(index) * 2, offset);
        };
        writeOfs(14, static_cast<uint16_t>(ofsAnalyze));  // analyze_path
        writeOfs(16, static_cast<uint16_t>(ofsComment));  // comment
        writeOfs(17, static_cast<uint16_t>(ofsTitle));    // title
        writeOfs(19, static_cast<uint16_t>(ofsFilename)); // filename
        writeOfs(20, static_cast<uint16_t>(ofsFilePath)); // file_path

        buf.replace(rowStart + ofsAnalyze, analyzePathBytes.size(), analyzePathBytes);
        buf.replace(rowStart + ofsComment, commentBytes.size(), commentBytes);
        buf.replace(rowStart + ofsTitle, titleBytes.size(), titleBytes);
        buf.replace(rowStart + ofsFilename, filenameBytes.size(), filenameBytes);
        buf.replace(rowStart + ofsFilePath, filePathBytes.size(), filePathBytes);
    };

    constexpr size_t TrackRowStride = 300;  // generous -- real string data varies per row
    writeTrackRow(page1 + HeapStart + 0 * TrackRowStride, 100, 5, "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT");
    writeTrackRow(page1 + HeapStart + 1 * TrackRowStride, 101, 5, "/PIONEER/USBANLZ/P001/00000002/ANLZ0000.DAT");
    writeTrackRow(page1 + HeapStart + 2 * TrackRowStride, 102, 6, "/PIONEER/USBANLZ/P001/00000003/ANLZ0000.DAT");
    setRowOfs(page1, 0, 0);
    setRowOfs(page1, 1, static_cast<uint16_t>(1 * TrackRowStride));
    setRowOfs(page1, 2, static_cast<uint16_t>(2 * TrackRowStride));

    // --- Page 2: 2 artist rows, ids 5 and 6 ---
    size_t page2 = LenPage * 2;
    writePageHeader(page2, 2, Pdb::PAGE_TYPE_ARTISTS, 2);
    setRowPresent(page2, 0b11);
    setRowOfs(page2, 0, 0);
    setRowOfs(page2, 1, 40);

    auto writeArtistRow = [&](size_t rowStart, uint32_t id, const std::string &name) {
        writeU16LE(buf, rowStart + 0, 0x0060);
        writeU32LE(buf, rowStart + 4, id);
        buf[rowStart + 8] = static_cast<char>(0x03);
        buf[rowStart + 9] = static_cast<char>(10);
        std::string nameBytes;
        appendDeviceSqlString(nameBytes, name);
        buf.replace(rowStart + 10, nameBytes.size(), nameBytes);
    };
    writeArtistRow(page2 + HeapStart + 0, 5, "Real Artist A");
    writeArtistRow(page2 + HeapStart + 40, 6, "Real Artist B");

    // --- Page 3: 1 playlist_tree row, id=9 ---
    size_t page3 = LenPage * 3;
    writePageHeader(page3, 3, Pdb::PAGE_TYPE_PLAYLIST_TREE, 1);
    setRowPresent(page3, 0b1);
    setRowOfs(page3, 0, 0);
    size_t playlistRowStart = page3 + HeapStart;
    writeU32LE(buf, playlistRowStart + 12, 9);  // id
    std::string playlistNameBytes;
    appendDeviceSqlString(playlistNameBytes, "Real Playlist");
    buf.replace(playlistRowStart + 20, playlistNameBytes.size(), playlistNameBytes);

    // --- Page 4: 3 playlist_entry rows: (9,100) (9,101) (9,102) ---
    size_t page4 = LenPage * 4;
    writePageHeader(page4, 4, Pdb::PAGE_TYPE_PLAYLIST_ENTRIES, 3);
    setRowPresent(page4, 0b111);
    setRowOfs(page4, 0, 0);
    setRowOfs(page4, 1, 12);
    setRowOfs(page4, 2, 24);
    auto writeEntryRow = [&](size_t rowStart, uint32_t entryIndex, uint32_t trackId, uint32_t playlistId) {
        writeU32LE(buf, rowStart + 0, entryIndex);
        writeU32LE(buf, rowStart + 4, trackId);
        writeU32LE(buf, rowStart + 8, playlistId);
    };
    writeEntryRow(page4 + HeapStart + 0, 0, 100, 9);
    writeEntryRow(page4 + HeapStart + 12, 1, 101, 9);
    writeEntryRow(page4 + HeapStart + 24, 2, 102, 9);

    return buf;
}

void writeFile(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void appendU32BE(std::string &out, uint32_t v)
{
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

// A section with `body` as its exact content -- the general form; a
// section with `extraBodyBytes` of filler content is just this with a
// generated body, used where a section's actual bytes don't matter
// (only whether it's present/absent, e.g. the color-waveform sections).
AnlzRawSection makeSectionWithBody(Anlz::section_tags_t tag, const std::string &body)
{
    std::string bytes(4, '\0');
    uint32_t fourcc = static_cast<uint32_t>(tag);
    bytes[0] = static_cast<char>((fourcc >> 24) & 0xFF);
    bytes[1] = static_cast<char>((fourcc >> 16) & 0xFF);
    bytes[2] = static_cast<char>((fourcc >> 8) & 0xFF);
    bytes[3] = static_cast<char>(fourcc & 0xFF);
    appendU32BE(bytes, 12);                                       // len_header
    appendU32BE(bytes, 12 + static_cast<uint32_t>(body.size()));  // len_tag
    bytes += body;
    return AnlzRawSection{fourcc, bytes};
}

AnlzRawSection makeSection(Anlz::section_tags_t tag, size_t extraBodyBytes)
{
    return makeSectionWithBody(tag, std::string(extraBodyBytes, 'x'));
}

AnlzFile blankAnlzFile()
{
    AnlzFile file;
    std::string header(12, '\0');
    header[0] = 'P';
    header[1] = 'M';
    header[2] = 'A';
    header[3] = 'I';
    header[7] = 12;  // len_header (BE u32, low byte only needed for value 12)
    file.headerBytes = header;
    return file;
}

void writeSyntheticAnlz(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    AnlzFile file = blankAnlzFile();
    file.sections.push_back(makeSection(Anlz::SECTION_TAGS_CUES, 8));
    file.sections.push_back(makeSection(Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW, 2000));
    file.sections.push_back(makeSection(Anlz::SECTION_TAGS_WAVE_SCROLL, 2000));
    file.writeRaw(path.string());
}

// One real CUES_2 (extended cue list) section holding a single memory
// cue with a real UTF-16BE comment -- the field kaitai_rekordbox_
// reader.cpp's readCues() actually surfaces from real files (real DJ
// free text, not just position/color), which this test needs to prove
// anonymizeRekordboxLibrary() obfuscates. Byte layout matches
// specs/rekordbox_anlz.ksy's cue_extended_tag/cue_extended_entry
// exactly; see rekordbox_library_anonymizer.cpp's own
// obfuscateCueComments() for the inverse (write-side) logic this
// verifies.
AnlzRawSection makeCueExtendedSectionWithComment(const std::string &commentText)
{
    std::string commentUtf16Be;
    for (char c : commentText) {
        commentUtf16Be.push_back('\0');
        commentUtf16Be.push_back(c);
    }
    commentUtf16Be += std::string(2, '\0');  // trailing NUL terminator
    uint32_t lenComment = static_cast<uint32_t>(commentUtf16Be.size());
    uint32_t lenEntry = 40 + 4 + lenComment;  // fixed prefix + len_comment field + comment text

    std::string entry = "PCP2";
    appendU32BE(entry, 28);         // len_header (not read by the code under test)
    appendU32BE(entry, lenEntry);   // len_entry
    appendU32BE(entry, 0);          // hot_cue (0 = memory cue)
    entry.push_back(static_cast<char>(0));  // type
    entry += std::string(3, '\0');          // pad
    appendU32BE(entry, 5000);       // time (ms)
    appendU32BE(entry, 0xFFFFFFFF); // loop_time (not a loop)
    entry.push_back(static_cast<char>(0));  // color_id
    entry += std::string(7, '\0');          // pad
    entry += std::string(4, '\0');          // loop_numerator + loop_denominator
    appendU32BE(entry, lenComment);
    entry += commentUtf16Be;

    std::string body(4, '\0');    // type (BE u32) = 0 (memory list)
    body.push_back(static_cast<char>(0));
    body.push_back(static_cast<char>(1));  // num_cues (BE u16) = 1
    body += std::string(2, '\0');          // pad
    body += entry;

    return makeSectionWithBody(Anlz::SECTION_TAGS_CUES_2, body);
}

std::string readCueCommentUtf8(const fs::path &anlzPath)
{
    AnlzFile file = AnlzFile::readRaw(anlzPath.string());
    for (const auto &section : file.sections) {
        if (section.fourcc != static_cast<uint32_t>(Anlz::SECTION_TAGS_CUES_2)) {
            continue;
        }
        const std::string &b = section.rawBytes;
        if (b.size() < 20 + 44) {
            return {};
        }
        size_t commentOffset = 20 + 44;
        uint32_t storedLenComment = readU32BE(b, 20 + 40);
        std::string utf8;
        for (size_t i = 0; i + 1 < storedLenComment && commentOffset + i + 1 < b.size(); i += 2) {
            char c = b[commentOffset + i + 1];
            if (c == '\0') {
                break;
            }
            utf8.push_back(c);
        }
        return utf8;
    }
    return {};
}

std::vector<uint32_t> presentTrackIds(const fs::path &pdbPath)
{
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    std::vector<uint32_t> ids;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (row->present()) {
                        if (auto *t = dynamic_cast<Pdb::track_row_t *>(row->body())) {
                            ids.push_back(t->id());
                        }
                    }
                }
            }
        });
    }
    return ids;
}

std::string trackFieldByIndex(const fs::path &pdbPath, uint32_t trackId, int fieldIndex)
{
    // fieldIndex: 16=comment, 17=title
    std::ifstream ifs(pdbPath, std::ifstream::binary);
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
                        result = fieldIndex == 17 ? sqlText(t->title()) : sqlText(t->comment());
                    }
                }
            }
        });
        return result;
    }
    return {};
}

std::string artistNameById(const fs::path &pdbPath, uint32_t artistId)
{
    std::ifstream ifs(pdbPath, std::ifstream::binary);
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

std::vector<std::pair<uint32_t, uint32_t>> presentPlaylistEntries(const fs::path &pdbPath)
{
    std::ifstream ifs(pdbPath, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_PLAYLIST_ENTRIES) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    if (auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(row->body())) {
                        entries.emplace_back(e->playlist_id(), e->track_id());
                    }
                }
            }
        });
    }
    return entries;
}

bool hasFourcc(const AnlzFile &f, Anlz::section_tags_t tag)
{
    uint32_t want = static_cast<uint32_t>(tag);
    return std::any_of(f.sections.begin(), f.sections.end(), [&](const AnlzRawSection &s) { return s.fourcc == want; });
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_rekordbox_library_anonymizer_test";
    fs::remove_all(root);
    fs::create_directories(root);

    fs::path sourceRoot = root / "source";
    fs::path destRoot = root / "dest";

    writeFile(sourceRoot / "rekordbox" / "export.pdb", buildSyntheticPdb());

    fs::path track100Anlz = sourceRoot / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT";
    fs::create_directories(track100Anlz.parent_path());
    AnlzFile track100File = blankAnlzFile();
    track100File.sections.push_back(makeSection(Anlz::SECTION_TAGS_CUES, 8));
    track100File.sections.push_back(makeSection(Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW, 2000));
    track100File.sections.push_back(makeCueExtendedSectionWithComment("Real DJ Note"));
    track100File.writeRaw(track100Anlz.string());

    writeSyntheticAnlz(sourceRoot / "USBANLZ" / "P001" / "00000002" / "ANLZ0000.DAT");
    writeSyntheticAnlz(sourceRoot / "USBANLZ" / "P001" / "00000003" / "ANLZ0000.DAT");

    assert(readCueCommentUtf8(track100Anlz) == "Real DJ Note");  // fixture self-check

    auto result = anonymizeRekordboxLibrary(sourceRoot.string(), destRoot.string());

    assert(result.errorMessage.empty());
    assert(result.tracksAnonymized == 3);
    // Both artists: one shared by two tracks, one by the third.
    assert(result.artistsRenamed == 2);
    assert(result.playlistsRenamed == 1);
    std::cout << "case 1 (anonymizeRekordboxLibrary: rename counts correct) OK\n";

    fs::path destPdb = destRoot / "rekordbox" / "export.pdb";
    auto ids = presentTrackIds(destPdb);
    std::sort(ids.begin(), ids.end());
    assert((ids == std::vector<uint32_t>{100, 101, 102}));
    std::cout << "case 2 (every track is kept) OK\n";

    std::string title100 = trackFieldByIndex(destPdb, 100, 17);
    std::string title101 = trackFieldByIndex(destPdb, 101, 17);
    assert(title100 != "Real Title 100");
    assert(title101 != "Real Title 101");
    assert(title100 != title101);  // distinct per-track placeholders
    std::string comment100 = trackFieldByIndex(destPdb, 100, 16);
    assert(comment100 != "Real Comment 100");
    // The last track too, and against what the source really holds, so
    // a pass that stopped short of the end could not pass this.
    const fs::path sourcePdb = sourceRoot / "rekordbox" / "export.pdb";
    assert(trackFieldByIndex(sourcePdb, 102, 17) == "Real Title 102");  // fixture self-check
    assert(trackFieldByIndex(destPdb, 102, 17) != "Real Title 102");
    assert(trackFieldByIndex(destPdb, 102, 17) != title101);
    assert(artistNameById(destPdb, 6) != artistNameById(sourcePdb, 6));
    std::cout << "case 3 (title/comment obfuscated and distinct per track) OK\n";

    assert(artistNameById(destPdb, 5) != "Real Artist A");  // shared by tracks 100 and 101 -- renamed
    std::cout << "case 4 (shared artist renamed once) OK\n";

    auto entries = presentPlaylistEntries(destPdb);
    bool has100 = false, has101 = false, has102 = false;
    for (const auto &e : entries) {
        if (e.second == 100) has100 = true;
        if (e.second == 101) has101 = true;
        if (e.second == 102) has102 = true;
    }
    assert(has100 && has101 && has102);
    std::cout << "case 5 (every playlist entry is kept) OK\n";

    AnlzFile kept1 = AnlzFile::readRaw((destRoot / "USBANLZ" / "P001" / "00000002" / "ANLZ0000.DAT").string());
    assert(hasFourcc(kept1, Anlz::SECTION_TAGS_CUES));                 // preserved
    assert(!hasFourcc(kept1, Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW));  // stripped
    assert(!hasFourcc(kept1, Anlz::SECTION_TAGS_WAVE_SCROLL));  // stripped too -- large and unused by this app's reader
    std::cout << "case 6 (a track's ANLZ: large unused waveform sections stripped, cues preserved) OK\n";

    std::string destComment = readCueCommentUtf8(destRoot / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT");
    assert(destComment != "Real DJ Note");   // obfuscated, not left as real DJ free text
    assert(!destComment.empty());            // and not just blanked -- see BRAINSTORM.md discussion
    std::cout << "case 6b (a track's real cue comment is obfuscated, not left verbatim or blanked) OK\n";

    std::error_code ec;
    assert(fs::exists(destRoot / "USBANLZ" / "P001" / "00000003" / "ANLZ0000.DAT", ec));
    std::cout << "case 7 (every track's analysis file is kept) OK\n";

    // Source untouched -- every edit happens on the destination copy.
    AnlzFile sourceStill = AnlzFile::readRaw((sourceRoot / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT").string());
    assert(hasFourcc(sourceStill, Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW));
    assert(readCueCommentUtf8(track100Anlz) == "Real DJ Note");
    std::cout << "case 8 (source library untouched) OK\n";

    // A pdb with no track rows and no playlist rows, only artists. Every
    // per-track and per-playlist pass has nothing to do, but the wholesale
    // artist rename still renames both rows in memory -- and the commit
    // used to be decided without counting that pass, so the file was
    // never written and "Real Artist A" went out as it came in.
    {
        std::string noTracks = buildSyntheticPdb();
        writeU16LE(noTracks, static_cast<size_t>(LenPage) * 1 + LenPage - 4, 0);  // tracks: none present
        writeU16LE(noTracks, static_cast<size_t>(LenPage) * 3 + LenPage - 4, 0);  // playlist tree: none
        writeU16LE(noTracks, static_cast<size_t>(LenPage) * 4 + LenPage - 4, 0);  // playlist entries: none
        const fs::path source2 = root / "no-tracks-source";
        const fs::path dest2 = root / "no-tracks-dest";
        writeFile(source2 / "rekordbox" / "export.pdb", noTracks);
        assert(artistNameById(source2 / "rekordbox" / "export.pdb", 5) == "Real Artist A");  // fixture self-check

        auto noTrackResult = anonymizeRekordboxLibrary(source2.string(), dest2.string());
        assert(noTrackResult.errorMessage.empty());
        assert(noTrackResult.tracksAnonymized == 0);
        assert(noTrackResult.artistsRenamed == 2);
        const fs::path dest2Pdb = dest2 / "rekordbox" / "export.pdb";
        assert(artistNameById(dest2Pdb, 5) != "Real Artist A");
        assert(artistNameById(dest2Pdb, 6) != "Real Artist B");
        std::cout << "case 9 (a pdb with only artist names is still written) OK\n";
    }

    // exportExt.pdb: kept when the scrub lands, removed when it does not.
    //
    // The anonymizer's own block had no test at all. Its comment calls
    // the failure path "the whole point" -- a file present in an export
    // is one that was scrubbed -- and that path had never run, so a
    // change making the scrub silently no-op would have shipped a real
    // My Tag vocabulary with nothing complaining.
    //
    // Two libraries, because one of them proves nothing on its own: a
    // scrub that never works keeps the file out of both, and a scrub
    // that never fails keeps it in both.
    {
        const fs::path realExt = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "exportExt.pdb";
        assert(fs::is_regular_file(realExt) && "the anonymized exportExt.pdb fixture is missing");

        // A real one: kept, and every name in it a placeholder.
        const fs::path src = root / "ext-source";
        const fs::path dst = root / "ext-dest";
        writeFile(src / "rekordbox" / "export.pdb", buildSyntheticPdb());
        fs::create_directories(src / "rekordbox");
        fs::copy_file(realExt, src / "rekordbox" / "exportExt.pdb");

        auto kept = anonymizeRekordboxLibrary(src.string(), dst.string());
        assert(kept.errorMessage.empty());
        const fs::path keptExt = dst / "rekordbox" / "exportExt.pdb";
        assert(fs::is_regular_file(keptExt) && "a scrubbable exportExt.pdb must survive the export");
        assert(kept.tagsRenamed > 1 && "and its tag names must have been rewritten");
        assert(std::find(kept.removedUnanonymizableFiles.begin(), kept.removedUnanonymizableFiles.end(),
                         std::string("exportExt.pdb")) == kept.removedUnanonymizableFiles.end());
        std::cout << "case 10 (a real exportExt.pdb is scrubbed and kept: " << kept.tagsRenamed
                  << " tag name(s)) OK\n";

        // One the writer cannot read: removed, reported as removed, and
        // NOT left sitting in the export for the verifier to find.
        const fs::path badSrc = root / "ext-bad-source";
        const fs::path badDst = root / "ext-bad-dest";
        writeFile(badSrc / "rekordbox" / "export.pdb", buildSyntheticPdb());
        // Same size as a real one so nothing rejects it on length alone,
        // but not a pdb: PdbRowWriter's construction throws on it.
        writeFile(badSrc / "rekordbox" / "exportExt.pdb", std::string(73728, '\x7f'));

        auto dropped = anonymizeRekordboxLibrary(badSrc.string(), badDst.string());
        assert(dropped.errorMessage.empty() && "an unscrubbable exportExt.pdb must not fail the whole export");
        assert(!fs::exists(badDst / "rekordbox" / "exportExt.pdb")
               && "an exportExt.pdb that could not be scrubbed must not be in the export");
        assert(dropped.tagsRenamed == 0);
        assert(std::find(dropped.removedUnanonymizableFiles.begin(), dropped.removedUnanonymizableFiles.end(),
                         std::string("exportExt.pdb")) != dropped.removedUnanonymizableFiles.end()
               && "and the manifest has to say it was removed");
        std::cout << "case 11 (an exportExt.pdb that cannot be scrubbed is removed and reported) OK\n";
    }

    // An analysis file that cannot be scrubbed is named, not swallowed.
    // Its PATH section holds the audio file's real path -- artist, album
    // and title on a typical library -- and MANIFEST.txt promises a
    // contributor that those are gone. The verifier refuses such an
    // export, but from a check that cannot say which file went wrong;
    // this is the one that can.
    {
        const fs::path badSrc = root / "anlz-bad-source";
        const fs::path badDst = root / "anlz-bad-dest";
        writeFile(badSrc / "rekordbox" / "export.pdb", buildSyntheticPdb());
        // Track 100's analysis file, the right name in the right place
        // and not an ANLZ file at all: readRaw() throws on it.
        writeFile(badSrc / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT", std::string(512, '\x7f'));
        writeSyntheticAnlz(badSrc / "USBANLZ" / "P001" / "00000002" / "ANLZ0000.DAT");
        writeSyntheticAnlz(badSrc / "USBANLZ" / "P001" / "00000003" / "ANLZ0000.DAT");

        auto result = anonymizeRekordboxLibrary(badSrc.string(), badDst.string());
        // Dropped, not left behind: an analysis file is derived data,
        // and AnonymizeLibrary produces no export at all when anything
        // is still in it, so listing this without removing it would let
        // one truncated file destroy the whole export.
        assert(result.errorMessage.empty() && "one unscrubbable analysis file must not refuse the export");
        assert(!fs::exists(badDst / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT")
               && "a file that could not be scrubbed must not be in the export");
        assert(result.unremovedUnanonymizableFiles.empty());

        // Exactly one line, and it names the file by where it sits in
        // the export: rekordbox calls every one of them ANLZ0000.DAT, so
        // the directory is the only identifying part. One line, because
        // three reported failures would pass a "does it mention the
        // name" check just as well.
        if (result.removedUnanonymizableFiles.size() != 1) {
            std::cerr << "expected exactly one dropped analysis file, got "
                      << result.removedUnanonymizableFiles.size() << "\n";
            for (const auto &line : result.removedUnanonymizableFiles) {
                std::cerr << "  " << line << "\n";
            }
        }
        assert(result.removedUnanonymizableFiles.size() == 1);
        const std::string &line = result.removedUnanonymizableFiles.front();
        assert(line.find("USBANLZ/P001/00000001/ANLZ0000.DAT") != std::string::npos
               && "the line has to say WHICH analysis file, and they are all called ANLZ0000.DAT");
        assert(line.find("could not be scrubbed") != std::string::npos);
        assert(line.find(badDst.string()) == std::string::npos
               && "and not carry the machine's own path into MANIFEST.txt");
        std::cout << "case 12 (an analysis file that cannot be scrubbed is dropped and named) OK\n";
    }

    // The count reaches the result, and the default fixture shows why
    // that is worth a case of its own: with ordinary field widths
    // nothing is cut and the number is a truthful zero, so a run on it
    // cannot tell a working counter from a disconnected one. This
    // fixture's fields are one and two bytes wide, which is what the
    // short fields of a real export look like beside a placeholder.
    {
        const fs::path src = root / "trunc-source";
        const fs::path dst = root / "trunc-dest";
        writeFile(src / "rekordbox" / "export.pdb", buildSyntheticPdb(/*narrowStrings=*/true));
        writeSyntheticAnlz(src / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT");
        writeSyntheticAnlz(src / "USBANLZ" / "P001" / "00000002" / "ANLZ0000.DAT");
        writeSyntheticAnlz(src / "USBANLZ" / "P001" / "00000003" / "ANLZ0000.DAT");

        auto result = anonymizeRekordboxLibrary(src.string(), dst.string());
        assert(result.errorMessage.empty());
        if (result.placeholdersTruncated <= 0) {
            std::cerr << "nothing was reported as cut short, on a fixture whose fields are one and two bytes "
                         "wide\n";
        }
        assert(result.placeholdersTruncated > 0
               && "the writer's count has to reach the result, or the manifest says nothing");
        std::cout << "case 13 (placeholders cut to fit are counted and reported: "
                  << result.placeholdersTruncated << ") OK\n";
    }

    // A PPTH section whose len_header is nonsense must be left alone,
    // not indexed with it.
    //
    // obfuscatePathSection() guarded with `lenHeader + lenPath >
    // sectionBytes.size()`, and both are uint32_t, so the sum wraps:
    // len_header = 0xFFFFFFF8 with len_path = 0x10 adds to 8 and passes
    // a check meant to stop exactly this. The indexing that follows is
    // unchecked operator[] about 4 GB past the buffer -- a heap write
    // outside any catch. AnlzFile::readRaw() validates section framing
    // but never a section's own len_header, so such a section can come
    // off a stick.
    //
    // Without the fix this is undefined behaviour: the run may crash
    // rather than reach the assertion. A crash here IS the failure.
    {
        const fs::path root2 = root / "hostile-ppth";
        const fs::path src = root2 / "source";
        const fs::path dst = root2 / "dest";
        writeFile(src / "rekordbox" / "export.pdb", buildSyntheticPdb());

        std::string ppth(4, '\0');
        const uint32_t fourcc = static_cast<uint32_t>(Anlz::SECTION_TAGS_PATH);
        ppth[0] = static_cast<char>((fourcc >> 24) & 0xFF);
        ppth[1] = static_cast<char>((fourcc >> 16) & 0xFF);
        ppth[2] = static_cast<char>((fourcc >> 8) & 0xFF);
        ppth[3] = static_cast<char>(fourcc & 0xFF);
        // Written by hand: two appendU32BE overloads are visible here.
        auto be32 = [&ppth](uint32_t v) {
            ppth.push_back(static_cast<char>((v >> 24) & 0xFF));
            ppth.push_back(static_cast<char>((v >> 16) & 0xFF));
            ppth.push_back(static_cast<char>((v >> 8) & 0xFF));
            ppth.push_back(static_cast<char>(v & 0xFF));
        };
        be32(0xFFFFFFF8u);  // len_header: the wrap
        be32(28u);          // len_tag, so the framing itself is sane
        be32(0x10u);        // len_path: 0xFFFFFFF8 + 0x10 wraps to 8
        ppth += std::string(12, 'p');

        AnlzFile file = blankAnlzFile();
        file.sections.push_back(AnlzRawSection{fourcc, ppth});
        const fs::path anlz = src / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT";
        fs::create_directories(anlz.parent_path());
        file.writeRaw(anlz.string());

        auto hostile = anonymizeRekordboxLibrary(src.string(), dst.string());
        assert(hostile.errorMessage.empty() && "a malformed section must not fail the whole export");

        // Left exactly as it was: refused, not partly rewritten.
        const fs::path out = dst / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT";
        assert(fs::is_regular_file(out));
        std::ifstream in(out, std::ios::binary);
        const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(after.find(std::string(12, 'p')) != std::string::npos
               && "the malformed section's bytes must be untouched");
        std::cout << "case 14 (a PPTH whose len_header wraps is refused, not indexed) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
