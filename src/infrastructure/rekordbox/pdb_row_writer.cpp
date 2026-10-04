// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <algorithm>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/rekordbox/generated/rekordbox_pdb.h"
#include "infrastructure/rekordbox/little_endian.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"

namespace seabass::infrastructure::rekordbox
{

using Pdb = rekordbox_pdb_t;
namespace fs = std::filesystem;

namespace
{

// Byte offsets within the root header / a page header, derived directly
// from specs/rekordbox_pdb.ksy's seq field layout (not from a generated
// accessor -- these two fields have no dedicated "position" instance,
// only a value accessor, unlike the row/group offsets below which are
// read straight off the trusted parser).
//   root: u4(unknown) + u4(len_page) + u4(num_tables) + u4(next_unused_page)
//         + u4(unknown) + u4(sequence) -> sequence at byte 20.
constexpr size_t HeaderSequenceOffset = 20;
constexpr size_t HeaderLenPageOffset = 4;
constexpr size_t HeaderNumTablesOffset = 8;
//   page: [4]gap + u4(page_index) + u4(type/type_ext) + page_ref(u4 next_page)
//         + u4(sequence) -> sequence at byte 16, relative to page start.
constexpr size_t PageSequenceOffset = 16;
// playlist_entry_row: u4(entry_index) + u4(track_id) + u4(playlist_id) --
// track_id starts right after entry_index.
constexpr size_t PlaylistEntryTrackIdOffset = 4;

// The file header's next_unused_page, and the table list after it: one
// 16-byte entry per table (type, empty_candidate, first_page, last_page).
constexpr size_t HeaderNextUnusedPageOffset = 12;
constexpr size_t TablesOffset = 28;
constexpr size_t TableEntrySize = 16;
constexpr uint32_t PlaylistEntriesTableType = 8;
constexpr uint32_t PlaylistTreeTableType = 7;
constexpr size_t PageHeaderSize = 40;

// An index page (page_flags 0x40, a table's first page), past its 32-byte
// page header: how many entries it can hold, the next slot, the entry
// count, the first freed slot (0x1fff: none), then the entries, unused
// ones 0x1ffffff8.
constexpr size_t IndexCapacityOffset = 36;
// The index page's own next_page: the table's first data page, 0x3ffffff
// while the table has none.
constexpr size_t IndexFirstDataPageOffset = 44;
constexpr size_t IndexNextOffsetOffset = 38;
constexpr size_t IndexNumEntriesOffset = 56;
constexpr size_t IndexFirstEmptyOffset = 58;
constexpr size_t IndexEntriesOffset = 60;
constexpr uint16_t IndexNoFreeSlot = 0x1FFF;
constexpr uint32_t IndexEmptySlot = 0x1FFFFFF8;
// track_row: derived from specs/rekordbox_pdb.ksy's seq field layout,
// confirmed against the generated parser's own _read() order (rekordbox_
// pdb.cpp) -- subtype(u2)+index_shift(u2)+bitmask(u4)+sample_rate(u4)+
// composer_id(u4)+file_size(u4)+unnamed(u4)+unnamed(u2)+unnamed(u2) = 28
// bytes before artwork_id, then key_id right after it, then
// original_artist_id/label_id/remixer_id/bitrate/track_number (5 x u4 =
// 20 bytes) before tempo. All three are plain fixed-size u4 fields (no
// variable-length data anywhere before them in the row), so -- like
// PlaylistEntryTrackIdOffset above -- they're safe to overwrite in place
// without touching the row's length or anything after it.
constexpr size_t TrackArtworkIdOffset = 28;
constexpr size_t TrackKeyIdOffset = 32;
constexpr size_t TrackTempoOffset = 56;
// track_row.rating, a u1 counted straight off specs/rekordbox_pdb.ksy's
// seq: subtype(2) index_shift(2) bitmask(4) sample_rate(4)
// composer_id(4) file_size(4) +4 +2 +2 artwork_id(4) key_id(4)
// original_artist_id(4) label_id(4) remixer_id(4) bitrate(4)
// track_number(4) tempo(4) genre_id(4) album_id(4) artist_id(4) id(4)
// disc_number(2) play_count(2) year(2) sample_depth(2) duration(2) +2
// color_id(1) -> 89. The count is checked by the three offsets above and
// ofs_strings below, which land on 28, 32, 56 and 94 exactly as this
// file already had them.
constexpr size_t TrackRatingOffset = 89;
// track_row.play_count, a u2, from the same count: id(4) ends at 76 and
// disc_number(2) takes 76 and 77.
constexpr size_t TrackPlayCountOffset = 78;

// track_row's ofs_strings array: 21 x u2, each the byte offset (relative
// to row_base) of one device_sql_string field. Continuing the same
// cumulative layout documented above from tempo (56): genre_id/album_id/
// artist_id/id (4 x u4 = 16 bytes) + disc_number/play_count/year/
// sample_depth/duration/unnamed (6 x u2 = 12 bytes) + color_id/rating (2
// x u1 = 2 bytes) + two unnamed u2 fields (4 bytes) = 34 more bytes ->
// 56 + 34 = 90, then tempo's own 4 bytes were already counted above (56
// is tempo's *offset*, so add tempo's 4 bytes too) -> ofs_strings starts
// at 60 + 34 = 94. Cross-checked directly against specs/rekordbox_pdb.
// ksy's track_row seq, which is the authoritative source here.
constexpr size_t TrackOfsStringsOffset = 94;
// Indices into ofs_strings -- see specs/rekordbox_pdb.ksy's track_row
// `instances`, which names each of the 21 entries in this exact order.
// The other free-text slots on a track row, per specs/rekordbox_pdb.ksy.
// isrc names the exact commercial recording; texter and message are free
// text; mix_name is "Extended Mix" and friends. None were ever scrubbed.
constexpr int TrackStringIndexIsrc = 0;
constexpr int TrackStringIndexTexter = 1;
constexpr int TrackStringIndexMessage = 5;
constexpr int TrackStringIndexMixName = 12;
constexpr int TrackStringIndexComment = 16;
constexpr int TrackStringIndexTitle = 17;
constexpr int TrackStringIndexFilename = 19;
constexpr int TrackStringIndexFilePath = 20;

// artist_row: subtype(u2) + index_shift(u2) + id(u4) + unnamed(u1) +
// ofs_name_near(u1) = name's near offset lives at byte 9; ofs_name_far
// (u2), used instead when subtype's 0x04 bit is set, lives at byte 10
// -- see specs/rekordbox_pdb.ksy's artist_row.
// tag_row's own layout, from specs/rekordbox_pdb.ksy, relative to
// row_base. Named here rather than inlined because the name is reached
// through one of two offsets depending on a flag in the first field, and
// that indirection is the whole trick of the row.
constexpr size_t TagRowSubtypeOffset = 0;       // u2
constexpr size_t TagRowOfsNameNearOffset = 29;     // u1
constexpr size_t TagRowOfsUnknownNearOffset = 30;  // u1, a second string, normally empty
constexpr size_t TagRowOfsNameFarOffset = 30;      // u2 at 0x1e, read when subtype & 0x04
constexpr uint16_t TagRowFarNameFlag = 0x04;

constexpr size_t ArtistSubtypeOffset = 0;
constexpr size_t ArtistNameOffsetNear = 9;
constexpr size_t ArtistNameOffsetFar = 10;
constexpr uint16_t ArtistSubtypeFarNameFlag = 0x04;

// album_row has the same near/far name indirection artist_row does, at
// different offsets: subtype(u2) at 0, ofs_name_near(u1) at 0x15, and
// ofs_name_far(u2) at 0x16 when subtype's 0x04 bit is set -- see
// specs/rekordbox_pdb.ksy's album_row.
constexpr size_t AlbumSubtypeOffset = 0;
constexpr size_t AlbumNameOffsetNear = 0x15;
constexpr size_t AlbumNameOffsetFar = 0x16;
constexpr uint16_t AlbumSubtypeFarNameFlag = 0x04;

// One row-index group is sixteen 2-byte row offsets plus the present
// and transaction flag words: 0x24 bytes, built backwards from the end
// of the page -- see specs/rekordbox_pdb.ksy's row_group.
constexpr size_t RowGroupSizeBytes = 0x24;

// track_row carries twenty-one string offsets, and the strings they
// point at are the tail of the row -- so the furthest of them is where
// the row really ends.
constexpr int TrackStringCount = 21;

// genre_row and label_row are the simple case: id(u4) and then the name
// immediately after it, no indirection at all.
constexpr size_t SimpleNameRowNameOffset = 4;

// playlist_tree_row: parent_id(u4) + unnamed(u4) + sort_order(u4) +
// id(u4) + raw_is_folder(u4) = name starts right at byte 20, no
// indirection -- see specs/rekordbox_pdb.ksy's playlist_tree_row.
constexpr size_t PlaylistTreeNameOffset = 20;

// One device_sql_string value's on-disk shape at some absolute buffer
// offset: its total byte span (header + text, matching
// specs/rekordbox_pdb.ksy's device_sql_string/device_sql_short_ascii/
// device_sql_long_ascii/device_sql_long_utf16le) and how many text bytes
// are actually available within it. Never includes the header itself --
// overwriteDeviceSqlStringInPlace() below only ever touches text bytes,
// so a value's length/kind framing is never disturbed.
struct DeviceSqlStringSpan
{
    size_t totalBytes = 0;
    size_t textCapacityBytes = 0;  // for UTF-16LE, a *byte* capacity (2 bytes/code unit), not a code-unit count
    bool isUtf16 = false;
};

DeviceSqlStringSpan readDeviceSqlStringSpan(const std::string &buffer, size_t absOffset)
{
    auto lengthAndKind = static_cast<uint8_t>(buffer.at(absOffset));
    DeviceSqlStringSpan span;
    if (lengthAndKind == 0x40 || lengthAndKind == 0x90) {
        // device_sql_long_ascii / device_sql_long_utf16le: 4-byte header
        // (kind u1 + length u2 + one unused byte), length includes the
        // header itself.
        uint16_t length = readU16LE(buffer, absOffset + 1);
        span.totalBytes = length;
        span.textCapacityBytes = length >= 4 ? static_cast<size_t>(length) - 4 : 0;
        span.isUtf16 = (lengthAndKind == 0x90);
    } else {
        // device_sql_short_ascii: length_and_kind is odd; the whole
        // field (1 header byte + text) is length_and_kind >> 1 bytes.
        size_t total = static_cast<size_t>(lengthAndKind) >> 1;
        span.totalBytes = total;
        span.textCapacityBytes = total >= 1 ? total - 1 : 0;
    }
    return span;
}

// Whether the DeviceSQL string at `nameAt` lies wholly inside the page
// that holds the row starting at `rowBodyOffset` -- where it begins AND
// where it ends.
//
// Every in-place text write needs this, because both numbers it checks
// come out of the file: where a name starts is a u1 or u2 offset in the
// row, and how long it is is the string's own header, a u2 for a long
// one. A page is 4096 bytes and those reach 65535, so a wrong or hostile
// value writes into another table's page. buffer.at() only objects once
// the write leaves the FILE, and landing in a neighbouring page is
// inside it.
//
// One definition, because the bound was written three times and was
// missing from two more writers that needed it: the tag names had it,
// overwriteAllNames() gained it later (issue #46), and overwriteArtistName()
// and the track strings -- the per-id passes the anonymizer runs before
// either -- never did. Each was a hardening applied to one of the places
// that needed it.
bool stringStaysInRowPage(const std::string &buffer, size_t rowBodyOffset, size_t nameAt)
{
    const size_t lenPage = readU32LE(buffer, HeaderLenPageOffset);
    if (lenPage == 0) {
        return false;
    }
    const size_t pageEnd = (rowBodyOffset / lenPage + 1) * lenPage;
    if (nameAt < rowBodyOffset || nameAt >= pageEnd || pageEnd > buffer.size()) {
        return false;
    }
    const DeviceSqlStringSpan span = readDeviceSqlStringSpan(buffer, nameAt);
    return span.totalBytes != 0 && nameAt + span.totalBytes <= pageEnd;
}

// Whether every byte of `text` is representable by the writers below,
// which is exactly: plain ASCII.
//
// Both branches of overwriteDeviceSqlStringInPlace() write bytes. The
// ASCII branch copies them straight in, and the UTF-16 branch writes
// each BYTE as the low half of a code unit with a zero high byte. So a
// UTF-8 sequence arriving there is transliterated as Latin-1: "Cé"
// (43 c3 a9) is stored as "CÃ" (43 00 c3 00), two valid code units, in
// a field that then reparses cleanly with every length correct. On top
// of that, capacity is counted in code units while the input is counted
// in bytes, so the tail is dropped as well -- the a9 in that example.
//
// Nothing downstream can see either. See docs/write-path-rules.md,
// "Refuse, do not transliterate", which this is the live example for.
//
// Deliberately narrow. This refuses what the field cannot represent and
// nothing else: ASCII arriving through a UTF-16 field is the ordinary
// case and stays fine. The same document records a refusal that was
// itself the bug, failing 635 of 1118 tracks on an everyday situation,
// which is what over-refusing costs here.
bool isPlainAscii(const std::string &text)
{
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return c < 0x80; });
}

