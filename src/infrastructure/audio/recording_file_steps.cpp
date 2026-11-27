// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/recording_file_steps.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <system_error>

#include "infrastructure/audio/recording_temp_names.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

namespace fs = std::filesystem;

constexpr std::size_t TokenDigits = RecordingTempTokenDigits;

char foldAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalIgnoringAsciiCase(const std::string &a, const std::string &b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return foldAscii(x) == foldAscii(y); });
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

std::optional<bool> somethingAt(const fs::path &path, std::error_code *error)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(longPathSafe(path), ec);
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (ec || status.type() == fs::file_type::none) {
        if (error != nullptr) {
            *error = ec ? ec : std::make_error_code(std::errc::io_error);
        }
        return std::nullopt;
    }
    return true;
}

std::optional<bool> sameFile(const fs::path &a, const fs::path &b, std::error_code *error)
{
    const std::optional<bool> aThere = somethingAt(a, error);
    if (!aThere) {
        return std::nullopt;
    }
    const std::optional<bool> bThere = somethingAt(b, error);
    if (!bThere) {
        return std::nullopt;
    }
    std::error_code ec;
    if (*aThere && *bThere) {
        const bool same = fs::equivalent(longPathSafe(a), longPathSafe(b), ec);
        if (ec) {
            if (error != nullptr) {
                *error = ec;
            }
            return std::nullopt;
        }
        return same;
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

std::string lookError(const std::string &name, const std::error_code &error)
{
    if (error == std::errc::filename_too_long) {
        return "the name " + name + " is too long for this drive";
    }
    return "the place for " + name + " could not be looked at (" + error.message() + ")";
}

bool removeSecondName(const fs::path &secondName, const fs::path &kept)
{
    std::error_code ec;
    if (fs::symlink_status(longPathSafe(secondName), ec).type() == fs::file_type::not_found) {
        return true;
    }
    if (fs::symlink_status(longPathSafe(secondName), ec).type() != fs::file_type::regular
        || !fs::equivalent(longPathSafe(secondName), longPathSafe(kept), ec) || ec) {
        return false;
    }
    return fs::remove(longPathSafe(secondName), ec) && !ec;
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

std::size_t utf16Units(const fs::path &path)
{
#if defined(_WIN32)
    return path.native().size();
#else
    // From UTF-8: a code point past U+FFFF (four bytes) takes two units,
    // any other one; continuation bytes take none.
    std::size_t units = 0;
    for (const char c : path.native()) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte & 0xC0) == 0x80) {
            continue;
        }
        units += byte >= 0xF0 ? 2 : 1;
    }
    return units;
#endif
}

std::string mediaPathProblem(const fs::path &temp)
{
    const std::size_t units = utf16Units(temp);
    if (units <= MaxMediaPathUnits) {
        return {};
    }
    return "the path of its temporary file in " + pathToUtf8(temp.parent_path()) + " would be "
        + std::to_string(units) + " characters long, and the encoder on Windows takes at most "
        + std::to_string(MaxMediaPathUnits) + ". Choose a folder with a shorter path, or make the artist or the track "
          "name shorter";
}

}  // namespace seabass::infrastructure::audio
