// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace seabass::infrastructure::local
{

// The one JSON shape this project writes by hand: a single flat object
// whose values are strings or integers. Enough for the edit-lock cookie
// (and the pending-deletion manifest's lines, which predate this and
// keep their own copy); no library, no nesting, no arrays.

inline std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0xF];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// {"key":"value",...} with every value quoted as a string, keys in map
// order, one line, no trailing newline.
inline std::string writeFlatObject(const std::map<std::string, std::string> &fields)
{
    std::string out = "{";
    bool first = true;
    for (const auto &[key, value] : fields) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "\"" + jsonEscape(key) + "\":\"" + jsonEscape(value) + "\"";
    }
    out += "}";
    return out;
}

// Parses what writeFlatObject() produces, plus unquoted number/true/false/
// null values (returned as their literal text) so a hand-edited or
// foreign cookie still reads. std::nullopt for anything malformed.
inline std::optional<std::map<std::string, std::string>> parseFlatObject(const std::string &text)
{
    std::map<std::string, std::string> fields;
    size_t i = 0;
    auto skipSpace = [&]() {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t')) {
            ++i;
        }
    };
    auto parseString = [&](std::string &out) -> bool {
        if (i >= text.size() || text[i] != '"') {
            return false;
        }
        ++i;
        while (i < text.size()) {
            char c = text[i++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= text.size()) {
                return false;
            }
            char e = text[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > text.size()) {
                    return false;
                }
                unsigned int code = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = text[i++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') {
                        code |= static_cast<unsigned int>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        code |= static_cast<unsigned int>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        code |= static_cast<unsigned int>(h - 'A' + 10);
                    } else {
                        return false;
                    }
                }
                // BMP code points only (surrogate pairs are not something
                // this project ever writes); encoded as UTF-8.
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;  // unterminated
    };

    skipSpace();
    if (i >= text.size() || text[i] != '{') {
        return std::nullopt;
    }
    ++i;
    skipSpace();
    if (i < text.size() && text[i] == '}') {
        return fields;
    }
    while (true) {
        skipSpace();
        std::string key;
        if (!parseString(key)) {
            return std::nullopt;
        }
        skipSpace();
        if (i >= text.size() || text[i] != ':') {
            return std::nullopt;
        }
        ++i;
        skipSpace();
        std::string value;
        if (i < text.size() && text[i] == '"') {
            if (!parseString(value)) {
                return std::nullopt;
            }
        } else {
            size_t start = i;
            while (i < text.size() && text[i] != ',' && text[i] != '}' && text[i] != ' ' && text[i] != '\n'
                   && text[i] != '\r' && text[i] != '\t') {
                ++i;
            }
            if (i == start) {
                return std::nullopt;
            }
            value = text.substr(start, i - start);
        }
        fields[key] = value;
        skipSpace();
        if (i >= text.size()) {
            return std::nullopt;
        }
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (text[i] == '}') {
            return fields;
        }
        return std::nullopt;
    }
}

}  // namespace seabass::infrastructure::local
