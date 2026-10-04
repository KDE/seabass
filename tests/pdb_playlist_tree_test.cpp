// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// PdbRowWriter's playlist tree (#62): creating, reordering and deleting
// playlists and folders, against rekordbox 7.2.18 doing the same on a stick
// (tests/fixtures/pdb_playlist_tree, one export.pdb per step). The first
// playlist on a stick is replayed byte for byte. After that rekordbox's
// choice of page varies from step to step, so each later step is compared
// by what it holds: the tree (ids, parents, sort orders, folders, names),
// every playlist's entries in order, and the encoded rows themselves.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::rekordbox::PdbRowWriter;

namespace
{

constexpr size_t LenPage = 4096;
const std::string LongName = "T\xc3\xabst \xc3\x9cn\xc3\xaf" "code Playlist with a name long enough that it cannot be a short device "
                             "string, which tops out at one hundred and twenty six bytes";

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const fs::path &p, const std::string &bytes)
{
    std::ofstream(p, std::ios::binary | std::ios::trunc) << bytes;
}

uint32_t u32(const std::string &b, size_t o)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = v << 8 | static_cast<unsigned char>(b[o + static_cast<size_t>(i)]);
    }
    return v;
}

void put32(std::string &b, size_t o, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b[o + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
    }
}

using Tree = std::vector<std::tuple<uint32_t, uint32_t, uint32_t, bool, std::string>>;

Tree treeOf(const std::string &path)
{
    Tree t;
    for (const auto &n : PdbRowWriter(path).playlistTree()) {
        t.emplace_back(n.id, n.parentId, n.sortOrder, n.isFolder, n.name);
    }
    std::sort(t.begin(), t.end());
    return t;
}

// Every playlist's track ids in entry order, by reading the rows.
std::map<uint32_t, std::vector<uint32_t>> entriesOf(const std::string &bytes)
{
    std::map<uint32_t, std::map<uint32_t, uint32_t>> byIndex;
    for (size_t p = 1; p * LenPage < bytes.size(); ++p) {
        const size_t base = p * LenPage;
        if (u32(bytes, base + 8) != 8 || (static_cast<unsigned char>(bytes[base + 27]) & 0x40) != 0) {
            continue;
        }
        const uint32_t n = u32(bytes, base + 24) & 0x1FFF;
        for (uint32_t i = 0; i < n; ++i) {
            const size_t groupBase = base + LenPage - (i / 16) * 0x24;
            const uint32_t present = u32(bytes, groupBase - 4) & 0xFFFF;
            if (((present >> (i % 16)) & 1) == 0) {
                continue;
            }
            const size_t heap = u32(bytes, groupBase - 6 - 2 * (i % 16)) & 0xFFFF;
            const size_t row = base + 40 + heap;
            byIndex[u32(bytes, row + 8)][u32(bytes, row)] = u32(bytes, row + 4);
        }
    }
    std::map<uint32_t, std::vector<uint32_t>> out;
    for (const auto &[playlist, rows] : byIndex) {
        uint32_t expected = 1;
        for (const auto &[index, track] : rows) {
            assert(index == expected++ && "entry_index contiguous from 1");
            out[playlist].push_back(track);
        }
    }
    return out;
}

// The bytes of the present playlist_tree row with this id.
std::string treeRowBytes(const std::string &bytes, uint32_t id)
{
    for (size_t p = 1; p * LenPage < bytes.size(); ++p) {
        const size_t base = p * LenPage;
        if (u32(bytes, base + 8) != 7 || (static_cast<unsigned char>(bytes[base + 27]) & 0x40) != 0) {
            continue;
        }
        const uint32_t n = u32(bytes, base + 24) & 0x1FFF;
        std::vector<size_t> offsets;
        for (uint32_t i = 0; i < n; ++i) {
            offsets.push_back(u32(bytes, base + LenPage - (i / 16) * 0x24 - 6 - 2 * (i % 16)) & 0xFFFF);
        }
        for (uint32_t i = 0; i < n; ++i) {
            const size_t groupBase = base + LenPage - (i / 16) * 0x24;
            if (((u32(bytes, groupBase - 4) >> (i % 16)) & 1) == 0 || u32(bytes, base + 40 + offsets[i] + 12) != id) {
                continue;
            }
            const size_t end = i + 1 < n ? offsets[i + 1] : (u32(bytes, base + 30) & 0xFFFF);
            return bytes.substr(base + 40 + offsets[i], end - offsets[i]);
        }
    }
    return {};
}

