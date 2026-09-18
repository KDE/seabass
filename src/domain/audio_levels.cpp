// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/audio_levels.hpp"

#include <algorithm>
#include <cmath>

namespace seabass::domain
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
// A level falls back this fast once the sound that raised it is gone. It
// rises at once: a kick that is eased in is a kick that is missed.
constexpr double kReleaseSeconds = 0.14;
// What the beat has to stand out from is the last second or two of bass.
constexpr double kAverageSeconds = 1.5;
// Two beats cannot be closer than this: 0.18 s is 333 BPM.
constexpr double kBeatHoldSeconds = 0.18;
// Levels are measured against the track's own recent loudness, not
// against full scale. Fixed gains were tried first, on real tracks:
// mastered techno sat pinned at 1.0 a third of the time, and a display
// that is always at full has stopped moving. The reference is the
// loudest the whole signal has been lately, forgotten over these seconds.
constexpr double kReferenceSeconds = 6.0;
// Below this the reference stops following: near-silence is not to be
// amplified into a light show.
constexpr double kReferenceFloor = 0.02;
// A band's share of the whole differs -- most of a dance track's energy
// is bass, little of it is hi-hats. These bring each band's own peaks to
// about 1.
constexpr double kLowGain = 1.2;
constexpr double kMidGain = 1.8;
constexpr double kHighGain = 5.0;


float sampleAt(const void *data, std::size_t index, SampleFormat format)
{
    switch (format) {
    case SampleFormat::UInt8:
        return (static_cast<const std::uint8_t *>(data)[index] - 128) / 128.0f;
    case SampleFormat::Int16:
        return static_cast<const std::int16_t *>(data)[index] / 32768.0f;
    case SampleFormat::Int32:
        return static_cast<float>(static_cast<const std::int32_t *>(data)[index] / 2147483648.0);
    case SampleFormat::Float:
        return static_cast<const float *>(data)[index];
    }
    return 0.0f;
}
}  // namespace

AudioLevels AudioLevelMeter::feed(const void *data, std::size_t frames, int channels, SampleFormat format,
                                  int sampleRate)
{
    if (data == nullptr || channels <= 0) {
        return feed(nullptr, 0, sampleRate);
    }
    m_mono.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            sum += sampleAt(data, frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel), format);
        }
        m_mono[frame] = sum / static_cast<float>(channels);
    }
    return feed(m_mono.data(), frames, sampleRate);
}

AudioLevels AudioLevelMeter::feed(const float *mono, std::size_t frames, int sampleRate)
{
    m_levels.beat = false;
    if (mono == nullptr || frames == 0 || sampleRate <= 0) {
        return m_levels;
    }

    if (sampleRate != m_sampleRate) {
        designFilters(sampleRate);
    }
    double lowSquares = 0.0;
    double midSquares = 0.0;
    double highSquares = 0.0;
    double allSquares = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double x = mono[i];
        allSquares += x * x;
        const double low = m_low.step(x);
        const double mid = m_midLowPass.step(m_midHighPass.step(x));
        const double high = m_high.step(x);
        lowSquares += low * low;
        midSquares += mid * mid;
        highSquares += high * high;
    }
    const double count = static_cast<double>(frames);
    const double blockSeconds = count / static_cast<double>(sampleRate);
    const double all = std::sqrt(allSquares / count);
    m_reference = std::max(all, m_reference * std::exp(-blockSeconds / kReferenceSeconds));
    const double reference = std::max(m_reference, kReferenceFloor);

    const double lowUnclamped = std::sqrt(lowSquares / count) / reference * kLowGain;
    const double low = std::min(1.0, lowUnclamped);
    const double mid = std::min(1.0, std::sqrt(midSquares / count) / reference * kMidGain);
    const double high = std::min(1.0, std::sqrt(highSquares / count) / reference * kHighGain);

    const double release = std::exp(-blockSeconds / kReleaseSeconds);
    m_levels.low = std::max(low, m_levels.low * release);
    m_levels.mid = std::max(mid, m_levels.mid * release);
    m_levels.high = std::max(high, m_levels.high * release);

    // A beat is the low band RISING well clear of its own recent average.
    // Standing clear is not enough: a held bass note stands clear of the
    // quiet before it for a second and more, and was a beat in every
    // block of that. Compared BEFORE this block joins the average, or a
    // kick would raise the bar it has to clear; and unclamped, or a loud
    // track pinned at 1.0 would never rise.
    m_sinceBeatSeconds += blockSeconds;
    const bool rising = lowUnclamped > m_previousLow * 1.25 + 0.04;
    if (rising && lowUnclamped > 0.12 && lowUnclamped > m_lowAverage * 1.4
        && m_sinceBeatSeconds >= kBeatHoldSeconds) {
        m_levels.beat = true;
        m_sinceBeatSeconds = 0.0;
    }
    m_previousLow = lowUnclamped;
    const double averaging = 1.0 - std::exp(-blockSeconds / kAverageSeconds);
    m_lowAverage += averaging * (lowUnclamped - m_lowAverage);

    return m_levels;
}

// Butterworth low- and high-pass sections, from the RBJ audio EQ cookbook.
void AudioLevelMeter::designFilters(int sampleRate)
{
    const auto design = [sampleRate](double cutoffHz, bool highPass) {
        const double w0 = 2.0 * kPi * cutoffHz / static_cast<double>(sampleRate);
        const double cosW0 = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        Biquad f;
        if (highPass) {
            f.b0 = (1.0 + cosW0) / 2.0 / a0;
            f.b1 = -(1.0 + cosW0) / a0;
        } else {
            f.b0 = (1.0 - cosW0) / 2.0 / a0;
            f.b1 = (1.0 - cosW0) / a0;
        }
        f.b2 = f.b0;
        f.a1 = -2.0 * cosW0 / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    };
    m_low = design(160.0, false);
    m_midHighPass = design(160.0, true);
    m_midLowPass = design(2500.0, false);
    m_high = design(2500.0, true);
    m_sampleRate = sampleRate;
}

void AudioLevelMeter::reset()
{
    *this = AudioLevelMeter();
}

}  // namespace seabass::domain