// Fits `text` into exactly `capacityBytes`: truncated if too long,
// right-padded with ASCII spaces if shorter.
//
// Byte-level truncation and padding are safe here only because callers
// have already refused anything that is not plain ASCII (isPlainAscii
// above). That used to be a comment stating a precondition nothing
// checked, which is what docs/write-path-rules.md's corollary is about:
// a comment saying "callers always pass X" is a note, not a guarantee.
std::string fitAsciiToCapacity(const std::string &text, size_t capacityBytes, bool *truncated = nullptr)
{
    if (truncated != nullptr && text.size() > capacityBytes) {
        *truncated = true;
    }
    std::string fitted = text.substr(0, capacityBytes);
    fitted.resize(capacityBytes, ' ');
    return fitted;
}

// Re-encodes newText into the device_sql_string value already sitting
// at absOffset, filling exactly its existing text capacity (never its
// header) -- see DeviceSqlStringSpan's own comment for why this never
// resizes or reflows anything.
// Returns false, having written nothing, when `newText` is not
// representable in the field.
// truncated (optional): set to true when the text did not fit and the
// tail was dropped. Never reset, so one flag can be handed to a run of
// calls. Truncation is correct behaviour here -- a row cannot grow
// without reflowing its page, so preserving the byte length is the
// contract, and anonymizationPlaceholder() puts its hash in front of
// the readable word precisely because the tail gets eaten. What was
// missing is that a finished export could not say how many of its
// fields were too small to carry a whole placeholder.
bool overwriteDeviceSqlStringInPlace(std::string &buffer, size_t absOffset, const std::string &newText,
                                     bool *truncated = nullptr)
{
    if (!isPlainAscii(newText)) {
        return false;
    }
    DeviceSqlStringSpan span = readDeviceSqlStringSpan(buffer, absOffset);
    size_t headerBytes = span.totalBytes - span.textCapacityBytes;
    // A field with no capacity at all held nothing and takes nothing:
    // an empty comment is the ordinary case on a real track, and
    // counting those would report one cut per track for text that was
    // never there. overwriteAllTagNames() skips such a field entirely
    // for the mirror-image reason.
    bool *report = span.textCapacityBytes > 0 ? truncated : nullptr;
    if (span.isUtf16) {
        size_t capacityUnits = span.textCapacityBytes / 2;
        std::string fitted = fitAsciiToCapacity(newText, capacityUnits, report);
        for (size_t i = 0; i < capacityUnits; ++i) {
            size_t textOffset = absOffset + headerBytes + i * 2;
            buffer.at(textOffset) = fitted[i];
            buffer.at(textOffset + 1) = '\0';
        }
    } else {
        std::string fitted = fitAsciiToCapacity(newText, span.textCapacityBytes, report);
        for (size_t i = 0; i < fitted.size(); ++i) {
            buffer.at(absOffset + headerBytes + i) = fitted[i];
        }
    }
    return true;
}

size_t trackStringAbsOffset(const std::string &buffer, size_t rowBodyOffset, int stringIndex)
{
    size_t ofsFieldOffset = rowBodyOffset + TrackOfsStringsOffset + static_cast<size_t>(stringIndex) * 2;
    uint16_t relOffset = readU16LE(buffer, ofsFieldOffset);
    return rowBodyOffset + relOffset;
}

size_t albumNameAbsOffset(const std::string &buffer, size_t rowBodyOffset)
{
    uint16_t subtype = readU16LE(buffer, rowBodyOffset + AlbumSubtypeOffset);
    uint16_t relOffset = (subtype & AlbumSubtypeFarNameFlag)
                              ? readU16LE(buffer, rowBodyOffset + AlbumNameOffsetFar)
                              : static_cast<uint8_t>(buffer.at(rowBodyOffset + AlbumNameOffsetNear));
    return rowBodyOffset + relOffset;
}

size_t artistNameAbsOffset(const std::string &buffer, size_t rowBodyOffset)
{
    uint16_t subtype = readU16LE(buffer, rowBodyOffset + ArtistSubtypeOffset);
    uint16_t relOffset = (subtype & ArtistSubtypeFarNameFlag)
                              ? readU16LE(buffer, rowBodyOffset + ArtistNameOffsetFar)
                              : static_cast<uint8_t>(buffer.at(rowBodyOffset + ArtistNameOffsetNear));
    return rowBodyOffset + relOffset;
}

// Loose but real sanity bounds -- real rekordbox exports use len_page
// 4096; this just rejects an obviously-wrong-format file (wrong file
// entirely, truncated header, garbage) before any offset math trusts
// it, not a strict format validator.
constexpr uint32_t MinPlausibleLenPage = 256;
constexpr uint32_t MaxPlausibleLenPage = 1u << 20;  // 1 MiB
constexpr uint32_t MaxPlausibleNumTables = 64;       // real files have ~20

std::string readWholeFile(const std::string &path)
{
    std::ifstream ifs(pathFromUtf8(path), std::ifstream::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("could not open " + path);
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

std::uint32_t checksumOf(const std::string &buffer)
{
    unsigned long crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(buffer.data()), static_cast<uInt>(buffer.size()));
    return static_cast<std::uint32_t>(crc);
}

// Rejects a file too small/implausible to even hold a real root header,
// or whose len_page/num_tables are outside any sane real-world range --
// catches "this isn't really an export.pdb" (or a truncated/corrupt
// one) before any byte offset computed from those values is trusted.
void validateLooksLikeRealPdb(const std::string &buffer)
{
    constexpr size_t MinRootHeaderSize = 28;  // through the end of `gap`, before any table entries
    if (buffer.size() < MinRootHeaderSize) {
        throw std::runtime_error("not a valid export.pdb: file too small to hold a root header");
    }
    uint32_t lenPage = readU32LE(buffer, HeaderLenPageOffset);
    if (lenPage < MinPlausibleLenPage || lenPage > MaxPlausibleLenPage) {
        throw std::runtime_error("not a valid export.pdb: implausible len_page in header");
    }
    uint32_t numTables = readU32LE(buffer, HeaderNumTablesOffset);
    if (numTables == 0 || numTables > MaxPlausibleNumTables) {
        throw std::runtime_error("not a valid export.pdb: implausible num_tables in header");
    }
    if (buffer.size() < static_cast<size_t>(lenPage)) {
        throw std::runtime_error("not a valid export.pdb: file smaller than its own declared page size");
    }
}

// Re-parses the fully edited buffer with the real generated parser --
// a structural sanity check that a bug in this class produced something
// still readable, run before the result is ever written to disk. Only
// walks the tables this class can edit; a full library scan isn't
// needed to catch a broken row/page/header.
//
// Deliberately only forces row->body() (a row's fixed seq fields), not
// the device_sql_string `instances` overwriteTrackText()/
// overwriteArtistName()/overwritePlaylistName() write into (title/
// comment/filename/file_path/name) -- those are validated far more
// precisely by pdb_row_writer_string_test.cpp actually reading the
// written text back through the real accessors, and forcing them here
// unconditionally would trip on any row whose *other*, untouched string
// fields simply happen not to be pointing at another real
// device_sql_string (as in this file's own synthetic test fixture).
bool reparsesCleanly(const std::string &buffer, bool isExt)
{
    try {
        std::istringstream iss(buffer);
        kaitai::kstream ks(&iss);
        Pdb pdb(isExt, &ks);
        if (isExt) {
            // An ext file has none of the table types checked below, so
            // the loop would skip every table and return true having
            // parsed nothing -- a verification that cannot fail, which
            // is worse than none because commit() trusts it. The rows
            // are reached through body_ext(), not body().
            // EVERY ext table, not only the tags one. zeroUnusedSpace()
            // clears the free space between rows on tag_tracks pages too
            // (Shape::ExtOther), so a bad heap_pos or row-start there
            // would destroy tag-to-track assignments -- and a check that
            // walked only the tags tables would wave it through. The
            // point of this function is that a bug in this class never
            // reaches disk; it cannot do that for pages it does not read.
            bool sawARow = false;
            for (const auto &table : *pdb.tables()) {
                forEachDataPage(*table, [&](Pdb::page_t *page) {
                    for (const auto &group : *page->row_groups()) {
                        for (const auto &row : *group->rows()) {
                            if (row->present()) {
                                (void)row->body_ext();  // force the row to actually parse
                                sawARow = true;
                            }
                        }
                    }
                });
            }
            // An ext file that parsed to no rows at all is how a bad
            // edit presents itself, so it is a failure rather than a
            // quiet pass.
            return sawARow;
        }
        for (const auto &table : *pdb.tables()) {
            if (table->type() != Pdb::PAGE_TYPE_TRACKS && table->type() != Pdb::PAGE_TYPE_PLAYLIST_ENTRIES &&
                table->type() != Pdb::PAGE_TYPE_ARTISTS && table->type() != Pdb::PAGE_TYPE_PLAYLIST_TREE) {
                continue;
            }
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (row->present()) {
                            (void)row->body();  // force the row to actually parse
                        }
                    }
                }
            });
        }
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

// What findRow() needs to hand back to a caller so it can overwrite
// either the row's presence bit or a fixed-size field inside its body --
// every offset is absolute within the file/buffer.
struct FoundRow
{
    uint32_t pageIndex = 0;
    size_t presentFlagsOffset = 0;
    uint16_t rowIndexBit = 0;
    size_t rowBodyOffset = 0;
};

// Locates the first present row of `wantedType` for which `matches`
// returns true, using the exact same trusted, already-tested parser and
// page-walking helper (forEachDataPage(), pdb_lookup.hpp) the read path
// uses -- against `buffer` (this session's in-memory, possibly
// already-edited copy) rather than a fresh file handle, so a caller
// always sees its own prior edits within the same session.
std::optional<FoundRow> findRow(const std::string &buffer, Pdb::page_type_t wantedType,
                                 const std::function<bool(kaitai::kstruct *)> &matches)
{
    std::optional<FoundRow> found;
    std::istringstream iss(buffer);
    kaitai::kstream ks(&iss);
    Pdb pdb(false, &ks);  // both callers are export.pdb concepts

    for (const auto &table : *pdb.tables()) {
        if (found || table->type() != wantedType) {
            continue;
        }
        forEachDataPage(*table, [&](Pdb::page_t *page) {
            if (found) {
                return;
            }
            for (const auto &group : *page->row_groups()) {
                if (found) {
                    return;
                }
                for (const auto &row : *group->rows()) {
                    if (!row->present() || !matches(row->body())) {
                        continue;
                    }
                    FoundRow f;
                    f.pageIndex = page->page_index();
                    f.presentFlagsOffset =
                        static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(group->base()) - 4;
                    f.rowIndexBit = row->row_index();
                    f.rowBodyOffset = static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(row->row_base());
                    found = f;
                    return;
                }
            }
        });
    }
    return found;
}

