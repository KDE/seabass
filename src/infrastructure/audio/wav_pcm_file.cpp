// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/wav_pcm_file.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <vector>

#include "infrastructure/long_paths.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

using stick_backup::PosixArchiveFile;

std::uint16_t le16(const std::byte *p)
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8));
}

std::uint32_t le32(const std::byte *p)
{
    return static_cast<std::uint32_t>(le16(p)) | (static_cast<std::uint32_t>(le16(p + 2)) << 16);
}

std::uint64_t le64(const std::byte *p)
{
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

bool isId(const std::byte *p, const char *id)
{
    return std::memcmp(p, id, 4) == 0;
}

constexpr std::uint16_t FormatPcm = 1;
constexpr std::uint16_t FormatExtensible = 0xFFFE;
// KSDATAFORMAT_SUBTYPE_PCM after its first two bytes (the format tag).
constexpr std::array<unsigned char, 14> PcmSubFormatTail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                             0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
// The largest plain RIFF: its size field counts the file less 8 bytes.
constexpr std::uint64_t PlainRiffLimit = std::uint64_t{0xFFFFFFFF} + 8;

struct Fmt
{
    int channels = 0;
    int sampleRate = 0;
    int blockAlign = 0;
    int bitsPerSample = 0;
};

// The fmt chunk's payload as a PCM format this reads, or the reason not.
std::optional<Fmt> parseFmt(const std::vector<std::byte> &payload, std::string &error)
{
    if (payload.size() < 16) {
        error = "its format header is damaged";
        return std::nullopt;
    }
    const std::uint16_t tag = le16(payload.data());
    Fmt fmt;
    fmt.channels = le16(payload.data() + 2);
    fmt.sampleRate = static_cast<int>(le32(payload.data() + 4));
    fmt.blockAlign = le16(payload.data() + 12);
    fmt.bitsPerSample = le16(payload.data() + 14);
    if (tag == FormatExtensible) {
        if (payload.size() < 40 || le16(payload.data() + 16) < 22) {
            error = "its extensible format header is damaged";
            return std::nullopt;
        }
        const int validBits = le16(payload.data() + 18);
        const std::byte *sub = payload.data() + 24;
        const bool pcm = le16(sub) == FormatPcm
            && std::equal(PcmSubFormatTail.begin(), PcmSubFormatTail.end(), sub + 2,
                          [](unsigned char a, std::byte b) { return a == std::to_integer<unsigned char>(b); });
        if (!pcm) {
            error = "its samples are not PCM (floating point or compressed audio is not read)";
            return std::nullopt;
        }
        if (validBits != fmt.bitsPerSample) {
            error = "its samples hold " + std::to_string(validBits) + " bits in slots of "
                + std::to_string(fmt.bitsPerSample) + ", which is not read";
            return std::nullopt;
        }
    } else if (tag != FormatPcm) {
        error = "its samples are not PCM (floating point or compressed audio is not read)";
        return std::nullopt;
    }
    if (fmt.bitsPerSample != 16 && fmt.bitsPerSample != 24) {
        error = "its samples have " + std::to_string(fmt.bitsPerSample) + " bits; only 16 and 24 bit are read";
        return std::nullopt;
    }
    if (fmt.channels != 1 && fmt.channels != 2) {
        error = "it has " + std::to_string(fmt.channels) + " channels; only mono and stereo are read";
        return std::nullopt;
    }
    if (std::find(std::begin(RecordingSampleRates), std::end(RecordingSampleRates), fmt.sampleRate)
        == std::end(RecordingSampleRates)) {
        error = "its sample rate of " + std::to_string(fmt.sampleRate)
            + " Hz is not one of 44100, 48000, 88200 and 96000";
        return std::nullopt;
    }
    if (fmt.blockAlign != fmt.channels * fmt.bitsPerSample / 8) {
        error = "its format header is damaged (the frame size does not fit the sample size)";
        return std::nullopt;
    }
    return fmt;
}

}  // namespace

