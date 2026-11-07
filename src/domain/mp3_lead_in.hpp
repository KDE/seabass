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
    //   LameL3 "L3.99...": the short form some LAME 3.99 builds wrote.
    //          LAME's tag all the same (class C or D), but FFmpeg trims
    //          only after "LAME", "Lavf" or "Lavc" (a 4-byte compare;
    //          measured on tests/fixtures/mp3-click/click-D-l399-44100.mp3)
    //   Lavc   FFmpeg's muxer, "Lavc..." or "Lavf...": it writes the
    //          delay/padding fields but no valid LAME CRC
    //   Other  any other non-empty string
    enum class Tag { None, Lame, LameL3, Lavc, Other };

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
    // audio, and rekordbox's frame map then skips the Info frame. Only a
    // Xing/Info frame is decided by this: a VBRI frame plays on rekordbox
    // whatever its mode.
    bool infoFrameModeMatchesAudio = false;
    // A Fraunhofer VBRI frame leads the stream instead of Xing/Info.
    bool vbri = false;
    // The tag frame (Xing, Info or VBRI) starts at the first byte after
    // the ID3v2 tags, or of the file when there are none. FFmpeg looks
    // for it only there: after junk or padding it decodes the frame as
    // audio. libmpg123 resyncs over anything. Measured on
    // tests/fixtures/mp3-click/click-D-lame-junk-44100.mp3 and -id3junk.
    bool tagFrameFirst = true;
    // The tag frame counts the stream's frames or bytes: at least one of
    // the two fields is there and not 0. FFmpeg plays a tag frame that
    // counts neither as a frame of silence, and still trims (measured on
    // click-D-lame-nocounts-44100.mp3 and click-VBRI-nocounts-44100.mp3).
    bool tagFrameHasCounts = true;
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
//   C  LAME tag ("LAME" or "L3.99") whose CRC is wrong
//   D  LAME tag with a correct CRC, or a VBRI frame
enum class Mp3Case { A, B, C, D };

Mp3Case classify(const Mp3LeadIn &leadIn);

// The two decoders whose timelines Seabass translates between.
//   RekordboxMpg123NoGapless  rekordbox 7 and current Pioneer players:
//       libmpg123 with gapless off. Nothing is trimmed; a leading
//       Xing/Info frame plays as one frame of silence when its channel
//       mode is the audio's, and is skipped when it is not. A VBRI frame
//       always plays as one frame of silence: libmpg123 does not parse
//       VBRI and decodes it like audio.
//   EngineFfmpegGapless  Engine DJ 5.1 on the desktop (FFmpeg 4.4.3,
//       libavformat's mp3 demuxer): the tag frame (Xing, Info or VBRI)
//       is not played, and for a "LAME", "Lavc" or "Lavf" tag (not
//       "L3.99") encoder_delay
//       + 529 samples are trimmed as well (start_skip_samples = delay +
//       528 + 1). Nothing is trimmed after a VBRI frame. A tag frame
//       that is not the first thing after the ID3v2 tags is not read at
//       all: played as one frame of silence, nothing trimmed. One that
//       counts neither frames nor bytes is played, and its trim still
//       applies.
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