// One playlist_entry row matching a given track id, plus which
// playlist it's in -- what reassignPlaylistMemberships() needs to
// decide repoint-vs-remove per row. Unlike findRow(), collects every
// match in one page-walk rather than stopping at the first.
struct PlaylistEntryMatch
{
    uint32_t playlistId = 0;
    FoundRow row;
};

std::vector<PlaylistEntryMatch> findAllPlaylistEntriesForTrack(const std::string &buffer, uint32_t trackId)
{
    std::vector<PlaylistEntryMatch> matches;
    std::istringstream iss(buffer);
    kaitai::kstream ks(&iss);
    Pdb pdb(false, &ks);  // both callers are export.pdb concepts

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
                    auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(row->body());
                    if (!e || e->track_id() != trackId) {
                        continue;
                    }
                    PlaylistEntryMatch m;
                    m.playlistId = e->playlist_id();
                    m.row.pageIndex = page->page_index();
                    m.row.presentFlagsOffset =
                        static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(group->base()) - 4;
                    m.row.rowIndexBit = row->row_index();
                    m.row.rowBodyOffset =
                        static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(row->row_base());
                    matches.push_back(m);
                }
            }
        });
    }
    return matches;
}

// Every present entry of one playlist, in entry_index order, with where
// each row is.
struct PlaylistEntryRow
{
    uint32_t entryIndex = 0;
    uint32_t trackId = 0;
    FoundRow row;
};

std::vector<PlaylistEntryRow> playlistEntriesOf(const std::string &buffer, uint32_t playlistId)
{
    std::vector<PlaylistEntryRow> rows;
    std::istringstream iss(buffer);
    kaitai::kstream ks(&iss);
    Pdb pdb(false, &ks);

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
                    auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(row->body());
                    if (!e || e->playlist_id() != playlistId) {
                        continue;
                    }
                    PlaylistEntryRow r;
                    r.entryIndex = e->entry_index();
                    r.trackId = e->track_id();
                    r.row.pageIndex = page->page_index();
                    r.row.presentFlagsOffset =
                        static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(group->base()) - 4;
                    r.row.rowIndexBit = row->row_index();
                    r.row.rowBodyOffset =
                        static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(row->row_base());
                    rows.push_back(r);
                }
            }
        });
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.entryIndex < b.entryIndex; });
    return rows;
}

// Rows take whole multiples of four bytes of a page's heap: a 23-byte
// playlist-tree row is followed by one byte of padding (#62's reference
// export).
size_t paddedRowSize(size_t bytes)
{
    return (bytes + 3) & ~size_t(3);
}

void appendU32(std::string &out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

std::string playlistEntryRowBytes(uint32_t entryIndex, uint32_t trackId, uint32_t playlistId)
{
    std::string row;
    appendU32(row, entryIndex);
    appendU32(row, trackId);
    appendU32(row, playlistId);
    return row;
}

// UTF-8 to UTF-16 code units; a malformed byte becomes U+FFFD.
std::u16string utf16Of(const std::string &utf8)
{
    std::u16string out;
    for (size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6 && i + 1 < utf8.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c >> 4) == 0xE && i + 2 < utf8.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6)
                 | (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < utf8.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 12)
                 | ((static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(utf8[i + 3]) & 0x3Fu);
            len = 4;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return out;
}

// A DeviceSQL string as rekordbox writes it: plain ASCII up to 126 bytes
// as a short string (one byte: (length + 1) * 2 + 1), longer ASCII as a
// long one (0x40, a 16-bit total length, a zero byte), anything else as
// UTF-16LE (0x90, the same header). "Q1" and a 137-character Unicode
// playlist name in #62's reference export are byte for byte this.
std::string encodeDeviceSqlString(const std::string &text)
{
    std::string out;
    if (isPlainAscii(text) && text.size() <= 126) {
        out.push_back(static_cast<char>(((text.size() + 1) << 1) | 1));
        out += text;
        return out;
    }
    std::string body;
    char kind = 0x40;
    if (isPlainAscii(text)) {
        body = text;
    } else {
        kind = static_cast<char>(0x90);
        for (const char16_t unit : utf16Of(text)) {
            body.push_back(static_cast<char>(unit & 0xFF));
            body.push_back(static_cast<char>(unit >> 8));
        }
    }
    const size_t total = 4 + body.size();
    if (total > 0xFFFF) {
        throw std::invalid_argument("a name that long does not fit a DeviceSQL string");
    }
    out.push_back(kind);
    out.push_back(static_cast<char>(total & 0xFF));
    out.push_back(static_cast<char>(total >> 8));
    out.push_back('\0');
    out += body;
    return out;
}

// One present playlist_tree row, its fields and its encoded name as they
// are on the page, and where it is.
struct TreeRow
{
    uint32_t parentId = 0;
    uint32_t sortOrder = 0;
    uint32_t id = 0;
    bool isFolder = false;
    std::string name;
    std::string nameBytes;
    FoundRow row;
};

constexpr size_t TreeRowNameOffset = 20;

std::vector<TreeRow> treeRowsOf(const std::string &buffer)
{
    std::vector<TreeRow> rows;
    std::istringstream iss(buffer);
    kaitai::kstream ks(&iss);
    Pdb pdb(false, &ks);
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
                    auto *t = dynamic_cast<Pdb::playlist_tree_row_t *>(row->body());
                    if (!t) {
                        continue;
                    }
                    TreeRow r;
                    r.parentId = t->parent_id();
                    r.sortOrder = t->sort_order();
                    r.id = t->id();
                    r.isFolder = t->is_folder();
                    r.name = sqlText(t->name());
                    r.row.pageIndex = page->page_index();
                    r.row.presentFlagsOffset =
                        static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(group->base()) - 4;
                    r.row.rowIndexBit = row->row_index();
                    r.row.rowBodyOffset =
                        static_cast<size_t>(pdb.len_page()) * page->page_index() + static_cast<size_t>(row->row_base());
                    const size_t nameAt = r.row.rowBodyOffset + TreeRowNameOffset;
                    r.nameBytes = buffer.substr(nameAt, readDeviceSqlStringSpan(buffer, nameAt).totalBytes);
                    rows.push_back(r);
                }
            }
        });
    }
    return rows;
}

// The highest id any playlist_tree row has had, deleted rows included, so
// a new playlist never takes the id of one removed earlier.
uint32_t highestPlaylistTreeId(const std::string &buffer)
{
    uint32_t highest = 0;
    const size_t lenPage = readU32LE(buffer, HeaderLenPageOffset);
    const uint32_t numTables = readU32LE(buffer, HeaderNumTablesOffset);
    for (uint32_t t = 0; t < numTables; ++t) {
        const size_t entry = TablesOffset + TableEntrySize * t;
        if (readU32LE(buffer, entry) != PlaylistTreeTableType) {
            continue;
        }
        const uint32_t lastPage = readU32LE(buffer, entry + 12);
        uint32_t page = readU32LE(buffer, entry + 8);
        for (size_t guard = 0; guard < 100000; ++guard) {
            const size_t base = lenPage * page;
            if (base + lenPage > buffer.size()) {
                break;
            }
            if ((static_cast<unsigned char>(buffer[base + 27]) & 0x40) == 0) {
                const uint32_t numRowOffsets = readU32LE(buffer, base + 24) & 0x1FFFu;
                for (uint32_t i = 0; i < numRowOffsets; ++i) {
                    const size_t groupBase = base + lenPage - (i / 16) * RowGroupSizeBytes;
                    const size_t heap = readU16LE(buffer, groupBase - 6 - 2 * (i % 16));
                    highest = std::max(highest, readU32LE(buffer, base + PageHeaderSize + heap + 12));
                }
            }
            if (page == lastPage) {
                break;
            }
            page = readU32LE(buffer, base + 12);
        }
    }
    return highest;
}

// The byte ranges a row actually uses: its fixed header, and each
// string it points at. Everything else between this row and the next is
// space the format has forgotten about.
//
// A single "the row runs to here" answer is not enough. rekordbox leaves
// a row the space it was first given and moves the name around inside
// it, so an old value can sit *before* the current one as easily as
// after -- 15 real artist names and 2 playlist names survived a pass
// that cleared only the tail of each row.
//
// Every case returns the whole extent, clearing nothing, the moment
// anything fails to add up. Measuring a row short clears bytes it still
// needs: reading key_row's name at genre_row's offset once left the
// catalog unparseable. Erring towards keeping is the only safe
// direction, and the caller's own reparse check is the backstop.
// The live bytes of a tag_row: its fixed header, the device_sql_string
// its name lives in, and the second string ofs_unknown_near points at.
//
// The fixed header is 31 bytes, through ofs_unknown_near at offset 30 --
// NOT 30, which was the first attempt and cost two live bytes per row:
// the ofs_unknown_near byte itself, and the 0x03 empty string it points
// to. 28 rows, 56 bytes, and the names still read back correctly
// afterwards, so no name-level check could have seen it. Found by
// diffing the bytes the pass cleared against the raw file.
//
// When subtype has 0x04 the name offset is a u2 at 0x1e, which occupies
// bytes 30 and 31 and so subsumes ofs_unknown_near; the header is 32
// bytes then and there is no separate unknown string to keep.
std::vector<std::pair<size_t, size_t>> tagRowKeepRanges(const std::string &buffer, size_t rowBase, size_t bound)
{
    const std::vector<std::pair<size_t, size_t>> wholeExtent{{rowBase, bound}};
    if (rowBase + TagRowOfsNameFarOffset + 2 > bound) {
        return wholeExtent;
    }
    const uint16_t subtype = readU16LE(buffer, rowBase + TagRowSubtypeOffset);
    const bool far = (subtype & TagRowFarNameFlag) != 0;
    const size_t header = rowBase + (far ? TagRowOfsNameFarOffset + 2 : TagRowOfsUnknownNearOffset + 1);
    if (header > bound) {
        return wholeExtent;
    }

    std::vector<std::pair<size_t, size_t>> keep{{rowBase, header}};
    // Every string this row owns. A string the row points at but this
    // function forgets is a live byte that gets cleared, so a miss here
    // is silent corruption rather than a missed scrub.
    auto keepStringAt = [&](size_t absOffset) {
        if (absOffset < header || absOffset >= bound) {
            return false;
        }
        const DeviceSqlStringSpan span = readDeviceSqlStringSpan(buffer, absOffset);
        if (span.totalBytes == 0 || absOffset + span.totalBytes > bound) {
            return false;
        }
        keep.emplace_back(absOffset, absOffset + span.totalBytes);
        return true;
    };

    const size_t nameOffset = far ? readU16LE(buffer, rowBase + TagRowOfsNameFarOffset)
                                  : static_cast<size_t>(static_cast<unsigned char>(buffer[rowBase + TagRowOfsNameNearOffset]));
    if (!keepStringAt(rowBase + nameOffset)) {
        return wholeExtent;
    }
    if (!far) {
        const size_t unknownOffset =
            static_cast<size_t>(static_cast<unsigned char>(buffer[rowBase + TagRowOfsUnknownNearOffset]));
        // Absent or unreadable is not a failure: keep the whole row
        // rather than clear a byte this cannot account for.
        if (!keepStringAt(rowBase + unknownOffset)) {
            return wholeExtent;
        }
    }
    return keep;
}

