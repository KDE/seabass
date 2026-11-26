// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/flac_stream_info.hpp"

#include <cstring>
#include <exception>

#include "infrastructure/long_paths.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

unsigned at(const std::array<std::byte, 34> &block, std::size_t i)
{
    return std::to_integer<unsigned>(block[i]);
}

std::optional<FlacStreamInfo> read(const std::filesystem::path &path)
{
    using stick_backup::PosixArchiveFile;
    const PosixArchiveFile file(longPathSafe(path), PosixArchiveFile::OpenMode::ReadOnly);
    const std::uint64_t size = file.size();
    std::uint64_t offset = 0;
    std::array<std::byte, 10> id3{};
    if (size >= id3.size()) {
        file.readAt(0, id3);
        if (std::memcmp(id3.data(), "ID3", 3) == 0) {
            // A synchsafe size: seven bits a byte, without the 10 byte
            // header, and another 10 with a footer.
            std::uint64_t tagSize = 0;
            for (std::size_t i = 6; i < 10; ++i) {
                tagSize = (tagSize << 7) | (std::to_integer<unsigned>(id3[i]) & 0x7FU);
            }
            const bool footer = (std::to_integer<unsigned>(id3[5]) & 0x10U) != 0;
            offset = 10 + tagSize + (footer ? 10 : 0);
        }
    }
    std::array<std::byte, 4> marker{};
    if (offset + 4 > size) {
        return std::nullopt;
    }
    file.readAt(offset, marker);
    if (std::memcmp(marker.data(), "fLaC", 4) != 0) {
        return std::nullopt;
    }
    offset += 4;

    std::optional<FlacStreamInfo> info;
    for (bool last = false; !last;) {
        std::array<std::byte, 4> header{};
        if (offset + 4 > size) {
            return std::nullopt;
        }
        file.readAt(offset, header);
        const unsigned flags = std::to_integer<unsigned>(header[0]);
        last = (flags & 0x80U) != 0;
        const unsigned type = flags & 0x7FU;
        const std::uint64_t length = (std::uint64_t{std::to_integer<unsigned>(header[1])} << 16)
            | (std::uint64_t{std::to_integer<unsigned>(header[2])} << 8) | std::to_integer<unsigned>(header[3]);
        offset += 4;
        if (offset + length > size) {
            return std::nullopt;
        }
        if (type == 0) {
            // STREAMINFO is the first block and exactly 34 bytes.
            if (info || length != 34) {
                return std::nullopt;
            }
            std::array<std::byte, 34> block{};
            file.readAt(offset, block);
            FlacStreamInfo found;
            for (std::size_t i = 0; i < block.size(); ++i) {
                found.raw[i] = static_cast<std::uint8_t>(at(block, i));
            }
            found.sampleRate = static_cast<int>((at(block, 10) << 12) | (at(block, 11) << 4) | (at(block, 12) >> 4));
            found.channels = static_cast<int>(((at(block, 12) >> 1) & 0x7U) + 1);
            found.bitsPerSample = static_cast<int>((((at(block, 12) & 0x1U) << 4) | (at(block, 13) >> 4)) + 1);
            found.totalFrames = (std::uint64_t{at(block, 13) & 0xFU} << 32) | (std::uint64_t{at(block, 14)} << 24)
                | (std::uint64_t{at(block, 15)} << 16) | (std::uint64_t{at(block, 16)} << 8) | at(block, 17);
            for (std::size_t i = 0; i < 16; ++i) {
                found.md5[i] = static_cast<std::uint8_t>(at(block, 18 + i));
            }
            info = found;
        } else if (!info) {
            return std::nullopt;
        }
        offset += length;
    }
    if (!info) {
        return std::nullopt;
    }
    info->audioOffset = offset;
    return info;
}

}  // namespace

std::optional<FlacStreamInfo> readFlacStreamInfo(const std::filesystem::path &path)
{
    try {
        return read(path);
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace seabass::infrastructure::audio
