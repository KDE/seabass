// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/mp3_lead_in.hpp"

namespace seabass::domain
{

namespace
{

// How one decoder treats the start of a file. Every number in #50's
// model is in this table and nowhere else, so a click measurement on the
// real software changes a row here and nothing else.
//
// Sources: rekordbox from rbxport PR 295 (static analysis of rekordbox
// 7.2.19 plus a 400-file measurement) and Pioneer's 2012 note (24 ms at
// 48 kHz, 26 ms at 44.1 kHz). Engine from its bundled FFmpeg 4.4.3
// (libavformat/mp3dec.c, measured on the Mac 2026-11-06).
struct DecoderModel
{
    // Plays the tag frame as one frame of silence when its channel mode
    // is the first audio frame's.
    bool playsTagFrameWhenModesMatch;
    // ... and when it is not.
    bool playsTagFrameWhenModesDiffer;
    // Plays a VBRI frame as one frame of silence, whatever its channel
    // mode: libmpg123 has no VBRI parser and decodes it like audio;
    // FFmpeg recognises it and skips it (measured on the VBRI click files).
    bool playsVbriFrame;
    // Trims encoderDelay + extraTrimSamples for these tags.
    bool trimsLame;
    bool trimsLavc;
    int extraTrimSamples;
};

constexpr DecoderModel RekordboxModel{true, false, true, false, false, 0};
// 529 = the decoder's own 528-sample delay plus 1 (libavformat).
constexpr DecoderModel EngineModel{false, false, false, true, true, 529};

const DecoderModel &modelOf(Mp3Decoder decoder)
{
    return decoder == Mp3Decoder::RekordboxMpg123NoGapless ? RekordboxModel : EngineModel;
}

}  // namespace

Mp3Case classify(const Mp3LeadIn &leadIn)
{
    if (leadIn.vbri) {
        return Mp3Case::D;
    }
    if (!leadIn.infoFrame) {
        return Mp3Case::A;
    }
    if (leadIn.tag != Mp3LeadIn::Tag::Lame) {
        return Mp3Case::B;
    }
    return leadIn.lameCrcValid ? Mp3Case::D : Mp3Case::C;
}

int samplesBeforeAudio(const Mp3LeadIn &leadIn, Mp3Decoder decoder)
{
    const DecoderModel &model = modelOf(decoder);
    int samples = 0;
    if (leadIn.infoFrame) {
        const bool plays = leadIn.infoFrameModeMatchesAudio ? model.playsTagFrameWhenModesMatch
                                                            : model.playsTagFrameWhenModesDiffer;
        if (plays) {
            samples += leadIn.samplesPerFrame;
        }
    } else if (leadIn.vbri && model.playsVbriFrame) {
        // The channel mode does not matter here: the mode comparison is
        // part of libmpg123's Xing/Info handling, which a VBRI frame never
        // reaches.
        samples += leadIn.samplesPerFrame;
    }
    // The delay field lives in the Xing/Info frame's LAME extension, so
    // there is nothing to trim without one.
    const bool trims = leadIn.infoFrame
        && ((leadIn.tag == Mp3LeadIn::Tag::Lame && model.trimsLame)
            || (leadIn.tag == Mp3LeadIn::Tag::Lavc && model.trimsLavc));
    if (trims) {
        samples -= leadIn.encoderDelay + model.extraTrimSamples;
    }
    return samples;
}

double engineToRekordboxOffsetMs(const Mp3LeadIn &leadIn)
{
    if (leadIn.sampleRate <= 0) {
        return 0.0;
    }
    const int difference = samplesBeforeAudio(leadIn, Mp3Decoder::RekordboxMpg123NoGapless)
        - samplesBeforeAudio(leadIn, Mp3Decoder::EngineFfmpegGapless);
    return difference * 1000.0 / leadIn.sampleRate;
}

bool hasMp3Extension(std::string_view path)
{
    if (path.size() < 4) {
        return false;
    }
    const std::string_view ext = path.substr(path.size() - 4);
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return ext[0] == '.' && lower(ext[1]) == 'm' && lower(ext[2]) == 'p' && ext[3] == '3';
}

}  // namespace seabass::domain