std::vector<std::pair<size_t, size_t>> rowKeepRanges(const std::string &buffer, Pdb::page_type_t pageType,
                                                      size_t rowBase, size_t bound)
{
    std::vector<std::pair<size_t, size_t>> keep;
    const std::vector<std::pair<size_t, size_t>> wholeExtent{{rowBase, bound}};

    auto addString = [&](size_t absOffset, size_t minOffset) {
        // An unused slot points back into the fixed header rather than
        // at a string; there is nothing there to keep.
        if (absOffset < minOffset || absOffset >= bound) {
            return false;
        }
        const DeviceSqlStringSpan span = readDeviceSqlStringSpan(buffer, absOffset);
        if (span.totalBytes == 0 || absOffset + span.totalBytes > bound) {
            return false;
        }
        keep.emplace_back(absOffset, absOffset + span.totalBytes);
        return true;
    };

    try {
        switch (pageType) {
        case Pdb::PAGE_TYPE_TRACKS: {
            const size_t header = rowBase + TrackOfsStringsOffset + TrackStringCount * 2;
            if (header > bound) {
                return wholeExtent;
            }
            keep.emplace_back(rowBase, header);
            for (int i = 0; i < TrackStringCount; ++i) {
                addString(trackStringAbsOffset(buffer, rowBase, i), header);
            }
            break;
        }
        case Pdb::PAGE_TYPE_ARTISTS: {
            const uint16_t subtype = readU16LE(buffer, rowBase + ArtistSubtypeOffset);
            const size_t header =
                rowBase + ((subtype & ArtistSubtypeFarNameFlag) ? ArtistNameOffsetFar + 2 : ArtistNameOffsetNear + 1);
            if (header > bound || !addString(artistNameAbsOffset(buffer, rowBase), header)) {
                return wholeExtent;
            }
            keep.emplace_back(rowBase, header);
            break;
        }
        case Pdb::PAGE_TYPE_ALBUMS: {
            const uint16_t subtype = readU16LE(buffer, rowBase + AlbumSubtypeOffset);
            const size_t header =
                rowBase + ((subtype & AlbumSubtypeFarNameFlag) ? AlbumNameOffsetFar + 2 : AlbumNameOffsetNear + 1);
            if (header > bound || !addString(albumNameAbsOffset(buffer, rowBase), header)) {
                return wholeExtent;
            }
            keep.emplace_back(rowBase, header);
            break;
        }
        case Pdb::PAGE_TYPE_GENRES:
        case Pdb::PAGE_TYPE_LABELS: {
            const size_t header = rowBase + SimpleNameRowNameOffset;
            if (header > bound || !addString(header, header)) {
                return wholeExtent;
            }
            keep.emplace_back(rowBase, header);
            break;
        }
        case Pdb::PAGE_TYPE_PLAYLIST_TREE: {
            const size_t header = rowBase + PlaylistTreeNameOffset;
            if (header > bound || !addString(header, header)) {
                return wholeExtent;
            }
            keep.emplace_back(rowBase, header);
            break;
        }
        default:
            // Including keys: key_row is id + id2 + name, a shape this
            // has already been burned by getting wrong.
            return wholeExtent;
        }
    } catch (const std::exception &) {
        return wholeExtent;
    }
    return keep.empty() ? wholeExtent : keep;
}

}  // namespace

PdbRowWriter::PdbRowWriter(std::string pdbPath, Format format)
    : m_format(format), m_pdbPath(std::move(pdbPath)), m_buffer(readWholeFile(m_pdbPath))
{
    validateLooksLikeRealPdb(m_buffer);
    m_originalFileSize = fs::file_size(pathFromUtf8(m_pdbPath));
    m_originalMtime = fs::last_write_time(pathFromUtf8(m_pdbPath));
    // Checksummed here, before any edit method mutates m_buffer in
    // place -- this is the pristine baseline commit() re-derives a fresh
    // on-disk read against, never m_buffer's later (edited) state.
    m_originalChecksum = checksumOf(m_buffer);
}

bool PdbRowWriter::trackExists(uint32_t trackId) const
{
    return findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
               auto *t = dynamic_cast<Pdb::track_row_t *>(body);
               return t != nullptr && t->id() == trackId;
           }).has_value();
}

bool PdbRowWriter::removeTrack(uint32_t trackId)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == trackId;
    });
    if (!found) {
        return false;
    }
    deleteRowAt(found->pageIndex, found->presentFlagsOffset, found->rowIndexBit);
    return true;
}

size_t PdbRowWriter::copyTrackFieldsIfMissing(uint32_t donorTrackId, uint32_t targetTrackId, bool copyKey,
                                               bool copyTempo, bool copyArtwork)
{
    if (!copyKey && !copyTempo && !copyArtwork) {
        return 0;
    }
    auto donor = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == donorTrackId;
    });
    if (!donor) {
        throw std::runtime_error("no rekordbox track with id=" + std::to_string(donorTrackId));
    }
    auto target = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == targetTrackId;
    });
    if (!target) {
        throw std::runtime_error("no rekordbox track with id=" + std::to_string(targetTrackId));
    }

    // Copies the donor's own already-valid field value/reference
    // directly (e.g. key_id -> the same keys-table row the donor
    // already points at) rather than re-deriving one from a parsed
    // string/double -- simpler and exact, no risk of picking a
    // different keys-table row for an enharmonically-equivalent
    // spelling the way a fresh string parse could.
    size_t affected = 0;
    if (copyKey) {
        uint32_t keyId = readU32LE(m_buffer, donor->rowBodyOffset + TrackKeyIdOffset);
        writeU32LE(m_buffer, target->rowBodyOffset + TrackKeyIdOffset, keyId);
        ++affected;
    }
    if (copyTempo) {
        uint32_t tempo = readU32LE(m_buffer, donor->rowBodyOffset + TrackTempoOffset);
        writeU32LE(m_buffer, target->rowBodyOffset + TrackTempoOffset, tempo);
        ++affected;
    }
    if (copyArtwork) {
        uint32_t artworkId = readU32LE(m_buffer, donor->rowBodyOffset + TrackArtworkIdOffset);
        writeU32LE(m_buffer, target->rowBodyOffset + TrackArtworkIdOffset, artworkId);
        ++affected;
    }
    m_editedPageIndices.insert(target->pageIndex);
    return affected;
}

bool PdbRowWriter::setTrackRating(uint32_t trackId, int rating)
{
    // The format documents 0 to 5 stars in one byte. Anything else is a
    // caller bug, and writing it would put a number in the file that no
    // player has a way to render.
    if (rating < 0 || rating > 5) {
        throw std::runtime_error("rekordbox rating must be 0 to 5, got " + std::to_string(rating));
    }
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == trackId;
    });
    if (!found) {
        return false;
    }
    // One byte, already there, in a row that is neither resized nor
    // moved -- the same shape as every other edit in this class.
    m_buffer.at(found->rowBodyOffset + TrackRatingOffset) = static_cast<char>(rating);
    m_editedPageIndices.insert(found->pageIndex);
    return true;
}

bool PdbRowWriter::setTrackPlayCount(uint32_t trackId, int playCount)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == trackId;
    });
    if (!found) {
        return false;
    }
    const int clamped = std::clamp(playCount, 0, 65535);
    writeU16LE(m_buffer, found->rowBodyOffset + TrackPlayCountOffset, static_cast<uint16_t>(clamped));
    m_editedPageIndices.insert(found->pageIndex);
    return true;
}

namespace
{

// Overwrite one of a track row's strings in place -- but only if the slot
// really points at a string. An unused slot points back into the row's
// fixed header (rowKeepRanges above relies on exactly that), and writing
// "a string" there fills header bytes with spaces: sample rate, file size,
// the artist and album ids. The page still parses, so commit()'s reparse
// check passes and the kept track ships with corrupted metadata. ISRC,
// texter, message and mix name -- the slots the anonymizer blanks -- are
// the ones most often unused.
bool overwriteTrackStringIfUsed(std::string &buffer, size_t rowBodyOffset, auto stringIndex, const std::string &text,
                                bool *truncated = nullptr)
{
    const size_t header = rowBodyOffset + TrackOfsStringsOffset + TrackStringCount * 2;
    const size_t absOffset = trackStringAbsOffset(buffer, rowBodyOffset, stringIndex);
    if (absOffset < header || !stringStaysInRowPage(buffer, rowBodyOffset, absOffset)) {
        return false;
    }
    return overwriteDeviceSqlStringInPlace(buffer, absOffset, text, truncated);
}

// A string slot whose offset points past the row header but whose string
// does not stay inside the row's page: one the writer above would refuse.
// Asked of every field before any is written, so a row is changed whole
// or not at all.
bool trackStringLeavesItsPage(const std::string &buffer, size_t rowBodyOffset, auto stringIndex)
{
    const size_t header = rowBodyOffset + TrackOfsStringsOffset + TrackStringCount * 2;
    const size_t absOffset = trackStringAbsOffset(buffer, rowBodyOffset, stringIndex);
    return absOffset >= header && !stringStaysInRowPage(buffer, rowBodyOffset, absOffset);
}

}  // namespace

bool PdbRowWriter::overwriteTrackText(uint32_t trackId, const TrackTextOverride &text)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == trackId;
    });
    if (!found) {
        return false;
    }
    // Every field checked before any of them is written. A row with the
    // title replaced and the comment refused is a row this call has
    // half-changed, and its caller would be told nothing either way,
    // since the return says only whether the row was found.
    if (!isPlainAscii(text.title) || !isPlainAscii(text.comment) || !isPlainAscii(text.filename)
        || !isPlainAscii(text.filePath)) {
        return false;
    }
    // And every field's LOCATION, for the same reason. A slot whose
    // string leaves the page is refused by the write below, and that
    // refusal used to be dropped on the floor: the other fields were
    // written, this returned true, and the anonymizer reported the track
    // scrubbed with its real title still in it.
    for (const auto index : {TrackStringIndexTitle, TrackStringIndexComment, TrackStringIndexFilename,
                             TrackStringIndexFilePath}) {
        if (trackStringLeavesItsPage(m_buffer, found->rowBodyOffset, index)) {
            return false;
        }
    }
    // One count per field that did not fit, so a finished export can say
    // how many of them were too small to carry a whole placeholder.
    // Truncation itself is the contract here (a row cannot grow without
    // reflowing its page), not a fault.
    for (const auto &[index, value] : {std::pair{TrackStringIndexTitle, text.title},
                                       std::pair{TrackStringIndexComment, text.comment},
                                       std::pair{TrackStringIndexFilename, text.filename},
                                       std::pair{TrackStringIndexFilePath, text.filePath}}) {
        bool truncated = false;
        overwriteTrackStringIfUsed(m_buffer, found->rowBodyOffset, index, value, &truncated);
        if (truncated) {
            ++m_truncatedTextFields;
        }
    }
    m_editedPageIndices.insert(found->pageIndex);
    return true;
}

bool PdbRowWriter::overwriteTrackExtraText(uint32_t trackId, const TrackExtraTextOverride &text)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == trackId;
    });
    if (!found) {
        return false;
    }
    if (!isPlainAscii(text.isrc) || !isPlainAscii(text.texter) || !isPlainAscii(text.message)
        || !isPlainAscii(text.mixName)) {
        return false;  // all four, before any of them: see overwriteTrackText()
    }
    for (const auto index : {TrackStringIndexIsrc, TrackStringIndexTexter, TrackStringIndexMessage,
                             TrackStringIndexMixName}) {
        if (trackStringLeavesItsPage(m_buffer, found->rowBodyOffset, index)) {
            return false;  // and their locations: see overwriteTrackText()
        }
    }
    // Not counted: these four exist to be emptied, and empty never
    // truncates. Counting them would mean nothing.
    overwriteTrackStringIfUsed(m_buffer, found->rowBodyOffset, TrackStringIndexIsrc, text.isrc);
    overwriteTrackStringIfUsed(m_buffer, found->rowBodyOffset, TrackStringIndexTexter, text.texter);
    overwriteTrackStringIfUsed(m_buffer, found->rowBodyOffset, TrackStringIndexMessage, text.message);
    overwriteTrackStringIfUsed(m_buffer, found->rowBodyOffset, TrackStringIndexMixName, text.mixName);
    m_editedPageIndices.insert(found->pageIndex);
    return true;
}

