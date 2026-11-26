// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/local_keep_recording_files.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

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

}  // namespace

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
