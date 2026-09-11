// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>

namespace seabass::infrastructure::rekordbox
{

// Bounds-checked (std::string::at(), not operator[]) throughout: a
// corrupt or truncated ANLZ file feeding a bad offset in here must fail
// loudly and cleanly, not read/write past the buffer.

inline uint16_t readU16BE(const std::string &data, size_t offset)
{
    return static_cast<uint16_t>((static_cast<unsigned char>(data.at(offset)) << 8) |
                                  static_cast<unsigned char>(data.at(offset + 1)));
}

inline uint32_t readU32BE(const std::string &data, size_t offset)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset))) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset + 1))) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset + 2))) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(data.at(offset + 3)));
}

inline void appendU16BE(std::string &out, uint16_t value)
{
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

inline void appendU32BE(std::string &out, uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

inline void writeU32BE(std::string &data, size_t offset, uint32_t value)
{
    data.at(offset) = static_cast<char>((value >> 24) & 0xFF);
    data.at(offset + 1) = static_cast<char>((value >> 16) & 0xFF);
    data.at(offset + 2) = static_cast<char>((value >> 8) & 0xFF);
    data.at(offset + 3) = static_cast<char>(value & 0xFF);
}

}  // namespace seabass::infrastructure::rekordbox