bool PdbRowWriter::overwriteArtistName(uint32_t artistId, const std::string &text)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_ARTISTS, [&](kaitai::kstruct *body) {
        auto *a = dynamic_cast<Pdb::artist_row_t *>(body);
        return a != nullptr && a->id() == artistId;
    });
    if (!found) {
        return false;
    }
    // Bounded: the name offset is read out of the row, and this is the
    // per-artist pass the anonymizer runs BEFORE the wholesale one that
    // was bounded first. See stringStaysInRowPage().
    const size_t nameAt = artistNameAbsOffset(m_buffer, found->rowBodyOffset);
    if (!stringStaysInRowPage(m_buffer, found->rowBodyOffset, nameAt)) {
        return false;
    }
    bool artistTruncated = false;
    if (!overwriteDeviceSqlStringInPlace(m_buffer, nameAt, text, &artistTruncated)) {
        return false;  // nothing written, so this page is not marked edited
    }
    if (artistTruncated) {
        ++m_truncatedTextFields;
    }
    m_editedPageIndices.insert(found->pageIndex);
    return true;
}

bool PdbRowWriter::overwritePlaylistName(uint32_t playlistId, const std::string &text)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_PLAYLIST_TREE, [&](kaitai::kstruct *body) {
        auto *p = dynamic_cast<Pdb::playlist_tree_row_t *>(body);
        return p != nullptr && p->id() == playlistId;
    });
    if (!found) {
        return false;
    }
    bool playlistTruncated = false;
    if (!overwriteDeviceSqlStringInPlace(m_buffer, found->rowBodyOffset + PlaylistTreeNameOffset, text,
                                         &playlistTruncated)) {
        return false;
    }
    if (playlistTruncated) {
        ++m_truncatedTextFields;
    }
    m_editedPageIndices.insert(found->pageIndex);
    return true;
}

int PdbRowWriter::overwriteAllTagNames(const std::function<std::string(size_t)> &placeholder, int *rowsLeftAlone)
{
    // Counted from the row list below, so every `continue` in the write
    // loop is accounted for without each one having to remember to say
    // so. There are five of them and a sixth is one edit away.
    *rowsLeftAlone = 0;
    if (m_format != Format::ExportExt) {
        return 0;
    }
    std::vector<size_t> rowBodyOffsets;
    {
        std::istringstream iss(m_buffer);
        kaitai::kstream ks(&iss);
        Pdb pdb(true, &ks);
        for (const auto &t : *pdb.tables()) {
            if (t->type_ext() != Pdb::PAGE_TYPE_EXT_TAGS) {
                continue;
            }
            forEachDataPage(*t, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        rowBodyOffsets.push_back(static_cast<size_t>(pdb.len_page()) * page->page_index()
                                                 + static_cast<size_t>(row->row_base()));
                    }
                }
            });
        }
    }

    int replaced = 0;
    for (size_t i = 0; i < rowBodyOffsets.size(); ++i) {
        const size_t base = rowBodyOffsets[i];
        if (base + TagRowOfsNameFarOffset + 2 > m_buffer.size()) {
            continue;
        }
        const uint16_t subtype = readU16LE(m_buffer, base + TagRowSubtypeOffset);
        const size_t nameOffset = (subtype & TagRowFarNameFlag) != 0
            ? readU16LE(m_buffer, base + TagRowOfsNameFarOffset)
            : static_cast<size_t>(static_cast<unsigned char>(m_buffer[base + TagRowOfsNameNearOffset]));
        // Bounded to the row's own page, start AND end: nameOffset is a
        // u2 read out of the file and reaches 65535 while a page is 4096
        // bytes, and the string's length is another file-supplied u2.
        // reparsesCleanly() walks only the tags tables, so damage to a
        // neighbouring tag_tracks page would be committed without
        // anything noticing. See stringStaysInRowPage().
        const size_t nameAt = base + nameOffset;
        if (!stringStaysInRowPage(m_buffer, base, nameAt)) {
            continue;
        }
        const DeviceSqlStringSpan span = readDeviceSqlStringSpan(m_buffer, nameAt);
        // Counted only when there was somewhere to write. A
        // device_sql_string with no text capacity takes the overwrite
        // and keeps its bytes, so counting the visit rather than the
        // change would let the anonymizer's `renamed > 0` guard pass,
        // and tools/anonymize_export_ext report "rewrote N tag name(s)",
        // with every real name still in the file.
        if (span.textCapacityBytes == 0) {
            continue;
        }
        // A placeholder this cannot represent is not written and not
        // counted. Every placeholder today is ASCII by construction, so
        // this changes nothing now; what it stops is a future caller
        // handing over a real name and being told it was rewritten.
        bool truncated = false;
        if (!overwriteDeviceSqlStringInPlace(m_buffer, nameAt, placeholder(i), &truncated)) {
            continue;
        }
        // Recorded HERE, beside the write, not in the scan loop above.
        // commit() refuses when this set is empty and its comment relies
        // on "empty means nothing was edited"; filling it while merely
        // looking at rows broke that, so a run where every row was
        // skipped still presented itself as having edited pages. Both
        // callers happen to short-circuit on a zero return today, which
        // is the only reason it did not matter.
        if (truncated) {
            ++m_truncatedTextFields;
        }
        // stringStaysInRowPage() has already refused a zero len_page.
        m_editedPageIndices.insert(static_cast<uint32_t>(base / readU32LE(m_buffer, HeaderLenPageOffset)));
        ++replaced;
    }
    *rowsLeftAlone = static_cast<int>(rowBodyOffsets.size()) - replaced;
    return replaced;
}

int PdbRowWriter::overwriteAllNames(NameTable table, const std::function<std::string(size_t)> &placeholder,
                                    int *rowsLeftAlone)
{
    *rowsLeftAlone = 0;
    // None of these tables exist in an exportExt.pdb, and the check that
    // would rule them out cannot be trusted there: `type` and `type_ext`
    // are the same u4 declared twice under opposite `if:` guards, so on
    // an ext parse `type()` is an absent field reading back as its
    // default rather than as anything this file says. Comparing against
    // it would be comparing against nothing. Refused at the door, the
    // way overwriteAllTagNames() refuses a Format::Export writer.
    if (m_format == Format::ExportExt) {
        return 0;
    }

    Pdb::page_type_t pageType = Pdb::PAGE_TYPE_GENRES;
    switch (table) {
    case NameTable::Genres:
        pageType = Pdb::PAGE_TYPE_GENRES;
        break;
    case NameTable::Albums:
        pageType = Pdb::PAGE_TYPE_ALBUMS;
        break;
    case NameTable::Labels:
        pageType = Pdb::PAGE_TYPE_LABELS;
        break;
    case NameTable::Artists:
        pageType = Pdb::PAGE_TYPE_ARTISTS;
        break;
    case NameTable::Playlists:
        pageType = Pdb::PAGE_TYPE_PLAYLIST_TREE;
        break;
    }

    // Collect every row first and only then write. Overwriting mutates
    // m_buffer, which is the very thing the parser below is reading, and
    // a name whose replacement shifts nothing still invalidates the
    // kaitai objects holding offsets into it.
    std::vector<FoundRow> rows;
    {
        std::istringstream iss(m_buffer);
        kaitai::kstream ks(&iss);
        Pdb pdb(m_format == Format::ExportExt, &ks);
        for (const auto &t : *pdb.tables()) {
            if (t->type() != pageType) {
                continue;
            }
            forEachDataPage(*t, [&](Pdb::page_t *page) {
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (!row->present()) {
                            continue;
                        }
                        FoundRow found;
                        found.pageIndex = page->page_index();
                        found.rowBodyOffset = static_cast<size_t>(pdb.len_page()) * page->page_index()
                            + static_cast<size_t>(row->row_base());
                        rows.push_back(found);
                    }
                }
            });
        }
    }

    int replaced = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        size_t nameAt = rows[i].rowBodyOffset + SimpleNameRowNameOffset;
        if (table == NameTable::Albums) {
            nameAt = albumNameAbsOffset(m_buffer, rows[i].rowBodyOffset);
        } else if (table == NameTable::Artists) {
            nameAt = artistNameAbsOffset(m_buffer, rows[i].rowBodyOffset);
        } else if (table == NameTable::Playlists) {
            nameAt = rows[i].rowBodyOffset + PlaylistTreeNameOffset;
        }
        // Bounded to the row's own page, before anything is written:
        // every one of those offsets is read out of the file. See
        // stringStaysInRowPage().
        if (!stringStaysInRowPage(m_buffer, rows[i].rowBodyOffset, nameAt)) {
            continue;
        }
        // A placeholder this cannot represent is not written, and is
        // counted as left alone rather than as rewritten. Every
        // placeholder today is ASCII by construction, so this changes
        // nothing now; what it stops is a future caller handing over a
        // real name and being told it was rewritten.
        bool truncated = false;
        if (!overwriteDeviceSqlStringInPlace(m_buffer, nameAt, placeholder(i), &truncated)) {
            continue;
        }
        if (truncated) {
            ++m_truncatedTextFields;
        }
        m_editedPageIndices.insert(rows[i].pageIndex);
        ++replaced;
    }
    // Every present row that was not rewritten, whichever check stopped
    // it -- derived rather than incremented at each `continue`, the way
    // overwriteAllTagNames() does it, so a skip added later is counted
    // without anyone remembering to.
    *rowsLeftAlone = static_cast<int>(rows.size()) - replaced;
    return replaced;
}

