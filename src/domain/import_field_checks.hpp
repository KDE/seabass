// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace seabass::domain
{

// What every library writer of the track import asks of the values it is
// given before it writes a row from them. One place, so that the formats
// refuse the same things: a value one of them took and another refused
// would leave the libraries of one stick saying different things.

// "YYYY-MM-DD" naming a day that exists, leap years included.
inline bool isCalendarDate(std::string_view text)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (const std::size_t i : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    const int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day = (text[8] - '0') * 10 + (text[9] - '0');
    if (month < 1 || month > 12 || day < 1) {
        return false;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    constexpr int Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return day <= Days[month - 1] + (month == 2 && leap ? 1 : 0);
}

// Whether `text` is UTF-8 as the standard has it: no stray or missing
// continuation byte, no overlong form, no surrogate, nothing past
// U+10FFFF.
inline bool isWellFormedUtf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t least = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        } else if ((lead >> 5) == 0x6) {
            length = 2;
            codePoint = lead & 0x1Fu;
            least = 0x80;
        } else if ((lead >> 4) == 0xE) {
            length = 3;
            codePoint = lead & 0x0Fu;
            least = 0x800;
        } else if ((lead >> 3) == 0x1E) {
            length = 4;
            codePoint = lead & 0x07u;
            least = 0x10000;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next >> 6) != 0x2) {
                return false;
            }
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }
        if (codePoint < least || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

// Whether a library row can hold `text` and find it again: well-formed
// UTF-8, no NUL inside, and no space, tab or NUL at its end. export.pdb's
// reader takes those off the end of every string it reads, so a name
// stored with one never equals itself read back, and each import would
// make the artist "DJ X " anew. The caller trims what a tag holds.
inline bool isStorableText(std::string_view text)
{
    if (!isWellFormedUtf8(text) || text.find('\0') != std::string_view::npos) {
        return false;
    }
    return text.empty() || (text.back() != ' ' && text.back() != '\t');
}

// Whether `path` is the one plain spelling of a file's place on a stick:
// from the root, '/' between names, no empty name, no "." or "..", no
// backslash. Every library's row for a file is made from this one text,
// and a path that climbs ("/../x") would put the file beside the stick.
inline bool isPlainStickPath(std::string_view path)
{
    if (path.size() < 2 || path.front() != '/' || path.find('\\') != std::string_view::npos) {
        return false;
    }
    for (std::size_t at = 0; at < path.size();) {
        const std::size_t next = std::min(path.find('/', at + 1), path.size());
        const std::string_view name = path.substr(at + 1, next - at - 1);
        if (name.empty() || name == "." || name == "..") {
            return false;
        }
        at = next;
    }
    return true;
}

}  // namespace seabass::domain
