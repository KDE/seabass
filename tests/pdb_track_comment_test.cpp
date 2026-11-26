// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PdbRowWriter::setTrackComment (#108): a track row is built again around
// its new comment, written over itself when it fits and moved when it does
// not. Held against every export.pdb in tests/fixtures: after any number
// of comments of any length, every track row reads back through the parser
// with every string and every fixed field as it was, only the comment
// changed, each id on exactly one present row, and every page of the track
// table adding up with no row running into the next.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::rekordbox::PdbRowWriter;
using seabass::infrastructure::rekordbox::sqlText;
using Pdb = rekordbox_pdb_t;

namespace
{

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const fs::path &p, const std::string &bytes)
{
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bytes;
}

uint16_t u16(const std::string &b, size_t o)
{
    return static_cast<uint16_t>(static_cast<unsigned char>(b[o]) | static_cast<unsigned char>(b[o + 1]) << 8);
}

uint32_t u32(const std::string &b, size_t o)
{
    return static_cast<uint32_t>(u16(b, o)) | static_cast<uint32_t>(u16(b, o + 2)) << 16;
}

void put16(std::string &b, size_t o, uint16_t v)
{
    b[o] = static_cast<char>(v & 0xFF);
    b[o + 1] = static_cast<char>(v >> 8);
}

void put32(std::string &b, size_t o, uint32_t v)
{
    put16(b, o, static_cast<uint16_t>(v & 0xFFFF));
    put16(b, o + 2, static_cast<uint16_t>(v >> 16));
}

// One present track row as the parser reads it: its twenty-one strings and
// its fixed fields (the 94 bytes before the string offsets, index_shift
// left out, since a moved row has another index on its page).
struct TrackRead
{
    std::vector<std::string> strings;
    std::string fixed;
};

struct Snapshot
{
    std::map<uint32_t, TrackRead> tracks;
    std::map<uint32_t, int> presentRowsById;
    // Pages of the track table, in chain order.
    std::vector<uint32_t> pages;
};

Snapshot snapshotOf(const std::string &bytes)
{
    Snapshot snap;
    std::istringstream iss(bytes);
    kaitai::kstream ks(&iss);
    Pdb pdb(false, &ks);
    for (const auto &table : *pdb.tables()) {
        if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
            continue;
        }
        seabass::infrastructure::rekordbox::forEachDataPage(*table, [&](Pdb::page_t *page) {
            snap.pages.push_back(page->page_index());
            for (const auto &group : *page->row_groups()) {
                for (const auto &row : *group->rows()) {
                    if (!row->present()) {
                        continue;
                    }
                    auto *t = dynamic_cast<Pdb::track_row_t *>(row->body());
                    assert(t != nullptr);
                    ++snap.presentRowsById[t->id()];
                    TrackRead read;
                    for (auto *s : {t->isrc(), t->texter(), t->unknown_string_2(), t->unknown_string_3(),
                                    t->unknown_string_4(), t->message(), t->kuvo_public(), t->autoload_hot_cues(),
                                    t->unknown_string_5(), t->unknown_string_6(), t->date_added(), t->release_date(),
                                    t->mix_name(), t->unknown_string_7(), t->analyze_path(), t->analyze_date(),
                                    t->comment(), t->title(), t->unknown_string_8(), t->filename(), t->file_path()}) {
                        read.strings.push_back(sqlText(s));
                    }
                    const size_t body = static_cast<size_t>(pdb.len_page()) * page->page_index() + row->row_base();
                    read.fixed = bytes.substr(body, 2) + bytes.substr(body + 4, 90);
                    snap.tracks[t->id()] = read;
                }
            }
        });
    }
    return snap;
}

// How far num_rows was off on each track page of the fixture being tested,
// before any edit.
std::map<uint32_t, long> &numRowsOff()
{
    static std::map<uint32_t, long> off;
    return off;
}

void recordNumRowsOff(const std::string &bytes, const std::vector<uint32_t> &pages)
{
    numRowsOff().clear();
    const size_t lenPage = u32(bytes, 4);
    for (const uint32_t page : pages) {
        const size_t base = lenPage * page;
        const uint32_t numRowOffsets = u32(bytes, base + 24) & 0x1FFFu;
        long present = 0;
        for (uint32_t i = 0; i < numRowOffsets; ++i) {
            present += (u16(bytes, base + lenPage - (i / 16) * 0x24 - 4) >> (i % 16)) & 1;
        }
        numRowsOff()[page] = static_cast<long>((u32(bytes, base + 24) >> 13) & 0x7FFu) - present;
    }
}