// rekordbox's file against Seabass's apart from sequence numbers.
bool sameApartFromSequences(std::string seabass, const std::string &rekordbox)
{
    if (seabass.size() != rekordbox.size()) {
        std::cerr << "sizes differ: " << seabass.size() << " vs " << rekordbox.size() << "\n";
        return false;
    }
    put32(seabass, 20, u32(rekordbox, 20));
    for (size_t p = 1; p * LenPage < seabass.size(); ++p) {
        put32(seabass, p * LenPage + 16, u32(rekordbox, p * LenPage + 16));
    }
    bool same = true;
    for (size_t o = 0; o < seabass.size(); ++o) {
        if (seabass[o] != rekordbox[o]) {
            std::cerr << "page " << o / LenPage << " differs at +" << o % LenPage << ": seabass "
                      << int(static_cast<unsigned char>(seabass[o])) << " rekordbox " << int(static_cast<unsigned char>(rekordbox[o]))
                      << "\n";
            same = false;
        }
    }
    return same;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: pdb_playlist_tree_test <tests/fixtures/pdb_playlist_tree>\n";
        return 2;
    }
    const fs::path ref = seabass::pathFromUtf8(argv[1]);
    const auto rk = [&](const char *name) { return readFile(ref / name); };
    const fs::path dir = seabass::testing::scratchRoot() / "seabass_pdb_playlist_tree_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "rekordbox");
    const fs::path pdb = dir / "rekordbox" / "export.pdb";
    const std::string path = seabass::pathToUtf8(pdb);

    // 1. The first playlist on a stick, replayed: Q1 with a01..a05 (track
    //    ids 1..5) on a stick with none. Both tables start their first
    //    data page, and the file is rekordbox's byte for byte.
    writeFile(pdb, rk("0-base.pdb"));
    {
        PdbRowWriter w(path);
        assert(w.createPlaylist(0, "Q1", false) == 1);
        assert(w.appendPlaylistEntries(1, {1, 2, 3, 4, 5}) == 5);
        assert(w.commit());
    }
    assert(sameApartFromSequences(readFile(pdb), rk("1-create-q1.pdb")) && "the first playlist is rekordbox's, byte for byte");
    std::cout << "case 1 (the first playlist on a stick, byte for byte) OK\n";

    // 2. A second playlist with a 137-character Unicode name, first in
    //    the top level as rekordbox put it: the same tree, the same
    //    entries, and its row (UTF-16 name and padding) rekordbox's bytes.
    {
        PdbRowWriter w(path);
        assert(w.createPlaylist(0, LongName, false, 0) == 2);
        assert(w.appendPlaylistEntries(2, {6, 7, 8}) == 3);
        assert(w.commit());
    }
    {
        const std::string mine = readFile(pdb);
        const std::string theirs = rk("2-long-name.pdb");
        assert(treeOf(path) == treeOf(seabass::pathToUtf8(ref / "2-long-name.pdb")));
        assert(entriesOf(mine) == entriesOf(theirs));
        assert(!treeRowBytes(theirs, 2).empty() && treeRowBytes(mine, 2) == treeRowBytes(theirs, 2));
        assert(treeRowBytes(mine, 1) == treeRowBytes(theirs, 1));
    }
    std::cout << "case 2 (a long Unicode name: tree, entries and row bytes as rekordbox's) OK\n";

    // 3. A folder holding a playlist, the folder first in the top level.
    {
        PdbRowWriter w(path);
        assert(w.createPlaylist(0, "F1", true, 0) == 3);
        assert(w.createPlaylist(3, "F1A", false) == 4);
        assert(w.appendPlaylistEntries(4, {13, 14}) == 2);
        bool refused = false;
        try {
            w.createPlaylist(4, "inside a playlist", false);
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused && "only a folder takes children");
        assert(w.commit());
    }
    assert(treeOf(path) == treeOf(seabass::pathToUtf8(ref / "3-folder.pdb")));
    assert(entriesOf(readFile(pdb)) == entriesOf(rk("3-folder.pdb")));
    {
        const auto ids = seabass::infrastructure::rekordbox::rekordboxPlaylistIdsByPath(seabass::pathToUtf8(dir));
        assert(ids.count("F1/F1A") && ids.at("F1/F1A") == 4 && ids.count("Q1") && ids.count(LongName));
    }
    std::cout << "case 3 (a folder with a playlist, read back by the app's own reader) OK\n";

    // 4. a05 to the top of Q1. The same order is no change; an order that
    //    is not the playlist's tracks is refused.
    {
        PdbRowWriter w(path);
        assert(!w.reorderPlaylist(1, {1, 2, 3, 4, 5}));
        bool refused = false;
        try {
            w.reorderPlaylist(1, {5, 1, 2, 3, 3});
        } catch (const std::invalid_argument &) {
            refused = true;
        }
        assert(refused);
        assert(w.reorderPlaylist(1, {5, 1, 2, 3, 4}));
        assert(w.commit());
    }
    assert(entriesOf(readFile(pdb)) == entriesOf(rk("4-reorder.pdb")));
    std::cout << "case 4 (reordering a playlist, as rekordbox did) OK\n";

    // 5. Deleting Q1: its entries go and the top level closes up. rekordbox
    //    had also moved Q1 to the top while reordering; with Q1 gone the
    //    tree is the same either way.
    {
        PdbRowWriter w(path);
        assert(w.deletePlaylist(1) == 1);
        assert(w.deletePlaylist(1) == 0);
        assert(w.commit());
    }
    assert(treeOf(path) == treeOf(seabass::pathToUtf8(ref / "5-delete.pdb")));
    assert(entriesOf(readFile(pdb)) == entriesOf(rk("5-delete.pdb")));
    std::cout << "case 5 (deleting a playlist, as rekordbox did) OK\n";

    // 6. Deleting the folder takes its playlist and that playlist's
    //    entries with it.
    {
        PdbRowWriter w(path);
        assert(w.deletePlaylist(3) == 2);
        assert(w.commit());
    }
    assert(treeOf(path) == treeOf(seabass::pathToUtf8(ref / "6-delete-folder.pdb")));
    assert(entriesOf(readFile(pdb)) == entriesOf(rk("6-delete-folder.pdb")));
    std::cout << "case 6 (deleting a folder with its playlist, as rekordbox did) OK\n";

    // 7. A new playlist after deletions takes a fresh id, never a removed
    //    one, and a long ASCII name is a long ASCII string.
    {
        PdbRowWriter w(path);
        const std::string longAscii(200, 'x');
        assert(w.createPlaylist(0, longAscii, false) == 5);
        assert(w.commit());
        const std::string row = treeRowBytes(readFile(pdb), 5);
        assert(row.size() >= 224 && static_cast<unsigned char>(row[20]) == 0x40 && u32(row, 21) % 0x10000 == 204);
    }
    std::cout << "case 7 (no reused ids; a long ASCII name) OK\n";

    fs::remove_all(dir);
    std::cout << "pdb_playlist_tree_test: all cases passed\n";
    return 0;
}
