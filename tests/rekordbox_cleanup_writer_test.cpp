// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/little_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cleanup_writer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;
using Pdb = rekordbox_pdb_t;

namespace
{

constexpr uint32_t LenPage = 512;

// Same shape/layout discipline as pdb_row_writer_test.cpp's own
// fixture builder (see that file for the byte-offset derivation this
// mirrors): tracks 100 and 101, and playlist_entries (playlist=1,
// track=100), (playlist=1, track=200-does-not-exist-as-a-track,
// but that's fine -- playlist_entries reference track ids without a
// hard foreign key at this level, matching how a stale-but-plausible
// duplicate scenario looks), (playlist=2, track=101).
std::string buildSyntheticPdb()
{
    std::string buf(static_cast<size_t>(LenPage) * 3, '\0');

    writeU32LE(buf, 4, LenPage);
    writeU32LE(buf, 8, 2);
    writeU32LE(buf, 20, 5);
    writeU32LE(buf, 28, static_cast<uint32_t>(Pdb::PAGE_TYPE_TRACKS));
    writeU32LE(buf, 36, 1);
    writeU32LE(buf, 40, 1);
    writeU32LE(buf, 44, static_cast<uint32_t>(Pdb::PAGE_TYPE_PLAYLIST_ENTRIES));
    writeU32LE(buf, 52, 2);
    writeU32LE(buf, 56, 2);

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
    constexpr size_t TrackRowSize = 136;
    constexpr size_t TrackIdFieldOffset = 72;

    size_t page1 = LenPage * 1;
    writePageHeader(page1, 1, Pdb::PAGE_TYPE_TRACKS, 2);
    setRowPresent(page1, 0b11);
    setRowOfs(page1, 0, 0);
    setRowOfs(page1, 1, TrackRowSize);
    writeU32LE(buf, page1 + HeapStart + 0, 100);
    writeU32LE(buf, page1 + HeapStart + TrackIdFieldOffset, 100);
    writeU32LE(buf, page1 + HeapStart + TrackRowSize + TrackIdFieldOffset, 101);

    size_t page2 = LenPage * 2;
    writePageHeader(page2, 2, Pdb::PAGE_TYPE_PLAYLIST_ENTRIES, 2);
    setRowPresent(page2, 0b11);
    setRowOfs(page2, 0, 0);
    setRowOfs(page2, 1, 12);
    constexpr size_t EntryRowSize = 12;
    auto writeEntryRow = [&](size_t rowStart, uint32_t entryIndex, uint32_t trackId, uint32_t playlistId) {
        writeU32LE(buf, rowStart + 0, entryIndex);
        writeU32LE(buf, rowStart + 4, trackId);
        writeU32LE(buf, rowStart + 8, playlistId);
    };
    writeEntryRow(page2 + HeapStart + 0 * EntryRowSize, 0, 100, 1);
    writeEntryRow(page2 + HeapStart + 1 * EntryRowSize, 0, 101, 2);

    return buf;
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

struct ReadBack
{
    std::vector<uint32_t> presentTrackIds;
    std::vector<std::pair<uint32_t, uint32_t>> presentEntries;  // (playlistId, trackId)
};

ReadBack readBack(const fs::path &path)
{
    ReadBack result;
    std::ifstream ifs(path, std::ifstream::binary);
    kaitai::kstream ks(&ifs);
    Pdb pdb(false, &ks);
    for (const auto &table : *pdb.tables()) {
        if (table->type() == Pdb::PAGE_TYPE_TRACKS) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) continue;
                        auto *t = dynamic_cast<Pdb::track_row_t *>(row->body());
                        if (t) result.presentTrackIds.push_back(t->id());
                    }
                }
            });
        } else if (table->type() == Pdb::PAGE_TYPE_PLAYLIST_ENTRIES) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) continue;
                        auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(row->body());
                        if (e) result.presentEntries.emplace_back(e->playlist_id(), e->track_id());
                    }
                }
            });
        }
    }
    return result;
}

bool containsEntry(const std::vector<std::pair<uint32_t, uint32_t>> &v, uint32_t playlistId, uint32_t trackId)
{
    for (const auto &e : v) {
        if (e.first == playlistId && e.second == trackId) return true;
    }
    return false;
}

}  // namespace

int main()
{
    fs::path scratchRoot = seabass::testing::scratchRoot() / "seabass_rekordbox_cleanup_writer_test";
    fs::remove_all(scratchRoot);
    fs::create_directories(scratchRoot / "rekordbox");
    fs::path pdbPath = scratchRoot / "rekordbox" / "export.pdb";

    std::string pristine = buildSyntheticPdb();

    // Track 100 (playlist 1) is removed, replaced by track 101 --
    // playlist 1 didn't already have 101, so its entry gets repointed;
    // playlist 2 (which already had 101) is untouched.
    {
        writeFile(pdbPath, pristine);
        RekordboxCleanupWriter writer(scratchRoot.string());
        writer.removeTrackReplacingWith("100", "101");

        auto rb = readBack(pdbPath);
        assert(rb.presentTrackIds.size() == 1);
        assert(rb.presentTrackIds[0] == 101);
        assert(containsEntry(rb.presentEntries, 1, 101));  // repointed
        assert(containsEntry(rb.presentEntries, 2, 101));  // untouched
        assert(rb.presentEntries.size() == 2);
        std::cout << "case 1 (removeTrackReplacingWith: removes track, fixes up playlist membership) OK\n";
    }

    // Not-found cases throw (doomed id, then survivor id).
    {
        writeFile(pdbPath, pristine);
        RekordboxCleanupWriter writer(scratchRoot.string());
        bool threw = false;
        try {
            writer.removeTrackReplacingWith("999999", "101");
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            writer.removeTrackReplacingWith("100", "999999");
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 2 (nonexistent doomed/survivor id throws) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
