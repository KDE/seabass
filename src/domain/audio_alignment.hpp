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
// 0.95: a loop with some variation in it scored its true lag 1.000 and
// its best rival 0.93 (tests/fixtures/alignment, real-620), one with 1%
// variation 0.99 (near-eq-1300).
inline constexpr double RivalSeparationMs = 5.0;
inline constexpr double RivalShare = 0.95;
// A file too short for three windows of its own gives the same samples
// to all three, and "the windows agree" then says nothing: a 4.5 s file
// with 400 ms cut out lined up "cleanly" at 0.71. For such a pair the
// overlap at the shift found is checked in parts of PartSeconds, and
// every part must correlate at PartShare of the window's coefficient. A
// cut anywhere leaves the part it falls in matching at another lag. Strict
// on purpose: for a short file, unknown is always an acceptable answer.
inline constexpr double PartSeconds = 0.5;
inline constexpr double PartShare = 0.9;
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
    // The samples complete so far, handed over and no longer held here,
    // so a long file need not be held whole while it is decoded.
    std::vector<float> drain();
    // Everything still pending, and the samples not drained yet. Call
    // once.
    std::vector<float> finish();

private:
    double m_ratio;  // output samples per input sample
    long long m_inputIndex = 0;
    long long m_base = 0;  // the output sample m_sum.front() is for
    std::deque<double> m_sum;
    std::deque<double> m_weight;
    std::vector<float> m_out;
};

// What is kept of one decoded file: only the stretches the three windows
// below can need, so a two hour mix is never held whole. `total` is the
// whole file's length in samples; `pieces` are in order and do not
// overlap, each held at exactly its size.
struct KeptSamples
{
    struct Piece
    {
        size_t start = 0;
        std::vector<float> samples;
    };
    size_t total = 0;
    std::vector<Piece> pieces;

    // Samples [start, start + length), when one piece holds them all.
    std::optional<std::span<const float>> slice(size_t start, size_t length) const;
    size_t bytes() const;
};

// The middle window sits at half of the SHORTER file in both, so in the
// longer one it is up to half the difference in length from its own
// middle. The middle kept reaches this far beyond the window on both
// sides: copies whose lengths differ by up to twice this can be lined
// up. Further apart than that, a pair is not measured (alignmentCovers()).
inline constexpr double MiddleReachSeconds = 90.0;

// Collects a decoded file at `rate` as it arrives, letting go of what no
// window can need as soon as it is sure of it: the middle of the file is
// at least half of what has arrived, so anything before that, less the
// reach, and after the first window, is let go. What is held at the
// most is about half the file; what is kept at the end is the first
// window, the middle and its reach, and the last window.
class KeptSamplesBuilder
{
public:
    explicit KeptSamplesBuilder(double rate);
    void append(std::span<const float> samples);
    KeptSamples finish();
    // The most samples held at once so far, for the tests.
    size_t mostHeld() const { return m_mostHeld; }

private:
    struct Block
    {
        size_t start = 0;
        std::vector<float> samples;
    };
    void letGo();
    size_t m_window;
    size_t m_reach;
    size_t m_total = 0;
    size_t m_held = 0;
    size_t m_mostHeld = 0;
    std::deque<Block> m_blocks;  // in order; a gap where samples were let go
};

// A whole decoded signal as KeptSamples, for a caller that has it whole.
KeptSamples keepAll(std::vector<float> samples);

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

// Every lag at which the two overlap by at least `minOverlap` samples is
// looked at; a best lag outside `range` (when given) is ambiguous, not an
// answer, so a peak outside the range still counts against an alias of it
// inside. nullopt when there is no such lag, or when either side is silent
// over the overlap at the peak (nothing to correlate).
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

// The same over what was kept of two files. Only meaningful when
// alignmentCovers() holds: a window that was not kept is no answer here.
std::optional<std::vector<WindowShift>> windowShifts(const KeptSamples &a, const KeptSamples &b, double rate,
                                                     double maxShiftSeconds);
std::optional<double> measureShiftMs(const KeptSamples &a, const KeptSamples &b, double rate,
                                     double maxShiftSeconds);

// True when what was kept of each file holds every window the pair needs.
// False for copies further apart in length than MiddleReachSeconds
// allows: that is not "does not line up", it is "not measured".
bool alignmentCovers(const KeptSamples &a, const KeptSamples &b, double rate);

}  // namespace seabass::domain::alignment
