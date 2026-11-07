// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <istream>
#include <optional>
#include <string>

#include "domain/mp3_lead_in.hpp"

namespace seabass::infrastructure::audio
{

// Reads what the start of an MP3 file says about its lead-in (#50): the
// first MPEG Layer III frame after any ID3v2 tag, whether it is a
// Xing/Info or VBRI frame, its LAME extension (encoder string, delay and
// padding, CRC) and the channel mode of the frame after it.
//
// Reads the 10-byte header of each ID3v2 tag in a row at the start (at
// most MaxId3TagsFollowed of them), seeks past them, and reads at most
// LeadInWindowBytes from there: never more than MaxLeadInBytesRead in
// all, whatever the file. No TagLib, so the cover art in the tag is never
// read.
//
// std::nullopt for anything that is not MPEG Layer III audio or whose
// frames cannot be followed: the callers then apply no correction.
std::optional<domain::Mp3LeadIn> readMp3LeadIn(const std::string &absoluteFilePath);

// The same from an open stream positioned at the start of the file. For
// tests that count what was read.
std::optional<domain::Mp3LeadIn> readMp3LeadIn(std::istream &in);

// The version of what readMp3LeadIn returns, stored with every lead-in
// the metadata cache keeps: an entry an earlier parser wrote is read
// again. Raise it whenever the parser starts answering differently for
// some file.
//   2  the CRC-protected tag offset and VBRI
//   3  where the tag frame sits, and whether it counts frames or bytes
constexpr int Mp3LeadInParserVersion = 3;

constexpr std::size_t LeadInWindowBytes = 16 * 1024;
constexpr std::size_t MaxId3TagsFollowed = 8;
// The headers of the tags, one more 10-byte read that finds no tag, and
// the window.
constexpr std::size_t MaxLeadInBytesRead = 10 * (MaxId3TagsFollowed + 1) + LeadInWindowBytes;

}  // namespace seabass::infrastructure::audio
