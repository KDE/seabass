// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/path_key.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace seabass::application
{

namespace
{

// Lowercases one code point, for the alphabets where the mapping is a
// plain offset within a contiguous block and can therefore be checked by
// reading it. Anything outside these blocks is returned unchanged.
//
// Deliberately not a full Unicode case-folding table. This function
// decides whether a file gets deleted, so the rule here is that every
// mapping is one a person can verify against a code chart in a minute.
// The blocks below cover Western and Central European, Greek and
// Cyrillic names, which is what a music library actually contains; a
// character outside them keeps its case and is compared exactly, which
// is the behaviour this function had for everything before.
std::uint32_t lowerCodePoint(std::uint32_t cp)
{
    // ASCII.
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32;
    }
    // Latin-1 Supplement. U+00D7 is the multiplication sign, not a
    // letter, and U+00DF has no single-character uppercase.
    if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) {
        return cp + 32;
    }
    // Latin Extended-A. Mostly even/odd pairs, but the parity flips
    // twice inside the block and there are four characters that are not
    // part of any pair, so the sub-ranges are spelled out rather than
    // approximated with one rule.
    if (cp >= 0x0100 && cp <= 0x0137) {  // even = upper
        return (cp % 2 == 0) ? cp + 1 : cp;
    }
    if (cp >= 0x0139 && cp <= 0x0148) {  // odd = upper
        return (cp % 2 == 1) ? cp + 1 : cp;
    }
    if (cp >= 0x014A && cp <= 0x0177) {  // even = upper again
        return (cp % 2 == 0) ? cp + 1 : cp;
    }
    if (cp == 0x0178) {  // LATIN CAPITAL LETTER Y WITH DIAERESIS -> U+00FF
        return 0x00FF;
    }
    if (cp >= 0x0179 && cp <= 0x017E) {  // odd = upper
        return (cp % 2 == 1) ? cp + 1 : cp;
    }
    // Greek. U+03A2 is unassigned.
    if ((cp >= 0x0391 && cp <= 0x03A1) || (cp >= 0x03A3 && cp <= 0x03AB)) {
        return cp + 32;
    }
    // The accented Greek capitals sit outside that block and do not
    // share its offset -- listed individually because there is no rule
    // to get right. Missing them made "ΕΛΛΆΔΑ" and "ελλάδα" different
    // files, which a test caught rather than a reading of the chart.
    switch (cp) {
    case 0x0386: return 0x03AC;  // Ά
    case 0x0388: return 0x03AD;  // Έ
    case 0x0389: return 0x03AE;  // Ή
    case 0x038A: return 0x03AF;  // Ί
    case 0x038C: return 0x03CC;  // Ό
    case 0x038E: return 0x03CD;  // Ύ
    case 0x038F: return 0x03CE;  // Ώ
    default: break;
    }
    // Cyrillic.
    if (cp >= 0x0410 && cp <= 0x042F) {
        return cp + 32;
    }
    if (cp >= 0x0400 && cp <= 0x040F) {
        return cp + 80;
    }
    return cp;
}

void encodeUtf8(std::uint32_t cp, std::string &out)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Lowercases a UTF-8 string. Bytes that are not valid UTF-8 are copied
// through untouched rather than replaced: a path Seabass cannot decode
// is still a real path to a real file, and mangling it here would make
// two spellings of one file differ, which is the failure this whole
// function exists to prevent.
std::string lowerUtf8(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto byte = static_cast<unsigned char>(in[i]);
        std::size_t length = 0;
        std::uint32_t cp = 0;
        if (byte < 0x80) {
            length = 1;
            cp = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            length = 2;
            cp = byte & 0x1F;
        } else if ((byte & 0xF0) == 0xE0) {
            length = 3;
            cp = byte & 0x0F;
        } else if ((byte & 0xF8) == 0xF0) {
            length = 4;
            cp = byte & 0x07;
        } else {
            out.push_back(in[i++]);  // stray continuation or invalid lead
            continue;
        }
        if (i + length > in.size()) {
            out.push_back(in[i++]);  // truncated sequence
            continue;
        }
        bool valid = true;
        for (std::size_t k = 1; k < length; ++k) {
            const auto cont = static_cast<unsigned char>(in[i + k]);
            if ((cont & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cont & 0x3F);
        }
        if (!valid) {
            out.push_back(in[i++]);
            continue;
        }
        encodeUtf8(lowerCodePoint(cp), out);
        i += length;
    }
    return out;
}

// Collapses "." and ".." components and doubled separators, the same
// job as std::filesystem::path::lexically_normal(), without ever
// constructing a std::filesystem::path: on Windows that path's
// value_type is wchar_t, so building one from a std::string re-encodes
// it through the current locale's narrow-to-wide codecvt, which throws
// std::filesystem::filesystem_error on bytes that are not valid UTF-8 --
// exactly the input this whole file is documented, and tested, to pass
// through untouched rather than mangle. normalizedPathKey() gates real
// deletion decisions, so a path with one undecodable byte must normalize
// to a key, not crash the process.
//
// Splitting on the literal '/' byte is safe even over invalid UTF-8:
// 0x2F never appears as a lead or continuation byte of a multi-byte
// sequence, so it cannot occur inside one, valid or not.
std::string lexicallyNormalizedPath(const std::string &in)
{
    const bool absolute = !in.empty() && in.front() == '/';
    std::vector<std::string> segments;
    std::size_t i = 0;
    while (i <= in.size()) {
        const std::size_t next = in.find('/', i);
        const std::string segment = in.substr(i, next == std::string::npos ? std::string::npos : next - i);
        if (segment.empty() || segment == ".") {
            // skip: doubled separator or a no-op component
        } else if (segment == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!absolute) {
                // Nothing to pop and no root to be swallowed by: keep it,
                // same as lexically_normal() does for a relative path.
                segments.push_back(segment);
            }
            // Absolute and nothing to pop: parent of root is root, drop it.
        } else {
            segments.push_back(segment);
        }
        if (next == std::string::npos) {
            break;
        }
        i = next + 1;
    }
    std::string out;
    if (absolute) {
        out.push_back('/');
    }
    for (std::size_t s = 0; s < segments.size(); ++s) {
        if (s != 0) {
            out.push_back('/');
        }
        out += segments[s];
    }
    return out;
}

}  // namespace

std::string normalizedPathKey(const std::string &path)
{
    if (path.empty()) {
        return path;
    }
    // export.pdb keeps its strings in fixed-length fields and space-pads
    // them, so a path read out of a rekordbox catalog arrives with
    // trailing spaces while the same path walked off the filesystem does
    // not. Trimmed here as well as at the reader, because this function
    // is the one place all three destructive comparisons agree on: if a
    // padded path ever reaches it from anywhere, the answer must still be
    // "same file" rather than "delete that". NULs too -- the format pads
    // some fields with those instead.
    std::string slashed = path;
    while (!slashed.empty()
           && (slashed.back() == ' ' || slashed.back() == '\t' || slashed.back() == '\0')) {
        slashed.pop_back();
    }
    // A trailing separator names the same thing as no trailing separator,
    // and lexically_normal() keeps it.
    while (slashed.size() > 1 && (slashed.back() == '/' || slashed.back() == '\\')) {
        slashed.pop_back();
    }
    if (slashed.empty()) {
        return {};
    }
    std::replace(slashed.begin(), slashed.end(), '\\', '/');
    return lowerUtf8(lexicallyNormalizedPath(slashed));
}

}  // namespace seabass::application
