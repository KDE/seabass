// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// readMp3LeadIn (#50) over the committed files in tests/fixtures/mp3-lead-in,
// made by tools/mp3-offset-probe/make-lead-in-fixtures.sh: the fields it
// parses and the class they give, per file, and that it never reads more
// than the start of a file.
//
// Usage: mp3_lead_in_reader_test <fixture dir>

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>

#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"

using namespace seabass;
using namespace seabass::domain;
using seabass::infrastructure::audio::MaxLeadInBytesRead;
using seabass::infrastructure::audio::readMp3LeadIn;

namespace
{

std::string g_dir;

Mp3LeadIn read(const std::string &name)
{
    const auto leadIn = readMp3LeadIn(g_dir + "/" + name);
    if (!leadIn) {
        std::cerr << "no lead-in read from " << name << "\n";
    }
    assert(leadIn.has_value());
    return *leadIn;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 0.01;
}

// A whole file in memory behind a streambuf that counts every byte it
// hands out, so the test sees how much of the file the reader consumed.
class CountingBuf : public std::stringbuf
{
public:
    explicit CountingBuf(std::string bytes)
        : std::stringbuf(std::move(bytes), std::ios::in)
    {
    }
    std::size_t delivered = 0;

protected:
    std::streamsize xsgetn(char *s, std::streamsize n) override
    {
        const std::streamsize got = std::stringbuf::xsgetn(s, n);
        delivered += static_cast<std::size_t>(got);
        return got;
    }
    int_type uflow() override
    {
        const int_type c = std::stringbuf::uflow();
        if (c != traits_type::eof()) {
            ++delivered;
        }
        return c;
    }
};

std::string slurp(const std::string &path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    std::ostringstream all;
    all << in.rdbuf();
    return all.str();
}

}  // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    g_dir = argv[1];

    // LAME CBR: an Info frame, joint stereo like the audio, LAME's 576
    // samples of delay, a valid CRC. Class D, 51.18 ms.
    {
        const Mp3LeadIn l = read("lame-cbr-44100.mp3");
        assert(l.infoFrame && !l.vbri);
        assert(l.tag == Mp3LeadIn::Tag::Lame);
        assert(l.lameCrcValid);
        assert(l.encoderDelay == 576);
        assert(l.encoderPadding == 1404);
        assert(l.infoFrameModeMatchesAudio);
        assert(l.sampleRate == 44100 && l.samplesPerFrame == 1152);
        assert(classify(l) == Mp3Case::D);
        assert(near(engineToRekordboxOffsetMs(l), 51.18));
    }

    // LAME VBR: a Xing frame, the same tag.
    {
        const Mp3LeadIn l = read("lame-vbr-44100.mp3");
        assert(l.infoFrame && l.tag == Mp3LeadIn::Tag::Lame && l.lameCrcValid);
        assert(l.encoderDelay == 576);
        assert(l.infoFrameModeMatchesAudio);
        assert(classify(l) == Mp3Case::D);
    }

    // LAME without a tag frame: class A, no correction.
    {
        const Mp3LeadIn l = read("lame-notag-44100.mp3");
        assert(!l.infoFrame && !l.vbri && l.tag == Mp3LeadIn::Tag::None);
        assert(l.encoderDelay == 0);
        assert(l.sampleRate == 44100);
        assert(classify(l) == Mp3Case::A);
        assert(engineToRekordboxOffsetMs(l) == 0.0);
    }

    // CRC-protected frames: LAME writes the tag at 4 + side-info bytes,
    // not after the header's two CRC bytes, and FFmpeg and libmpg123
    // look for it there. Class D like the unprotected file.
    {
        const Mp3LeadIn l = read("lame-crc-44100.mp3");
        assert(l.infoFrame && !l.vbri);
        assert(l.tag == Mp3LeadIn::Tag::Lame && l.lameCrcValid);
        assert(l.encoderDelay == 576);
        assert(classify(l) == Mp3Case::D);
        assert(near(engineToRekordboxOffsetMs(l), 51.18));
    }

    // A crafted VBRI frame in front of the file without a tag frame, in
    // the audio's channel mode and stereo over the joint-stereo audio.
    // rekordbox (libmpg123) plays it as one frame of silence either way,
    // Engine (FFmpeg) skips it and trims nothing: 1152 samples, 26.122 ms.
    for (const char *file : {"vbri-44100.mp3", "vbri-modediffers-44100.mp3"}) {
        const Mp3LeadIn l = read(file);
        assert(l.vbri && !l.infoFrame);
        assert(l.tag == Mp3LeadIn::Tag::None && l.encoderDelay == 0);
        assert(l.sampleRate == 44100 && l.samplesPerFrame == 1152);
        assert(classify(l) == Mp3Case::D);
        assert(near(engineToRekordboxOffsetMs(l), 26.122));
    }
    assert(read("vbri-44100.mp3").infoFrameModeMatchesAudio);
    assert(!read("vbri-modediffers-44100.mp3").infoFrameModeMatchesAudio);

    // The CBR file with its tag CRC broken: class C, same offset.
    {
        const Mp3LeadIn l = read("lame-badcrc-44100.mp3");
        assert(l.tag == Mp3LeadIn::Tag::Lame && !l.lameCrcValid);
        assert(l.encoderDelay == 576);
        assert(classify(l) == Mp3Case::C);
        assert(near(engineToRekordboxOffsetMs(l), 51.18));
    }

    // MPEG-2 at 22.05 kHz: 576-sample frames.
    {
        const Mp3LeadIn l = read("lame-mpeg2-22050.mp3");
        assert(l.sampleRate == 22050 && l.samplesPerFrame == 576);
        assert(l.infoFrame && l.tag == Mp3LeadIn::Tag::Lame && l.lameCrcValid);
        assert(l.infoFrameModeMatchesAudio);
        assert(classify(l) == Mp3Case::D);
        assert(near(engineToRekordboxOffsetMs(l), (576 + l.encoderDelay + 529) * 1000.0 / 22050));
    }

    // 48 kHz.
    {
        const Mp3LeadIn l = read("lame-cbr-48000.mp3");
        assert(l.sampleRate == 48000 && l.samplesPerFrame == 1152);
        assert(l.tag == Mp3LeadIn::Tag::Lame && l.lameCrcValid && l.encoderDelay == 576);
        assert(near(engineToRekordboxOffsetMs(l), 2257 / 48.0));
    }

    // Mono: the tag after 17 bytes of side information, not 32.
    {
        const Mp3LeadIn l = read("lame-mono-44100.mp3");
        assert(l.infoFrame && l.tag == Mp3LeadIn::Tag::Lame && l.lameCrcValid);
        assert(l.encoderDelay == 576);
        assert(l.infoFrameModeMatchesAudio);
        assert(classify(l) == Mp3Case::D);
    }

    // The CBR file behind an ID3v2.4 tag with a footer: the same answer
    // as without the tag.
    {
        const Mp3LeadIn l = read("lame-id3footer-44100.mp3");
        assert(l.infoFrame && l.tag == Mp3LeadIn::Tag::Lame && l.lameCrcValid);
        assert(l.encoderDelay == 576 && l.encoderPadding == 1404);
        assert(classify(l) == Mp3Case::D);
    }

    // FFmpeg: an ID3 tag, then a stereo "Lavc" Info frame over joint
    // stereo audio. Class B, 25 ms.
    {
        const Mp3LeadIn l = read("ffmpeg-lavc-44100.mp3");
        assert(l.infoFrame && l.tag == Mp3LeadIn::Tag::Lavc);
        assert(l.encoderDelay == 576);
        assert(!l.infoFrameModeMatchesAudio);
        assert(classify(l) == Mp3Case::B);
        assert(near(engineToRekordboxOffsetMs(l), 1105 * 1000.0 / 44100));
    }

    // Not an MP3: nothing.
    {
        assert(!readMp3LeadIn(g_dir + "/silence.wav").has_value());
        assert(!readMp3LeadIn(g_dir + "/does-not-exist.mp3").has_value());
    }

    // Never more than the start of a file. An MP3 behind a 1 MB ID3 tag
    // (the cover art case) and then 1 MB of bytes that are not frames: the
    // reader skips the tag without reading it and gives up after its
    // window, having read at most MaxLeadInBytesRead.
    {
        std::string bytes = "ID3";
        bytes += std::string("\x04\x00\x00", 3);
        const std::size_t tagSize = 1 << 20;
        bytes += static_cast<char>((tagSize >> 21) & 0x7F);
        bytes += static_cast<char>((tagSize >> 14) & 0x7F);
        bytes += static_cast<char>((tagSize >> 7) & 0x7F);
        bytes += static_cast<char>(tagSize & 0x7F);
        bytes += std::string(tagSize, 'x');
        bytes += std::string(1 << 20, '\0');
        CountingBuf buf(bytes);
        std::istream in(&buf);
        assert(!readMp3LeadIn(in).has_value());
        assert(buf.delivered > 0);
        assert(buf.delivered <= MaxLeadInBytesRead);
        assert(MaxLeadInBytesRead <= 64 * 1024);

        // And the real file read the same way counts the same.
        CountingBuf real(slurp(g_dir + "/lame-id3footer-44100.mp3"));
        std::istream realIn(&real);
        assert(readMp3LeadIn(realIn).has_value());
        assert(real.delivered <= MaxLeadInBytesRead);
    }

    std::cout << "mp3_lead_in_reader_test: all passed\n";
    return 0;
}
