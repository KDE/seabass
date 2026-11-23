// SPDX-FileCopyrightText: 2001-2025 Mixxx Development Team
// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Mixxx's two Queen Mary analyser plugins and the framing helper they
// share, ported from Mixxx 2.5.3 (commit
// 93275786c517e0804951f7428a5cf045aa541b8d):
// src/analyzer/plugins/analyzerqueenmarybeats.cpp,
// src/analyzer/plugins/analyzerqueenmarykey.cpp,
// src/analyzer/plugins/buffering_utils.cpp and the global key reduction
// of src/track/keyutils.cpp (KeyUtils::calculateGlobalKey). Mixxx is
// "Copyright (C) 2001-2025 Mixxx Development Team" and licensed under the
// GNU General Public License, version 2 or (at your option) any later
// version; so is this port.
//
// What changed in the port: the input is the PcmSource port's mono
// signal (the mean of the channels, which is what Mixxx's stereo downmix,
// (left + right) / 2, gives) rather than interleaved stereo; Qt's types
// and logging are gone. The qm-dsp configuration, the framing and the
// arithmetic are Mixxx's.
//
// For the benchmark tool only; not part of the app.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class DetectionFunction;
class GetKeyMode;

namespace seabass::baseline::mixxx
{

// Mixxx feeds its analysers in chunks of this many frames
// (src/analyzer/constants.h, kAnalysisFramesPerChunk). It matters for
// the key: a key change is stamped with the frame count at the end of
// the chunk that holds it.
inline constexpr std::size_t AnalysisFramesPerChunk = 4096;

// Frames the signal into overlapping windows: the first window is
// centred on the first sample, and finalize() pads with silence so the
// last samples are analysed too.
class DownmixAndOverlapHelper
{
public:
    using WindowReadyCallback = std::function<bool(double *buffer, std::size_t frames)>;
    bool initialize(std::size_t windowSize, std::size_t stepSize, const WindowReadyCallback &callback);
    bool processMonoSamples(const float *input, std::size_t frames);
    bool finalize();

private:
    bool processInner(const float *input, std::size_t numInputFrames);
    std::vector<double> m_buffer;
    std::size_t m_windowSize = 0;
    std::size_t m_stepSize = 0;
    std::size_t m_bufferWritePosition = 0;
    WindowReadyCallback m_callback;
};

// The Queen Mary tempo and beat tracker as Mixxx configures it: the
// complex spectral difference onset function over steps of 0.01161 s
// (512 frames at 44.1 kHz), a window of the next power of two above a
// 50th of the rate, then TempoTrackV2's beat period and beats with their
// default parameters.
class QueenMaryBeats
{
public:
    QueenMaryBeats();
    ~QueenMaryBeats();
    bool initialize(int sampleRate);
    bool processSamples(const float *mono, std::size_t frames);
    bool finalize();
    // The beats, in frames from the first sample, as the tracker gives
    // them: not ironed.
    const std::vector<double> &beats() const { return m_resultBeats; }
    int stepSizeFrames() const { return m_stepSizeFrames; }
    int windowSize() const { return m_windowSize; }

private:
    std::unique_ptr<DetectionFunction> m_pDetectionFunction;
    DownmixAndOverlapHelper m_helper;
    int m_sampleRate = 0;
    int m_windowSize = 0;
    int m_stepSizeFrames = 0;
    std::vector<double> m_detectionResults;
    std::vector<double> m_resultBeats;
};

// qm-dsp's key mode detector as Mixxx configures it (concert A at
// 440 Hz, qm-dsp's defaults for everything else). Keys are Mixxx's
// ChromaticKey numbers: 0 none, 1 to 12 C major to B major, 13 to 24 C
// minor to B minor.
class QueenMaryKey
{
public:
    QueenMaryKey();
    ~QueenMaryKey();
    // Feed chunks of AnalysisFramesPerChunk, as Mixxx does: a key change
    // is stamped with the frames fed so far, the current chunk included.
    bool initialize(int sampleRate);
    bool processSamples(const float *mono, std::size_t frames);
    bool finalize();
    // Each change of key and the frame it was stamped with.
    const std::vector<std::pair<int, double>> &keyChanges() const { return m_resultKeys; }

private:
    std::unique_ptr<GetKeyMode> m_pKeyMode;
    DownmixAndOverlapHelper m_helper;
    double m_currentFrame = 0;
    int m_prevKey = 0;
    bool m_failed = false;
    std::vector<std::pair<int, double>> m_resultKeys;
};

// KeyUtils::calculateGlobalKey: the key that holds for the most frames,
// the lowest key number on a tie; 0 when there are no changes.
int calculateGlobalKey(const std::vector<std::pair<int, double>> &keyChanges, double totalFrames);

}  // namespace seabass::baseline::mixxx
