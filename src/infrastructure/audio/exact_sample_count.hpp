// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace seabass::infrastructure::audio
{

// A file's length in samples (per channel) and its rate, as its container
// states them exactly, and where the count came from.
struct ExactSampleCount
{
    enum class Source { FlacStreamInfo, WavData, AiffComm, Mp4TrackDuration, Mp3InfoFrames };
    std::uint64_t samples = 0;
    double sampleRateHz = 0.0;
    Source source = Source::FlacStreamInfo;
};

// The count a Denon player stores for the file, as its own container
// gives it (measured on a Prime 4 against its own analysis, see
// tests/exact_sample_count_test.cpp):
//   .flac        STREAMINFO's total samples (TagLib); 0 there means "not
//                known": nothing
//   .wav         the data chunk's size over the frame size, PCM or float
//                only (TagLib)
//   .aif/.aiff   COMM's sample frame count (TagLib)
//   .m4a/.mp4    the sound track's duration in its media header (mdhd),
//                in samples, which is the sum of its sample table (stts):
//                no edit list, priming or gapless trim applied. What a
//                player stores, measured on 12 AAC-LC 44.1 kHz files with
//                no edit list. Files with an edit list or iTunSMPB, ALAC
//                and 48 kHz M4A are not measured: their count is read the
//                same way, not guessed at. (Read here: TagLib gives only a
//                rounded length.)
//   .mp3         the Xing/Info frame's frame count times the samples per
//                frame (1152 for MPEG-1 Layer III, 576 for MPEG-2 and 2.5),
//                encoder delay and padding included, the Info frame itself
//                not counted (TagLib). Measured right for 251 Lavc-encoded
//                44.1 kHz files; a LAME-encoded, VBR, 48 kHz or MPEG-2 MP3
//                has not been measured on a player. Nothing for an MP3
//                without a Xing/Info frame (its frames are not counted).
// Nothing for any other kind, a file that is not there or does not parse,
// or a count or rate of 0.
std::optional<ExactSampleCount> readExactSampleCount(const std::string &absoluteFilePath);

// The MP4 part on its own, over a file's bytes, for a test.
std::optional<ExactSampleCount> mp4SampleCountOf(std::string_view fileBytes);

}  // namespace seabass::infrastructure::audio
