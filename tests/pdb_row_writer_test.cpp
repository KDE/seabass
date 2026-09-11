// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

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

// A minimal, but structurally real, export.pdb: a root header with two
// tables (tracks, playlist_entries), each a single data page holding a
// handful of rows in one row_group. Byte offsets below are derived
// directly from specs/rekordbox_pdb.ksy's seq layout (root header:
// unknown/len_page/num_tables/next_unused_page/unknown/sequence/gap =
// 28 bytes, then table_t entries of 16 bytes each; page header: 40
// bytes; row_group tail builds backward from the page end) plus, for
// the bit-packed num_row_offsets(13)/num_rows(11) pair, the *exact*
// little-endian bit-packing traced from the real kaitai runtime's
// read_bits_int_le() (third_party/kaitai_struct_cpp_stl_runtime/kaitai/
// kaitaistream.cpp) rather than guessed from the spec's prose alone:
// the two fields pack into a 24-bit little-endian word as
// `numRowOffsets | (numRows << 13)`.
//
// Page 1 (tracks): rows with id=100, id=101.
// Page 2 (playlist_entries): (playlist=1, track=100), (playlist=1,
// track=200), (playlist=2, track=101).
std::string buildSyntheticPdb()
{
    std::string buf(static_cast<size_t>(LenPage) * 3, '\0');

    // Root header.
    writeU32LE(buf, 4, LenPage);
    writeU32LE(buf, 8, 2);   // num_tables
    writeU32LE(buf, 20, 5);  // sequence
    // table[0]: tracks, pages start/end at page index 1.
    writeU32LE(buf, 28, static_cast<uint32_t>(Pdb::PAGE_TYPE_TRACKS));
    writeU32LE(buf, 36, 1);  // first_page.index
    writeU32LE(buf, 40, 1);  // last_page.index
    // table[1]: playlist_entries, pages start/end at page index 2.
    writeU32LE(buf, 44, static_cast<uint32_t>(Pdb::PAGE_TYPE_PLAYLIST_ENTRIES));
    writeU32LE(buf, 52, 2);
    writeU32LE(buf, 56, 2);

    // Field-by-field cumulative offsets within the 40-byte page header
    // (gap 0-3, page_index 4-7, type 8-11, next_page 12-15, sequence
    // 16-19, unnamed 20-23, packed num_row_offsets/num_rows 24-26,
    // page_flags 27, free_size 28-29, used_size 30-31,
    // transaction_row_count 32-33, transaction_row_index 34-35, two
    // unnamed u2 36-39).
    auto packRowCounts = [](std::string &b, size_t pageStart, uint16_t numRowOffsets, uint16_t numRows) {
        uint32_t packed = (static_cast<uint32_t>(numRowOffsets) & 0x1FFF) |
                           ((static_cast<uint32_t>(numRows) & 0x7FF) << 13);
        b[pageStart + 24] = static_cast<char>(packed & 0xFF);
        b[pageStart + 25] = static_cast<char>((packed >> 8) & 0xFF);
        b[pageStart + 26] = static_cast<char>((packed >> 16) & 0xFF);
    };
    auto writePageHeader = [&](size_t pageStart, uint32_t pageIndex, Pdb::page_type_t type, uint16_t numRows) {
        writeU32LE(buf, pageStart + 4, pageIndex);
        writeU32LE(buf, pageStart + 8, static_cast<uint32_t>(type));
        writeU32LE(buf, pageStart + 12, pageIndex);  // next_page == self (only/last page)
        writeU32LE(buf, pageStart + 16, 5);          // page.sequence -- matches header's initial sequence
        buf[pageStart + 27] = static_cast<char>(0x24);  // page_flags: a real observed "normal" value (is_data_page)
        packRowCounts(buf, pageStart, numRows, numRows);
    };
    // Row-group tail (single group, group_index 0): base = LenPage
    // (page-relative). row_present_flags at base-4; row N's ofs_row at
    // base-(6+2N).
    auto setRowPresent = [&](size_t pageStart, uint16_t presentMask) {
        writeU16LE(buf, pageStart + LenPage - 4, presentMask);
    };
    auto setRowOfs = [&](size_t pageStart, int rowIndex, uint16_t ofsFromHeap) {
        writeU16LE(buf, pageStart + LenPage - (6 + 2 * static_cast<size_t>(rowIndex)), ofsFromHeap);
    };

    constexpr size_t HeapStart = 40;  // page header size, same for every page type

    // --- Page 1: tracks ---
    size_t page1 = LenPage * 1;
    writePageHeader(page1, 1, Pdb::PAGE_TYPE_TRACKS, 2);
    setRowPresent(page1, 0b11);
    setRowOfs(page1, 0, 0);
    setRowOfs(page1, 1, 136);  // track_row's fixed body is 136 bytes (see below)

    constexpr size_t TrackRowSize = 136;
    constexpr size_t TrackIdFieldOffset = 72;  // 17 u4 fields (68 bytes) + 2 u2 fields (4 bytes) before `id`
    auto writeTrackRow = [&](size_t rowStart, uint32_t id) { writeU32LE(buf, rowStart + TrackIdFieldOffset, id); };
    writeTrackRow(page1 + HeapStart + 0, 100);
    writeTrackRow(page1 + HeapStart + TrackRowSize, 101);

    // --- Page 2: playlist_entries ---
    size_t page2 = LenPage * 2;
    writePageHeader(page2, 2, Pdb::PAGE_TYPE_PLAYLIST_ENTRIES, 3);
    setRowPresent(page2, 0b111);
    setRowOfs(page2, 0, 0);
    setRowOfs(page2, 1, 12);
    setRowOfs(page2, 2, 24);

    constexpr size_t EntryRowSize = 12;
    auto writeEntryRow = [&](size_t rowStart, uint32_t entryIndex, uint32_t trackId, uint32_t playlistId) {
        writeU32LE(buf, rowStart + 0, entryIndex);
        writeU32LE(buf, rowStart + 4, trackId);
        writeU32LE(buf, rowStart + 8, playlistId);
    };
    writeEntryRow(page2 + HeapStart + 0 * EntryRowSize, 0, 100, 1);
    writeEntryRow(page2 + HeapStart + 1 * EntryRowSize, 1, 200, 1);
    writeEntryRow(page2 + HeapStart + 2 * EntryRowSize, 0, 101, 2);

    return buf;
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Independent verification, deliberately not reusing anything from
// PdbRowWriter's own internals: re-parses the file fresh with the real
// generated parser and reports which track ids and (playlist,track)
// entries are present. This is what proves an edit actually did (and
// only did) what it claims.
struct TrackFields
{
    uint32_t id = 0;
    uint32_t keyId = 0;
    uint32_t tempo = 0;
    uint32_t artworkId = 0;
};

struct ReadBack
{
    std::vector<uint32_t> presentTrackIds;
    std::vector<std::pair<uint32_t, uint32_t>> presentEntries;  // (playlistId, trackId)
    std::vector<uint32_t> presentEntryTrackIdsForPlaylist1;
    std::vector<TrackFields> trackFields;
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
                        if (!row->present()) {
                            continue;
                        }
                        auto *t = dynamic_cast<Pdb::track_row_t *>(row->body());
                        if (t) {
                            result.presentTrackIds.push_back(t->id());
                            result.trackFields.push_back(
                                TrackFields{t->id(), t->key_id(), t->tempo(), t->artwork_id()});
                        }
                    }
                }
            });
        } else if (table->type() == Pdb::PAGE_TYPE_PLAYLIST_ENTRIES) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(row->body());
                        if (e) {
                            result.presentEntries.emplace_back(e->playlist_id(), e->track_id());
                        }
                    }
                }
            });
        }
    }
    return result;
}

