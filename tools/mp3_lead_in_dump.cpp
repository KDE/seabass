// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What readMp3LeadIn() makes of each file, and what the #50 model makes of
// that: the parsed Mp3LeadIn fields, mp3guessenc's class, samplesBeforeAudio
// for rekordbox and Engine, and engineToRekordboxOffsetMs().
//
//   mp3_lead_in_dump FILE...
//
// A file readMp3LeadIn() refuses prints "unreadable". Exit code 0 unless
// no file was given.

#include <cstdio>
#include <string>

#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"

namespace
{

using seabass::domain::Mp3Case;
using seabass::domain::Mp3Decoder;
using seabass::domain::Mp3LeadIn;

const char *tagName(Mp3LeadIn::Tag tag)
{
    switch (tag) {
    case Mp3LeadIn::Tag::None:
        return "none";
    case Mp3LeadIn::Tag::Lame:
        return "LAME";
    case Mp3LeadIn::Tag::LameL3:
        return "L3.99";
    case Mp3LeadIn::Tag::Lavc:
        return "Lavc";
    case Mp3LeadIn::Tag::Other:
        return "other";
    }
    return "?";
}

char caseLetter(Mp3Case c)
{
    switch (c) {
    case Mp3Case::A:
        return 'A';
    case Mp3Case::B:
        return 'B';
    case Mp3Case::C:
        return 'C';
    case Mp3Case::D:
        return 'D';
    }
    return '?';
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE...\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        const auto leadIn = seabass::infrastructure::audio::readMp3LeadIn(path);
        if (!leadIn) {
            std::printf("%s\n  unreadable\n", path.c_str());
            continue;
        }
        std::printf("%s\n", path.c_str());
        std::printf("  case %c  infoFrame=%d vbri=%d tag=%s crcValid=%d modesMatch=%d first=%d counts=%d\n",
                    caseLetter(seabass::domain::classify(*leadIn)), leadIn->infoFrame, leadIn->vbri,
                    tagName(leadIn->tag), leadIn->lameCrcValid, leadIn->infoFrameModeMatchesAudio,
                    leadIn->tagFrameFirst, leadIn->tagFrameHasCounts);
        std::printf("  rate=%d samplesPerFrame=%d delay=%d padding=%d\n", leadIn->sampleRate,
                    leadIn->samplesPerFrame, leadIn->encoderDelay, leadIn->encoderPadding);
        std::printf("  samplesBeforeAudio rekordbox=%d engine=%d  engineToRekordboxOffsetMs=%.3f\n",
                    seabass::domain::samplesBeforeAudio(*leadIn, Mp3Decoder::RekordboxMpg123NoGapless),
                    seabass::domain::samplesBeforeAudio(*leadIn, Mp3Decoder::EngineFfmpegGapless),
                    seabass::domain::engineToRekordboxOffsetMs(*leadIn));
    }
    return 0;
}
