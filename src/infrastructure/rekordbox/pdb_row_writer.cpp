// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/pdb_row_writer.hpp"

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
constexpr int TrackStringIndexComment = 16;
constexpr int TrackStringIndexTitle = 17;
constexpr int TrackStringIndexFilename = 19;
constexpr int TrackStringIndexFilePath = 20;

// artist_row: subtype(u2) + index_shift(u2) + id(u4) + unnamed(u1) +
// ofs_name_near(u1) = name's near offset lives at byte 9; ofs_name_far
// (u2), used instead when subtype's 0x04 bit is set, lives at byte 10
// -- see specs/rekordbox_pdb.ksy's artist_row.
constexpr size_t ArtistSubtypeOffset = 0;
constexpr size_t ArtistNameOffsetNear = 9;
constexpr size_t ArtistNameOffsetFar = 10;
constexpr uint16_t ArtistSubtypeFarNameFlag = 0x04;

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

// Fits `text` into exactly `capacityBytes`: truncated if too long,
// right-padded with ASCII spaces if shorter. Anonymized placeholder
// text is always plain ASCII, so byte-level truncation/padding never
// splits a multi-byte character.
std::string fitAsciiToCapacity(const std::string &text, size_t capacityBytes)
{
    std::string fitted = text.substr(0, capacityBytes);
    fitted.resize(capacityBytes, ' ');
    return fitted;
}

// Re-encodes newText into the device_sql_string value already sitting
// at absOffset, filling exactly its existing text capacity (never its
// header) -- see DeviceSqlStringSpan's own comment for why this never
// resizes or reflows anything.
void overwriteDeviceSqlStringInPlace(std::string &buffer, size_t absOffset, const std::string &newText)
{
    DeviceSqlStringSpan span = readDeviceSqlStringSpan(buffer, absOffset);
    size_t headerBytes = span.totalBytes - span.textCapacityBytes;
    if (span.isUtf16) {
        size_t capacityUnits = span.textCapacityBytes / 2;
        std::string fitted = fitAsciiToCapacity(newText, capacityUnits);
        for (size_t i = 0; i < capacityUnits; ++i) {
            size_t textOffset = absOffset + headerBytes + i * 2;
            buffer.at(textOffset) = fitted[i];
            buffer.at(textOffset + 1) = '\0';
        }
    } else {
        std::string fitted = fitAsciiToCapacity(newText, span.textCapacityBytes);
        for (size_t i = 0; i < fitted.size(); ++i) {
            buffer.at(absOffset + headerBytes + i) = fitted[i];
        }
    }
}

size_t trackStringAbsOffset(const std::string &buffer, size_t rowBodyOffset, int stringIndex)
{
    size_t ofsFieldOffset = rowBodyOffset + TrackOfsStringsOffset + static_cast<size_t>(stringIndex) * 2;
    uint16_t relOffset = readU16LE(buffer, ofsFieldOffset);
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
bool reparsesCleanly(const std::string &buffer)
{
    try {
        std::istringstream iss(buffer);
        kaitai::kstream ks(&iss);
        Pdb pdb(false, &ks);
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
    Pdb pdb(false, &ks);

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

}  // namespace

PdbRowWriter::PdbRowWriter(std::string pdbPath) : m_pdbPath(std::move(pdbPath)), m_buffer(readWholeFile(m_pdbPath))
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

bool PdbRowWriter::removePlaylistEntry(uint32_t playlistId, uint32_t trackId)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_PLAYLIST_ENTRIES, [&](kaitai::kstruct *body) {
        auto *e = dynamic_cast<Pdb::playlist_entry_row_t *>(body);
        return e != nullptr && e->playlist_id() == playlistId && e->track_id() == trackId;
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

bool PdbRowWriter::overwriteTrackText(uint32_t trackId, const TrackTextOverride &text)
{
    auto found = findRow(m_buffer, Pdb::PAGE_TYPE_TRACKS, [&](kaitai::kstruct *body) {
        auto *t = dynamic_cast<Pdb::track_row_t *>(body);
        return t != nullptr && t->id() == trackId;
    });
    if (!found) {
        return false;
    }
    overwriteDeviceSqlStringInPlace(m_buffer, trackStringAbsOffset(m_buffer, found->rowBodyOffset, TrackStringIndexTitle),
                                     text.title);
    overwriteDeviceSqlStringInPlace(
        m_buffer, trackStringAbsOffset(m_buffer, found->rowBodyOffset, TrackStringIndexComment), text.comment);
    overwriteDeviceSqlStringInPlace(
        m_buffer, trackStringAbsOffset(m_buffer, found->rowBodyOffset, TrackStringIndexFilename), text.filename);
    overwriteDeviceSqlStringInPlace(
        m_buffer, trackStringAbsOffset(m_buffer, found->rowBodyOffset, TrackStringIndexFilePath), text.filePath);
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
    overwriteDeviceSqlStringInPlace(m_buffer, artistNameAbsOffset(m_buffer, found->rowBodyOffset), text);
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
    overwriteDeviceSqlStringInPlace(m_buffer, found->rowBodyOffset + PlaylistTreeNameOffset, text);
    m_editedPageIndices.insert(found->pageIndex);
    return true;
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
    if (!reparsesCleanly(m_buffer)) {
        return false;
    }

    if (!writeFileDurablyAtomic(m_pdbPath, m_buffer)) {
        return false;
    }

    m_editedPageIndices.clear();
    return true;
}

}  // namespace seabass::infrastructure::rekordbox
