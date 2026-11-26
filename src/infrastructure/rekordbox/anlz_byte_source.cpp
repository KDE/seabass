// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>

namespace seabass::infrastructure::rekordbox
{

std::string anlzRelativePath(const std::string &analyzePath, bool wantExt)
{
    std::string rel = analyzePath;
    const std::string marker = "/PIONEER/";
    size_t pos = rel.find(marker);
    if (pos != std::string::npos) {
        rel = rel.substr(pos + marker.size());
    }
    if (wantExt) {
        size_t dot = rel.rfind(".DAT");
        if (dot != std::string::npos) {
            rel = rel.substr(0, dot) + ".EXT";
        }
    }
    return rel;
}

std::string anlzSiblingRelativePath(const std::string &analyzePath, const std::string &extension)
{
    std::string rel = anlzRelativePath(analyzePath, /*wantExt=*/false);
    const size_t dot = rel.rfind(".DAT");
    if (dot != std::string::npos) {
        rel = rel.substr(0, dot) + extension;
    }
    return rel;
}

FilesystemAnlzSource::FilesystemAnlzSource(std::string pioneerRoot) : m_pioneerRoot(std::move(pioneerRoot)) {}

namespace
{

std::uint32_t bigEndian32(const char *bytes)
{
    const auto *b = reinterpret_cast<const unsigned char *>(bytes);
    return static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16
           | static_cast<std::uint32_t>(b[2]) << 8 | static_cast<std::uint32_t>(b[3]);
}

// Walks the sections of an analysis file of `size` bytes, reading
// `length` bytes at `offset` through `at`: the file header gives where the
// first section starts, and each section's header its own length. The
// section tagged `fourcc`, whole, or nullopt.
std::optional<std::string> walkToSection(std::uint64_t size, const std::string &fourcc,
                                         const std::function<std::optional<std::string>(std::uint64_t, std::size_t)> &at)
{
    const auto fileHeader = at(0, 12);
    if (!fileHeader || fileHeader->size() < 12 || fileHeader->compare(0, 4, "PMAI") != 0) {
        return std::nullopt;
    }
    std::uint64_t offset = bigEndian32(fileHeader->data() + 4);
    // A real file has a handful of sections; the bound only stops a
    // damaged one from walking for ever.
    for (int guard = 0; guard < 64 && offset + 12 <= size; ++guard) {
        const auto header = at(offset, 12);
        if (!header || header->size() < 12) {
            return std::nullopt;
        }
        const std::uint32_t length = bigEndian32(header->data() + 8);
        if (length < 12 || offset + length > size) {
            return std::nullopt;
        }
        if (header->compare(0, 4, fourcc) == 0) {
            auto section = at(offset, length);
            return section && section->size() == length ? section : std::nullopt;
        }
        offset += length;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> anlzSectionOf(const std::string &file, const std::string &fourcc)
{
    return walkToSection(file.size(), fourcc, [&](std::uint64_t offset, std::size_t length) -> std::optional<std::string> {
        if (offset >= file.size()) {
            return std::nullopt;
        }
        return file.substr(static_cast<std::size_t>(offset), length);
    });
}

std::optional<std::string> AnlzByteSource::readSection(const std::string &relativePath, const std::string &fourcc)
{
    const auto file = read(relativePath);
    if (!file) {
        return std::nullopt;
    }
    return anlzSectionOf(*file, fourcc);
}

namespace
{

// rekordbox puts PWV6 last but one in a .2EX, before a 20-byte PWVC:
// 3,640 bytes from the end on every file measured. One read of this much
// of the file's end finds it.
constexpr std::size_t TailBytes = 16 * 1024;

// The section tagged `fourcc` among the last bytes of a file of `size`
// bytes (`tail`, the file from size - tail.size()), found without the
// headers before it: a place where the tag stands whose own length, and
// those of every section after it, end exactly at the end of the file.
// That chain is what a walk from the start would have found; a stray
// "PWV6" in a waveform's bytes does not end the file.
std::optional<std::string> sectionInTail(const std::string &tail, std::uint64_t size, const std::string &fourcc)
{
    const std::uint64_t tailStart = size - tail.size();
    for (std::size_t at = tail.rfind(fourcc); at != std::string::npos; at = at == 0 ? std::string::npos : tail.rfind(fourcc, at - 1)) {
        std::size_t walk = at;
        bool chained = false;
        for (int guard = 0; guard < 16 && walk + 12 <= tail.size(); ++guard) {
            const std::uint32_t length = bigEndian32(tail.data() + walk + 8);
            if (length < 12 || walk + length > tail.size()) {
                break;
            }
            walk += length;
            if (walk == tail.size()) {
                chained = true;
                break;
            }
        }
        if (chained && tailStart + at >= 12) {
            const std::uint32_t length = bigEndian32(tail.data() + at + 8);
            return tail.substr(at, length);
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> FilesystemAnlzSource::readSection(const std::string &relativePath, const std::string &fourcc)
{
    std::ifstream ifs(pathFromUtf8(m_pioneerRoot) / pathFromUtf8(relativePath), std::ifstream::binary);
    if (!ifs.is_open()) {
        return std::nullopt;
    }
    ifs.seekg(0, std::ios::end);
    const auto end = ifs.tellg();
    if (end <= 0) {
        return std::nullopt;
    }
    // Each read from a stick is a request of its own over USB: one read of
    // the file's end (1.3 ms cold on a real stick) instead of a walk from
    // its start, a read per section header (3.2 ms), or the whole file
    // (2.5 ms; measured on 150 tracks, 2026-11-26). The walk stays for a
    // file whose section is not near its end.
    {
        const auto size = static_cast<std::uint64_t>(end);
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, TailBytes));
        std::string tail(want, '\0');
        ifs.seekg(static_cast<std::streamoff>(size - want));
        ifs.read(tail.data(), static_cast<std::streamsize>(want));
        if (ifs.gcount() == static_cast<std::streamsize>(want)) {
            if (auto section = sectionInTail(tail, size, fourcc)) {
                return section;
            }
        }
    }
    return walkToSection(static_cast<std::uint64_t>(end), fourcc,
                         [&](std::uint64_t offset, std::size_t length) -> std::optional<std::string> {
                             ifs.clear();
                             ifs.seekg(static_cast<std::streamoff>(offset));
                             std::string bytes(length, '\0');
                             ifs.read(bytes.data(), static_cast<std::streamsize>(length));
                             if (!ifs && ifs.gcount() != static_cast<std::streamsize>(length)) {
                                 return std::nullopt;
                             }
                             return bytes;
                         });
}

std::optional<std::string> FilesystemAnlzSource::read(const std::string &relativePath)
{
    std::ifstream ifs(pathFromUtf8(m_pioneerRoot) / pathFromUtf8(relativePath), std::ifstream::binary);
    if (!ifs.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << ifs.rdbuf();
    return buffer.str();
}

}  // namespace seabass::infrastructure::rekordbox
