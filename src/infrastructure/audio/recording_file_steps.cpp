// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/recording_file_steps.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <system_error>

#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

namespace fs = std::filesystem;

constexpr std::size_t TokenDigits = 16;

char foldAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalIgnoringAsciiCase(const std::string &a, const std::string &b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return foldAscii(x) == foldAscii(y); });
}

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

}  // namespace

fs::path uniqueTempBeside(const fs::path &target, const char *suffix)
{
    static thread_local std::mt19937_64 generator = []() {
        std::random_device device;
        const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::seed_seq seed{device(), device(), device(), static_cast<unsigned>(now), static_cast<unsigned>(now >> 32)};
        return std::mt19937_64(seed);
    }();
    static constexpr char Digits[] = "0123456789abcdef";
    const std::uint64_t value = generator();
    std::string token(TokenDigits, '0');
    for (std::size_t i = 0; i < TokenDigits; ++i) {
        token[i] = Digits[(value >> (4 * i)) & 0xF];
    }
    fs::path temp = target;
    temp += "." + token + suffix;
    return temp;
}

bool isTempNameOf(const std::string &name, const std::string &targetName, const char *suffix)
{
    const std::size_t suffixLength = std::strlen(suffix);
    if (name.size() != targetName.size() + 1 + TokenDigits + suffixLength) {
        return false;
    }
    if (name.compare(0, targetName.size(), targetName) != 0 || name[targetName.size()] != '.'
        || name.compare(name.size() - suffixLength, suffixLength, suffix) != 0) {
        return false;
    }
    const auto first = name.begin() + static_cast<std::ptrdiff_t>(targetName.size() + 1);
    return std::all_of(first, first + static_cast<std::ptrdiff_t>(TokenDigits), isHexDigit);
}

std::optional<bool> somethingAt(const fs::path &path)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(longPathSafe(path), ec);
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (ec || status.type() == fs::file_type::none) {
        return std::nullopt;
    }
    return true;
}

bool sameFile(const fs::path &a, const fs::path &b)
{
    const std::optional<bool> aThere = somethingAt(a);
    const std::optional<bool> bThere = somethingAt(b);
    if (!aThere || !bThere) {
        return true;
    }
    std::error_code ec;
    if (*aThere && *bThere) {
        const bool same = fs::equivalent(longPathSafe(a), longPathSafe(b), ec);
        return ec ? true : same;
    }
    if (*aThere != *bThere) {
        return false;
    }
    if (!equalIgnoringAsciiCase(pathToUtf8(a.filename()), pathToUtf8(b.filename()))) {
        return false;
    }
    const fs::path aFolder = a.parent_path();
    const fs::path bFolder = b.parent_path();
    const bool sameFolder = fs::equivalent(longPathSafe(aFolder), longPathSafe(bFolder), ec);
    if (!ec) {
        return sameFolder;
    }
    // A folder that is not there: as written.
    return equalIgnoringAsciiCase(pathToUtf8(aFolder.lexically_normal()), pathToUtf8(bFolder.lexically_normal()));
}

std::uint64_t flushExistingFile(const fs::path &path)
{
    stick_backup::PosixArchiveFile file(longPathSafe(path), stick_backup::PosixArchiveFile::OpenMode::ReadWriteExisting);
    file.barrier();
    return file.size();
}

std::optional<std::uint64_t> fileSizeOf(const fs::path &path)
{
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(longPathSafe(path), ec);
    if (ec) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(size);
}

std::vector<fs::path> tempsBeside(const fs::path &folder, const std::string &targetName, const char *suffix)
{
    std::vector<fs::path> found;
    std::error_code ec;
    DirectoryReader reader(folder, ec);
    if (ec) {
        return found;
    }
    fs::path child;
    while (reader.next(child, ec)) {
        if (!isTempNameOf(pathToUtf8(child.filename()), targetName, suffix)) {
            continue;
        }
        std::error_code statusEc;
        if (fs::symlink_status(longPathSafe(child), statusEc).type() == fs::file_type::regular) {
            found.push_back(child);
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace seabass::infrastructure::audio