int PdbRowWriter::zeroUnusedSpace()
{
    struct PageWork
    {
        uint32_t pageIndex = 0;
        // What shape the rows on this page are, decided where the parse
        // flag is still in hand. NOT page->type(): the generated parser
        // assigns m_type only under `if (!is_ext)` and _init() does not
        // initialise it, so reading type() on an ext parse is reading an
        // indeterminate value. PAGE_TYPE_TRACKS is 0, so the likeliest
        // garbage dispatches a tag_row down the track_row branch and
        // computes keep-ranges from 21 offsets that are not there.
        enum class Shape
        {
            Regular,  // an export.pdb page; pageType says which kind
            ExtTags,  // an exportExt.pdb tags page: tag_row
            ExtOther, // any other exportExt.pdb page: shape unknown here
        };
        Shape shape = Shape::Regular;
        Pdb::page_type_t pageType = Pdb::PAGE_TYPE_TRACKS;  // Shape::Regular only
        size_t numRows = 0;
        size_t rowOffsets = 0;  // num_row_offsets: slots ever allocated, valid or not
        size_t groups = 0;
        size_t pageStart = 0;
        size_t lenPage = 0;
        size_t heapStart = 0;   // absolute, first byte a row can occupy
        size_t indexEnd = 0;    // absolute, first byte of the row-index groups
        std::vector<std::pair<size_t, bool>> rows;  // absolute row_base, present
    };
    std::vector<PageWork> work;

    {
        std::istringstream iss(m_buffer);
        kaitai::kstream ks(&iss);
        const bool ext = m_format == Format::ExportExt;
        Pdb pdb(ext, &ks);
        const size_t lenPage = pdb.len_page();
        for (const auto &table : *pdb.tables()) {
            forEachDataPage(*table, [&](Pdb::page_t *page) {
                PageWork w;
                w.pageIndex = page->page_index();
                if (ext) {
                    w.shape = table->type_ext() == Pdb::PAGE_TYPE_EXT_TAGS ? PageWork::Shape::ExtTags
                                                                          : PageWork::Shape::ExtOther;
                } else {
                    w.shape = PageWork::Shape::Regular;
                    w.pageType = page->type();
                }
                const size_t pageStart = lenPage * static_cast<size_t>(w.pageIndex);
                w.heapStart = pageStart + static_cast<size_t>(page->heap_pos());
                const size_t groups = static_cast<size_t>(page->num_row_groups());
                if (groups * RowGroupSizeBytes >= lenPage) {
                    return;  // nonsense geometry: leave the page alone
                }
                w.indexEnd = pageStart + lenPage - groups * RowGroupSizeBytes;
                w.numRows = static_cast<size_t>(page->num_rows());
                w.rowOffsets = static_cast<size_t>(page->num_row_offsets());
                w.groups = groups;
                w.pageStart = pageStart;
                w.lenPage = lenPage;
                // Only the slots the page says it has allocated. The
                // generated parser materialises all 16 row_refs per
                // group whatever num_row_offsets says, so the tail of
                // them hold whatever u2 was left in the index from an
                // earlier, larger page. A leftover value that happens to
                // land inside a live row's heap range becomes a bogus
                // "row start" below, which truncates that row's
                // keep-range -- and everything from there to the next
                // real row, fixed fields, the whole ofs_strings array
                // and every string, is then zeroed as free space.
                //
                // reparsesCleanly() would not catch it: it forces
                // body(), and an all-zero track row still parses. The
                // index-slack loop further down already applies exactly
                // this bound (g * 16 + r < rowOffsets); this loop did
                // not.
                size_t slot = 0;
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
                        if (slot++ >= w.rowOffsets) {
                            continue;
                        }
                        w.rows.emplace_back(pageStart + static_cast<size_t>(row->row_base()), row->present());
                    }
                }
                work.push_back(std::move(w));
            });
        }
    }

    int zeroed = 0;
    for (auto &page : work) {
        if (page.indexEnd <= page.heapStart || page.indexEnd > m_buffer.size()) {
            continue;
        }
        // A page with no rows left is not a page to skip -- it is a page
        // whose heap is entirely free, still sitting in its table's chain
        // holding the text of everything it used to have. One such
        // playlist page kept a real playlist name through every other
        // pass here.

        // Every row start on the page, live or dead: a row runs until
        // the next one begins, whichever row that is.
        std::vector<size_t> starts;
        starts.reserve(page.rows.size());
        for (const auto &row : page.rows) {
            if (row.first >= page.heapStart && row.first < page.indexEnd) {
                starts.push_back(row.first);
            }
        }
        std::sort(starts.begin(), starts.end());
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

        // What the live rows occupy, and therefore what must survive.
        std::vector<std::pair<size_t, size_t>> keep;
        for (const auto &row : page.rows) {
            if (!row.second || row.first < page.heapStart || row.first >= page.indexEnd) {
                continue;
            }
            auto next = std::upper_bound(starts.begin(), starts.end(), row.first);
            const size_t bound = next == starts.end() ? page.indexEnd : *next;
            // An ext page whose row shape this function does not know
            // keeps its whole extent: the free space BETWEEN rows is
            // still cleared, and nothing guesses at bytes inside a row
            // it cannot parse.
            const std::vector<std::pair<size_t, size_t>> spans =
                page.shape == PageWork::Shape::ExtTags  ? tagRowKeepRanges(m_buffer, row.first, bound)
                : page.shape == PageWork::Shape::ExtOther ? std::vector<std::pair<size_t, size_t>>{{row.first, bound}}
                                                          : rowKeepRanges(m_buffer, page.pageType, row.first, bound);
            for (const auto &span : spans) {
                if (span.second > span.first) {
                    keep.emplace_back(span.first, std::min(span.second, bound));
                }
            }
        }
        std::sort(keep.begin(), keep.end());

        // Clear the complement of that, inside the heap only. The page
        // header and the row-index groups are never touched: the index
        // still has to describe which rows are absent.
        size_t cursor = page.heapStart;
        bool touched = false;
        auto clear = [&](size_t from, size_t to) {
            if (to <= from) {
                return;
            }
            std::fill(m_buffer.begin() + static_cast<std::ptrdiff_t>(from),
                      m_buffer.begin() + static_cast<std::ptrdiff_t>(to), '\0');
            zeroed += static_cast<int>(to - from);
            touched = true;
        };
        for (const auto &span : keep) {
            clear(cursor, std::min(span.first, page.indexEnd));
            cursor = std::max(cursor, span.second);
        }
        clear(cursor, page.indexEnd);

        // The row index itself has slack too. Each group has sixteen
        // 2-byte offset slots and the last group is almost never full,
        // so the slots past the end of the index hold whatever was
        // written over them last. A real playlist name survived every
        // pass above by sitting in exactly those bytes, on a page whose
        // last group had four rows in sixteen slots.
        //
        // The end of the index is num_row_offsets, NOT num_rows. The
        // format's own words: num_row_offsets is "the number of row
        // offsets that have ever been allocated, including those that
        // are no longer valid", num_rows is "the number of valid rows
        // currently present". Delete a row from the middle of a page and
        // the two diverge, and every slot between them is a LIVE row's
        // offset. Zeroing those pointed present rows at the start of the
        // heap: the catalog stopped parsing, commit()'s reparse check
        // refused to write it, and the anonymizer reported "failed to
        // commit anonymized export.pdb" -- so on the first real stick
        // this met, every rekordbox title, filename and path came out of
        // the anonymizer completely unscrubbed, and only the byte sweep
        // at the end stopped that export being handed to anybody.
        //
        // Two bounds now, because one of them was wrong for a year: a
        // slot is cleared only when it is past num_row_offsets AND its
        // present bit is clear. Nothing reads such a slot; the used
        // ones, the present flags and the transaction flags are left
        // exactly as they are.
        for (size_t g = 0; g < page.groups; ++g) {
            const size_t groupBase = page.pageStart + page.lenPage - g * RowGroupSizeBytes;
            const size_t presentFlagsOffset = groupBase - 4;
            const uint16_t presentFlags = presentFlagsOffset + 2 <= m_buffer.size()
                                              ? readU16LE(m_buffer, presentFlagsOffset)
                                              : 0xffff;  // unreadable: treat every slot as in use
            for (size_t r = 0; r < 16; ++r) {
                if (g * 16 + r < page.rowOffsets) {
                    continue;
                }
                if ((presentFlags >> r) & 1) {
                    continue;
                }
                const size_t slot = groupBase - 6 - r * 2;
                if (slot < page.indexEnd || slot + 2 > page.pageStart + page.lenPage) {
                    continue;
                }
                if (m_buffer[slot] != '\0' || m_buffer[slot + 1] != '\0') {
                    m_buffer[slot] = '\0';
                    m_buffer[slot + 1] = '\0';
                    zeroed += 2;
                    touched = true;
                }
            }
        }

        if (touched) {
            m_editedPageIndices.insert(page.pageIndex);
        }
    }
    return zeroed;
}

bool PdbRowWriter::repointPlaylistEntry(uint32_t playlistId, uint32_t oldTrackId, uint32_t newTrackId)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_PLAYLIST_ENTRIES, [&](kaitai::kstruct *body) {
        auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(body);
        return e != nullptr && e->playlist_id() == playlistId && e->track_id() == oldTrackId;
    });
    if (!found) {
        return false;
    }
    writeU32LE(m_buffer, found->rowBodyOffset + PlaylistEntryTrackIdOffset, newTrackId);
    m_editedPageIndices.insert(found->pageIndex);
    return true;
}

size_t PdbRowWriter::reassignPlaylistMemberships(uint32_t oldTrackId, uint32_t newTrackId)
{
    auto oldEntries = findAllPlaylistEntriesForTrack(m_buffer, oldTrackId);
    if (oldEntries.empty()) {
        return 0;
    }

    std::set<uint32_t> newTrackAlreadyIn;
    for (const auto &m : findAllPlaylistEntriesForTrack(m_buffer, newTrackId)) {
        newTrackAlreadyIn.insert(m.playlistId);
    }

    size_t affected = 0;
    for (const auto &m : oldEntries) {
        if (newTrackAlreadyIn.count(m.playlistId)) {
            // newTrackId is already in this playlist -- just drop the
            // oldTrackId entry rather than create a duplicate.
            deleteRowAt(m.row.pageIndex, m.row.presentFlagsOffset, m.row.rowIndexBit);
        } else {
            writeU32LE(m_buffer, m.row.rowBodyOffset + PlaylistEntryTrackIdOffset, newTrackId);
            // If oldTrackId somehow had more than one entry in the same
            // playlist, don't repoint the second one too -- one is
            // enough to preserve membership, the rest would be dupes.
            newTrackAlreadyIn.insert(m.playlistId);
        }
        m_editedPageIndices.insert(m.row.pageIndex);
        ++affected;
    }
    return affected;
}

void PdbRowWriter::deleteRowAt(uint32_t pageIndex, size_t presentFlagsOffset, uint16_t bit)
{
    const size_t lenPage = readU32LE(m_buffer, HeaderLenPageOffset);
    const size_t base = lenPage * pageIndex;
    uint16_t present = readU16LE(m_buffer, presentFlagsOffset);
    const uint16_t mask = static_cast<uint16_t>(1u << bit);
    m_editedPageIndices.insert(pageIndex);
    if ((present & mask) == 0) {
        return;
    }
    writeU16LE(m_buffer, presentFlagsOffset, static_cast<uint16_t>(present & ~mask));
    // num_rows is the 11 bits above num_row_offsets' 13; the fourth byte
    // is page_flags.
    uint32_t word = readU32LE(m_buffer, base + 24);
    const uint32_t numRows = (word >> 13) & 0x7FFu;
    if (numRows > 0) {
        word = (word & ~(0x7FFu << 13)) | ((numRows - 1) << 13);
    }
    word |= 0x10u << 24;
    writeU32LE(m_buffer, base + 24, word);
    // The row's index: its group (counted from the page end, 0x24 bytes
    // each, flags 4 bytes before the group's base) times sixteen plus bit.
    const size_t groupBase = presentFlagsOffset + 4;
    const uint32_t group = static_cast<uint32_t>((base + lenPage - groupBase) / 0x24);
    m_deletedRowsByPage[pageIndex].insert(group * 16 + bit);
}

size_t PdbRowWriter::removePlaylistEntry(uint32_t playlistId, uint32_t trackId)
{
    size_t removed = 0;
    for (const auto &m : findAllPlaylistEntriesForTrack(m_buffer, trackId)) {
        if (m.playlistId != playlistId) {
            continue;
        }
        deleteRowAt(m.row.pageIndex, m.row.presentFlagsOffset, m.row.rowIndexBit);
        ++removed;
    }
    return removed;
}

size_t PdbRowWriter::removePlaylistEntries(uint32_t playlistId, const std::set<uint32_t> &trackIds)
{
    const auto rows = playlistEntriesOf(m_buffer, playlistId);
    std::vector<uint32_t> keep;
    size_t removed = 0;
    for (const auto &e : rows) {
        if (trackIds.count(e.trackId)) {
            ++removed;
        } else {
            keep.push_back(e.trackId);
        }
    }
    if (removed == 0) {
        return 0;
    }
    rewritePlaylistEntries(playlistId, keep);
    return removed;
}

size_t PdbRowWriter::appendPlaylistEntries(uint32_t playlistId, const std::vector<uint32_t> &trackIds)
{
    uint32_t highestIndex = 0;
    std::set<uint32_t> already;
    for (const auto &e : playlistEntriesOf(m_buffer, playlistId)) {
        highestIndex = std::max(highestIndex, e.entryIndex);
        already.insert(e.trackId);
    }
    std::vector<uint32_t> adding;
    for (const uint32_t id : trackIds) {
        if (already.insert(id).second) {
            adding.push_back(id);
        }
    }
    if (adding.empty()) {
        return 0;
    }
    std::vector<std::string> rows;
    for (size_t i = 0; i < adding.size(); ++i) {
        rows.push_back(playlistEntryRowBytes(highestIndex + 1 + static_cast<uint32_t>(i), adding[i], playlistId));
    }
    writeRows(placeRows(PlaylistEntriesTableType, rows), rows);
    return adding.size();
}

