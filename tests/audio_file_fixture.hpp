// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// Audio files built byte by byte, each a few hundred bytes with every
// field named: FLAC, MP3/AIFF/WAV carrying an ID3v2.3 tag, MP4, and Ogg
// Vorbis/Opus. For tests of what reads a file's own tags and pictures
// (embedded_artwork_test, taglib_tag_reader_test), so what the reader is
// handed is visible in the test rather than in a binary blob.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "mp3_fixture.hpp"

namespace seabass::test_fixture::audio
{

namespace fs = std::filesystem;

inline std::string be16(std::uint32_t v)
{
    return {static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)};
}

inline std::string be24(std::uint32_t v)
{
    return {static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)};
}

inline std::string be32(std::uint32_t v)
{
    return be16(v >> 16) + be16(v & 0xffff);
}

inline std::string le16(std::uint32_t v)
{
    return {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
}

inline std::string le32(std::uint32_t v)
{
    return le16(v & 0xffff) + le16(v >> 16);
}

inline void writeFile(const fs::path &path, const std::string &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    assert(out.good());
}

// ---- FLAC -----------------------------------------------------------

// One metadata block: a header byte (bit 7 = last block, bits 0-6 = block
// type), a 24-bit big-endian body length, then the body.
inline std::string flacBlock(int type, bool last, const std::string &body)
{
    return std::string(1, static_cast<char>((last ? 0x80 : 0x00) | type)) + be24(body.size()) + body;
}

// STREAMINFO (type 0), 34 bytes: 44.1 kHz stereo 16 bit, no samples.
inline std::string flacStreamInfo()
{
    std::string body;
    body += be16(4096);     // minimum block size
    body += be16(4096);     // maximum block size
    body += be24(0);        // minimum frame size (unknown)
    body += be24(0);        // maximum frame size (unknown)
    // 20 bits sample rate (44100 = 0x0AC44), 3 bits channels-1 (1),
    // 5 bits bits-per-sample-1 (15), 36 bits total samples (0).
    body += "\x0a\xc4\x42\xf0";
    body += std::string(4, '\0');   // rest of total samples
    body += std::string(16, '\0');  // MD5 of the audio (none)
    assert(body.size() == 34);
    return body;
}

// PICTURE (type 6): the same layout FLAC and METADATA_BLOCK_PICTURE use.
inline std::string flacPictureBody(std::uint32_t pictureType, const std::string &data)
{
    const std::string mime = "image/png";
    std::string body;
    body += be32(pictureType);  // 3 = front cover, 4 = back cover
    body += be32(mime.size());  // MIME type length
    body += mime;               // MIME type
    body += be32(0);            // description length (none)
    body += be32(1);            // width
    body += be32(1);            // height
    body += be32(32);           // colour depth
    body += be32(0);            // colours used (0: not indexed)
    body += be32(data.size());  // picture data length
    body += data;               // picture data
    return body;
}

// "fLaC", STREAMINFO, the given further blocks (the last one flagged),
// then a few bytes standing in for audio frames.
inline std::string flacFile(const std::vector<std::pair<int, std::string>> &blocks)
{
    std::string out = "fLaC";
    out += flacBlock(0, blocks.empty(), flacStreamInfo());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        out += flacBlock(blocks[i].first, i + 1 == blocks.size(), blocks[i].second);
    }
    out += std::string("\xff\xf8", 2) + std::string(14, '\0');  // frame sync + zeros
    return out;
}

// ---- ID3v2.3 --------------------------------------------------------

// An APIC frame: "APIC", a 32-bit big-endian body size (v2.3 does not
// syncsafe frame sizes), two flag bytes, then text encoding (0 = Latin-1),
// the NUL-terminated MIME type, the picture type byte, an empty
// NUL-terminated description, and the picture.
inline std::string apicFrame(unsigned char pictureType, const std::string &data)
{
    std::string body;
    body += '\0';                         // text encoding
    body += std::string("image/png") + '\0';  // MIME type
    body += static_cast<char>(pictureType);   // 3 = front cover, 4 = back cover
    body += '\0';                         // description
    body += data;
    return std::string("APIC") + be32(body.size()) + std::string(2, '\0') + body;
}

