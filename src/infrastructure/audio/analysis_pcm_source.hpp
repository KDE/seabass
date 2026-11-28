// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>

#include "domain/pcm_source.hpp"

namespace seabass::infrastructure::audio
{

// The decoder an analysis reads a file through: the one on FFmpeg's
// libraries where this build has it (ffmpeg_pcm_source.hpp: one thread,
// nothing to race with), and Qt Multimedia's otherwise. Both give the
// same samples, which ffmpeg_pcm_source_test holds them to, so which one
// read a file is not part of an analysis's provenance.
std::unique_ptr<domain::PcmSource> openAnalysisPcmSource(const std::string &absoluteFilePath);

// Whether this build can decode at all.
bool analysisDecodingAvailable();

// "FFmpeg" or "Qt Multimedia": for a log line and for the tests.
const char *analysisDecoderName();

// Whether the decoder on FFmpeg's libraries is the one in use: true
// where this build has it, unless a test said otherwise. The classes
// that decode for something other than analysis ask it too (the silence
// probe, the alignment probe, the detail waveform), each keeping its Qt
// Multimedia path beside the other for builds without the libraries and
// for the tests that hold the two to the same answers.
bool ownDecoderInUse();

// The decoder on FFmpeg's libraries by its version ("lavc 60.31.102"),
// for a record of which decoder made a measurement; empty where it is
// not the one in use.
std::string ownDecoderVersion();

// For tests: decode through Qt Multimedia although this build has the
// other (false), or back (true). Nothing happens in a build without it.
void setOwnDecoderInUseForTesting(bool inUse);

}  // namespace seabass::infrastructure::audio