PdbRowWriter::RowPlacement PdbRowWriter::placeRows(uint32_t tableType, const std::vector<std::string> &rows) const
{
    const size_t lenPage = readU32LE(m_buffer, HeaderLenPageOffset);
    const uint32_t numTables = readU32LE(m_buffer, HeaderNumTablesOffset);
    std::optional<size_t> tableEntry;
    for (uint32_t t = 0; t < numTables; ++t) {
        const size_t entry = TablesOffset + TableEntrySize * t;
        if (readU32LE(m_buffer, entry) == tableType) {
            tableEntry = entry;
        }
    }
    if (!tableEntry) {
        throw std::runtime_error("export.pdb has no table of type " + std::to_string(tableType));
    }
    RowPlacement placement;
    placement.tableEntry = *tableEntry;
    placement.tableType = tableType;
    placement.lastPage = readU32LE(m_buffer, *tableEntry + 12);
    const size_t base = lenPage * placement.lastPage;
    if (base + lenPage > m_buffer.size() || readU32LE(m_buffer, base + 8) != tableType) {
        throw std::runtime_error("export.pdb's last page of table " + std::to_string(tableType) + " is not a page of that table");
    }
    if (rows.empty()) {
        return placement;
    }
    // A table holding no rows yet ends at its index page, which takes none.
    const bool lastIsDataPage = (static_cast<unsigned char>(m_buffer[base + 27]) & 0x40) == 0;
    if (lastIsDataPage) {
        const uint32_t numRowOffsets = readU32LE(m_buffer, base + 24) & 0x1FFFu;
        const uint16_t freeSize = readU16LE(m_buffer, base + 28);
        if (rowsFitting(numRowOffsets, freeSize, rows, 0) == rows.size()) {
            placement.onLastPage = rows.size();
            return placement;
        }
    }
    // rekordbox starts a new page for rows that do not all fit on the
    // last one, even when some would: given 34 entries and a last page
    // with room for 12, it put all 34 on a new page (#62's reference
    // export). Each new page is filled before the next is started.
    const uint16_t freshFree = static_cast<uint16_t>(lenPage - PageHeaderSize);
    for (size_t next = 0; next < rows.size();) {
        const size_t fit = rowsFitting(0, freshFree, rows, next);
        if (fit == 0) {
            throw PdbPageFull("export.pdb: a row of " + std::to_string(rows[next].size()) + " bytes does not fit on a page");
        }
        placement.perNewPage.push_back(fit);
        next += fit;
    }
    const uint32_t candidate = readU32LE(m_buffer, *tableEntry + 4);
    if (readU32LE(m_buffer, base + 12) != candidate) {
        throw PdbPageFull("export.pdb's last page of table " + std::to_string(tableType)
                          + " does not lead to the table's empty candidate page ("
                          + std::to_string(readU32LE(m_buffer, base + 12)) + " vs " + std::to_string(candidate)
                          + "); not starting a new page there");
    }
    // The candidate is a page nothing uses yet: past the end of the file
    // on the sticks surveyed, a page of zeros inside it on a fresh export.
    const size_t candidateBase = lenPage * candidate;
    if (candidateBase < m_buffer.size()) {
        const auto first = m_buffer.begin() + static_cast<std::ptrdiff_t>(candidateBase);
        const auto last = m_buffer.begin() + static_cast<std::ptrdiff_t>(std::min(candidateBase + lenPage, m_buffer.size()));
        if (std::any_of(first, last, [](char c) { return c != '\0'; })) {
            throw PdbPageFull("export.pdb's empty candidate page " + std::to_string(candidate)
                              + " is not empty; not starting a new page there");
        }
    }
    return placement;
}

size_t PdbRowWriter::rowsFitting(uint32_t numRowOffsets, uint16_t freeSize, const std::vector<std::string> &rows, size_t from)
{
    size_t fit = 0;
    size_t used = 0;
    while (from + fit < rows.size() && numRowOffsets + fit < 0x7FFu) {
        const size_t cost = paddedRowSize(rows[from + fit].size()) + 2 + ((numRowOffsets + fit) % 16 == 0 ? 4 : 0);
        if (used + cost > freeSize) {
            break;
        }
        used += cost;
        ++fit;
    }
    return fit;
}

void PdbRowWriter::writeRows(const RowPlacement &placement, const std::vector<std::string> &rows)
{
    size_t next = 0;
    if (placement.onLastPage > 0) {
        appendRowsToPage(placement.lastPage, rows, next, placement.onLastPage);
    }
    for (const size_t count : placement.perNewPage) {
        appendRowsToPage(startPage(placement.tableEntry, placement.tableType), rows, next, count);
    }
}

uint32_t PdbRowWriter::startPage(size_t tableEntry, uint32_t tableType)
{
    // As rekordbox started page 54 in #62's reference export: the table's
    // empty candidate becomes its last page, pointing at the file's next
    // unused page, which becomes the new candidate; next_unused_page moves
    // one on. The old last page already points at the candidate. When the
    // old last page is the table's index page (a table with no rows yet),
    // the index page also names the new page as its first data page.
    const size_t lenPage = readU32LE(m_buffer, HeaderLenPageOffset);
    const uint32_t oldLast = readU32LE(m_buffer, tableEntry + 12);
    const uint32_t page = readU32LE(m_buffer, tableEntry + 4);
    const uint32_t nextUnused = readU32LE(m_buffer, HeaderNextUnusedPageOffset);
    const size_t base = lenPage * page;
    if (m_buffer.size() < base + lenPage) {
        // A page past the end grows the file; any page in between stays
        // zeros, as a write past the end of a file leaves it.
        m_buffer.resize(base + lenPage, '\0');
    }
    writeU32LE(m_buffer, base + 4, page);
    writeU32LE(m_buffer, base + 8, tableType);
    writeU32LE(m_buffer, base + 12, nextUnused);
    writeU32LE(m_buffer, base + 24, 0x24u << 24);
    writeU16LE(m_buffer, base + 28, static_cast<uint16_t>(lenPage - PageHeaderSize));
    writeU16LE(m_buffer, base + 30, 0);
    writeU32LE(m_buffer, tableEntry + 4, nextUnused);
    writeU32LE(m_buffer, tableEntry + 12, page);
    writeU32LE(m_buffer, HeaderNextUnusedPageOffset, nextUnused + 1);
    const size_t oldBase = lenPage * oldLast;
    if ((static_cast<unsigned char>(m_buffer[oldBase + 27]) & 0x40) != 0) {
        writeU32LE(m_buffer, oldBase + IndexFirstDataPageOffset, page);
    }
    m_editedPageIndices.insert(page);
    return page;
}

void PdbRowWriter::appendRowsToPage(uint32_t pageIndex, const std::vector<std::string> &rows, size_t &next, size_t count)
{
    const size_t lenPage = readU32LE(m_buffer, HeaderLenPageOffset);
    const size_t base = lenPage * pageIndex;
    uint32_t numRowOffsets = readU32LE(m_buffer, base + 24) & 0x1FFFu;
    uint32_t numRows = (readU32LE(m_buffer, base + 24) >> 13) & 0x7FFu;
    uint16_t freeSize = readU16LE(m_buffer, base + 28);
    uint16_t usedSize = readU16LE(m_buffer, base + 30);

    // rekordbox adds entries one transaction each, so after a batch the
    // page names only the last row: transaction (1, that row) and its
    // transaction flag alone, every other group's cleared (34 rows added
    // in #62's reference export, one row on the fixture's pages).
    const size_t groupsBefore = numRowOffsets == 0 ? 0 : (numRowOffsets - 1) / 16 + 1;
    for (size_t g = 0; g < groupsBefore; ++g) {
        writeU16LE(m_buffer, base + lenPage - g * RowGroupSizeBytes - 2, 0);
    }
    for (size_t k = 0; k < count; ++k, ++next) {
        const uint32_t i = numRowOffsets;
        const size_t r = i % 16;
        const size_t groupBase = base + lenPage - (i / 16) * RowGroupSizeBytes;
        if (r == 0) {
            writeU16LE(m_buffer, groupBase - 4, 0);
            writeU16LE(m_buffer, groupBase - 2, 0);
        }
        const size_t heapOffset = usedSize;
        const std::string &row = rows[next];
        const size_t size = paddedRowSize(row.size());
        std::copy(row.begin(), row.end(), m_buffer.begin() + static_cast<std::ptrdiff_t>(base + PageHeaderSize + heapOffset));
        std::fill_n(m_buffer.begin() + static_cast<std::ptrdiff_t>(base + PageHeaderSize + heapOffset + row.size()),
                    size - row.size(), '\0');
        writeU16LE(m_buffer, groupBase - 6 - 2 * r, static_cast<uint16_t>(heapOffset));
        writeU16LE(m_buffer, groupBase - 4, static_cast<uint16_t>(readU16LE(m_buffer, groupBase - 4) | (1u << r)));
        freeSize = static_cast<uint16_t>(freeSize - (size + 2 + (r == 0 ? 4 : 0)));
        usedSize = static_cast<uint16_t>(usedSize + size);
        ++numRowOffsets;
        ++numRows;
    }
    const uint32_t last = numRowOffsets - 1;
    const size_t lastFlags = base + lenPage - (last / 16) * RowGroupSizeBytes - 2;
    writeU16LE(m_buffer, lastFlags, static_cast<uint16_t>(1u << (last % 16)));
    // num_row_offsets (13 bits) and num_rows (11 bits) share three bytes
    // with page_flags as the fourth, which is kept.
    const uint32_t flagsByte = readU32LE(m_buffer, base + 24) & 0xFF000000u;
    writeU32LE(m_buffer, base + 24, flagsByte | (numRows << 13) | numRowOffsets);
    writeU16LE(m_buffer, base + 28, freeSize);
    writeU16LE(m_buffer, base + 30, usedSize);
    writeU16LE(m_buffer, base + 32, 1);
    writeU16LE(m_buffer, base + 34, static_cast<uint16_t>(last));
    m_editedPageIndices.insert(pageIndex);
    m_appendedPages.insert(pageIndex);
}

std::vector<PdbRowWriter::TreeRowSlot> PdbRowWriter::siblingsOf(const std::string &buffer, uint32_t parentId)
{
    std::vector<TreeRow> level;
    for (const auto &r : treeRowsOf(buffer)) {
        if (r.parentId == parentId) {
            level.push_back(r);
        }
    }
    std::stable_sort(level.begin(), level.end(), [](const auto &a, const auto &b) { return a.sortOrder < b.sortOrder; });
    std::vector<TreeRowSlot> slots;
    for (const auto &r : level) {
        slots.push_back({r.id, r.parentId, r.isFolder, r.nameBytes});
    }
    return slots;
}

std::string PdbRowWriter::treeRowBytes(const TreeRowSlot &slot, uint32_t sortOrder)
{
    // parent_id, a field rekordbox always leaves 0, sort_order, id,
    // is_folder, then the name.
    std::string row;
    appendU32(row, slot.parentId);
    appendU32(row, 0);
    appendU32(row, sortOrder);
    appendU32(row, slot.id);
    appendU32(row, slot.isFolder ? 1 : 0);
    return row + slot.nameBytes;
}

std::vector<PdbRowWriter::PlaylistTreeNode> PdbRowWriter::playlistTree() const
{
    std::vector<PlaylistTreeNode> nodes;
    for (const auto &r : treeRowsOf(m_buffer)) {
        nodes.push_back({r.id, r.parentId, r.sortOrder, r.isFolder, r.name});
    }
    return nodes;
}

