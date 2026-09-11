// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>

namespace seabass::infrastructure::rekordbox
{

// export.pdb is little-endian (unlike ANLZ, which is big-endian -- see
// big_endian.hpp). Same shape as that file's helpers, just byte order.
//
// Deliberately use std::string::at() (bounds-checked, throws
// std::out_of_range) rather than operator[] (unchecked -- undefined
// behavior on a bad index) for every byte touched here: a corrupt or
// truncated export.pdb feeding a bad offset into these must fail loudly
// and cleanly, not read/write past the buffer.

inline uint16_t readU16LE(const std::string &data, size_t offset)
{
    return static_cast<uint16_t>(static_cast<unsigned char>(data.at(offset)) |
                                  (static_cast<unsigned char>(data.at(offset + 1)) << 8));
}

inline uint32_t readU32LE(const std::string &data, size_t offset)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset))) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset + 1))) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset + 2))) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset + 3))) << 24);
}

inline void writeU16LE(std::string &data, size_t offset, uint16_t value)
{
    data.at(offset) = static_cast<char>(value & 0xFF);
    data.at(offset + 1) = static_cast<char>((value >> 8) & 0xFF);
}

inline void writeU32LE(std::string &data, size_t offset, uint32_t value)
{
    data.at(offset) = static_cast<char>(value & 0xFF);
    data.at(offset + 1) = static_cast<char>((value >> 8) & 0xFF);
    data.at(offset + 2) = static_cast<char>((value >> 16) & 0xFF);
    data.at(offset + 3) = static_cast<char>((value >> 24) & 0xFF);
}

}  // namespace seabass::infrastructure::rekordbox
