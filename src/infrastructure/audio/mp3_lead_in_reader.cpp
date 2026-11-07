// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/mp3_lead_in_reader.hpp"

#include "infrastructure/paths/utf8_path.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace seabass::infrastructure::audio
{

namespace
{

using domain::Mp3LeadIn;

struct FrameHeader
{
    // 1 for MPEG-1, 2 for MPEG-2, 25 for MPEG-2.5.
    int version = 0;
    bool crcProtected = false;
    int bitrateKbps = 0;
    int sampleRate = 0;
    bool padding = false;
    // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono.
    int channelMode = 0;

    int samplesPerFrame() const { return version == 1 ? 1152 : 576; }
    std::size_t length() const
    {
        const int coefficient = version == 1 ? 144 : 72;
        return static_cast<std::size_t>(coefficient * bitrateKbps * 1000 / sampleRate + (padding ? 1 : 0));
    }
    // Where the Xing/Info tag starts: after the header and the side
    // information. Not after the 16-bit CRC of a protected frame: LAME
    // writes the tag at 4 + side info even then, and FFmpeg and libmpg123
    // look for it there, so a protected LAME file is trimmed like any
    // other (measured on tests/fixtures/mp3-click/click-D-lame-crc-44100.mp3).
    std::size_t xingOffset() const
    {
        const bool mono = channelMode == 3;
        const std::size_t sideInfo = version == 1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        return 4 + sideInfo;
    }
};

constexpr std::array<int, 16> BitratesV1{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1};
constexpr std::array<int, 16> BitratesV2{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1};

// A Layer III frame header at p, or nothing. Free-format bitrate (index
// 0) is refused: its frame length cannot be computed from the header.
std::optional<FrameHeader> parseHeader(const unsigned char *p)
{
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return std::nullopt;
    }
    const int versionBits = (p[1] >> 3) & 0x03;
    const int layerBits = (p[1] >> 1) & 0x03;
    if (versionBits == 1 || layerBits != 1) {  // reserved version, not Layer III
        return std::nullopt;
    }
    FrameHeader h;
    h.version = versionBits == 3 ? 1 : (versionBits == 2 ? 2 : 25);
    h.crcProtected = (p[1] & 0x01) == 0;
    const int bitrateIndex = (p[2] >> 4) & 0x0F;
    const int rateIndex = (p[2] >> 2) & 0x03;
    if (bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) {
        return std::nullopt;
    }
    h.bitrateKbps = (h.version == 1 ? BitratesV1 : BitratesV2)[static_cast<std::size_t>(bitrateIndex)];
    constexpr std::array<int, 3> RatesV1{44100, 48000, 32000};
    const int base = RatesV1[static_cast<std::size_t>(rateIndex)];
    h.sampleRate = h.version == 1 ? base : (h.version == 2 ? base / 2 : base / 4);
    h.padding = ((p[2] >> 1) & 0x01) != 0;
    h.channelMode = (p[3] >> 6) & 0x03;
    return h;
}

bool sameStream(const FrameHeader &a, const FrameHeader &b)
{
    return a.version == b.version && a.sampleRate == b.sampleRate;
}

std::uint32_t bigEndian32(const unsigned char *p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// LAME's tag CRC: CRC-16 with the polynomial 0x8005, initial value 0,
// processed least significant bit first (LAME's crc16_lookup table, the
// reflected form 0xA001).
std::uint16_t lameCrc16(const unsigned char *data, std::size_t size)
{
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001) : static_cast<std::uint16_t>(crc >> 1);
        }
    }
    return crc;
}

bool startsWith(const unsigned char *p, const char *prefix)
{
    return std::memcmp(p, prefix, std::strlen(prefix)) == 0;
}

// Containers that are not MPEG audio and could hold bytes that look like
// a frame sync inside their first kilobytes.
bool isOtherContainer(const unsigned char *p)
{
    return startsWith(p, "RIFF") || startsWith(p, "FORM") || startsWith(p, "fLaC") || startsWith(p, "OggS")
        || startsWith(p + 4, "ftyp");
}

