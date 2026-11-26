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

// A chunk id as the RIFF format has them: four printable ASCII characters.
bool isChunkId(const std::byte *p)
{
    return std::all_of(p, p + 4, [](std::byte b) {
        const auto c = std::to_integer<unsigned>(b);
        return c >= 0x20 && c <= 0x7E;
    });
}

}  // namespace

std::string wavDuration(std::uint64_t frames, int sampleRate)
{
    const std::uint64_t seconds = frames / static_cast<std::uint64_t>(sampleRate);
    const std::uint64_t h = seconds / 3600;
    const std::uint64_t m = seconds / 60 % 60;
    const std::uint64_t sec = seconds % 60;
    const auto two = [](std::uint64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    return h > 0 ? std::to_string(h) + ":" + two(m) + ":" + two(sec) : std::to_string(m) + ":" + two(sec);
}

namespace
{

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

WavPcmFile::WavPcmFile(const std::filesystem::path &path, WavReading reading)
{
    try {
        m_file = std::make_unique<PosixArchiveFile>(longPathSafe(path), PosixArchiveFile::OpenMode::ReadOnly);
    } catch (const std::exception &) {
        refuse("it could not be opened");
        return;
    }
    try {
        const std::uint64_t fileSize = m_file->size();
        std::array<std::byte, 12> head{};
        if (fileSize < head.size()) {
            refuse("it is not a WAV file");
            return;
        }
        m_file->readAt(0, head);
        const bool riff = isId(head.data(), "RIFF");
        m_layout.rf64 = isId(head.data(), "RF64") || isId(head.data(), "BW64");
        if ((!riff && !m_layout.rf64) || !isId(head.data() + 8, "WAVE")) {
            refuse("it is not a WAV file");
            return;
        }
        if (riff && fileSize > PlainRiffLimit) {
            refuse("it is a plain WAV over 4 GB, whose header cannot say how long its audio is");
            return;
        }

        std::optional<std::uint64_t> ds64DataSize;
        std::optional<Fmt> fmt;
        std::optional<std::uint64_t> dataOffset;
        std::uint64_t dataBytes = 0;
        // The data size the header leaves open (0 or 0xFFFFFFFF).
        bool noSize = false;
        // Where the chunks after the audio start, once both are found.
        std::uint64_t at = head.size();
        while (at + 8 <= fileSize && (!fmt || !dataOffset)) {
            std::array<std::byte, 8> chunk{};
            m_file->readAt(at, chunk);
            const std::uint32_t size32 = le32(chunk.data() + 4);
            const std::uint64_t payloadAt = at + 8;
            std::uint64_t size = size32;
            if (isId(chunk.data(), "ds64")) {
                if (!m_layout.rf64 || size < 24 || payloadAt + 24 > fileSize) {
                    refuse("its RF64 header is damaged");
                    return;
                }
                std::array<std::byte, 24> ds64{};
                m_file->readAt(payloadAt, ds64);
                ds64DataSize = le64(ds64.data() + 8);
            } else if (isId(chunk.data(), "fmt ")) {
                if (size > 1024 || payloadAt + size > fileSize) {
                    refuse("its format header is damaged");
                    return;
                }
                std::vector<std::byte> payload(static_cast<std::size_t>(size));
                m_file->readAt(payloadAt, payload);
                fmt = parseFmt(payload, m_error);
                if (!fmt) {
                    refuse(m_error);
                    return;
                }
            } else if (isId(chunk.data(), "data")) {
                if (m_layout.rf64 && size32 == 0xFFFFFFFF) {
                    if (!ds64DataSize) {
                        refuse("its RF64 header is damaged (no ds64 chunk before the audio)");
                        return;
                    }
                    size = *ds64DataSize;
                } else if (size32 == 0 || size32 == 0xFFFFFFFF) {
                    // What a recorder leaves when it was stopped before it
                    // could write the length: the audio runs to the end.
                    noSize = true;
                    size = fileSize - payloadAt;
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
            refuse("it is not a WAV file (no format header)");
            return;
        }
        if (!dataOffset) {
            refuse("it holds no audio (no data chunk)");
            return;
        }
        m_layout.sampleRate = fmt->sampleRate;
        m_layout.channels = fmt->channels;
        m_layout.bitsPerSample = fmt->bitsPerSample;
        m_layout.blockAlign = fmt->blockAlign;
        m_layout.dataOffset = *dataOffset;
        const auto align = static_cast<std::uint64_t>(fmt->blockAlign);
        const std::uint64_t held = fileSize - *dataOffset;

        std::optional<WavUnfinished::Kind> unfinished;
        if (noSize) {
            unfinished = WavUnfinished::Kind::NoSize;
        } else if (dataBytes > held) {
            unfinished = WavUnfinished::Kind::PastEnd;
        } else {
            if (dataBytes % align != 0) {
                refuse("its audio ends in the middle of a frame");
                return;
            }
            // The header must describe the whole file: after the audio,
            // only what onlyChunksFrom() takes. After an odd data chunk a
            // writer may have left out the pad byte: then the chunks must
            // end exactly at the end of the file.
            const bool described = onlyChunksFrom(at, fileSize, false)
                || ((dataBytes & 1U) != 0 && onlyChunksFrom(*dataOffset + dataBytes, fileSize, true));
            if (!described) {
                // Audio after a stale data size: whole frames to the end,
                // and not the start of a chunk (bytes that begin with a
                // chunk's id are a damaged chunk, not audio).
                bool chunkAfter = false;
                if (held - dataBytes >= 4) {
                    std::array<std::byte, 4> id{};
                    m_file->readAt(*dataOffset + dataBytes, id);
                    chunkAfter = isChunkId(id.data());
                }
                if ((held - dataBytes) % align == 0 && !chunkAfter) {
                    unfinished = WavUnfinished::Kind::StaleSize;
                } else {
                    refuse("its header says it holds " + wavDuration(dataBytes / align, fmt->sampleRate)
                           + " of audio, and the file has room for " + wavDuration(held / align, fmt->sampleRate)
                           + ". The recording was not finished properly (the recorder was stopped by a power loss or "
                             "the stick was pulled), so its header does not describe the whole file, and keeping it "
                             "would keep only part of the set");
                    return;
                }
            }
        }

        if (unfinished) {
            WavUnfinished info;
            info.kind = *unfinished;
            info.framesHeld = held / align;
            info.partialFrameBytes = held % align;
            info.framesDeclared = *unfinished == WavUnfinished::Kind::NoSize ? 0 : dataBytes / align;
            m_unfinished = info;
            if (info.framesHeld == 0) {
                refuse("it holds no audio");
                return;
            }
            const std::string holds = wavDuration(info.framesHeld, fmt->sampleRate);
            if (reading == WavReading::AsDeclared) {
                m_state = State::Unfinished;
                switch (info.kind) {
                    case WavUnfinished::Kind::NoSize:
                        m_error = "the player did not finish this recording: its header does not say how long the "
                                  "audio is. The file holds " + holds + " of audio";
                        break;
                    case WavUnfinished::Kind::PastEnd:
                        m_error = "the player did not finish this recording: its header says it holds "
                            + wavDuration(info.framesDeclared, fmt->sampleRate) + " of audio, and the file holds "
                            + holds;
                        break;
                    case WavUnfinished::Kind::StaleSize:
                        m_error = "the player did not finish this recording: its header says it holds "
                            + wavDuration(info.framesDeclared, fmt->sampleRate) + " of audio, and the file has room for "
                            + holds;
                        break;
                }
                return;
            }
            dataBytes = info.framesHeld * align;
        }
        m_layout.dataBytes = dataBytes;
        m_layout.frames = dataBytes / align;
        if (m_layout.frames == 0) {
            refuse("it holds no audio");
            return;
        }
        m_state = State::Open;
        m_error.clear();
    } catch (const std::exception &) {
        refuse("it could not be read");
    }
}

WavPcmFile::~WavPcmFile() = default;

void WavPcmFile::refuse(std::string why)
{
    m_state = State::Refused;
    m_error = std::move(why);
}

bool WavPcmFile::zeroBytesFrom(std::uint64_t at, std::uint64_t fileSize) const
{
    std::array<std::byte, 65536> piece{};
    while (at < fileSize) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(piece.size(), fileSize - at));
        m_file->readAt(at, std::span<std::byte>(piece.data(), count));
        if (std::any_of(piece.begin(), piece.begin() + static_cast<std::ptrdiff_t>(count),
                        [](std::byte b) { return b != std::byte{0}; })) {
            return false;
        }
        at += count;
    }
    return true;
}

bool WavPcmFile::onlyChunksFrom(std::uint64_t at, std::uint64_t fileSize, bool strict) const
{
    constexpr std::uint64_t Id3v1Bytes = 128;
    while (at < fileSize) {
        if (!strict) {
            // Padding to the end, or an ID3v1 block at the very end.
            if (zeroBytesFrom(at, fileSize)) {
                return true;
            }
            if (fileSize - at == Id3v1Bytes) {
                std::array<std::byte, 3> tag{};
                m_file->readAt(at, tag);
                if (std::memcmp(tag.data(), "TAG", 3) == 0) {
                    return true;
                }
            }
        }
        if (at + 8 > fileSize) {
            // A single pad byte of the chunk before is fine; any more is
            // not a chunk.
            return !strict && at + 1 == fileSize;
        }
        std::array<std::byte, 8> chunk{};
        m_file->readAt(at, chunk);
        if (!isChunkId(chunk.data()) || isId(chunk.data(), "data")) {
            return false;
        }
        const std::uint64_t size = le32(chunk.data() + 4);
        const std::uint64_t end = at + 8 + size;
        if (end > fileSize) {
            return false;
        }
        if (end == fileSize) {
            return true;  // the last chunk, its pad byte left out or not needed
        }
        at = end + (size & 1U);
    }
    return true;
}

void WavPcmFile::readFrames(std::uint64_t firstFrame, std::span<std::byte> out) const
{
    const auto align = static_cast<std::uint64_t>(m_layout.blockAlign);
    if (!m_file || out.size() % align != 0 || firstFrame + out.size() / align > m_layout.frames) {
        throw std::out_of_range("frames outside the WAV's audio");
    }
    m_file->readAt(m_layout.dataOffset + firstFrame * align, out);
}

}  // namespace seabass::infrastructure::audio
