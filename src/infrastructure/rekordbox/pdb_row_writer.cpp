// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/pdb_row_writer.hpp"

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
    if (span.isUtf16) {
        size_t capacityUnits = span.textCapacityBytes / 2;
        std::string fitted = fitAsciiToCapacity(newText, capacityUnits, truncated);
        for (size_t i = 0; i < capacityUnits; ++i) {
            size_t textOffset = absOffset + headerBytes + i * 2;
            buffer.at(textOffset) = fitted[i];
            buffer.at(textOffset + 1) = '\0';
        }
    } else {
        std::string fitted = fitAsciiToCapacity(newText, span.textCapacityBytes, truncated);
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
    std::ifstream ifs(path, std::ifstream::binary);
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
    m_originalFileSize = fs::file_size(m_pdbPath);
    m_originalMtime = fs::last_write_time(m_pdbPath);
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
    uint16_t flags = readU16LE(m_buffer, found->presentFlagsOffset);
    flags &= static_cast<uint16_t>(~(static_cast<uint16_t>(1) << found->rowIndexBit));
    writeU16LE(m_buffer, found->presentFlagsOffset, flags);
    m_editedPageIndices.insert(found->pageIndex);
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
    if (absOffset < header || absOffset >= buffer.size()) {
        return false;
    }
    return overwriteDeviceSqlStringInPlace(buffer, absOffset, text, truncated);
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
    bool artistTruncated = false;
    if (!overwriteDeviceSqlStringInPlace(m_buffer, artistNameAbsOffset(m_buffer, found->rowBodyOffset), text,
                                         &artistTruncated)) {
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
    size_t lenPage = 0;
    {
        std::istringstream iss(m_buffer);
        kaitai::kstream ks(&iss);
        Pdb pdb(true, &ks);
        lenPage = pdb.len_page();
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
        // Bounded to the row's own page. nameOffset is a u2 read out of
        // the file, so it reaches 65535 while a page is 4096 bytes: an
        // offset that is wrong, or a file that is hostile, otherwise
        // writes up to sixteen pages away into an unrelated table.
        // buffer.at() only catches that once it leaves the file
        // entirely, and reparsesCleanly() walks only the tags tables, so
        // damage to a neighbouring tag_tracks page would be committed
        // without anything noticing.
        const size_t pageOfRow = lenPage == 0 ? 0 : base / lenPage;
        const size_t pageEnd = (pageOfRow + 1) * lenPage;
        const size_t nameAt = base + nameOffset;
        if (lenPage == 0 || nameAt < base || nameAt >= pageEnd) {
            continue;
        }
        // Where the write ENDS, not only where it starts. The first
        // version of this guard checked the offset and stopped there,
        // and the length is the other file-supplied number:
        // overwriteDeviceSqlStringInPlace() writes textCapacityBytes,
        // which for a long string is a u2 read straight out of the file.
        // A name that begins inside the page and claims to be longer
        // than the page still walks into the next one, which is the same
        // bug the offset check was added for, one field along.
        // tagRowKeepRanges() had this test from the start; this did not.
        const DeviceSqlStringSpan span = readDeviceSqlStringSpan(m_buffer, nameAt);
        if (span.totalBytes == 0 || nameAt + span.totalBytes > pageEnd) {
            continue;
        }
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
        if (!overwriteDeviceSqlStringInPlace(m_buffer, nameAt, placeholder(i))) {
            continue;
        }
        // Recorded HERE, beside the write, not in the scan loop above.
        // commit() refuses when this set is empty and its comment relies
        // on "empty means nothing was edited"; filling it while merely
        // looking at rows broke that, so a run where every row was
        // skipped still presented itself as having edited pages. Both
        // callers happen to short-circuit on a zero return today, which
        // is the only reason it did not matter.
        m_editedPageIndices.insert(static_cast<uint32_t>(pageOfRow));
        ++replaced;
    }
    *rowsLeftAlone = static_cast<int>(rowBodyOffsets.size()) - replaced;
    return replaced;
}

int PdbRowWriter::overwriteAllNames(NameTable table, const std::function<std::string(size_t)> &placeholder)
{
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
        // A placeholder this cannot represent is not written and not
        // counted. Every placeholder today is ASCII by construction, so
        // this changes nothing now; what it stops is a future caller
        // handing over a real name and being told it was rewritten.
        if (!overwriteDeviceSqlStringInPlace(m_buffer, nameAt, placeholder(i))) {
            continue;
        }
        m_editedPageIndices.insert(rows[i].pageIndex);
        ++replaced;
    }
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
                for (const auto &group : *page->row_groups()) {
                    for (const auto &row : *group->rows()) {
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
            uint16_t flags = readU16LE(m_buffer, m.row.presentFlagsOffset);
            flags &= static_cast<uint16_t>(~(static_cast<uint16_t>(1) << m.row.rowIndexBit));
            writeU16LE(m_buffer, m.row.presentFlagsOffset, flags);
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
    auto currentSize = fs::file_size(m_pdbPath, statEc);
    auto currentMtime = fs::last_write_time(m_pdbPath, statEc);
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
    return true;
}

}  // namespace seabass::infrastructure::rekordbox
