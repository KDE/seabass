// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "domain/waveform.hpp"

namespace seabass::infrastructure::audio
{

// An Engine track's scrolling waveform for the zoomed popup, made from
// its audio file (domain/audio_detail_waveform.hpp) the way a Denon
// player makes its own: decoded with QAudioDecoder, and column 0 starts
// at that decoder's sample 0, where Engine's beat grid and cues count
// from, so the grid can be judged against the kicks.
//
// How far that holds:
//   * MP3: measured against the player. Engine's lead-in model
//     (domain/mp3_lead_in.hpp, confirmed on a Prime 4 for #50) is where
//     this decoder puts sample 0 on every lead-in class
//     (qt_decoder_timeline_test, audio_detail_waveform_test).
//   * WAV, FLAC, M4A, AIFF: the same as a stock FFmpeg decode, which is
//     what Engine uses; not checked against a player.
//   * Only for Qt's FFmpeg backend (the one every Seabass build ships):
//     another backend may count from elsewhere.
//
// Whole file, on the calling thread, which may be any thread with a
// QCoreApplication in the process (a nested QEventLoop runs the decoder).
// Meant for a worker thread: a six-minute MP3 takes a moment.

// Decodes `absoluteFilePath` to mono floats, handing them to `sink` as
// they come, with the file's rate. False when it cannot be decoded whole:
// no backend, unreadable, a rate that changes partway, timed out, or
// `cancelled` said so (asked at every buffer). `durationKnown`, when
// given, is told the decoder's idea of the length in ms once it has one.
//
// Two decoders, one answer: where this build has the decoder on FFmpeg's
// libraries (analysis_pcm_source.hpp: ownDecoderInUse()) the file is read
// through that, in the calling thread, and otherwise through Qt
// Multimedia; ffmpeg_decoders_test holds the two to the same samples.
// The cancel and the timeout end the decode on both, a read that does
// not come back included.
bool decodeMono(const std::string &absoluteFilePath,
                const std::function<void(const float *samples, std::size_t count, int sampleRate)> &sink,
                const std::function<bool()> &cancelled = {}, int timeoutMs = 120000,
                const std::function<void(long long durationMs)> &durationKnown = {});

// The scrolling waveform of the file, 150 columns a second from its
// decoder's sample 0. Empty when decodeMono() fails.
domain::DetailWaveform audioDetailWaveform(const std::string &absoluteFilePath,
                                           const std::function<bool()> &cancelled = {});

}  // namespace seabass::infrastructure::audio
