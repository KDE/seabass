// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string_view>

namespace seabass::domain
{

// What the start of an MP3 file says about how a decoder counts its first
// sample (#50). rekordbox and Engine DJ decode the same file with
// different libraries, and the two disagree on where time 0 is: a cue at
// the same audio instant is stored 0, 25, 26 or 51 ms apart at 44.1 kHz
// depending on how the file was encoded. This struct is the part of the
// file that decides which; infrastructure/audio/mp3_lead_in_reader reads
// it, and everything below turns it into a number without any I/O.
struct Mp3LeadIn
{
    // The encoder string in the LAME extension of a Xing/Info frame.
    //   None   no Xing/Info frame, or one without an encoder string
    //   Lame   "LAME..." (also LAME forks that keep the prefix)
    //   Lavc   FFmpeg's muxer, "Lavc..." or "Lavf...": it writes the
    //          delay/padding fields but no valid LAME CRC
    //   Other  any other non-empty string
    enum class Tag { None, Lame, Lavc, Other };

    // A Xing or Info frame leads the stream: a whole frame that carries
    // the tag and plays as silence if a decoder does not recognise it.
    bool infoFrame = false;
    Tag tag = Tag::None;
    // The LAME tag's CRC-16 over the frame up to it is correct.
    bool lameCrcValid = false;
    // From the LAME extension's 3 bytes: samples the encoder put before
    // the audio, and after it. 0 when there is no such field.
    int encoderDelay = 0;
    int encoderPadding = 0;
    // The Info frame's channel mode is the first audio frame's. LAME
    // writes it so; FFmpeg writes a stereo Info frame over joint-stereo
    // audio, and rekordbox's frame map then skips the Info frame.
    bool infoFrameModeMatchesAudio = false;
    // A Fraunhofer VBRI frame leads the stream instead of Xing/Info.
    bool vbri = false;
    int sampleRate = 0;
    // 1152 for MPEG-1 Layer III, 576 for MPEG-2 and 2.5.
    int samplesPerFrame = 0;
};

// mp3guessenc's four classes, the ones Mixxx and dj-data-converter use.
// The class is adopted; their per-class millisecond values are specific
// to their own decoders and are not.
//   A  no Xing/Info/VBRI frame
//   B  Xing/Info without a LAME tag: no encoder string, "Lavc"/"Lavf",
//      or any other non-LAME string
//   C  LAME tag whose CRC is wrong
//   D  LAME tag with a correct CRC, or a VBRI frame
enum class Mp3Case { A, B, C, D };

Mp3Case classify(const Mp3LeadIn &leadIn);

// The two decoders whose timelines Seabass translates between.
//   RekordboxMpg123NoGapless  rekordbox 7 and current Pioneer players:
//       libmpg123 with gapless off. Nothing is trimmed; the leading tag
//       frame plays as one frame of silence when its channel mode is the
//       audio's, and is skipped when it is not.
//   EngineFfmpegGapless  Engine DJ 5.1 on the desktop (FFmpeg 4.4.3,
//       libavformat's mp3 demuxer): the tag frame is never played, and
//       for a LAME, Lavc or Lavf tag encoder_delay + 529 samples are
//       trimmed as well (start_skip_samples = delay + 528 + 1).
enum class Mp3Decoder { RekordboxMpg123NoGapless, EngineFfmpegGapless };

// Samples the decoder plays before the first sample of the first AUDIO
// frame (the frame after any tag frame). Negative when the decoder trims
// into the audio: Engine on a LAME file starts encoderDelay + 529 samples
// into it.
int samplesBeforeAudio(const Mp3LeadIn &leadIn, Mp3Decoder decoder);

// rekordbox position minus Engine position of the same audio instant, in
// ms at the file's sample rate. Positive when rekordbox's number is the
// larger one: add it to an Engine position to get rekordbox's, subtract
// it from a rekordbox position to get Engine's. 51.18 ms for a LAME file
// at 44.1 kHz with a 576-sample delay.
//
// A file whose lead-in could not be read has no Mp3LeadIn at all, and
// the callers use 0 for it: the correction is never worse than not
// having one.
double engineToRekordboxOffsetMs(const Mp3LeadIn &leadIn);

// Whether a path names an MP3 by its extension, in any case: the files
// whose lead-in is worth reading at all.
bool hasMp3Extension(std::string_view path);

}  // namespace seabass::domain