// Every page of the track table adds up (free + used + row offsets + group
// flags = the heap), every row offset lies inside the used part, and no
// present row's strings run into the row after it.
void checkTrackPages(const std::string &bytes, const Snapshot &snap)
{
    const size_t lenPage = u32(bytes, 4);
    for (const uint32_t page : snap.pages) {
        const size_t base = lenPage * page;
        const uint32_t numRowOffsets = u32(bytes, base + 24) & 0x1FFFu;
        const uint32_t numRows = (u32(bytes, base + 24) >> 13) & 0x7FFu;
        const size_t freeSize = u16(bytes, base + 28);
        const size_t usedSize = u16(bytes, base + 30);
        const size_t groups = numRowOffsets == 0 ? 0 : (numRowOffsets - 1) / 16 + 1;
        assert(freeSize + usedSize + 2 * numRowOffsets + 4 * groups == lenPage - 40 && "the page adds up");
        std::vector<size_t> offsets;
        std::vector<size_t> present;
        for (uint32_t i = 0; i < numRowOffsets; ++i) {
            const size_t groupBase = base + lenPage - (i / 16) * 0x24;
            const size_t offset = u16(bytes, groupBase - 6 - 2 * (i % 16));
            assert(offset < usedSize);
            offsets.push_back(offset);
            if ((u16(bytes, groupBase - 4) >> (i % 16)) & 1) {
                present.push_back(offset);
            }
        }
        // num_rows counts the present rows, give or take what the page
        // was already off by: 148 pages of the anonymized library count
        // more rows than they mark present, as that export came.
        const auto known = numRowsOff().find(page);
        const long off = static_cast<long>(numRows) - static_cast<long>(present.size());
        if (known == numRowsOff().end() ? off != 0 : off != known->second) {
            std::cerr << "page " << page << ": num_rows " << numRows << ", present " << present.size() << "\n";
            assert(false && "num_rows counts the present rows");
        }
        std::sort(offsets.begin(), offsets.end());
        for (const size_t offset : present) {
            const size_t row = base + 40 + offset;
            size_t end = 0;
            for (int k = 0; k < 21; ++k) {
                const size_t at = row + u16(bytes, row + 94 + 2 * k);
                const auto kind = static_cast<unsigned char>(bytes[at]);
                const size_t length = kind == 0x40 || kind == 0x90 ? u16(bytes, at + 1) : kind >> 1;
                end = std::max(end, at + length - row);
            }
            const auto next = std::upper_bound(offsets.begin(), offsets.end(), offset);
            const size_t limit = next == offsets.end() ? usedSize : *next;
            assert(offset + end <= limit && "a row ends before the next one starts");
        }
    }
}

// After an edit: every track there before is there now, on exactly one
// present row, with every string and fixed field as `expected` says.
void expectTracks(const std::string &bytes, const std::map<uint32_t, TrackRead> &expected)
{
    const Snapshot snap = snapshotOf(bytes);
    checkTrackPages(bytes, snap);
    assert(snap.tracks.size() == expected.size());
    for (const auto &[id, want] : expected) {
        assert(snap.presentRowsById.at(id) == 1 && "each track on exactly one present row");
        const TrackRead &got = snap.tracks.at(id);
        if (got.strings != want.strings || got.fixed != want.fixed) {
            for (size_t k = 0; k < want.strings.size(); ++k) {
                if (got.strings[k] != want.strings[k]) {
                    std::cerr << "track " << id << " string " << k << ": \"" << got.strings[k] << "\", expected \""
                              << want.strings[k] << "\"\n";
                }
            }
            assert(false && "the row reads back as it was, but its comment");
        }
    }
}

std::string asciiOf(size_t n, char c)
{
    return std::string(n, c);
}

// "é" n times, one UTF-16 unit each, and an emoji (two) at the end.
std::string wideOf(size_t n)
{
    std::string s;
    for (size_t i = 0; i + 2 < n; ++i) {
        s += "\xc3\xa9";
    }
    return s + "\xf0\x9f\x8e\xb6";
}

std::string commentFor(size_t i, int round)
{
    switch ((i + static_cast<size_t>(round)) % 6) {
    case 0:
        return "";
    case 1:
        return "short";
    case 2:
        return asciiOf(60, 'a');
    case 3:
        return asciiOf(200, 'b');  // past 126: long ASCII
    case 4:
        return wideOf(150);        // UTF-16
    default:
        return "Caf\xc3\xa9 " + asciiOf(20 + i % 40, 'c');
    }
}

