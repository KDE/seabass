// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PdbRowWriter::appendPlaylistEntries (#62) against rekordbox's own
// appends. The committed one_stick_two_catalogs export has two pages
// whose last transaction was rekordbox adding one playlist entry (tx =
// (1, last row)): undoing that row by hand and appending the same track
// with Seabass must give rekordbox's page back, byte for byte. Then the
// rules around it: a new group, a track already there, and, against a
// reference export rekordbox made for this, a new page and a removal.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::rekordbox::PdbPageFull;
using seabass::infrastructure::rekordbox::PdbRowWriter;

namespace
{

constexpr size_t LenPage = 4096;

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

struct Counters
{
    uint32_t rowOffsets;
    uint32_t rows;
    uint16_t freeSize;
    uint16_t usedSize;
    uint16_t txCount;
    uint16_t txIndex;
};

Counters countersOf(const std::string &b, uint32_t page)
{
    const size_t base = LenPage * page;
    const uint32_t c = u32(b, base + 24) & 0xFFFFFF;
    return {c & 0x1FFF, c >> 13, u16(b, base + 28), u16(b, base + 30), u16(b, base + 32), u16(b, base + 34)};
}

// rekordbox's free-space rule, measured on four of its exports.
bool freeRuleHolds(const std::string &b, uint32_t page)
{
    const Counters c = countersOf(b, page);
    const uint32_t groups = c.rowOffsets == 0 ? 0 : (c.rowOffsets - 1) / 16 + 1;
    return c.freeSize == LenPage - 40 - c.usedSize - (groups * 4 + c.rowOffsets * 2);
}

// The page as it was before rekordbox appended its last row: the row's
// bytes, offset slot and flags gone, the counters one back. The
// transaction fields are left as rekordbox left them, which is what the
// append writes again.
std::string withoutLastRow(std::string b, uint32_t page)
{
    const size_t base = LenPage * page;
    const Counters c = countersOf(b, page);
    const uint32_t i = c.rowOffsets - 1;
    const size_t groupBase = base + LenPage - (i / 16) * 0x24;
    const uint32_t r = i % 16;
    std::fill(b.begin() + static_cast<std::ptrdiff_t>(base + 40 + i * 12),
              b.begin() + static_cast<std::ptrdiff_t>(base + 40 + i * 12 + 12), '\0');
    put16(b, groupBase - 6 - 2 * r, 0);
    put16(b, groupBase - 4, static_cast<uint16_t>(u16(b, groupBase - 4) & ~(1u << r)));
    if (r == 0) {
        put16(b, groupBase - 4, 0);
        put16(b, groupBase - 2, 0);
    }
    const uint32_t flags = u32(b, base + 24) & 0xFF000000u;
    put32(b, base + 24, flags | ((c.rows - 1) << 13) | (c.rowOffsets - 1));
    put16(b, base + 28, static_cast<uint16_t>(c.freeSize + 12 + 2 + (r == 0 ? 4 : 0)));
    put16(b, base + 30, static_cast<uint16_t>(c.usedSize - 12));
    return b;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: pdb_append_playlist_entries_test <one_stick_two_catalogs export.pdb>\n";
        return 2;
    }
    const std::string original = readFile(seabass::pathFromUtf8(argv[1]));
    const fs::path dir = seabass::testing::scratchRoot() / "seabass_pdb_append_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path pdb = dir / "export.pdb";

    // 1. rekordbox's own append, replayed: page 52 (a row starting... no,
    //    the 16th row of group 0) and page 18 (row 84, inside group 5).
    //    Page 52 is the table's last page, which is the only page the
    //    append writes, so page 18 is replayed by making it the last.
    for (const uint32_t page : {52u, 18u}) {
        std::string before = withoutLastRow(original, page);
        const size_t base = LenPage * page;
        const Counters rk = countersOf(original, page);
        const uint32_t i = rk.rowOffsets - 1;
        const uint32_t entryIndex = u32(original, base + 40 + i * 12);
        const uint32_t trackId = u32(original, base + 40 + i * 12 + 4);
        const uint32_t playlistId = u32(original, base + 40 + i * 12 + 8);
        // Point the table's last page at this one (entry 8 of 20 tables is
        // playlist_entries; its last_page is at 28 + 16 * 8 + 12).
        uint32_t table = 0;
        for (uint32_t t = 0; t < u32(original, 8); ++t) {
            if (u32(original, 28 + 16 * t) == 8) {
                table = t;
            }
        }
        put32(before, 28 + 16 * table + 12, page);
        writeFile(pdb, before);
        PdbRowWriter writer(seabass::pathToUtf8(pdb));
        assert(writer.appendPlaylistEntries(playlistId, {trackId}) == 1);
        assert(writer.commit());
        std::string after = readFile(pdb);
        // commit() stamps the page's and the header's sequence; rekordbox's
        // page carries its own. Compared apart from those two fields.
        put32(after, base + 16, u32(original, base + 16));
        assert(u32(after, base + 40 + i * 12) == entryIndex && "the entry index continues the playlist");
        const bool same = after.compare(base, LenPage, original, base, LenPage) == 0;
        if (!same) {
            for (size_t o = 0; o < LenPage; ++o) {
                if (after[base + o] != original[base + o]) {
                    std::cerr << "page " << page << " differs at +" << o << ": seabass " << int(static_cast<unsigned char>(after[base + o]))
                              << " rekordbox " << int(static_cast<unsigned char>(original[base + o])) << "\n";
                }
            }
        }
        assert(same && "Seabass's append is rekordbox's, byte for byte");
        std::cout << "case 1 (page " << page << ": rekordbox's own append replayed, byte for byte) OK\n";
    }

