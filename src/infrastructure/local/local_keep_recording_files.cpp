// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/local_keep_recording_files.hpp"

#include <filesystem>
#include <fstream>
#include <cerrno>
#include <iterator>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "infrastructure/fs_remove.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

namespace
{

fs::path local(const std::string &utf8)
{
    std::error_code ec;
    const fs::path absolute = fs::absolute(pathFromUtf8(utf8), ec);
    return longPathSafe(ec ? pathFromUtf8(utf8) : absolute);
}

// `path` made absolute and resolved as far as it is there (links
// followed, "." and ".." taken away), and the nearest folder of it that
// is there: `path` itself when it is.
std::optional<fs::path> nearestThere(const std::string &utf8)
{
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(local(utf8), ec);
    if (ec) {
        return std::nullopt;
    }
    while (true) {
        const fs::file_status status = fs::status(resolved, ec);
        if (status.type() != fs::file_type::not_found && !ec) {
            return resolved;
        }
        if (status.type() != fs::file_type::not_found) {
            return std::nullopt;  // there, but it would not say what
        }
        const fs::path parent = resolved.parent_path();
        if (parent.empty() || parent == resolved) {
            return std::nullopt;
        }
        resolved = parent;
    }
}

}  // namespace

std::optional<bool> LocalKeepRecordingFiles::isWithin(const std::string &path, const std::string &folder)
{
    std::error_code ec;
    const fs::path root = fs::canonical(local(folder), ec);
    if (ec || !fs::is_directory(root, ec)) {
        return std::nullopt;
    }
    const std::optional<fs::path> there = nearestThere(path);
    if (!there) {
        return std::nullopt;
    }
    // Compared as files, each folder up the way with the root: the same
    // file, whatever the spelling, the letters' case or the mount it is
    // reached through.
    for (fs::path step = *there;; step = step.parent_path()) {
        std::error_code same;
        if (fs::equivalent(step, root, same) && !same) {
            return true;
        }
        if (!step.has_parent_path() || step.parent_path() == step) {
            return false;
        }
    }
}

std::optional<std::uint64_t> LocalKeepRecordingFiles::maxNameBytes(const std::string &folder)
{
    const std::optional<fs::path> there = nearestThere(folder);
    if (!there) {
        return std::nullopt;
    }
#if defined(_WIN32)
    // NTFS, FAT and exFAT: 255 UTF-16 units, never more than a name's
    // bytes of UTF-8.
    return 255;
#else
    errno = 0;
    const long longest = ::pathconf(there->c_str(), _PC_NAME_MAX);
    if (longest <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(longest);
#endif
}

std::optional<std::uint64_t> LocalKeepRecordingFiles::fileSize(const std::string &path)
{
    std::error_code ec;
    const fs::path file = local(path);
    if (!fs::is_regular_file(file, ec)) {
        return std::nullopt;
    }
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(size);
}

std::optional<bool> LocalKeepRecordingFiles::exists(const std::string &path)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(local(path), ec);
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (ec) {
        return std::nullopt;
    }
    return true;
}

std::optional<std::uint64_t> LocalKeepRecordingFiles::freeBytes(const std::string &folder)
{
    std::error_code ec;
    const fs::space_info space = fs::space(local(folder), ec);
    if (ec || space.capacity == 0 || space.available == static_cast<std::uintmax_t>(-1)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(space.available);
}

std::optional<std::string> LocalKeepRecordingFiles::readFile(const std::string &path, std::uint64_t maxBytes)
{
    const std::optional<std::uint64_t> size = fileSize(path);
    if (!size || *size > maxBytes) {
        return std::nullopt;
    }
    std::ifstream in(local(path), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad() || bytes.size() > maxBytes) {
        return std::nullopt;
    }
    return bytes;
}

std::string LocalKeepRecordingFiles::makeFolder(const std::string &folder)
{
    std::error_code ec;
    const fs::path target = local(folder);
    if (fs::is_directory(target, ec)) {
        return {};
    }
    fs::create_directories(target, ec);
    if (fs::is_directory(target)) {
        return {};
    }
    if (fs::exists(target)) {
        return "something that is not a folder is in its place";
    }
    return ec ? ec.message() : std::string("it is not there");
}

std::string LocalKeepRecordingFiles::removeFile(const std::string &path)
{
    std::error_code ec;
    const fs::path file = local(path);
    const fs::file_status status = fs::symlink_status(file, ec);
    if (status.type() == fs::file_type::not_found) {
        return {};
    }
    if (status.type() != fs::file_type::regular) {
        return "it is not a file";
    }
    std::string failure;
    if (!removeEntry(file, failure)) {
        return failure;
    }
    return {};
}

}  // namespace seabass::infrastructure