WavPcmFile::WavPcmFile(const std::filesystem::path &path)
{
    try {
        m_file = std::make_unique<PosixArchiveFile>(longPathSafe(path), PosixArchiveFile::OpenMode::ReadOnly);
    } catch (const std::exception &) {
        m_error = "it could not be opened";
        return;
    }
    try {
        const std::uint64_t fileSize = m_file->size();
        std::array<std::byte, 12> head{};
        if (fileSize < head.size()) {
            m_error = "it is not a WAV file";
            return;
        }
        m_file->readAt(0, head);
        const bool riff = isId(head.data(), "RIFF");
        m_layout.rf64 = isId(head.data(), "RF64") || isId(head.data(), "BW64");
        if ((!riff && !m_layout.rf64) || !isId(head.data() + 8, "WAVE")) {
            m_error = "it is not a WAV file";
            return;
        }
        if (riff && fileSize > PlainRiffLimit) {
            m_error = "it is a plain WAV over 4 GB, whose header cannot say how long its audio is";
            return;
        }

        std::optional<std::uint64_t> ds64DataSize;
        std::optional<Fmt> fmt;
        std::optional<std::uint64_t> dataOffset;
        std::uint64_t dataBytes = 0;
        std::uint64_t at = head.size();
        while (at + 8 <= fileSize && (!fmt || !dataOffset)) {
            std::array<std::byte, 8> chunk{};
            m_file->readAt(at, chunk);
            const std::uint32_t size32 = le32(chunk.data() + 4);
            const std::uint64_t payloadAt = at + 8;
            std::uint64_t size = size32;
            if (isId(chunk.data(), "ds64")) {
                if (!m_layout.rf64 || size < 24 || payloadAt + 24 > fileSize) {
                    m_error = "its RF64 header is damaged";
                    return;
                }
                std::array<std::byte, 24> ds64{};
                m_file->readAt(payloadAt, ds64);
                ds64DataSize = le64(ds64.data() + 8);
            } else if (isId(chunk.data(), "fmt ")) {
                if (size > 1024 || payloadAt + size > fileSize) {
                    m_error = "its format header is damaged";
                    return;
                }
                std::vector<std::byte> payload(static_cast<std::size_t>(size));
                m_file->readAt(payloadAt, payload);
                fmt = parseFmt(payload, m_error);
                if (!fmt) {
                    return;
                }
            } else if (isId(chunk.data(), "data")) {
                if (m_layout.rf64 && size32 == 0xFFFFFFFF) {
                    if (!ds64DataSize) {
                        m_error = "its RF64 header is damaged (no ds64 chunk before the audio)";
                        return;
                    }
                    size = *ds64DataSize;
                } else if (size32 == 0 || size32 == 0xFFFFFFFF) {
                    // What a recorder leaves when it was stopped before it
                    // could write the length.
                    m_error = "its header does not say how long the audio is (the recording was not finished)";
                    return;
                }
                dataOffset = payloadAt;
                dataBytes = size;
            }
            // Chunks are padded to an even length.
            const std::uint64_t next = payloadAt + size + (size & 1U);
            if (next <= at) {
                break;
            }
            at = next;
        }
        if (!fmt) {
            m_error = "it is not a WAV file (no format header)";
            return;
        }
        if (!dataOffset) {
            m_error = "it holds no audio (no data chunk)";
            return;
        }
        const std::uint64_t held = fileSize - *dataOffset;
        if (dataBytes > held) {
            m_error = "it was cut short: its header promises " + std::to_string(dataBytes)
                + " bytes of audio and the file holds " + std::to_string(held);
            return;
        }
        if (dataBytes % static_cast<std::uint64_t>(fmt->blockAlign) != 0) {
            m_error = "its audio ends in the middle of a frame";
            return;
        }
        m_layout.sampleRate = fmt->sampleRate;
        m_layout.channels = fmt->channels;
        m_layout.bitsPerSample = fmt->bitsPerSample;
        m_layout.blockAlign = fmt->blockAlign;
        m_layout.dataOffset = *dataOffset;
        m_layout.dataBytes = dataBytes;
        m_layout.frames = dataBytes / static_cast<std::uint64_t>(fmt->blockAlign);
        if (m_layout.frames == 0) {
            m_error = "it holds no audio";
        }
    } catch (const std::exception &) {
        m_error = "it could not be read";
    }
}

WavPcmFile::~WavPcmFile() = default;

void WavPcmFile::readFrames(std::uint64_t firstFrame, std::span<std::byte> out) const
{
    const auto align = static_cast<std::uint64_t>(m_layout.blockAlign);
    if (!m_file || out.size() % align != 0 || firstFrame + out.size() / align > m_layout.frames) {
        throw std::out_of_range("frames outside the WAV's audio");
    }
    m_file->readAt(m_layout.dataOffset + firstFrame * align, out);
}

}  // namespace seabass::infrastructure::audio