// "ID3", version 3.0, no flags, a 28-bit syncsafe size, then the frames.
inline std::string id3Tag(const std::vector<std::string> &frames)
{
    std::string all;
    for (const std::string &frame : frames) {
        all += frame;
    }
    const std::uint32_t size = all.size();
    std::string header = "ID3";
    header += '\x03';
    header += '\x00';
    header += '\x00';
    header += static_cast<char>((size >> 21) & 0x7f);
    header += static_cast<char>((size >> 14) & 0x7f);
    header += static_cast<char>((size >> 7) & 0x7f);
    header += static_cast<char>(size & 0x7f);
    return header + all;
}

inline std::string mp3Frames(int count)
{
    const std::vector<unsigned char> frame = seabass::test_fixture::mp3::silentFrame();
    std::string out;
    for (int i = 0; i < count; ++i) {
        out.append(reinterpret_cast<const char *>(frame.data()), frame.size());
    }
    return out;
}

// ---- AIFF / WAV -----------------------------------------------------

// An IFF chunk: four-character id, 32-bit length (big-endian for AIFF,
// little-endian for RIFF/WAV), body, and a pad byte when the body is odd.
inline std::string iffChunk(const std::string &id, const std::string &body, bool bigEndian)
{
    std::string out = id + (bigEndian ? be32(body.size()) : le32(body.size())) + body;
    if (body.size() % 2 != 0) {
        out += '\0';
    }
    return out;
}

// FORM/AIFF: COMM (2 channels, 1 frame, 16 bit, 44100 Hz as an 80-bit
// IEEE extended float), SSND (offset, block size, one silent frame), and
// an "ID3 " chunk carrying an ID3v2 tag -- where AIFF keeps its tags.
inline std::string aiffFile(const std::string &id3)
{
    std::string comm;
    comm += be16(2);  // channels
    comm += be32(1);  // sample frames
    comm += be16(16); // bits per sample
    comm += std::string("\x40\x0e\xac\x44\x00\x00\x00\x00\x00\x00", 10);  // 44100.0
    std::string ssnd = be32(0) + be32(0) + std::string(4, '\0');
    std::string body = "AIFF" + iffChunk("COMM", comm, true) + iffChunk("SSND", ssnd, true) +
                       iffChunk("ID3 ", id3, true);
    return "FORM" + be32(body.size()) + body;
}

// RIFF/WAVE: fmt (PCM, stereo, 44100 Hz, 16 bit), a data chunk with one
// silent frame, and an "id3 " chunk with the ID3v2 tag.
inline std::string wavFile(const std::string &id3)
{
    std::string fmt;
    fmt += le16(1);           // PCM
    fmt += le16(2);           // channels
    fmt += le32(44100);       // sample rate
    fmt += le32(44100 * 4);   // byte rate
    fmt += le16(4);           // block align
    fmt += le16(16);          // bits per sample
    std::string body = "WAVE" + iffChunk("fmt ", fmt, false) + iffChunk("data", std::string(4, '\0'), false) +
                       iffChunk("id3 ", id3, false);
    return "RIFF" + le32(body.size()) + body;
}

// ---- MP4 ------------------------------------------------------------

// An atom: 32-bit big-endian size including this 8-byte header, then the
// four-character type, then the body.
inline std::string atom(const std::string &type, const std::string &body)
{
    return be32(body.size() + 8) + type + body;
}

// A "data" atom inside an ilst item: a 32-bit type indicator (version 0
// plus the well-known type: 13 = JPEG, 14 = PNG), a 32-bit locale (0),
// then the value.
inline std::string dataAtom(std::uint32_t wellKnownType, const std::string &value)
{
    return atom("data", be32(wellKnownType) + be32(0) + value);
}

// ftyp, then moov/udta/meta/ilst holding `ilstBody`. meta is a "full
// atom": four bytes of version and flags before its children, which is
// where TagLib expects them.
inline std::string mp4File(const std::string &ilstBody)
{
    const std::string ftyp = atom("ftyp", "M4A " + be32(0) + "M4A " + "isom");
    const std::string meta = atom("meta", be32(0) + atom("hdlr", be32(0) + be32(0) + "mdir" + "appl" +
                                                                    be32(0) + be32(0) + std::string(1, '\0')) +
                                              atom("ilst", ilstBody));
    return ftyp + atom("moov", atom("udta", meta));
}

// ---- Ogg ------------------------------------------------------------