fs::path copyIn(const fs::path &dir, const fs::path &source, const std::string &name)
{
    const fs::path to = dir / name;
    fs::copy_file(source, to, fs::copy_options::overwrite_existing);
    return to;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: pdb_track_comment_test <export.pdb>...\n";
        return 1;
    }
    const fs::path dir = seabass::testing::scratchRoot() / "seabass_pdb_track_comment_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    for (int a = 1; a < argc; ++a) {
        const fs::path source = seabass::pathFromUtf8(argv[a]);
        const std::string name = "fixture" + std::to_string(a) + ".pdb";
        {
            const std::string original = readFile(source);
            recordNumRowsOff(original, snapshotOf(original).pages);
        }

        // 1. Every track (every fifth on a big library, to keep the test
        //    short) given comments of every kind, two rounds, then all
        //    emptied: each row reads back whole every time.
        {
            const fs::path pdbPath = copyIn(dir, source, name);
            auto expected = snapshotOf(readFile(pdbPath)).tracks;
            std::vector<uint32_t> ids;
            for (const auto &[id, read] : expected) {
                ids.push_back(id);
            }
            const size_t stride = ids.size() > 200 ? 5 : 1;
            size_t moved = 0;
            for (int round = 0; round < 3; ++round) {
                PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
                for (size_t i = 0; i < ids.size(); i += stride) {
                    const std::string comment = round == 2 ? std::string() : commentFor(i, round);
                    std::string refusal;
                    if (!writer.setTrackComment(ids[i], comment, &refusal)) {
                        std::cerr << "track " << ids[i] << ": " << refusal << "\n";
                        assert(false && "every fixture row takes a comment");
                    }
                    expected[ids[i]].strings[16] = comment;
                }
                assert(writer.commit());
                const std::string bytes = readFile(pdbPath);
                expectTracks(bytes, expected);
                moved = std::max(moved, snapshotOf(bytes).pages.size());
            }
            // Zeroing the free space afterwards leaves every row whole.
            PdbRowWriter scrub(seabass::pathToUtf8(pdbPath));
            assert(scrub.zeroUnusedSpace() > 0);
            assert(scrub.commit());
            expectTracks(readFile(pdbPath), expected);
            std::cout << source.parent_path().parent_path().filename().string() << ": case 1 ("
                      << (ids.size() + stride - 1) / stride << " rows, three rounds, " << moved
                      << " track pages at most) OK\n";
        }

        // 2. Every row of one page given ever longer comments in one
        //    session: the page fills, rows go to the last page and new
        //    pages, and the page they left keeps its accounting.
        {
            const fs::path pdbPath = copyIn(dir, source, name);
            const std::string before = readFile(pdbPath);
            const Snapshot snap = snapshotOf(before);
            auto expected = snap.tracks;
            const size_t lenPage = u32(before, 4);
            const uint32_t firstPage = snap.pages.front();
            std::vector<uint32_t> onPage;
            {
                std::istringstream iss(before);
                kaitai::kstream ks(&iss);
                Pdb pdb(false, &ks);
                for (const auto &table : *pdb.tables()) {
                    if (table->type() != Pdb::PAGE_TYPE_TRACKS) {
                        continue;
                    }
                    seabass::infrastructure::rekordbox::forEachDataPage(*table, [&](Pdb::page_t *page) {
                        if (page->page_index() != firstPage) {
                            return;
                        }
                        for (const auto &group : *page->row_groups()) {
                            for (const auto &row : *group->rows()) {
                                if (row->present()) {
                                    onPage.push_back(dynamic_cast<Pdb::track_row_t *>(row->body())->id());
                                }
                            }
                        }
                    });
                }
            }
            assert(!onPage.empty());
            PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
            for (const size_t length : {10, 40, 90, 300, 700}) {
                for (const uint32_t id : onPage) {
                    const std::string comment = asciiOf(length, 'g');
                    std::string refusal;
                    assert(writer.setTrackComment(id, comment, &refusal));
                    expected[id].strings[16] = comment;
                }
            }
            assert(writer.commit());
            const std::string after = readFile(pdbPath);
            expectTracks(after, expected);
            const Snapshot now = snapshotOf(after);
            assert(now.pages.size() > snap.pages.size() && "rows that outgrew their page went to a new one");
            const uint8_t flags = static_cast<uint8_t>(after[lenPage * firstPage + 27]);
            assert((flags & 0x10) && "the page rows left says it has deleted rows");
            std::cout << "  case 2 (" << onPage.size() << " rows of page " << firstPage << " grown to 700 bytes: "
                      << now.pages.size() - snap.pages.size() << " new pages) OK\n";
        }

        // 3. What trackCommentCapacity() says is what the writer takes:
        //    that many units written, one more refused with nothing
        //    changed. A short comment stays where the row is.
        {
            const fs::path pdbPath = copyIn(dir, source, name);
            auto expected = snapshotOf(readFile(pdbPath)).tracks;
            const uint32_t id = expected.begin()->first;
            PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
            const auto capacity = writer.trackCommentCapacity(id);
            assert(capacity && capacity->utf16 && capacity->units > 1000);
            std::string refusal;
            assert(!writer.setTrackComment(id, wideOf(capacity->units + 1), &refusal));
            assert(refusal.find("room for") != std::string::npos);
            assert(!writer.commit() && "a refused comment changes nothing");
            assert(writer.setTrackComment(id, wideOf(capacity->units), &refusal));
            expected[id].strings[16] = wideOf(capacity->units);
            assert(writer.commit());
            expectTracks(readFile(pdbPath), expected);

            // In place: the row keeps its page and its offset.
            const fs::path again = copyIn(dir, source, name);
            const std::string bytes = readFile(again);
            PdbRowWriter small(seabass::pathToUtf8(again));
            assert(small.setTrackComment(id, "ok", &refusal));
            assert(small.commit());
            const std::string after = readFile(again);
            const size_t lenPage = u32(bytes, 4);
            for (const uint32_t page : snapshotOf(bytes).pages) {
                const size_t base = lenPage * page;
                assert(bytes.substr(base + 24, 12) == after.substr(base + 24, 12)
                       && "a comment that fits leaves every page's rows where they are");
            }
            std::cout << "  case 3 (capacity " << capacity->units << " units taken, one more refused; in place) OK\n";
        }
    }

    // 4. A row not laid out as rekordbox lays one out (two slots naming one
    //    string): no capacity, the comment refused as unwritable rather
    //    than as "no room", the file left alone.
    {
        const fs::path pdbPath = copyIn(dir, seabass::pathFromUtf8(argv[1]), "shared.pdb");
        std::string bytes = readFile(pdbPath);
        const Snapshot snap = snapshotOf(bytes);
        const size_t lenPage = u32(bytes, 4);
        const size_t base = lenPage * snap.pages.front();
        // The page's first present row.
        size_t first = 0;
        while (!((u16(bytes, base + lenPage - 4) >> first) & 1)) {
            ++first;
        }
        const size_t row = base + 40 + u16(bytes, base + lenPage - 6 - 2 * first);
        const uint32_t id = u32(bytes, row + 72);
        put16(bytes, row + 94 + 2 * 16, u16(bytes, row + 94 + 2 * 15));
        writeFile(pdbPath, bytes);
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        assert(!writer.trackCommentCapacity(id));
        std::string refusal;
        assert(!writer.setTrackComment(id, "x", &refusal));
        assert(refusal.find("cannot be written here") != std::string::npos);
        assert(!writer.commit());
        assert(readFile(pdbPath) == bytes);
        std::cout << "case 4 (a row with a shared string slot is refused as unwritable) OK\n";
    }

    // 5. A track table that cannot take a new page (its last page does not
    //    lead to its empty candidate): the capacity is what the row's own
    //    page holds, a comment past it is refused and nothing changes.
    {
        const fs::path pdbPath = copyIn(dir, seabass::pathFromUtf8(argv[1]), "nopage.pdb");
        std::string bytes = readFile(pdbPath);
        const uint32_t numTables = u32(bytes, 8);
        for (uint32_t t = 0; t < numTables; ++t) {
            const size_t entry = 28 + 16 * t;
            if (u32(bytes, entry) == 0) {
                put32(bytes, entry + 4, u32(bytes, entry + 4) + 7);
            }
        }
        writeFile(pdbPath, bytes);
        const uint32_t id = snapshotOf(bytes).tracks.begin()->first;
        PdbRowWriter writer(seabass::pathToUtf8(pdbPath));
        const auto capacity = writer.trackCommentCapacity(id);
        assert(capacity && capacity->units < 1000);
        std::string refusal;
        assert(!writer.setTrackComment(id, asciiOf(1500, 'n'), &refusal));
        assert(!writer.commit());
        assert(readFile(pdbPath) == bytes);
        std::cout << "case 5 (no new page possible: capacity " << capacity->units << " units, a longer comment refused) OK\n";
    }

    fs::remove_all(dir);
    std::cout << "pdb_track_comment_test: all OK\n";
    return 0;
}