    // 2. Appending on the real last page: crossing into a new group of
    //    sixteen, counters by the rules, the transaction naming the last
    //    row added, and the file still parsing with the rows in order.
    {
        writeFile(pdb, original);
        const Counters c0 = countersOf(original, 52);
        assert(c0.rowOffsets == 16);
        PdbRowWriter writer(seabass::pathToUtf8(pdb));
        assert(writer.appendPlaylistEntries(4, {1, 2, 3}) == 3);
        assert(writer.commit());
        const std::string after = readFile(pdb);
        const Counters c = countersOf(after, 52);
        assert(c.rowOffsets == 19 && c.rows == 19 && c.usedSize == c0.usedSize + 36);
        assert(c.freeSize == c0.freeSize - (3 * 14 + 4) && freeRuleHolds(after, 52));
        assert(c.txCount == 1 && c.txIndex == 18 && "one transaction per row: the last one is named");
        const size_t base = LenPage * 52;
        assert(u16(after, base + LenPage - 2) == 0 && "group 0's transaction flags cleared");
        assert(u16(after, base + LenPage - 0x24 - 2) == 0x4 && "group 1's mark the last row only");
        assert(u16(after, base + LenPage - 0x24 - 4) == 0x7);
        std::istringstream iss(after);
        kaitai::kstream ks(&iss);
        rekordbox_pdb_t parsed(false, &ks);
        std::vector<std::pair<uint32_t, uint32_t>> playlist4;
        for (const auto &table : *parsed.tables()) {
            if (table->type() != rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES) {
                continue;
            }
            auto *page = table->first_page()->body();
            for (int guard = 0; guard < 100; ++guard) {
                if (page->is_data_page()) {
                    for (const auto &group : *page->row_groups()) {
                        for (const auto &row : *group->rows()) {
                            if (!row->present()) {
                                continue;
                            }
                            auto *e = dynamic_cast<rekordbox_pdb_t::playlist_entry_row_t *>(row->body());
                            if (e && e->playlist_id() == 4) {
                                playlist4.emplace_back(e->entry_index(), e->track_id());
                            }
                        }
                    }
                }
                if (page->page_index() == table->last_page()->index()) {
                    break;
                }
                page = page->next_page()->body();
            }
        }
        assert(playlist4.size() == 11);
        assert((playlist4[8] == std::pair<uint32_t, uint32_t>{9, 1}));
        assert((playlist4[10] == std::pair<uint32_t, uint32_t>{11, 3}));
        std::cout << "case 2 (a new group, the counters, the transaction, and the playlist in order) OK\n";
    }

    // 3. A track already in the playlist is not added again; nothing to
    //    add writes nothing.
    {
        writeFile(pdb, original);
        PdbRowWriter writer(seabass::pathToUtf8(pdb));
        const uint32_t member = u32(original, LenPage * 52 + 40 + 15 * 12 + 4);
        assert(writer.appendPlaylistEntries(4, {member}) == 0);
        assert(!writer.commit() && "no page was edited");
        std::cout << "case 3 (a track already there is not added twice) OK\n";
    }

    // 4. A last page that does not lead to the table's empty candidate:
    //    a new page is not started there, and nothing changes.
    {
        std::string bent = original;
        put32(bent, LenPage * 52 + 12, 12345);
        writeFile(pdb, bent);
        PdbRowWriter writer(seabass::pathToUtf8(pdb));
        std::vector<uint32_t> many;
        for (uint32_t id = 10000; id < 10400; ++id) {
            many.push_back(id);
        }
        bool refused = false;
        try {
            writer.appendPlaylistEntries(4, many);
        } catch (const PdbPageFull &) {
            refused = true;
        }
        assert(refused);
        assert(!writer.commit() && "nothing was edited");
        assert(readFile(pdb) == bent);
        std::cout << "case 4 (a last page not leading to the candidate is refused, nothing written) OK\n";
    }

