// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <string>

namespace seabass::infrastructure::compression
{

// Thin wrapper over zlib's deflate/inflate (level 6, the zlib default --
// a good size/speed tradeoff for the mostly-text track/cue data this is
// used on). Throws std::runtime_error on failure.
std::string compress(const std::string &data);

// originalSize must be the exact uncompressed size (the caller is expected
// to have stored it alongside the compressed bytes -- zlib's inflate needs
// to know the output buffer size upfront). Throws std::runtime_error if
// decompression fails or doesn't produce exactly originalSize bytes.
std::string decompress(const std::string &data, size_t originalSize);

}  // namespace seabass::infrastructure::compression