bool contains(const std::vector<uint32_t> &v, uint32_t x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

bool containsEntry(const std::vector<std::pair<uint32_t, uint32_t>> &v, uint32_t playlistId, uint32_t trackId)
{
    for (const auto &e : v) {
        if (e.first == playlistId && e.second == trackId) {
            return true;
        }
    }
    return false;
}

uint32_t pageSequence(const std::string &buf, uint32_t pageIndex)
{
    return readU32LE(buf, static_cast<size_t>(LenPage) * pageIndex + 16);
}

std::optional<TrackFields> findTrackFields(const ReadBack &rb, uint32_t id)
{
    for (const auto &f : rb.trackFields) {
        if (f.id == id) {
            return f;
        }
    }
    return std::nullopt;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_pdb_row_writer_test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path pdbPath = root / "export.pdb";

    std::string pristine = buildSyntheticPdb();

    // Self-check: the fixture itself reads back exactly as designed
    // before we ever test a mutation against it.
    {
        writeFile(pdbPath, pristine);
        auto rb = readBack(pdbPath);
        assert(rb.presentTrackIds.size() == 2);
        assert(contains(rb.presentTrackIds, 100));
        assert(contains(rb.presentTrackIds, 101));
        assert(rb.presentEntries.size() == 3);
        assert(containsEntry(rb.presentEntries, 1, 100));
        assert(containsEntry(rb.presentEntries, 1, 200));
        assert(containsEntry(rb.presentEntries, 2, 101));
        std::cout << "case 1 (synthetic fixture reads back as designed) OK\n";
    }

    // removeTrack: clears the track, leaves the other track and every
    // playlist entry untouched, bumps sequence numbers correctly.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        assert(writer.removeTrack(100));
        assert(writer.commit());

        auto rb = readBack(pdbPath);
        assert(!contains(rb.presentTrackIds, 100));
        assert(contains(rb.presentTrackIds, 101));
        assert(rb.presentEntries.size() == 3);  // playlist_entries page untouched

        std::string after = readFile(pdbPath);
        assert(after.size() == pristine.size());
        assert(pageSequence(after, 1) == 5);   // the edited (tracks) page got the old header sequence
        assert(readU32LE(after, 20) == 6);     // header sequence incremented
        assert(pageSequence(after, 2) == 5);   // untouched page's sequence unchanged
        std::cout << "case 2 (removeTrack: clears one track, sequence bump correct) OK\n";
    }

    // removePlaylistEntry: clears exactly the targeted (playlist, track)
    // pair, leaves the others (including a same-playlist, different-track
    // entry) untouched.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        bool removedEntry = writer.removePlaylistEntry(1, 200);
        assert(removedEntry);
        assert(writer.commit());

        auto rb = readBack(pdbPath);
        assert(rb.presentTrackIds.size() == 2);  // tracks page untouched
        assert(!containsEntry(rb.presentEntries, 1, 200));
        assert(containsEntry(rb.presentEntries, 1, 100));
        assert(containsEntry(rb.presentEntries, 2, 101));
        std::cout << "case 3 (removePlaylistEntry: clears exactly the targeted entry) OK\n";
    }

    // repointPlaylistEntry: rewrites track_id in place; entry_index and
    // playlist_id (and every other row) are untouched.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        bool repointed = writer.repointPlaylistEntry(1, 200, 999);
        assert(repointed);
        assert(writer.commit());

        auto rb = readBack(pdbPath);
        assert(!containsEntry(rb.presentEntries, 1, 200));
        assert(containsEntry(rb.presentEntries, 1, 999));
        assert(containsEntry(rb.presentEntries, 1, 100));
        assert(containsEntry(rb.presentEntries, 2, 101));
        assert(rb.presentEntries.size() == 3);  // repoint, not a delete+insert -- count unchanged
        std::cout << "case 4 (repointPlaylistEntry: rewrites track_id in place) OK\n";
    }

    // reassignPlaylistMemberships: track 100 is only in playlist 1;
    // track 101 is only in playlist 2 -- neither playlist already has
    // the other, so the (1,100) entry gets repointed to (1,101).
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        size_t reassignedCount1 = writer.reassignPlaylistMemberships(100, 101);
        assert(reassignedCount1 == 1);
        assert(writer.commit());

        auto rb = readBack(pdbPath);
        assert(!containsEntry(rb.presentEntries, 1, 100));
        assert(containsEntry(rb.presentEntries, 1, 101));  // repointed
        assert(containsEntry(rb.presentEntries, 1, 200));  // untouched
        assert(containsEntry(rb.presentEntries, 2, 101));  // untouched
        assert(rb.presentEntries.size() == 3);
        std::cout << "case 4b (reassignPlaylistMemberships: repoints when target playlist lacks the survivor) OK\n";
    }

    // reassignPlaylistMemberships: track 200 is in playlist 1; track
    // 100 is ALSO already in playlist 1 -- so the (1,200) entry must be
    // dropped, not repointed (no duplicate (1,100) entries).
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        size_t reassignedCount2 = writer.reassignPlaylistMemberships(200, 100);
        assert(reassignedCount2 == 1);
        assert(writer.commit());

        auto rb = readBack(pdbPath);
        assert(!containsEntry(rb.presentEntries, 1, 200));
        assert(containsEntry(rb.presentEntries, 1, 100));  // still exactly one entry, not duplicated
        assert(containsEntry(rb.presentEntries, 2, 101));  // untouched
        assert(rb.presentEntries.size() == 2);
        std::cout << "case 4c (reassignPlaylistMemberships: drops rather than duplicates) OK\n";
    }

    // reassignPlaylistMemberships: no entries for the old track id ->
    // no-op, returns 0.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        size_t reassignedCount3 = writer.reassignPlaylistMemberships(999999, 100);
        assert(reassignedCount3 == 0);
        assert(!writer.commit());
        assert(readFile(pdbPath) == pristine);
        std::cout << "case 4d (reassignPlaylistMemberships: no matching entries -> no-op) OK\n";
    }

    // Not-found cases return false and never mark anything dirty.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        assert(!writer.removeTrack(999999));
        assert(!writer.removePlaylistEntry(1, 999999));
        assert(!writer.repointPlaylistEntry(1, 999999, 1));
        // Nothing was ever successfully edited -- commit() must refuse,
        // and the real file on disk must never have been touched.
        assert(!writer.commit());
        assert(readFile(pdbPath) == pristine);
        std::cout << "case 5 (not-found edits are no-ops; commit() refuses) OK\n";
    }

    // The core safety guarantee: if commit() is never called at all
    // (simulating a crash mid-session), the real file on disk is
    // completely untouched, byte for byte -- even though a real edit
    // was made against the in-memory copy.
    {
        writeFile(pdbPath, pristine);
        {
            PdbRowWriter writer(pdbPath.string());
            assert(writer.removeTrack(100));
            // writer goes out of scope here without commit() ever being called.
        }
        assert(readFile(pdbPath) == pristine);
        std::cout << "case 6 (no commit() -> original file untouched) OK\n";
    }

    // Format sanity check: a file too small, or with implausible
    // header values, is rejected at construction rather than trusted
    // for offset math.
    {
        writeFile(pdbPath, "too small");
        bool threw = false;
        try {
            PdbRowWriter writer(pdbPath.string());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        std::string garbage = pristine;
        writeU32LE(garbage, 4, 0xFFFFFFFF);  // implausible len_page
        writeFile(pdbPath, garbage);
        threw = false;
        try {
            PdbRowWriter writer(pdbPath.string());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 7 (implausible/too-small file rejected at construction) OK\n";
    }

    // Staleness check: if the file on disk changes between construction
    // and commit() -- something else touched it in the meantime -- the
    // writer must refuse rather than clobber that change with its own
    // now-stale in-memory copy.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        assert(writer.removeTrack(100));

        // Simulate an external modification (different size, so this
        // doesn't depend on filesystem mtime resolution).
        std::string externallyModified = pristine + "X";
        writeFile(pdbPath, externallyModified);

        assert(!writer.commit());
        assert(readFile(pdbPath) == externallyModified);
        std::cout << "case 8 (file changed since construction -> commit() refuses) OK\n";
    }

    // copyTrackFieldsIfMissing: track 101 (donor) has real key_id/tempo/
    // artwork_id; track 100 (target) has none of them (all zero, same as
    // the pristine fixture). Copying all three should land exactly the
    // donor's raw values on the target row, leaving the donor untouched.
    {
        constexpr size_t HeapStart = 40;
        constexpr size_t TrackRowSize = 136;
        constexpr size_t TrackArtworkIdOffset = 28;
        constexpr size_t TrackKeyIdOffset = 32;
        constexpr size_t TrackTempoOffset = 56;
        size_t page1 = LenPage * 1;
        size_t donorRowStart = page1 + HeapStart + TrackRowSize;  // track id=101

        std::string withDonorFields = pristine;
        writeU32LE(withDonorFields, donorRowStart + TrackKeyIdOffset, 7);
        writeU32LE(withDonorFields, donorRowStart + TrackTempoOffset, 12800);  // 128.00 bpm, fixed-point
        writeU32LE(withDonorFields, donorRowStart + TrackArtworkIdOffset, 42);
        writeFile(pdbPath, withDonorFields);

        PdbRowWriter writer(pdbPath.string());
        size_t copiedCount1 = writer.copyTrackFieldsIfMissing(101, 100, true, true, true);
        assert(copiedCount1 == 3);
        assert(writer.commit());

        auto rb = readBack(pdbPath);
        auto target = findTrackFields(rb, 100);
        auto donor = findTrackFields(rb, 101);
        assert(target.has_value() && donor.has_value());
        assert(target->keyId == 7);
        assert(target->tempo == 12800);
        assert(target->artworkId == 42);
        assert(donor->keyId == 7);  // donor row itself untouched
        assert(donor->tempo == 12800);
        assert(donor->artworkId == 42);
        std::cout << "case 9 (copyTrackFieldsIfMissing: copies key/tempo/artwork from donor onto target) OK\n";
    }

    // copyTrackFieldsIfMissing: only one field flagged -- the other two
    // stay untouched (zero) on the target, and the return count reflects
    // just the one field actually copied.
    {
        constexpr size_t HeapStart = 40;
        constexpr size_t TrackRowSize = 136;
        constexpr size_t TrackKeyIdOffset = 32;
        size_t page1 = LenPage * 1;
        size_t donorRowStart = page1 + HeapStart + TrackRowSize;  // track id=101

        std::string withDonorFields = pristine;
        writeU32LE(withDonorFields, donorRowStart + TrackKeyIdOffset, 9);
        writeFile(pdbPath, withDonorFields);

        PdbRowWriter writer(pdbPath.string());
        size_t copiedCount2 = writer.copyTrackFieldsIfMissing(101, 100, true, false, false);
        assert(copiedCount2 == 1);
        assert(writer.commit());

        auto rb = readBack(pdbPath);
        auto target = findTrackFields(rb, 100);
        assert(target.has_value());
        assert(target->keyId == 9);
        assert(target->tempo == 0);
        assert(target->artworkId == 0);
        std::cout << "case 10 (copyTrackFieldsIfMissing: only the flagged field is copied) OK\n";
    }

    // copyTrackFieldsIfMissing: unknown donor or target track id throws,
    // never marks anything dirty.
    {
        writeFile(pdbPath, pristine);
        PdbRowWriter writer(pdbPath.string());
        bool threw = false;
        try {
            writer.copyTrackFieldsIfMissing(999999, 100, true, true, true);
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            writer.copyTrackFieldsIfMissing(101, 999999, true, true, true);
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        assert(!writer.commit());
        assert(readFile(pdbPath) == pristine);
        std::cout << "case 11 (copyTrackFieldsIfMissing: unknown track id throws, no edit) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
