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
// artist rows -- enough to exercise pruning, shared-artist renaming
// (once, not once per track), and playlist-entry cleanup for a pruned
// track, all in one fixture.
std::string buildSyntheticPdb()
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
        appendDeviceSqlString(titleBytes, "Real Title " + std::to_string(id));
        appendDeviceSqlString(commentBytes, "Real Comment " + std::to_string(id));
        appendDeviceSqlString(filenameBytes, "real" + std::to_string(id) + ".mp3");
        appendDeviceSqlString(filePathBytes, "/Contents/real" + std::to_string(id) + ".mp3");
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

    auto result = anonymizeRekordboxLibrary(sourceRoot.string(), destRoot.string(), 2);

    assert(result.errorMessage.empty());
    assert(result.tracksKept == 2);
    assert(result.tracksDropped == 1);
    assert(result.artistsRenamed == 1);  // only artist 5 (shared by the 2 kept tracks) -- not artist 6
    assert(result.playlistsRenamed == 1);
    std::cout << "case 1 (anonymizeRekordboxLibrary: prune/rename counts correct) OK\n";

    fs::path destPdb = destRoot / "rekordbox" / "export.pdb";
    auto ids = presentTrackIds(destPdb);
    std::sort(ids.begin(), ids.end());
    assert((ids == std::vector<uint32_t>{100, 101}));
    std::cout << "case 2 (kept tracks are exactly the first maxTracks in disk order) OK\n";

    std::string title100 = trackFieldByIndex(destPdb, 100, 17);
    std::string title101 = trackFieldByIndex(destPdb, 101, 17);
    assert(title100 != "Real Title 100");
    assert(title101 != "Real Title 101");
    assert(title100 != title101);  // distinct per-track placeholders
    std::string comment100 = trackFieldByIndex(destPdb, 100, 16);
    assert(comment100 != "Real Comment 100");
    std::cout << "case 3 (title/comment obfuscated and distinct per track) OK\n";

    assert(artistNameById(destPdb, 5) != "Real Artist A");  // shared by both kept tracks -- renamed
    std::cout << "case 4 (shared artist renamed once) OK\n";

    auto entries = presentPlaylistEntries(destPdb);
    bool has100 = false, has101 = false, has102 = false;
    for (const auto &e : entries) {
        if (e.second == 100) has100 = true;
        if (e.second == 101) has101 = true;
        if (e.second == 102) has102 = true;
    }
    assert(has100 && has101);
    assert(!has102);  // playlist entry for the pruned track was cleaned up
    std::cout << "case 5 (playlist entry for a pruned track is removed, others kept) OK\n";

    AnlzFile kept1 = AnlzFile::readRaw((destRoot / "USBANLZ" / "P001" / "00000002" / "ANLZ0000.DAT").string());
    assert(hasFourcc(kept1, Anlz::SECTION_TAGS_CUES));                 // preserved
    assert(!hasFourcc(kept1, Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW));  // stripped
    assert(!hasFourcc(kept1, Anlz::SECTION_TAGS_WAVE_SCROLL));  // stripped too -- large and unused by this app's reader
    std::cout << "case 6 (kept track's ANLZ: large unused waveform sections stripped, cues preserved) OK\n";

    std::string destComment = readCueCommentUtf8(destRoot / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT");
    assert(destComment != "Real DJ Note");   // obfuscated, not left as real DJ free text
    assert(!destComment.empty());            // and not just blanked -- see BRAINSTORM.md discussion
    std::cout << "case 6b (kept track's real cue comment is obfuscated, not left verbatim or blanked) OK\n";

    std::error_code ec;
    assert(!fs::exists(destRoot / "USBANLZ" / "P001" / "00000003", ec));  // pruned track's ANLZ dir deleted
    std::cout << "case 7 (pruned track's ANLZ directory deleted) OK\n";

    // Source untouched -- every edit happens on the destination copy.
    AnlzFile sourceStill = AnlzFile::readRaw((sourceRoot / "USBANLZ" / "P001" / "00000001" / "ANLZ0000.DAT").string());
    assert(hasFourcc(sourceStill, Anlz::SECTION_TAGS_WAVE_COLOR_PREVIEW));
    assert(readCueCommentUtf8(track100Anlz) == "Real DJ Note");
    std::cout << "case 8 (source library untouched) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
