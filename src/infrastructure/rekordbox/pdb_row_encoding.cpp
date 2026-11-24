// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/pdb_row_encoding.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace seabass::infrastructure::rekordbox
{

namespace
{

void appendU16(std::string &out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void appendU32(std::string &out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

bool isPlainAscii(const std::string &text)
{
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x80; });
}

// UTF-8 to UTF-16 code units; a malformed byte becomes U+FFFD.
std::u16string utf16Of(const std::string &utf8)
{
    std::u16string out;
    for (size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0xFFFD;
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

// Where a string that begins at `at` (from the row's first byte) really
// goes: a long string (0x40 or 0x90) starts on a multiple of four, with
// zero bytes before it; a short one starts where it is. In rekordbox's
// rows an artist's long name is at 12 where a short one is at 10, an
// album's at 24 for 22, and a track's long comment two bytes after the
// string before it ends.
size_t stringStart(size_t at, const std::string &encoded)
{
    const bool isShort = (static_cast<unsigned char>(encoded[0]) & 1) != 0;
    return isShort ? at : (at + 3) & ~size_t(3);
}

// The name of an artist or album row, behind the one-byte offset that
// leads to it.
void appendNameBehindOffset(std::string &row, const std::string &name)
{
    const std::string encoded = encodeDeviceSqlString(name);
    const size_t nameAt = stringStart(row.size() + 1, encoded);
    row.push_back(static_cast<char>(nameAt));
    row.append(nameAt - row.size(), '\0');
    row += encoded;
}

}  // namespace

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

std::string encodePdbNameRow(std::uint32_t id, const std::string &name)
{
    std::string row;
    appendU32(row, id);
    row += encodeDeviceSqlString(name);
    return row;
}

std::string encodePdbKeyRow(std::uint32_t id, const std::string &name)
{
    std::string row;
    appendU32(row, id);
    appendU32(row, id);
    row += encodeDeviceSqlString(name);
    return row;
}

std::string encodePdbArtistRow(std::uint32_t rowIndex, std::uint32_t id, const std::string &name)
{
    std::string row;
    appendU16(row, 0x60);
    appendU16(row, rowIndex * 0x20);
    appendU32(row, id);
    row.push_back('\x03');
    appendNameBehindOffset(row, name);
    return row;
}

std::string encodePdbAlbumRow(std::uint32_t rowIndex, std::uint32_t id, std::uint32_t artistId, const std::string &name)
{
    std::string row;
    appendU16(row, 0x80);
    appendU16(row, rowIndex * 0x20);
    appendU32(row, 0);
    appendU32(row, artistId);
    appendU32(row, id);
    appendU32(row, 0);
    row.push_back('\x03');
    appendNameBehindOffset(row, name);
    return row;
}

std::string encodePdbTrackRow(const PdbTrackRowFields &f)
{
    std::string row;
    appendU16(row, 0x24);
    appendU16(row, f.rowIndex * 0x20);
    appendU32(row, f.bitmask);
    appendU32(row, f.sampleRate);
    appendU32(row, f.composerId);
    appendU32(row, f.fileSize);
    appendU32(row, f.masterContentId);
    appendU32(row, f.masterDbId);
    appendU32(row, f.artworkId);
    appendU32(row, f.keyId);
    appendU32(row, f.originalArtistId);
    appendU32(row, f.labelId);
    appendU32(row, f.remixerId);
    appendU32(row, f.bitrate);
    appendU32(row, f.trackNumber);
    appendU32(row, f.tempo);
    appendU32(row, f.genreId);
    appendU32(row, f.albumId);
    appendU32(row, f.artistId);
    appendU32(row, f.id);
    appendU16(row, f.discNumber);
    appendU16(row, f.playCount);
    appendU16(row, f.year);
    appendU16(row, f.sampleDepth);
    appendU16(row, f.duration);
    appendU16(row, f.unknown86);
    row.push_back(static_cast<char>(f.colorId));
    row.push_back(static_cast<char>(f.rating));
    appendU16(row, f.fileType);
    appendU16(row, f.unknown92);

    const std::array<const std::string *, 21> strings = {
        &f.isrc,           &f.texter,       &f.informationUpdateCount, &f.analysisDataUpdateCount, &f.cueUpdateCount,
        &f.message,        &f.kuvoPublic,   &f.autoloadHotcues,        &f.unknownString8,          &f.unknownString9,
        &f.dateAdded,      &f.releaseDate,  &f.mixName,                &f.unknownString13,         &f.analyzePath,
        &f.analyzeDate,    &f.comment,      &f.title,                  &f.unknownString18,         &f.filename,
        &f.filePath,
    };
    // The offsets, then the strings in the same order, each straight
    // after the one before.
    std::string heap;
    const size_t stringsAt = row.size() + 2 * strings.size();
    for (const std::string *text : strings) {
        const std::string encoded = encodeDeviceSqlString(*text);
        const size_t at = stringStart(stringsAt + heap.size(), encoded);
        if (at > 0xFFFF) {
            throw std::invalid_argument("a track row that long does not fit its own offsets");
        }
        appendU16(row, static_cast<std::uint32_t>(at));
        heap.append(at - stringsAt - heap.size(), '\0');
        heap += encoded;
    }
    row += heap;
    return row;
}

}  // namespace seabass::infrastructure::rekordbox