uint32_t PdbRowWriter::createPlaylist(uint32_t parentId, const std::string &name, bool isFolder, std::optional<size_t> position)
{
    if (m_format != Format::Export) {
        throw std::logic_error("playlists live in export.pdb");
    }
    if (name.empty() || name.find('/') != std::string::npos) {
        throw std::invalid_argument("a playlist name must be non-empty and hold no '/'");
    }
    const auto tree = treeRowsOf(m_buffer);
    // Playlists are matched by path across the three libraries, so a level
    // holds a name once.
    for (const auto &r : tree) {
        if (r.parentId == parentId && r.name == name) {
            throw std::invalid_argument("\"" + name + "\" is already there");
        }
    }
    if (parentId != 0) {
        const auto parent = std::find_if(tree.begin(), tree.end(), [&](const auto &r) { return r.id == parentId; });
        if (parent == tree.end() || !parent->isFolder) {
            throw std::invalid_argument("export.pdb has no folder " + std::to_string(parentId));
        }
    }
    const uint32_t id = highestPlaylistTreeId(m_buffer) + 1;
    std::vector<TreeRowSlot> level = siblingsOf(m_buffer, parentId);
    TreeRowSlot added;
    added.id = id;
    added.parentId = parentId;
    added.isFolder = isFolder;
    added.nameBytes = encodeDeviceSqlString(name);
    const size_t at = std::min(position.value_or(level.size()), level.size());
    level.insert(level.begin() + static_cast<std::ptrdiff_t>(at), added);
    rewriteTreeLevel(parentId, level);
    return id;
}

size_t PdbRowWriter::deletePlaylist(uint32_t id)
{
    const auto tree = treeRowsOf(m_buffer);
    const auto node = std::find_if(tree.begin(), tree.end(), [&](const auto &r) { return r.id == id; });
    if (node == tree.end()) {
        return 0;
    }
    // The node and everything under it, folders included.
    std::set<uint32_t> doomed{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto &r : tree) {
            if (doomed.count(r.parentId) && doomed.insert(r.id).second) {
                grew = true;
            }
        }
    }
    std::vector<TreeRowSlot> level;
    for (auto &slot : siblingsOf(m_buffer, node->parentId)) {
        if (slot.id != id) {
            level.push_back(slot);
        }
    }
    // Room for the rewritten level first, so a refusal changes nothing.
    std::vector<std::string> rows;
    for (size_t i = 0; i < level.size(); ++i) {
        rows.push_back(treeRowBytes(level[i], static_cast<uint32_t>(i)));
    }
    const auto placement = placeRows(PlaylistTreeTableType, rows);
    for (const auto &r : tree) {
        if (doomed.count(r.id) && r.parentId != node->parentId) {
            deleteRowAt(r.row.pageIndex, r.row.presentFlagsOffset, r.row.rowIndexBit);
        }
        if (doomed.count(r.id) && !r.isFolder) {
            for (const auto &e : playlistEntriesOf(m_buffer, r.id)) {
                deleteRowAt(e.row.pageIndex, e.row.presentFlagsOffset, e.row.rowIndexBit);
            }
        }
    }
    for (const auto &r : tree) {
        if (r.parentId == node->parentId) {
            deleteRowAt(r.row.pageIndex, r.row.presentFlagsOffset, r.row.rowIndexBit);
        }
    }
    writeRows(placement, rows);
    return doomed.size();
}

bool PdbRowWriter::reorderPlaylist(uint32_t playlistId, const std::vector<uint32_t> &trackIds)
{
    const auto entries = playlistEntriesOf(m_buffer, playlistId);
    std::vector<uint32_t> now;
    for (const auto &e : entries) {
        now.push_back(e.trackId);
    }
    if (now == trackIds) {
        return false;
    }
    std::vector<uint32_t> a = now;
    std::vector<uint32_t> b = trackIds;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) {
        throw std::invalid_argument("a new order must hold the playlist's tracks, each as often as now");
    }
    rewritePlaylistEntries(playlistId, trackIds);
    return true;
}

void PdbRowWriter::rewritePlaylistEntries(uint32_t playlistId, const std::vector<uint32_t> &trackIds)
{
    const auto old = playlistEntriesOf(m_buffer, playlistId);
    std::vector<std::string> rows;
    for (size_t i = 0; i < trackIds.size(); ++i) {
        rows.push_back(playlistEntryRowBytes(static_cast<uint32_t>(i + 1), trackIds[i], playlistId));
    }
    const auto placement = placeRows(PlaylistEntriesTableType, rows);
    for (const auto &e : old) {
        deleteRowAt(e.row.pageIndex, e.row.presentFlagsOffset, e.row.rowIndexBit);
    }
    writeRows(placement, rows);
}

void PdbRowWriter::rewriteTreeLevel(uint32_t parentId, const std::vector<TreeRowSlot> &level)
{
    const auto tree = treeRowsOf(m_buffer);
    // rekordbox changes one level of the tree by deleting all of its rows
    // and adding them again, sort_order 0 on (#62's reference export:
    // creating, reordering and deleting each rewrote the top level).
    std::vector<std::string> rows;
    for (size_t i = 0; i < level.size(); ++i) {
        rows.push_back(treeRowBytes(level[i], static_cast<uint32_t>(i)));
    }
    const auto placement = placeRows(PlaylistTreeTableType, rows);
    for (const auto &r : tree) {
        if (r.parentId == parentId) {
            deleteRowAt(r.row.pageIndex, r.row.presentFlagsOffset, r.row.rowIndexBit);
        }
    }
    writeRows(placement, rows);
}

void PdbRowWriter::listDeletionInTableIndex(uint32_t pageIndex)
{
    // rekordbox lists a playlist or playlist-tree page it deleted rows
    // from on the table's index page (its first): the entry is the page
    // index shifted left by three, num_entries and next_offset one higher
    // (#62's reference exports: page 17 for entries, 16 and 65 for the
    // tree). Only the plain case is written, an index whose
    // entries are all in use with no free list (first_empty 0x1fff);
    // an index with freed slots is left as it is, as rekordbox itself
    // leaves some pages with deletions unlisted.
    const size_t lenPage = readU32LE(m_buffer, HeaderLenPageOffset);
    const size_t pageBase = lenPage * pageIndex;
    const uint32_t tableType = readU32LE(m_buffer, pageBase + 8);
    if (m_format != Format::Export || (tableType != PlaylistEntriesTableType && tableType != PlaylistTreeTableType)) {
        return;
    }
    const uint32_t numTables = readU32LE(m_buffer, HeaderNumTablesOffset);
    for (uint32_t t = 0; t < numTables; ++t) {
        const size_t entry = TablesOffset + TableEntrySize * t;
        if (readU32LE(m_buffer, entry) != tableType) {
            continue;
        }
        const uint32_t indexPage = readU32LE(m_buffer, entry + 8);
        const size_t base = lenPage * indexPage;
        if (base + lenPage > m_buffer.size() || (static_cast<unsigned char>(m_buffer[base + 27]) & 0x40) == 0) {
            return;
        }
        const uint16_t capacity = readU16LE(m_buffer, base + IndexCapacityOffset);
        const uint16_t nextOffset = readU16LE(m_buffer, base + IndexNextOffsetOffset);
        const uint16_t numEntries = readU16LE(m_buffer, base + IndexNumEntriesOffset);
        const uint16_t firstEmpty = readU16LE(m_buffer, base + IndexFirstEmptyOffset);
        const size_t entries = base + IndexEntriesOffset;
        if (firstEmpty != IndexNoFreeSlot || numEntries != nextOffset || nextOffset >= capacity
            || entries + 4u * (nextOffset + 1u) > base + lenPage) {
            return;
        }
        for (uint16_t i = 0; i < nextOffset; ++i) {
            if (readU32LE(m_buffer, entries + 4u * i) >> 3 == pageIndex) {
                return;
            }
        }
        if (readU32LE(m_buffer, entries + 4u * nextOffset) != IndexEmptySlot) {
            return;
        }
        writeU32LE(m_buffer, entries + 4u * nextOffset, pageIndex << 3);
        writeU16LE(m_buffer, base + IndexNextOffsetOffset, static_cast<uint16_t>(nextOffset + 1));
        writeU16LE(m_buffer, base + IndexNumEntriesOffset, static_cast<uint16_t>(numEntries + 1));
        m_editedPageIndices.insert(indexPage);
        return;
    }
}

bool PdbRowWriter::commit()
{
    if (m_editedPageIndices.empty()) {
        return false;
    }

    // Staleness check: if the real file changed since we read it,
    // something else touched it in the meantime -- our in-memory copy is
    // no longer a safe base to overwrite it with. size/mtime are a cheap
    // pre-filter but proven insufficient alone on Windows: an in-process
    // external write that doesn't change the file's length can leave
    // both blind (confirmed empirically -- fs::last_write_time() genuinely
    // does not observe an in-process rewrite of identical length on
    // Windows, while a fresh read of the file's actual bytes does). A
    // whole-file checksum against a real re-read is the authoritative
    // signal; size/mtime stay as an OR alongside it since they're free
    // and catch the common case without reading the whole file again.
    std::error_code statEc;
    auto currentSize = fs::file_size(pathFromUtf8(m_pdbPath), statEc);
    auto currentMtime = fs::last_write_time(pathFromUtf8(m_pdbPath), statEc);
    bool statMismatch = statEc || currentSize != m_originalFileSize || currentMtime != m_originalMtime;
    bool checksumMismatch = true;
    try {
        checksumMismatch = checksumOf(readWholeFile(m_pdbPath)) != m_originalChecksum;
    } catch (const std::exception &) {
        checksumMismatch = true;  // couldn't even re-read it -- treat as stale, not as "unchanged"
    }
    if (statMismatch || checksumMismatch) {
        return false;
    }

    uint32_t lenPage = readU32LE(m_buffer, HeaderLenPageOffset);
    uint32_t currentSequence = readU32LE(m_buffer, HeaderSequenceOffset);
    // A page this session deleted rows from records them as its last
    // transaction, the way rekordbox does: the count, the first row, and
    // transaction flags marking exactly those rows.
    for (const auto &[pageIndex, rows] : m_deletedRowsByPage) {
        if (rows.empty() || m_appendedPages.contains(pageIndex)) {
            continue;
        }
        const size_t base = static_cast<size_t>(lenPage) * pageIndex;
        const uint32_t numRowOffsets = readU32LE(m_buffer, base + 24) & 0x1FFFu;
        const size_t groups = numRowOffsets == 0 ? 0 : (numRowOffsets - 1) / 16 + 1;
        for (size_t g = 0; g < groups; ++g) {
            writeU16LE(m_buffer, base + lenPage - g * 0x24 - 2, 0);
        }
        for (const uint32_t row : rows) {
            const size_t flagsAt = base + lenPage - (row / 16) * 0x24 - 2;
            writeU16LE(m_buffer, flagsAt, static_cast<uint16_t>(readU16LE(m_buffer, flagsAt) | (1u << (row % 16))));
        }
        writeU16LE(m_buffer, base + 32, static_cast<uint16_t>(rows.size()));
        writeU16LE(m_buffer, base + 34, static_cast<uint16_t>(*rows.begin()));
    }
    for (const auto &[pageIndex, rows] : m_deletedRowsByPage) {
        if (!rows.empty()) {
            listDeletionInTableIndex(pageIndex);
        }
    }
    for (uint32_t pageIndex : m_editedPageIndices) {
        size_t pageSequenceOffset = static_cast<size_t>(lenPage) * pageIndex + PageSequenceOffset;
        writeU32LE(m_buffer, pageSequenceOffset, currentSequence);
    }
    writeU32LE(m_buffer, HeaderSequenceOffset, currentSequence + 1);

    // A bug in this class producing a broken file must never reach
    // disk -- confirm the edited result is still structurally readable
    // before writing it anywhere.
    if (!reparsesCleanly(m_buffer, m_format == Format::ExportExt)) {
        return false;
    }

    if (!writeFileDurablyAtomic(m_pdbPath, m_buffer)) {
        return false;
    }

    m_editedPageIndices.clear();
    m_deletedRowsByPage.clear();
    m_appendedPages.clear();
    return true;
}

}  // namespace seabass::infrastructure::rekordbox
