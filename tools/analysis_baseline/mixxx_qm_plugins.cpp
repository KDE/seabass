// SPDX-FileCopyrightText: 2001-2025 Mixxx Development Team
// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Ported from Mixxx 2.5.3; see mixxx_qm_plugins.hpp. Mixxx's own comments
// are kept where they explain the method.

#include <dsp/keydetection/GetKeyMode.h>
#include <dsp/onsets/DetectionFunction.h>
#include <dsp/tempotracking/TempoTrackV2.h>
#include <maths/MathUtilities.h>

#include "analysis_baseline/mixxx_qm_plugins.hpp"

#include <algorithm>
#include <cstdint>
#include <map>

namespace seabass::baseline::mixxx
{

// --- DownmixAndOverlapHelper (buffering_utils.cpp) ---

bool DownmixAndOverlapHelper::initialize(std::size_t windowSize, std::size_t stepSize,
                                         const WindowReadyCallback &callback)
{
    m_buffer.assign(windowSize, 0.0);
    m_callback = callback;
    m_windowSize = windowSize;
    m_stepSize = stepSize;
    // Make sure the first frame is centred in the FFT window, so the
    // result is significant from the first step on.
    m_bufferWritePosition = windowSize / 2;
    return m_windowSize > 0 && m_stepSize > 0 && m_stepSize <= m_windowSize && callback;
}

bool DownmixAndOverlapHelper::processMonoSamples(const float *input, std::size_t frames)
{
    return processInner(input, frames);
}

bool DownmixAndOverlapHelper::finalize()
{
    // We need to append at least windowSize / 2 - stepSize of silence to
    // have valid results for the last samples. Since we proceed in fixed
    // steps, up to stepSize - 1 samples remain unprocessed, which is why
    // windowSize / 2 - 1 is used below instead.
    const std::size_t framesToFillWindow = m_windowSize - m_bufferWritePosition;
    const std::size_t numInputFrames = std::max(framesToFillWindow, m_windowSize / 2 - 1);
    return processInner(nullptr, numInputFrames);
}

bool DownmixAndOverlapHelper::processInner(const float *input, std::size_t numInputFrames)
{
    std::size_t inRead = 0;
    double *downmix = m_buffer.data();
    while (inRead < numInputFrames) {
        const std::size_t readAvailable = numInputFrames - inRead;
        const std::size_t writeAvailable = m_windowSize - m_bufferWritePosition;
        const std::size_t numFrames = std::min(readAvailable, writeAvailable);
        if (input) {
            for (std::size_t i = 0; i < numFrames; ++i) {
                downmix[m_bufferWritePosition + i] = input[inRead + i];
            }
        } else {
            // The finalize call: silence completes the samples left in
            // the buffer.
            for (std::size_t i = 0; i < numFrames; ++i) {
                downmix[m_bufferWritePosition + i] = 0;
            }
        }
        m_bufferWritePosition += numFrames;
        inRead += numFrames;
        if (m_bufferWritePosition == m_windowSize) {
            if (!m_callback(downmix, m_windowSize)) {
                return false;
            }
            for (std::size_t i = 0; i < (m_windowSize - m_stepSize); ++i) {
                downmix[i] = downmix[i + m_stepSize];
            }
            m_bufferWritePosition -= m_stepSize;
        }
    }
    return true;
}

// --- AnalyzerQueenMaryBeats ---

namespace
{

// This determines the resolution of the resulting beat map. About 12 ms
// (86 Hz) is a fair compromise between accuracy and analysis speed, also
// matching the preferred window and step sizes of the BeatTrack VAMP
// plugin. For a 44.1 kHz track this is 512 sample steps.
constexpr float kStepSecs = 0.01161f;
// Results in 43 Hz at 44.1 kHz, 47 Hz at 48 kHz and at 96 kHz.
constexpr int kMaximumBinSizeHz = 50;

DFConfig makeDetectionFunctionConfig(int stepSizeFrames, int windowSize)
{
    // The defaults of the VAMP beat tracker plugin Mixxx 2.0 used.
    DFConfig config;
    config.DFType = DF_COMPLEXSD;
    config.stepSize = stepSizeFrames;
    config.frameLength = windowSize;
    config.dbRise = 3;
    config.adaptiveWhitening = false;
    config.whiteningRelaxCoeff = -1;
    config.whiteningFloor = -1;
    return config;
}

}  // namespace

QueenMaryBeats::QueenMaryBeats() = default;
QueenMaryBeats::~QueenMaryBeats() = default;

bool QueenMaryBeats::initialize(int sampleRate)
{
    m_detectionResults.clear();
    m_resultBeats.clear();
    m_sampleRate = sampleRate;
    // Mixxx's SampleRate is an unsigned 32-bit integer: the step is the
    // rate times a float, truncated, and the window size is from an
    // integer division.
    const auto rate = static_cast<std::uint32_t>(sampleRate);
    m_stepSizeFrames = static_cast<int>(rate * kStepSecs);
    m_windowSize = MathUtilities::nextPowerOfTwo(static_cast<int>(rate / kMaximumBinSizeHz));
    m_pDetectionFunction = std::make_unique<DetectionFunction>(makeDetectionFunctionConfig(m_stepSizeFrames, m_windowSize));
    return m_helper.initialize(static_cast<std::size_t>(m_windowSize), static_cast<std::size_t>(m_stepSizeFrames),
                               [this](double *window, std::size_t) {
                                   m_detectionResults.push_back(m_pDetectionFunction->processTimeDomain(window));
                                   return true;
                               });
}

bool QueenMaryBeats::processSamples(const float *mono, std::size_t frames)
{
    if (!m_pDetectionFunction) {
        return false;
    }
    return m_helper.processMonoSamples(mono, frames);
}

bool QueenMaryBeats::finalize()
{
    m_helper.finalize();

    int nonZeroCount = static_cast<int>(m_detectionResults.size());
    while (nonZeroCount > 0 && m_detectionResults.at(static_cast<std::size_t>(nonZeroCount - 1)) <= 0.0) {
        --nonZeroCount;
    }

    std::vector<double> df;
    std::vector<double> beatPeriod;
    std::vector<double> tempi;
    const auto requiredSize = static_cast<std::size_t>(std::max(0, nonZeroCount - 2));
    df.reserve(requiredSize);
    beatPeriod.reserve(requiredSize);

    // Skip the first 2 results, as they might have detected noise as an
    // onset; that is how VAMP does it and it seems to work best.
    for (int i = 2; i < nonZeroCount; ++i) {
        df.push_back(m_detectionResults.at(static_cast<std::size_t>(i)));
        beatPeriod.push_back(0.0);
    }

    TempoTrackV2 tt(static_cast<float>(m_sampleRate), m_stepSizeFrames);
    tt.calculateBeatPeriod(df, beatPeriod, tempi);

    std::vector<double> beats;
    tt.calculateBeats(df, beatPeriod, beats);

    m_resultBeats.reserve(beats.size());
    for (const double beat : beats) {
        // Half a step is added because the beat is detected between the
        // two samples. Mixxx halves the step in integer arithmetic.
        m_resultBeats.push_back((beat * m_stepSizeFrames) + m_stepSizeFrames / 2);
    }

    m_pDetectionFunction.reset();
    return true;
}

// --- AnalyzerQueenMaryKey ---

namespace
{

// Tuning frequency of concert A in Hertz, the VAMP plugin's default.
constexpr int kTuningFrequencyHertz = 440;

}  // namespace

QueenMaryKey::QueenMaryKey() = default;
QueenMaryKey::~QueenMaryKey() = default;

bool QueenMaryKey::initialize(int sampleRate)
{
    m_prevKey = 0;
    m_resultKeys.clear();
    m_currentFrame = 0;
    m_failed = false;

    // Mixxx passes its SampleRate, converted to a double.
    GetKeyMode::Config config(static_cast<double>(static_cast<std::uint32_t>(sampleRate)), kTuningFrequencyHertz);
    m_pKeyMode = std::make_unique<GetKeyMode>(config);
    const auto windowSize = static_cast<std::size_t>(m_pKeyMode->getBlockSize());
    const auto stepSize = static_cast<std::size_t>(m_pKeyMode->getHopSize());

    return m_helper.initialize(windowSize, stepSize, [this](double *window, std::size_t) {
        const int key = m_pKeyMode->process(window);
        // Mixxx's ChromaticKey_IsValid: 0 (none) to 24.
        if (key < 0 || key > 24) {
            m_failed = true;
            return false;
        }
        if (key != m_prevKey) {
            m_resultKeys.emplace_back(key, m_currentFrame);
            m_prevKey = key;
        }
        return true;
    });
}

bool QueenMaryKey::processSamples(const float *mono, std::size_t frames)
{
    if (!m_pKeyMode) {
        return false;
    }
    // Counted before the chunk is processed, as in Mixxx: a change found
    // in this chunk is stamped with its end.
    m_currentFrame += static_cast<double>(frames);
    return m_helper.processMonoSamples(mono, frames);
}

bool QueenMaryKey::finalize()
{
    m_helper.finalize();
    m_pKeyMode.reset();
    return !m_failed;
}

int calculateGlobalKey(const std::vector<std::pair<int, double>> &keyChanges, double totalFrames)
{
    if (keyChanges.size() == 1) {
        return keyChanges[0].first;
    }
    // Mixxx's QMap is ordered by key number, and only a strictly larger
    // share replaces the best: on a tie the lowest key number wins.
    std::map<int, double> histogram;
    for (std::size_t i = 0; i < keyChanges.size(); ++i) {
        const int key = keyChanges[i].first;
        const double startFrame = keyChanges[i].second;
        const double nextFrame = (i == keyChanges.size() - 1) ? totalFrames : keyChanges[i + 1].second;
        histogram[key] += (nextFrame - startFrame);
    }
    double maxDelta = 0;
    int maxKey = 0;
    for (const auto &[key, frames] : histogram) {
        if (frames > maxDelta) {
            maxKey = key;
            maxDelta = frames;
        }
    }
    return maxKey;
}

}  // namespace seabass::baseline::mixxx