// Reads up to `size` bytes; returns how many arrived.
std::size_t readSome(std::istream &in, unsigned char *into, std::size_t size)
{
    in.read(reinterpret_cast<char *>(into), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(in.gcount());
}

// The tag frame's contents, filled into `leadIn`. `frame` is the whole
// frame, `size` how much of it is in the window.
void readTagFrame(const unsigned char *frame, std::size_t size, const FrameHeader &h, Mp3LeadIn &leadIn)
{
    // VBRI sits 32 bytes after the header whatever the channel mode.
    constexpr std::size_t VbriOffset = 4 + 32;
    if (size >= VbriOffset + 4 && startsWith(frame + VbriOffset, "VBRI")) {
        leadIn.vbri = true;
        // Version, delay and quality (2 bytes each), then the byte and
        // frame counts.
        leadIn.tagFrameHasCounts = size >= VbriOffset + 18
            && (bigEndian32(frame + VbriOffset + 10) != 0 || bigEndian32(frame + VbriOffset + 14) != 0);
        return;
    }
    std::size_t at = h.xingOffset();
    if (size < at + 8 || !(startsWith(frame + at, "Xing") || startsWith(frame + at, "Info"))) {
        return;
    }
    leadIn.infoFrame = true;
    const std::uint32_t flags = bigEndian32(frame + at + 4);
    at += 8;
    std::uint32_t frames = 0;
    std::uint32_t bytes = 0;
    if (flags & 0x1) {
        frames = size >= at + 4 ? bigEndian32(frame + at) : 0;
        at += 4;
    }
    if (flags & 0x2) {
        bytes = size >= at + 4 ? bigEndian32(frame + at) : 0;
        at += 4;
    }
    leadIn.tagFrameHasCounts = frames != 0 || bytes != 0;
    if (flags & 0x4) {
        at += 100;  // seek table
    }
    if (flags & 0x8) {
        at += 4;  // quality
    }
    // The LAME extension: a 9-byte encoder string, then fields up to the
    // tag's CRC 34 bytes after the string starts.
    const std::size_t stringAt = at;
    if (size < stringAt + 36 || stringAt + 36 > h.length()) {
        return;
    }
    const unsigned char *s = frame + stringAt;
    if (startsWith(s, "LAME")) {
        leadIn.tag = Mp3LeadIn::Tag::Lame;
    } else if (startsWith(s, "L3.99")) {
        leadIn.tag = Mp3LeadIn::Tag::LameL3;
    } else if (startsWith(s, "Lavc") || startsWith(s, "Lavf")) {
        leadIn.tag = Mp3LeadIn::Tag::Lavc;
    } else if (s[0] >= 0x20 && s[0] < 0x7F) {
        leadIn.tag = Mp3LeadIn::Tag::Other;
    } else {
        return;
    }
    if (leadIn.tag == Mp3LeadIn::Tag::Other) {
        return;
    }
    const unsigned char *d = s + 21;
    leadIn.encoderDelay = (d[0] << 4) | (d[1] >> 4);
    leadIn.encoderPadding = ((d[1] & 0x0F) << 8) | d[2];
    const std::size_t crcAt = stringAt + 34;
    const std::uint16_t stored = static_cast<std::uint16_t>((frame[crcAt] << 8) | frame[crcAt + 1]);
    leadIn.lameCrcValid = lameCrc16(frame, crcAt) == stored;
}

}  // namespace

std::optional<domain::Mp3LeadIn> readMp3LeadIn(std::istream &in)
{
    // Every ID3v2 tag in a row at the start, the way FFmpeg reads them:
    // the frames start after the last one.
    std::streamoff audioStart = 0;
    for (std::size_t tags = 0;; ++tags) {
        std::array<unsigned char, 10> id3{};
        in.clear();
        in.seekg(audioStart);
        if (!in || readSome(in, id3.data(), id3.size()) != id3.size()) {
            return std::nullopt;
        }
        if (!startsWith(id3.data(), "ID3")) {
            if (tags == 0 && isOtherContainer(id3.data())) {
                return std::nullopt;
            }
            break;
        }
        if (tags == MaxId3TagsFollowed) {
            return std::nullopt;
        }
        // The size is syncsafe (7 bits a byte) and counts the tag after
        // its header, without the footer. The unsynchronisation flag
        // changes how the frames inside are encoded, never this size, so
        // skipping the tag does not care about it.
        if ((id3[6] | id3[7] | id3[8] | id3[9]) & 0x80) {
            return std::nullopt;
        }
        const std::streamoff size = (std::streamoff(id3[6]) << 21) | (std::streamoff(id3[7]) << 14)
            | (std::streamoff(id3[8]) << 7) | std::streamoff(id3[9]);
        const bool footer = (id3[5] & 0x10) != 0;
        audioStart += 10 + size + (footer ? 10 : 0);
    }

    in.clear();
    in.seekg(audioStart);
    if (!in) {
        return std::nullopt;
    }
    std::vector<unsigned char> window(LeadInWindowBytes);
    const std::size_t got = readSome(in, window.data(), window.size());

    // The first frame header whose successor is where its length says
    // and belongs to the same stream: one header alone is too easily a
    // pair of bytes in padding or a stray tag.
    for (std::size_t i = 0; i + 4 <= got; ++i) {
        const std::optional<FrameHeader> first = parseHeader(window.data() + i);
        if (!first) {
            continue;
        }
        const std::size_t next = i + first->length();
        if (next + 4 > got) {
            continue;
        }
        const std::optional<FrameHeader> second = parseHeader(window.data() + next);
        if (!second || !sameStream(*first, *second)) {
            continue;
        }
        Mp3LeadIn leadIn;
        leadIn.sampleRate = first->sampleRate;
        leadIn.samplesPerFrame = first->samplesPerFrame();
        readTagFrame(window.data() + i, got - i, *first, leadIn);
        // FFmpeg reads the tag frame only right after the ID3v2 tags;
        // libmpg123 finds it after anything.
        leadIn.tagFrameFirst = i == 0;
        // The tag frame is not audio, so the first audio frame is the one
        // after it, and that is the channel mode rekordbox compares.
        leadIn.infoFrameModeMatchesAudio = first->channelMode == second->channelMode;
        return leadIn;
    }
    return std::nullopt;
}

std::optional<domain::Mp3LeadIn> readMp3LeadIn(const std::string &absoluteFilePath)
{
    std::ifstream in(pathFromUtf8(absoluteFilePath), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return readMp3LeadIn(in);
}

}  // namespace seabass::infrastructure::audio
