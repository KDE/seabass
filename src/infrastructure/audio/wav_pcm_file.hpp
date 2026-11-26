// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace seabass::infrastructure::stick_backup
{
class PosixArchiveFile;
}

namespace seabass::infrastructure::audio
{

// Where the samples of a PCM WAV are and what they are.
struct WavLayout
{
    int sampleRate = 0;
    int channels = 0;
    // 16 or 24: the bits each sample holds, which is also the bits it
    // takes in the file (a 20 bit recording in 24 bit slots is refused).
    int bitsPerSample = 0;
    // Bytes of one frame: one sample of every channel.
    int blockAlign = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataBytes = 0;
    std::uint64_t frames = 0;
    // An RF64 or BW64 file, whose sizes are in its ds64 chunk.
    bool rf64 = false;
};

// The sample rates a recording is taken at. Others are refused: no
// player Seabass knows records at another, and none of them is tested.
inline constexpr int RecordingSampleRates[] = {44100, 48000, 88200, 96000};

// A PCM WAV as DJ hardware records it, read piece by piece: the header is
// read when the file is opened, the samples only as asked for, so a
// recording of hours is never in memory.
//
// Reads the plain RIFF form (sizes of 32 bits, so up to 4 GB) and the
// RF64 and BW64 forms that recorders switch to past it. PCM samples of 16
// or 24 bits, one or two channels, at one of RecordingSampleRates, in a
// WAVE_FORMAT_PCM or a WAVE_FORMAT_EXTENSIBLE (PCM sub format) header.
//
// Refused, with the reason in error(): a file that is not a WAV, another
// sample format (float, 8 or 32 bit, compressed), a data chunk that says
// more than the file holds (a recording cut short), a data chunk whose
// size the header leaves open (0 or 0xFFFFFFFF in a plain RIFF, what a
// recorder that was stopped by a power cut leaves), and a plain RIFF over
// 4 GB, whose data size cannot be told from its header.
//
// Never throws from the constructor; readFrames() throws on a read error
// (the file was taken away meanwhile).
class WavPcmFile
{
public:
    explicit WavPcmFile(const std::filesystem::path &path);
    ~WavPcmFile();
    WavPcmFile(const WavPcmFile &) = delete;
    WavPcmFile &operator=(const WavPcmFile &) = delete;

    bool isOpen() const { return m_error.empty(); }
    const std::string &error() const { return m_error; }
    const WavLayout &layout() const { return m_layout; }

    // The bytes of `out.size() / blockAlign` frames from `firstFrame`,
    // as the file has them (little endian, interleaved). The span must
    // hold whole frames that lie inside the file.
    void readFrames(std::uint64_t firstFrame, std::span<std::byte> out) const;

private:
    std::unique_ptr<stick_backup::PosixArchiveFile> m_file;
    WavLayout m_layout;
    std::string m_error;
};

}  // namespace seabass::infrastructure::audio
