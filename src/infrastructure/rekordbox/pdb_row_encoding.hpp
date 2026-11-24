// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>

namespace seabass::infrastructure::rekordbox
{

// The rows of export.pdb's track table and its lookup tables as bytes,
// for rows Seabass adds (the track import). Each encoder is held against
// rekordbox 7.2.18's own rows in pdb_row_encoding_test: a row rekordbox
// wrote, read into these fields and encoded again, is the same bytes.
//
// A row's place on its page is not the encoder's business, with one
// exception: `rowIndex`, the row's number on the page it goes to, which
// the track, artist and album rows repeat inside themselves (times 0x20,
// the field the format's description calls index_shift).

// A DeviceSQL string as rekordbox writes it: plain ASCII up to 126 bytes
// as a short string (one byte: (length + 1) * 2 + 1), longer ASCII as a
// long one (0x40, a 16-bit total length, a zero byte), anything else as
// UTF-16LE (0x90, the same header). Throws std::invalid_argument for a
// text too long for the 16-bit length.
std::string encodeDeviceSqlString(const std::string &text);

// genre, label and artwork rows: the id, then the name (an artwork row's
// name is its path, "/PIONEER/Artwork/00001/a1.jpg").
std::string encodePdbNameRow(std::uint32_t id, const std::string &name);

// A key row holds its id twice.
std::string encodePdbKeyRow(std::uint32_t id, const std::string &name);

// Artist and album rows reach their name through an offset. In these and
// in a track row, a string that is not a short one starts on a multiple
// of four bytes from the row's start, zero bytes before it.
std::string encodePdbArtistRow(std::uint32_t rowIndex, std::uint32_t id, const std::string &name);
std::string encodePdbAlbumRow(std::uint32_t rowIndex, std::uint32_t id, std::uint32_t artistId, const std::string &name);

// Every field of a track row. The names are the format description's
// (specs/rekordbox_pdb.ksy) where it has one; the fields it calls
// unknown are named for the exportLibrary.db column that holds the same
// value on the same track (measured on two rekordbox 7.2.18 exports,
// 1172 rows).
struct PdbTrackRowFields
{
    std::uint32_t rowIndex = 0;
    // exportLibrary.db's contentLink family: 0xC0700 or 0xD0700 on every
    // row seen.
    std::uint32_t bitmask = 0xC0700;
    std::uint32_t sampleRate = 0;
    std::uint32_t composerId = 0;
    std::uint32_t fileSize = 0;
    // rekordbox's own collection: the track's id there, and the
    // collection's. 0 for a row no rekordbox collection holds.
    std::uint32_t masterContentId = 0;
    std::uint32_t masterDbId = 0;
    std::uint32_t artworkId = 0;
    std::uint32_t keyId = 0;
    std::uint32_t originalArtistId = 0;
    std::uint32_t labelId = 0;
    std::uint32_t remixerId = 0;
    std::uint32_t bitrate = 0;
    std::uint32_t trackNumber = 0;
    // BPM times 100.
    std::uint32_t tempo = 0;
    std::uint32_t genreId = 0;
    std::uint32_t albumId = 0;
    std::uint32_t artistId = 0;
    std::uint32_t id = 0;
    std::uint16_t discNumber = 0;
    std::uint16_t playCount = 0;
    std::uint16_t year = 0;
    std::uint16_t sampleDepth = 0;
    // Whole seconds.
    std::uint16_t duration = 0;
    // 41 on every row seen; what it counts is not known.
    std::uint16_t unknown86 = 41;
    std::uint8_t colorId = 0;
    std::uint8_t rating = 0;
    // 1 mp3, 4 m4a, 5 flac, 11 wav, 12 aiff.
    std::uint16_t fileType = 0;
    // 3 on every row seen.
    std::uint16_t unknown92 = 3;

    // The 21 strings, in the row's order.
    std::string isrc;
    std::string texter;
    // exportLibrary.db's informationUpdateCount, analysisDataUpdateCount
    // and cueUpdateCount as decimal text; empty where that column is NULL.
    std::string informationUpdateCount;
    std::string analysisDataUpdateCount;
    std::string cueUpdateCount;
    std::string message;
    std::string kuvoPublic = "ON";
    std::string autoloadHotcues = "ON";
    std::string unknownString8;
    std::string unknownString9;
    std::string dateAdded;
    std::string releaseDate;
    std::string mixName;
    std::string unknownString13;
    std::string analyzePath;
    std::string analyzeDate;
    std::string comment;
    std::string title;
    std::string unknownString18;
    std::string filename;
    std::string filePath;
};

// The row, from its first byte to the end of its last string. rekordbox
// leaves some 48 bytes of room after that before the next row; how much
// is the caller's to add.
std::string encodePdbTrackRow(const PdbTrackRowFields &fields);

}  // namespace seabass::infrastructure::rekordbox
