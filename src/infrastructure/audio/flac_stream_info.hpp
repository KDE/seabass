// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace seabass::infrastructure::audio
{

// What a FLAC file's STREAMINFO block says about its audio, and where its
// audio frames start.
struct FlacStreamInfo
{
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    // Frames (one sample of every channel); 0 when the encoder did not
    // know it.
    std::uint64_t totalFrames = 0;
    // The MD5 of the decoded samples; all zero when the encoder did not
    // compute it.
    std::array<std::uint8_t, 16> md5{};
    // The 34 bytes of the block as they are in the file.
    std::array<std::uint8_t, 34> raw{};
    // The offset of the first audio frame: everything from here to the
    // end of the file is audio, and a tag writer leaves it as it is.
    std::uint64_t audioOffset = 0;
};

// Reads the metadata blocks of a native FLAC file (an ID3v2 tag in front
// of them is skipped). nullopt for a file that is not one, or whose
// metadata runs past its end. Never throws.
std::optional<FlacStreamInfo> readFlacStreamInfo(const std::filesystem::path &path);

}  // namespace seabass::infrastructure::audio