    // 5. rekordbox's own deletion, replayed: the anonymized fixture's last
    //    playlist page is 104 rows rekordbox deleted in one transaction
    //    (num_rows 0, page_flags 0x34, transaction (104, 0), every row's
    //    transaction bit set). Restoring the rows and deleting the same
    //    entries with Seabass must give that page back.
    if (argc > 2) {
        const std::string anonymized = readFile(seabass::pathFromUtf8(argv[2]));
        const uint32_t page = 320;
        const size_t base = LenPage * page;
        const Counters rk = countersOf(anonymized, page);
        assert(rk.rowOffsets == 104 && rk.rows == 0 && "precondition: the page rekordbox emptied");
        std::string before = anonymized;
        for (uint32_t g = 0; g * 16 < rk.rowOffsets; ++g) {
            const uint32_t inGroup = std::min<uint32_t>(16, rk.rowOffsets - g * 16);
            put16(before, base + LenPage - g * 0x24 - 4, static_cast<uint16_t>((1u << inGroup) - 1));
        }
        put32(before, base + 24, (0x24u << 24) | (rk.rowOffsets << 13) | rk.rowOffsets);
        writeFile(pdb, before);
        PdbRowWriter writer(seabass::pathToUtf8(pdb));
        for (uint32_t i = 0; i < rk.rowOffsets; ++i) {
            const uint32_t trackId = u32(before, base + 40 + i * 12 + 4);
            const uint32_t playlistId = u32(before, base + 40 + i * 12 + 8);
            assert(writer.removePlaylistEntry(playlistId, trackId) >= 1);
        }
        assert(writer.commit());
        std::string after = readFile(pdb);
        put32(after, base + 16, u32(anonymized, base + 16));
        const bool same = after.compare(base, LenPage, anonymized, base, LenPage) == 0;
        if (!same) {
            for (size_t o = 0; o < LenPage; ++o) {
                if (after[base + o] != anonymized[base + o]) {
                    std::cerr << "page " << page << " differs at +" << o << ": seabass "
                              << int(static_cast<unsigned char>(after[base + o])) << " rekordbox "
                              << int(static_cast<unsigned char>(anonymized[base + o])) << "\n";
                }
            }
        }
        assert(same && "Seabass's deletion is rekordbox's, byte for byte");
        std::cout << "case 5 (rekordbox's own deletion of 104 entries replayed, byte for byte) OK\n";
    }

    // 6..8 need rekordbox's reference export (tests/fixtures/pdb_new_page).
    if (argc > 3) {
        const fs::path ref = seabass::pathFromUtf8(argv[3]);
        const std::string eight = readFile(ref / "1-eight-playlists.pdb");
        const std::string nine = readFile(ref / "2-ninth-playlist.pdb");
        const std::string removed = readFile(ref / "3-one-entry-removed.pdb");
        // Seabass's file against rekordbox's, apart from the sequence
        // numbers (the header's and every page's) and the pages rekordbox
        // changed for reasons of its own (the playlist tree, page 16).
        const auto sameAsRekordbox = [](std::string seabass, const std::string &rekordbox, const char *what) {
            assert(seabass.size() == rekordbox.size());
            put32(seabass, 20, u32(rekordbox, 20));
            for (size_t p = 1; p * LenPage < seabass.size(); ++p) {
                put32(seabass, p * LenPage + 16, u32(rekordbox, p * LenPage + 16));
            }
            bool same = true;
            for (size_t o = 0; o < seabass.size(); ++o) {
                if (o / LenPage != 16 && seabass[o] != rekordbox[o]) {
                    std::cerr << what << ": page " << o / LenPage << " differs at +" << o % LenPage << ": seabass "
                              << int(static_cast<unsigned char>(seabass[o])) << " rekordbox "
                              << int(static_cast<unsigned char>(rekordbox[o])) << "\n";
                    same = false;
                }
            }
            return same;
        };

        // 6. rekordbox's ninth playlist, replayed: 34 entries that do not
        //    all fit on page 18 go on a new page, the table's empty
        //    candidate 54, and the file header and table follow.
        {
            std::vector<uint32_t> tracks;
            for (uint32_t i = 0; i < 34; ++i) {
                assert(u32(nine, LenPage * 54 + 40 + i * 12 + 8) == 9);
                tracks.push_back(u32(nine, LenPage * 54 + 40 + i * 12 + 4));
            }
            writeFile(pdb, eight);
            PdbRowWriter writer(seabass::pathToUtf8(pdb));
            assert(writer.appendPlaylistEntries(9, tracks) == 34);
            assert(writer.commit());
            assert(sameAsRekordbox(readFile(pdb), nine, "case 6") && "Seabass's new page is rekordbox's, byte for byte");
            std::cout << "case 6 (rekordbox's new playlist page replayed, byte for byte) OK\n";
        }

        // 7. rekordbox taking d05 (track 26) out of P09, replayed: the
        //    playlist's rows deleted, the other 33 added again from
        //    entry_index 1, and page 54 listed on the index page.
        {
            writeFile(pdb, nine);
            PdbRowWriter writer(seabass::pathToUtf8(pdb));
            assert(writer.removePlaylistEntries(9, {26}) == 1);
            assert(writer.commit());
            assert(sameAsRekordbox(readFile(pdb), removed, "case 7") && "Seabass's removal is rekordbox's, byte for byte");
            PdbRowWriter again(seabass::pathToUtf8(pdb));
            assert(again.removePlaylistEntries(9, {26}) == 0 && "a track not in the playlist removes nothing");
            assert(!again.commit());
            std::cout << "case 7 (rekordbox's removal of one entry replayed, byte for byte) OK\n";
        }
    }

