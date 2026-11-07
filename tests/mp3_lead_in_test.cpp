// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::Mp3LeadIn (#50): mp3guessenc's classes, and the model of how
// rekordbox and Engine count the start of an MP3, one case per row of the
// table in the issue.

#include <cassert>
#include <cmath>
#include <iostream>

#include "domain/mp3_lead_in.hpp"

using namespace seabass::domain;

namespace
{

bool near(double a, double b)
{
    return std::abs(a - b) < 0.01;
}

Mp3LeadIn mpeg1(int sampleRate = 44100)
{
    Mp3LeadIn l;
    l.sampleRate = sampleRate;
    l.samplesPerFrame = 1152;
    return l;
}

Mp3LeadIn lame(bool crcValid, int sampleRate = 44100, int samplesPerFrame = 1152)
{
    Mp3LeadIn l;
    l.infoFrame = true;
    l.tag = Mp3LeadIn::Tag::Lame;
    l.lameCrcValid = crcValid;
    l.encoderDelay = 576;
    l.encoderPadding = 1000;
    l.infoFrameModeMatchesAudio = true;
    l.sampleRate = sampleRate;
    l.samplesPerFrame = samplesPerFrame;
    return l;
}

}  // namespace

int main()
{
    constexpr auto Rb = Mp3Decoder::RekordboxMpg123NoGapless;
    constexpr auto En = Mp3Decoder::EngineFfmpegGapless;

    // A: no tag frame. Both decoders start at the first audio sample.
    {
        const Mp3LeadIn l = mpeg1();
        assert(classify(l) == Mp3Case::A);
        assert(samplesBeforeAudio(l, Rb) == 0);
        assert(samplesBeforeAudio(l, En) == 0);
        assert(engineToRekordboxOffsetMs(l) == 0.0);
    }

    // B: Info frame without an encoder string, channel modes equal.
    // rekordbox plays it as a frame of silence, Engine skips it: 26 ms.
    {
        Mp3LeadIn l = mpeg1();
        l.infoFrame = true;
        l.infoFrameModeMatchesAudio = true;
        assert(classify(l) == Mp3Case::B);
        assert(samplesBeforeAudio(l, Rb) == 1152);
        assert(samplesBeforeAudio(l, En) == 0);
        assert(near(engineToRekordboxOffsetMs(l), 1152 * 1000.0 / 44100));
        assert(near(engineToRekordboxOffsetMs(l), 26.12));

        // Modes differ: rekordbox skips it too, and nothing is apart.
        l.infoFrameModeMatchesAudio = false;
        assert(samplesBeforeAudio(l, Rb) == 0);
        assert(engineToRekordboxOffsetMs(l) == 0.0);
    }

    // B: an unknown encoder string is not trimmed by either side.
    {
        Mp3LeadIn l = mpeg1();
        l.infoFrame = true;
        l.tag = Mp3LeadIn::Tag::Other;
        l.encoderDelay = 576;
        l.infoFrameModeMatchesAudio = true;
        assert(classify(l) == Mp3Case::B);
        assert(samplesBeforeAudio(l, En) == 0);
        assert(near(engineToRekordboxOffsetMs(l), 26.12));
    }

    // B: FFmpeg's own encode (Lavc), a stereo Info frame over joint
    // stereo audio. rekordbox skips the frame and trims nothing; Engine
    // trims delay + 529: 1105 samples, 25 ms.
    {
        Mp3LeadIn l = mpeg1();
        l.infoFrame = true;
        l.tag = Mp3LeadIn::Tag::Lavc;
        l.encoderDelay = 576;
        l.infoFrameModeMatchesAudio = false;
        assert(classify(l) == Mp3Case::B);
        assert(samplesBeforeAudio(l, Rb) == 0);
        assert(samplesBeforeAudio(l, En) == -1105);
        assert(near(engineToRekordboxOffsetMs(l), 25.06));
    }

    // D: LAME with a valid CRC. rekordbox 1152 before the audio, Engine
    // 576 + 529 into it: 2257 samples, 51.18 ms.
    {
        const Mp3LeadIn l = lame(true);
        assert(classify(l) == Mp3Case::D);
        assert(samplesBeforeAudio(l, Rb) == 1152);
        assert(samplesBeforeAudio(l, En) == -1105);
        assert(near(engineToRekordboxOffsetMs(l), 2257 * 1000.0 / 44100));
        assert(near(engineToRekordboxOffsetMs(l), 51.18));
    }

    // C: the same with a wrong CRC. Neither decoder checks it, so the
    // offset is the same; only the class differs.
    {
        const Mp3LeadIn l = lame(false);
        assert(classify(l) == Mp3Case::C);
        assert(near(engineToRekordboxOffsetMs(l), 51.18));
    }

    // D: a VBRI frame. FFmpeg recognises VBRI, skips the frame and trims
    // nothing. libmpg123 does not parse VBRI at all: it decodes the frame
    // as one frame of silence whatever its channel mode, so the mode
    // comparison that decides a Xing/Info frame does not apply. Measured
    // on click-VBRI-44100.mp3 and click-VBRI-modediffers-44100.mp3: lead
    // 2257 (libmpg123) and 1105 (FFmpeg) both ways.
    for (const bool modesMatch : {true, false}) {
        Mp3LeadIn l = mpeg1();
        l.vbri = true;
        l.infoFrameModeMatchesAudio = modesMatch;
        assert(classify(l) == Mp3Case::D);
        assert(samplesBeforeAudio(l, Rb) == 1152);
        assert(samplesBeforeAudio(l, En) == 0);
        assert(near(engineToRekordboxOffsetMs(l), 26.12));
    }

    // 48 kHz: the same samples are fewer milliseconds.
    {
        const Mp3LeadIn l = lame(true, 48000);
        assert(near(engineToRekordboxOffsetMs(l), 2257 / 48.0));
        assert(near(engineToRekordboxOffsetMs(l), 47.02));
    }

    // MPEG-2 at 22.05 kHz: a frame is 576 samples.
    {
        const Mp3LeadIn l = lame(true, 22050, 576);
        assert(samplesBeforeAudio(l, Rb) == 576);
        assert(samplesBeforeAudio(l, En) == -1105);
        assert(near(engineToRekordboxOffsetMs(l), 1681 * 1000.0 / 22050));

        Mp3LeadIn info = mpeg1(22050);
        info.samplesPerFrame = 576;
        info.infoFrame = true;
        info.infoFrameModeMatchesAudio = true;
        assert(near(engineToRekordboxOffsetMs(info), 576 * 1000.0 / 22050));
    }

    // The sign: an Engine position plus the offset is rekordbox's
    // position of the same instant. A sound at audio sample 44100 (one
    // second into the audio frames) on the LAME file is at
    // 1152 + 44100 samples in rekordbox and 44100 - 1105 in Engine.
    {
        const Mp3LeadIn l = lame(true);
        const double rekordboxMs = (1152 + 44100) * 1000.0 / 44100;
        const double engineMs = (44100 - 1105) * 1000.0 / 44100;
        assert(engineToRekordboxOffsetMs(l) > 0.0);
        assert(near(engineMs + engineToRekordboxOffsetMs(l), rekordboxMs));
    }

    // No sample rate: nothing to convert with, so no correction.
    {
        Mp3LeadIn l = lame(true);
        l.sampleRate = 0;
        assert(engineToRekordboxOffsetMs(l) == 0.0);
    }

    std::cout << "mp3_lead_in_test: all passed\n";
    return 0;
}
