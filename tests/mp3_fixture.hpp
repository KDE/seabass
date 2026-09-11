// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// Builds MP3 files byte by byte, for tests that need a real audio file
// on disk rather than a checked-in fixture.
//
// Hand-built because the interesting case is precisely the one a normal
// encoder never produces on demand -- a VBR stream with no Xing header,
// which is what makes a duration an estimate rather than a fact -- and
// because a hand-built frame makes the expected duration arithmetic
// explicit instead of magic.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace seabass::test_fixture::mp3
{

namespace fs = std::filesystem;

// MPEG-1 Layer III, 128 kbps, 44100 Hz, stereo, no padding, no CRC.
constexpr unsigned char FrameHeader[4] = {0xFF, 0xFB, 0x90, 0x00};
constexpr int FrameBytes = 417;  // 144 * 128000 / 44100, truncated
constexpr int SamplesPerFrame = 1152;
constexpr int SampleRate = 44100;

inline std::vector<unsigned char> silentFrame()
{
    std::vector<unsigned char> frame(FrameBytes, 0);
    std::memcpy(frame.data(), FrameHeader, sizeof(FrameHeader));
    return frame;
}

// A Xing header sits in the first frame's data area at an offset fixed
// by version + channel mode: MPEG-1 stereo is 32 bytes past the 4-byte
// frame header. Only the "frames" and "bytes" fields are filled in,
// which is what a length calculation needs.
inline std::vector<unsigned char> xingFrame(unsigned frames, unsigned bytes)
{
    std::vector<unsigned char> frame = silentFrame();
    size_t offset = 4 + 32;
    std::memcpy(frame.data() + offset, "Xing", 4);
    offset += 4;
    frame[offset + 3] = 0x03;  // flags: frames present | bytes present
    offset += 4;
    for (int i = 0; i < 4; ++i) {
        frame[offset + i] = static_cast<unsigned char>((frames >> (8 * (3 - i))) & 0xFF);
    }
    offset += 4;
    for (int i = 0; i < 4; ++i) {
        frame[offset + i] = static_cast<unsigned char>((bytes >> (8 * (3 - i))) & 0xFF);
    }
    return frame;
}

inline void writeBytes(const fs::path &path, const std::vector<unsigned char> &data)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

// frameCount silent frames, the first optionally carrying a Xing header
// that honestly describes the file. Without one, a reader has to
// estimate the length from the bitrate.
inline void writeMp3(const fs::path &path, int frameCount, bool withXing)
{
    std::vector<unsigned char> data;
    for (int i = 0; i < frameCount; ++i) {
        std::vector<unsigned char> frame =
            (i == 0 && withXing)
                ? xingFrame(static_cast<unsigned>(frameCount), static_cast<unsigned>(frameCount * FrameBytes))
                : silentFrame();
        data.insert(data.end(), frame.begin(), frame.end());
    }
    writeBytes(path, data);
}

inline double expectedSeconds(int frameCount)
{
    return double(frameCount) * SamplesPerFrame / SampleRate;
}

}  // namespace seabass::test_fixture::mp3