// The Ogg page checksum: CRC-32 with polynomial 0x04C11DB7, not
// reflected, initial value 0, over the whole page with its CRC field
// zeroed.
inline std::uint32_t oggCrc(const std::string &page)
{
    std::uint32_t crc = 0;
    for (unsigned char byte : page) {
        crc ^= static_cast<std::uint32_t>(byte) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

// One page holding whole packets: "OggS", version 0, header type (2 =
// beginning of stream, 4 = end of stream), 64-bit granule position,
// 32-bit serial number, 32-bit page sequence, the CRC, the segment count
// and the lacing table (255s then the remainder, per packet), then the
// packets themselves. All little-endian.
inline std::string oggPage(unsigned char headerType, std::uint32_t sequence, const std::vector<std::string> &packets)
{
    std::string lacing;
    std::string data;
    for (const std::string &packet : packets) {
        std::size_t left = packet.size();
        while (left >= 255) {
            lacing += '\xff';
            left -= 255;
        }
        lacing += static_cast<char>(left);
        data += packet;
    }
    assert(lacing.size() < 256);
    std::string page = "OggS";
    page += '\0';                          // version
    page += static_cast<char>(headerType); // header type
    page += std::string(8, '\0');          // granule position
    page += le32(0x5eab0055);              // serial number
    page += le32(sequence);                // page sequence number
    page += le32(0);                       // CRC, filled in below
    page += static_cast<char>(lacing.size());
    page += lacing;
    page += data;
    const std::string crc = le32(oggCrc(page));
    page.replace(22, 4, crc);
    return page;
}

inline std::string base64(const std::string &in)
{
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t n = (static_cast<unsigned char>(in[i]) << 16) |
                                (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (i + 1 == in.size()) {
        const std::uint32_t n = static_cast<unsigned char>(in[i]) << 16;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const std::uint32_t n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// A Vorbis comment list: vendor length + vendor, comment count, then each
// comment as length + "KEY=value". Little-endian lengths.
inline std::string vorbisComments(const std::vector<std::string> &comments)
{
    const std::string vendor = "seabass test";
    std::string out = le32(vendor.size()) + vendor + le32(comments.size());
    for (const std::string &comment : comments) {
        out += le32(comment.size()) + comment;
    }
    return out;
}

inline std::string pictureComment(std::uint32_t pictureType, const std::string &data)
{
    return "METADATA_BLOCK_PICTURE=" + base64(flacPictureBody(pictureType, data));
}

// Ogg Vorbis: the identification header alone on the first page (packet
// type 1, "vorbis", version 0, 2 channels, 44100 Hz, bitrates, block
// sizes 256/2048, framing bit), the comment header (type 3) and a stub
// setup header (type 5) on the second, and one audio packet on a third
// that ends the stream. The third page is not decoration: TagLib stops
// reading at an end-of-stream page before it hands out the packets on
// it, so headers on the last page read as missing.
inline std::string oggVorbisFile(const std::vector<std::string> &comments)
{
    std::string ident = "\x01vorbis";
    ident += le32(0);       // vorbis version
    ident += '\x02';        // channels
    ident += le32(44100);   // sample rate
    ident += le32(0);       // maximum bitrate
    ident += le32(128000);  // nominal bitrate
    ident += le32(0);       // minimum bitrate
    ident += '\xb8';        // block sizes: 2^8 and 2^11
    ident += '\x01';        // framing bit
    const std::string comment = "\x03vorbis" + vorbisComments(comments) + '\x01';
    const std::string setup = std::string("\x05vorbis") + std::string(8, '\0');
    return oggPage(0x02, 0, {ident}) + oggPage(0x00, 1, {comment, setup}) + oggPage(0x04, 2, {std::string(4, '\0')});
}

// Ogg Opus: "OpusHead" (version 1, 2 channels, pre-skip 312, 48000 Hz,
// gain 0, mapping family 0) alone on the first page, "OpusTags" with the
// comment list on the second, one audio packet on the last.
inline std::string oggOpusFile(const std::vector<std::string> &comments)
{
    std::string head = "OpusHead";
    head += '\x01';       // version
    head += '\x02';       // channels
    head += le16(312);    // pre-skip
    head += le32(48000);  // input sample rate
    head += le16(0);      // output gain
    head += '\0';         // channel mapping family
    const std::string tags = "OpusTags" + vorbisComments(comments);
    return oggPage(0x02, 0, {head}) + oggPage(0x00, 1, {tags}) + oggPage(0x04, 2, {std::string(1, '\0')});
}

}  // namespace seabass::test_fixture::audio
