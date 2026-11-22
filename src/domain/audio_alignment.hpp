// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <complex>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace seabass::domain::alignment
{

// The arithmetic behind AudioAlignmentProbe, with no decoder in it: what
// a decoder hands over is a run of samples, and everything after that is
// here, where it can be tested against signals made in the test itself.
// The method is the one validated on a real stick (see
// audio_alignment_probe.hpp): cross-correlate three windows, start,
// middle and end, and believe the answer only when all three agree.

// Every file is brought to this rate before comparing. 22050 Hz keeps
// everything up to 11 kHz, far more than a cross-correlation of music
// needs, and a 40 s window is then 882000 samples.
inline constexpr double AnalysisRate = 22050.0;
inline constexpr double WindowSeconds = 40.0;
// A window's normalised correlation at its peak must reach this. Two
// encodes of one master correlate well above 0.9; a different edit or
// another recording stays far under it.
inline constexpr double MinNormalisedPeak = 0.5;
// The three windows' shifts must agree to within this. A copy with a
// section cut out of the middle agrees at the start and not at the end,
// and no one shift is then right for every cue.
inline constexpr double MaxDisagreementMs = 2.0;
// Less overlap than this between two windows at a lag is too little
// music to call a match on.
inline constexpr double MinOverlapSeconds = 1.0;
// Music built from a repeated bar or beat matches itself one bar or one
// beat away almost as well as at the true shift. A window whose best lag
// has a rival further away than RivalSeparationMs reaching RivalShare of
// its coefficient cannot tell the two apart, and says nothing.
inline constexpr double RivalSeparationMs = 5.0;
inline constexpr double RivalShare = 0.9;
// How far apart two copies are looked for: the length window in which
// audio is compared at all (MatchingPolicy::compareAudioSeconds()) plus
// a margin, never less than MinShiftSearchSeconds. Measured shifts on a
// real stick reached 1.3 s. A copy further out than this is not lined up.
inline constexpr double ShiftSearchMarginSeconds = 2.0;
inline constexpr double MinShiftSearchSeconds = 5.0;
double shiftSearchSeconds(double compareAudioSeconds);

// In-place radix-2 FFT. `data.size()` must be a power of two (and not
// 0). `inverse` runs the inverse transform, scaled by 1/N, so a forward
// and an inverse transform give the input back.
void fft(std::vector<std::complex<double>> &data, bool inverse);

// Brings a mono stream from one sample rate to another without moving
// anything in time.
//
// Output sample n stands for the instant n / outRate exactly: it is the
// average of the input over the interval of one output sample centred
// there, each input sample taken to cover the interval of one input
// sample centred on its own instant, with the parts of a sample that
// fall into two output intervals split by how much falls into each. At
// an integer ratio (44.1 kHz to 22.05 kHz) this is plain averaging of
// neighbours; at any other (48 kHz) the same rule still keeps the timing
// exact, which is the property a shift measured in output samples rests
// on. A box filter is a poor anti-alias filter, and does not need to be
// a good one: both files go through the same one, and aliasing smears
// a correlation peak without moving it.
class Resampler
{
public:
    Resampler(double inRate, double outRate);

    void push(float sample);
    // Everything still pending, and the samples so far. Call once.
    std::vector<float> finish();

private:
    double m_ratio;  // output samples per input sample
    long long m_inputIndex = 0;
    long long m_base = 0;  // the output sample m_sum.front() is for
    std::deque<double> m_sum;
    std::deque<double> m_weight;
    std::vector<float> m_out;
};

// The lag at which `b` best matches `a`, in samples, and how well.
// lag = index of a sound in b minus its index in a.
//
// The best lag is the one with the highest correlation COEFFICIENT over
// the overlap (1 for identical shapes, 0 for nothing in common), not the
// highest raw sum: the raw sum grows with the overlap, so in music made
// of a repeated bar it favoured the bar nearer lag 0 over the true one.
struct CorrelationPeak
{
    long long lag = 0;
    double normalised = 0.0;
    // Another local peak more than `rivalSeparation` samples away reached
    // RivalShare of this one, or the best lag lies on the edge of the
    // lags searched (the true one may be past it). Either way the lag is
    // not known.
    bool ambiguous = false;
};

// The lags searched, both ends included.
struct LagRange
{
    long long lowest = 0;
    long long highest = 0;
};

// Only lags inside `range` (every lag when there is none) at which the
// two overlap by at least `minOverlap` samples are considered. nullopt
// when there is no such lag, or when either side is silent over the
// overlap at the peak (nothing to correlate).
std::optional<CorrelationPeak> correlationPeak(std::span<const float> a, std::span<const float> b,
                                               size_t minOverlap, std::optional<LagRange> range = std::nullopt,
                                               size_t rivalSeparation = 1);

// One window's answer, for the rule below and for a test to look at.
struct WindowShift
{
    double shiftMs = 0.0;
    double normalised = 0.0;
    bool ambiguous = false;
};

// The rule: no window ambiguous, every window's peak at least
// MinNormalisedPeak and the shifts within MaxDisagreementMs of each
// other. The median of the three when it holds, nullopt (does not line
// up) when it does not. An empty list is nullopt too.
std::optional<double> cleanShiftMs(const std::vector<WindowShift> &windows);

// The three windows of two decoded files, at `rate`: the first
// WindowSeconds of each, the WindowSeconds around half of the shorter
// file in both, and the last WindowSeconds of each. Each window's lag is
// turned into a shift from the start of the files (an end window's lag
// counts from where each file's window begins), and only shifts within
// `maxShiftSeconds` either way are searched. Files shorter than a window
// use what they have. nullopt when a window cannot be measured.
std::optional<std::vector<WindowShift>> windowShifts(std::span<const float> a, std::span<const float> b,
                                                     double rate, double maxShiftSeconds);

// windowShifts() and then cleanShiftMs(): where a sound sits in `b`
// minus where it sits in `a`, in ms, or nullopt when the two do not line
// up cleanly.
std::optional<double> measureShiftMs(std::span<const float> a, std::span<const float> b, double rate,
                                     double maxShiftSeconds);

}  // namespace seabass::domain::alignment