    // 8. More entries than two pages hold, with the empty candidate past
    //    the end of the file (page 67 of a 66-page file): two new pages,
    //    the file grown, the page between them zeros, the playlist
    //    readable in order with its numbering contiguous.
    {
        writeFile(pdb, original);
        const uint32_t pagesBefore = static_cast<uint32_t>(original.size() / LenPage);
        uint32_t table = 0;
        for (uint32_t t = 0; t < u32(original, 8); ++t) {
            if (u32(original, 28 + 16 * t) == 8) {
                table = t;
            }
        }
        const uint32_t candidate = u32(original, 28 + 16 * table + 4);
        const uint32_t nextUnused = u32(original, 12);
        assert(candidate > pagesBefore && "precondition: the candidate lies past the end of the file");
        std::vector<uint32_t> many;
        for (uint32_t id = 10000; id < 10400; ++id) {
            many.push_back(id);
        }
        PdbRowWriter writer(seabass::pathToUtf8(pdb));
        assert(writer.appendPlaylistEntries(4, many) == 400);
        assert(writer.commit());
        const std::string after = readFile(pdb);
        assert(after.size() == LenPage * (nextUnused + 1));
        assert(u32(after, 28 + 16 * table + 12) == nextUnused && "the second new page is the last");
        assert(u32(after, 28 + 16 * table + 4) == nextUnused + 1 && u32(after, 12) == nextUnused + 2);
        assert(u32(after, LenPage * candidate + 12) == nextUnused);
        assert(after.compare(LenPage * pagesBefore, LenPage, std::string(LenPage, '\0')) == 0);
        const Counters first = countersOf(after, candidate);
        const Counters second = countersOf(after, nextUnused);
        assert(first.rows == 284 && second.rows == 116 && freeRuleHolds(after, candidate) && freeRuleHolds(after, nextUnused));
        assert(first.txCount == 1 && first.txIndex == 283 && second.txCount == 1 && second.txIndex == 115);
        std::istringstream iss(after);
        kaitai::kstream ks(&iss);
        rekordbox_pdb_t parsed(false, &ks);
        std::vector<uint32_t> indices;
        for (const auto &t : *parsed.tables()) {
            if (t->type() != rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES) {
                continue;
            }
            auto *page = t->first_page()->body();
            for (int guard = 0; guard < 200; ++guard) {
                if (page->is_data_page()) {
                    for (const auto &group : *page->row_groups()) {
                        for (const auto &row : *group->rows()) {
                            auto *e = row->present() ? dynamic_cast<rekordbox_pdb_t::playlist_entry_row_t *>(row->body()) : nullptr;
                            if (e && e->playlist_id() == 4) {
                                indices.push_back(e->entry_index());
                            }
                        }
                    }
                }
                if (page->page_index() == t->last_page()->index()) {
                    break;
                }
                page = page->next_page()->body();
            }
        }
        assert(indices.size() == 408);
        for (size_t i = 0; i < indices.size(); ++i) {
            assert(indices[i] == i + 1);
        }
        std::cout << "case 8 (two new pages past the end of the file, the playlist in order) OK\n";
    }

    fs::remove_all(dir);
    std::cout << "pdb_append_playlist_entries_test: all cases passed\n";
    return 0;
}
