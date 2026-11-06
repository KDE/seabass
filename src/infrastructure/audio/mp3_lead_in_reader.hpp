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
// Reads the 10-byte ID3v2 header, seeks past the tag, and reads at most
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

constexpr std::size_t LeadInWindowBytes = 16 * 1024;
constexpr std::size_t MaxLeadInBytesRead = 10 + LeadInWindowBytes;

}  // namespace seabass::infrastructure::audio
