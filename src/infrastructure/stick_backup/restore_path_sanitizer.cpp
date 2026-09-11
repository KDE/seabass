// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/restore_path_sanitizer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

#include "infrastructure/stick_backup/stick_tree_walker.hpp"

namespace seabass::infrastructure::stick_backup
{

namespace
{

constexpr std::array<std::string_view, 22> WindowsReservedNames = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

bool fail(std::string *reason, std::string message)
{
    if (reason != nullptr) {
        *reason = std::move(message);
    }
    return false;
}

bool isWindowsReservedStem(std::string_view segment)
{
    std::string_view stem = segment.substr(0, segment.find('.'));
    std::string upper(stem);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return std::find(WindowsReservedNames.begin(), WindowsReservedNames.end(), upper) != WindowsReservedNames.end();
}

bool checkSegment(std::string_view segment, TargetOs os, std::string *reason)
{
    if (segment.empty()) {
        return fail(reason, "empty path segment");
    }
    if (segment == "." || segment == "..") {
        return fail(reason, "relative segment '" + std::string(segment) + "'");
    }
    for (unsigned char c : segment) {
        if (c == 0) {
            return fail(reason, "NUL byte in name");
        }
        if (c == '\\') {
            return fail(reason, "backslash in name");
        }
        if (os == TargetOs::Windows) {
            if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
                return fail(reason, "character not allowed on Windows");
            }
        }
    }
    if (os == TargetOs::Windows) {
        if (segment.back() == '.' || segment.back() == ' ') {
            return fail(reason, "trailing dot or space is not allowed on Windows");
        }
        if (isWindowsReservedStem(segment)) {
            return fail(reason, "reserved device name on Windows");
        }
    }
    return true;
}

}  // namespace

std::optional<std::filesystem::path> sanitizeEntryName(std::string_view entryName, TargetOs os, std::string *reason,
                                                      bool *isDirectory)
{
    if (isDirectory != nullptr) {
        *isDirectory = false;
    }
    if (entryName.empty()) {
        fail(reason, "empty name");
        return std::nullopt;
    }
    std::string_view name = entryName;
    if (name.back() == '/') {
        name.remove_suffix(1);
        if (isDirectory != nullptr) {
            *isDirectory = true;
        }
        if (name.empty()) {
            fail(reason, "empty directory name");
            return std::nullopt;
        }
    }
    if (name.front() == '/') {
        fail(reason, "absolute path");
        return std::nullopt;
    }
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':') {
        fail(reason, "drive letter");
        return std::nullopt;
    }

    std::filesystem::path result;
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t slash = name.find('/', start);
        std::string_view segment = name.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!checkSegment(segment, os, reason)) {
            return std::nullopt;
        }
        result /= pathFromUtf8(segment);
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return result;
}

}  // namespace seabass::infrastructure::stick_backup
