// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "musicanalyze/grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <initializer_list>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace musicanalyze
{

namespace
{

// ---------------------------------------------------------------- filters

struct Biquad
{
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// The two sections of a fourth-order Butterworth filter have these Qs; a
// second-order one has 1/sqrt(2).
constexpr std::array<double, 2> ButterworthFourthQ{0.54119610014619698, 1.3065629648763766};

Biquad lowpass(double hz, double rate, double q)
{
    const double w = 2.0 * std::numbers::pi * hz / rate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double c = std::cos(w);
    const double a0 = 1.0 + alpha;
    return {(1.0 - c) / 2.0 / a0, (1.0 - c) / a0, (1.0 - c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0};
}

Biquad highpass(double hz, double rate, double q)
{
    const double w = 2.0 * std::numbers::pi * hz / rate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double c = std::cos(w);
    const double a0 = 1.0 + alpha;
    return {(1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0};
}

// A sample through the filters one after the other, each with its own
// two values of memory.
template <std::size_t Count>
double step(const std::array<Biquad, Count> &filters, std::array<double, Count> &z1, std::array<double, Count> &z2,
            double value)
{
    for (std::size_t f = 0; f < Count; ++f) {
        const double out = filters[f].b0 * value + z1[f];
        z1[f] = filters[f].b1 * value - filters[f].a1 * out + z2[f];
        z2[f] = filters[f].b2 * value - filters[f].a2 * out;
        value = out;
    }
    return value;
}

// Forwards and then backwards, so what the filters delay one way they
// advance the other: an onset stays where it is in time. All of them one
// way and then all of them the other, which is the same as each both
// ways in turn.
//
// The track is gone over once each way, every sample through all the
// filters before the next: the same sums in the same order as a pass a
// filter (on 280 tracks every beat came out the same), and much the
// faster: a pass a filter reads and writes the whole track for each and
// waits for one filter's memory, where the filters of a band are four to
// work on at once. The filters are the largest part of what an analysis
// costs; measured with a pass a filter, an analysis took twice as long.
template <std::size_t Count>
void runBothWays(const std::array<Biquad, Count> &filters, std::vector<double> &x)
{
    std::array<double, Count> z1{};
    std::array<double, Count> z2{};
    for (double &sample : x) {
        sample = step(filters, z1, z2, sample);
    }
    z1.fill(0.0);
    z2.fill(0.0);
    for (std::size_t i = x.size(); i-- > 0;) {
        x[i] = step(filters, z1, z2, x[i]);
    }
}

// -------------------------------------------------------- small arithmetic

// The place of a peak near index i, to a fraction of a step: the top of
// the parabola through the value there and its two neighbours. Where the
// value at i is not the highest of the three (the highest of a search
// that ended on a slope), there is no top between them and i itself is
// the answer: the parabola's would be anywhere, steps away or before 0.
double parabolaPeak(const std::vector<double> &values, std::size_t i)
{
    if (i == 0 || i + 1 >= values.size()) {
        return static_cast<double>(i);
    }
    const double a = values[i - 1];
    const double b = values[i];
    const double c = values[i + 1];
    const double d = a - 2.0 * b + c;
    if (!(b >= a && b >= c) || !(d < 0.0)) {
        return static_cast<double>(i);
    }
    return static_cast<double>(i) + std::clamp(0.5 * (a - c) / d, -0.5, 0.5);
}

// The value `share` of the way through the sorted values, between two of
// them where it falls between.
double quantile(std::vector<double> values, double share)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double at = share * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(at);
    const std::size_t upper = std::min(lower + 1, values.size() - 1);
    return values[lower] + (at - static_cast<double>(lower)) * (values[upper] - values[lower]);
}

void fft(std::vector<std::complex<double>> &data, bool inverse)
{
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    const double sign = inverse ? 1.0 : -1.0;
    // One table for the largest stage, read with a stride by the smaller
    // ones: a twiddle built by repeated multiplication drifts. Kept from
    // call to call, one each way: the tempo curve transforms the same
    // length twice for every two seconds of a track.
    thread_local std::array<std::vector<std::complex<double>>, 2> tables;
    std::vector<std::complex<double>> &twiddle = tables[inverse ? 1 : 0];
    if (twiddle.size() != n / 2) {
        twiddle.resize(n / 2);
        for (std::size_t k = 0; k < n / 2; ++k) {
            twiddle[k] = std::polar(1.0, sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n));
        }
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const std::size_t half = length / 2;
        const std::size_t stride = n / length;
        for (std::size_t start = 0; start < n; start += length) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<double> t = twiddle[k * stride] * data[start + k + half];
                data[start + k + half] = data[start + k] - t;
                data[start + k] += t;
            }
        }
    }
    if (inverse) {
        for (std::complex<double> &value : data) {
            value /= static_cast<double>(n);
        }
    }
}

// How much x, less its mean, repeats itself at each lag from 0 to
// `longest` steps.
std::vector<double> autocorrelation(const std::vector<double> &x, std::size_t longest)
{
    std::size_t n = 1;
    while (n < x.size() + longest) {
        n <<= 1;
    }
    double mean = 0.0;
    for (const double value : x) {
        mean += value;
    }
    mean /= static_cast<double>(x.size());
    std::vector<std::complex<double>> z(n);
    for (std::size_t i = 0; i < x.size(); ++i) {
        z[i] = x[i] - mean;
    }
    fft(z, false);
    for (std::complex<double> &value : z) {
        value = std::norm(value);
    }
    fft(z, true);
    std::vector<double> result(longest + 1);
    for (std::size_t lag = 0; lag <= longest; ++lag) {
        result[lag] = z[lag].real();
    }
    return result;
}

// ------------------------------------------------------ the onset function

// The bands whose envelopes' rises make the onset function, in Hz. Up to
// 5 kHz and no further: above it are the hats, between the beats.
constexpr std::array<std::array<double, 2>, 5> Bands{{{30.0, 70.0}, {70.0, 150.0}, {150.0, 400.0}, {400.0, 1500.0},
                                                      {1500.0, 5000.0}}};
// The first band is for the tempo and not for the place of the beat.
// Under 70 Hz is the bass line as much as the kick, and in house and
// trance the bass plays between the beats: with that band heard for the
// place of the beat, six tracks more of the tuning half of the
// collection came out half a beat from rekordbox (39 for 33). Without it
// for the tempo, five tracks more of 110 of varied music came out at
// another tempo (34 for 29): where there is no kick drum on every beat,
// the bass is what repeats.
constexpr std::size_t FirstBandForTheBeat = 1;
// The envelope is the band's power smoothed at this frequency: slow
// enough to be an envelope, fast enough that a hit's rise takes a few
// milliseconds of it and not twenty.
constexpr double EnvelopeSmoothingHz = 60.0;
// Up to here a band's power is not its square. The square of a tone at f
// ripples at 2f; from 400 Hz up that is far above the smoothing and gone,
// below it the smoothing lets it through, and a held bass note (or the
// hum under a recording) rises a hundred times a second: onsets that are
// none, in the bands where the kick is, and through what leaks into the
// band next door past any test of whether a band has onsets. So the low
// bands take the power of the analytic signal, which has no ripple.
constexpr double PowerBySquareFromHz = 400.0;

// The power of a band that has nothing above 400 Hz, without the ripple
// of its square: the squared size of its analytic signal (the band, and
// the band a quarter of a cycle on). `step` is filled with how many
// samples of the band one value stands for: the band is thinned to about
// 1400 values a second first, which holds everything under 700 Hz, and
// transformed in pieces of 16384 that overlap by half, the middle half
// of each kept, since the ends of a piece are where the transform has to
// make something up.
std::vector<double> rippleFreePower(const std::vector<double> &band, double rate, std::size_t &step)
{
    step = std::max<std::size_t>(1, static_cast<std::size_t>(rate / 1400.0));
    const std::size_t count = (band.size() + step - 1) / step;
    constexpr std::size_t Piece = 16384;
    std::vector<double> power(count, 0.0);
    std::vector<std::complex<double>> z(Piece);
    for (std::size_t start = 0; start < count; start += Piece / 2) {
        const std::size_t length = std::min(Piece, count - start);
        std::fill(z.begin(), z.end(), std::complex<double>(0.0, 0.0));
        for (std::size_t i = 0; i < length; ++i) {
            z[i] = band[(start + i) * step];
        }
        fft(z, false);
        // The analytic signal: the upper half of the spectrum doubled,
        // the lower half (the mirror) dropped.
        for (std::size_t k = 1; k < Piece / 2; ++k) {
            z[k] *= 2.0;
        }
        for (std::size_t k = Piece / 2 + 1; k < Piece; ++k) {
            z[k] = 0.0;
        }
        fft(z, true);
        const std::size_t from = start == 0 ? 0 : Piece / 4;
        const std::size_t to = start + Piece >= count ? length : 3 * Piece / 4;
        for (std::size_t i = from; i < to; ++i) {
            power[start + i] = std::norm(z[i]);
        }
        if (start + Piece >= count) {
            break;
        }
    }
    return power;
}

// The onset function twice over: every band, which is what the tempo is
// read from, and the bands from 70 Hz, which is what says where in the
// beat the beat is.
struct Onsets
{
    std::vector<double> forTempo;
    std::vector<double> forBeat;
};

// One value a millisecond: value i is at i ms on the source's timeline.
// Reads the samples once, into a working copy of its own; whoever holds
// them can let go of them as soon as this returns.
Onsets onsetFunction(std::span<const float> samples, int sampleRate)
{
    // A sample that is no number (a decoder's slip) is silence: left in,
    // the filters would spread it over the whole track.
    std::vector<double> audio(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        audio[i] = std::isfinite(samples[i]) ? static_cast<double>(samples[i]) : 0.0;
    }
    // Half the rate is enough for 5 kHz and halves the work: every other
    // sample of the audio, low-passed both ways first so nothing folds
    // down and nothing moves in time.
    double rate = static_cast<double>(sampleRate);
    if (rate >= 40000.0) {
        runBothWays(std::array{lowpass(0.2 * rate, rate, ButterworthFourthQ[0]),
                               lowpass(0.2 * rate, rate, ButterworthFourthQ[1])},
                    audio);
        const std::size_t half = (audio.size() + 1) / 2;
        for (std::size_t i = 0; i < half; ++i) {
            audio[i] = audio[2 * i];
        }
        audio.resize(half);
        audio.shrink_to_fit();
        rate /= 2.0;
    }
    if (audio.size() < 2) {
        return Onsets{};
    }

    const auto milliseconds = static_cast<std::size_t>(static_cast<double>(audio.size() - 1) * 1000.0 / rate);
    std::vector<double> band;
    std::vector<double> envelope(milliseconds);
    // Per band: its rises, a value a millisecond, what they add up to, and
    // the band's mean level.
    std::array<std::vector<double>, Bands.size()> rises;
    std::array<double, Bands.size()> risen{};
    std::array<double, Bands.size()> levels{};
    for (std::size_t b = 0; b < Bands.size(); ++b) {
        const auto &edges = Bands[b];
        band = audio;
        const double top = std::min(edges[1], 0.45 * rate);
        runBothWays(std::array{highpass(edges[0], rate, ButterworthFourthQ[0]), highpass(edges[0], rate, ButterworthFourthQ[1]),
                               lowpass(top, rate, ButterworthFourthQ[0]), lowpass(top, rate, ButterworthFourthQ[1])},
                    band);
        // The band's power, at `powerRate` values a second.
        double powerRate = rate;
        if (edges[1] <= PowerBySquareFromHz && rate >= 4.0 * PowerBySquareFromHz) {
            std::size_t step = 1;
            band = rippleFreePower(band, rate, step);
            powerRate = rate / static_cast<double>(step);
        } else {
            for (double &value : band) {
                value *= value;
            }
        }
        runBothWays(std::array{lowpass(EnvelopeSmoothingHz, powerRate, std::numbers::sqrt2 / 2.0)}, band);
        for (std::size_t ms = 0; ms < milliseconds; ++ms) {
            const double at = static_cast<double>(ms) * powerRate / 1000.0;
            const auto below = std::min(static_cast<std::size_t>(at), band.size() - 1);
            const std::size_t above = std::min(below + 1, band.size() - 1);
            const double part = at - static_cast<double>(below);
            const double power = band[below] * (1.0 - part) + band[above] * part;
            envelope[ms] = std::sqrt(std::max(power, 0.0));
        }
        rises[b].assign(milliseconds, 0.0);
        for (std::size_t ms = 1; ms < milliseconds; ++ms) {
            rises[b][ms] = std::max(envelope[ms] - envelope[ms - 1], 0.0);
            risen[b] += rises[b][ms];
            levels[b] += envelope[ms];
        }
        levels[b] /= static_cast<double>(milliseconds);
    }

    // The bands are summed, each counted alike whatever its level: scaled
    // so that its rises have a mean of 1. Two kinds of band are left out,
    // because scaled up like that they would be taken for a rhythm:
    //
    //  - a band with next to nothing in it, a hundredth of the loudest
    //    band's level: what leaks into it from a tone next door, or the
    //    noise floor of 16 bits. On 250 tracks of the collection the
    //    quietest band of any track is at 7 hundredths of the loudest.
    //  - a band that holds a level and hardly rises: a tone, a pad, hum.
    //    Drums rise by many times the band's mean level every second (8
    //    times at the least on those tracks, 15 to 27 on the middle one),
    //    a held note by next to nothing.
    constexpr double ThereAtAll = 0.01;
    constexpr double RisesASecondOfTheLevel = 1.0;
    const double loudest = *std::max_element(levels.begin(), levels.end());
    Onsets onsets;
    onsets.forTempo.assign(milliseconds, 0.0);
    onsets.forBeat.assign(milliseconds, 0.0);
    const double seconds = static_cast<double>(milliseconds) / 1000.0;
    for (std::size_t b = 0; b < Bands.size(); ++b) {
        if (!(levels[b] > ThereAtAll * loudest) || !(risen[b] > RisesASecondOfTheLevel * levels[b] * seconds)) {
            continue;
        }
        const double scale = static_cast<double>(milliseconds) / risen[b];
        for (std::size_t ms = 1; ms < milliseconds; ++ms) {
            onsets.forTempo[ms] += scale * rises[b][ms];
            if (b >= FirstBandForTheBeat) {
                onsets.forBeat[ms] += scale * rises[b][ms];
            }
        }
    }
    return onsets;
}

// ------------------------------------------------------------- the limits

// The lags at which a rhythm is looked for, in ms: 200 BPM to 60 BPM.
// That finds a level of the rhythm; which of its relatives is the beat is
// chosen after, inside the tempo range.
constexpr std::size_t ShortestPeriodMs = 300;
constexpr std::size_t LongestPeriodMs = 1000;

// A tempo range as beat periods in ms.
struct Periods
{
    double shortest = 0.0;
    double longest = 0.0;
    // Whether the range has room for a tempo and its double. Where it
    // has, the relative nearer 128 BPM is preferred; in a range of one
    // octave nothing is, or drum and bass at 174 in a range of 88 to 175
    // would stand behind two thirds of itself for being further from 128.
    //
    // 128 and not the middle of the range: with the preference around
    // the middle of 70 to 180 (112 BPM), 29 tracks of the tuning half of
    // the collection came out at another tempo than rekordbox's for 7,
    // and 40 of 110 of varied music for 29. Between 75 and 150 rekordbox
    // takes 150 more often than not.
    bool prefersNear128 = false;
};

Periods periodsOf(const TempoRange &range)
{
    // Half a percent of room at both ends: a track at 70 BPM measures
    // 70.00 or 69.99, and is at 70 in a range from 70.
    constexpr double Room = 0.005;
    constexpr double MoreThanAnOctave = 2.05;
    return {60000.0 / range.highBpm * (1.0 - Room), 60000.0 / range.lowBpm * (1.0 + Room),
            range.highBpm > MoreThanAnOctave * range.lowBpm};
}

// ------------------------------------------------------------------- folds

// The onset function over `beats` beats from `startMs`, summed onto one
// beat: about one step a millisecond. `used` is how many of the beats lay
// inside the track.
std::vector<double> fold(const std::vector<double> &onsets, double startMs, double period, std::size_t beats,
                         std::size_t &used)
{
    used = 0;
    // A period that is no period (not a number, or outside any tempo)
    // folds to nothing: one flat step.
    if (!(period >= 100.0 && period <= 10000.0) || onsets.size() < 2) {
        return std::vector<double>(1, 0.0);
    }
    const auto steps = static_cast<std::size_t>(std::llround(period));
    const double step = period / static_cast<double>(steps);
    std::vector<double> total(steps, 0.0);
    for (std::size_t k = 0; k < beats; ++k) {
        const double from = startMs + static_cast<double>(k) * period;
        const double to = from + static_cast<double>(steps - 1) * step;
        if (from < 0.0 || to >= static_cast<double>(onsets.size() - 1)) {
            continue;
        }
        for (std::size_t s = 0; s < steps; ++s) {
            const double at = from + static_cast<double>(s) * step;
            const auto below = static_cast<std::size_t>(at);
            const double part = at - static_cast<double>(below);
            total[s] += onsets[below] * (1.0 - part) + onsets[below + 1] * part;
        }
        ++used;
    }
    if (used > 0) {
        for (double &value : total) {
            value /= static_cast<double>(used);
        }
    }
    return total;
}

// A fold smoothed around the beat (it has no ends: the step after the
// last is the first), with a raised-cosine window that reaches `reach`
// steps either way. The fine one (5 steps, 9 ms across where it is not
// zero) makes a peak one peak and not the ripple on it.
constexpr std::ptrdiff_t FineReach = 5;

std::vector<double> smoothed(const std::vector<double> &profile, std::ptrdiff_t reach = FineReach)
{
    const auto n = static_cast<std::ptrdiff_t>(profile.size());
    if (n == 0 || reach < 1) {
        return profile;
    }
    std::vector<double> window(static_cast<std::size_t>(2 * reach + 1));
    double sum = 0.0;
    for (std::ptrdiff_t k = 0; k <= 2 * reach; ++k) {
        window[static_cast<std::size_t>(k)] =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(2 * reach));
        sum += window[static_cast<std::size_t>(k)];
    }
    std::vector<double> result(profile.size(), 0.0);
    for (std::ptrdiff_t i = 0; i < n; ++i) {
        double value = 0.0;
        if (i >= reach && i + reach < n) {
            // Away from the ends nothing wraps round: the same sum, without
            // working out where each step lies. Folds are smoothed by the
            // thousand for every track.
            const double *around = profile.data() + (i - reach);
            for (std::ptrdiff_t k = 0; k <= 2 * reach; ++k) {
                value += window[static_cast<std::size_t>(k)] * around[k];
            }
        } else {
            for (std::ptrdiff_t k = -reach; k <= reach; ++k) {
                value += window[static_cast<std::size_t>(k + reach)] * profile[static_cast<std::size_t>(((i + k) % n + n) % n)];
            }
        }
        result[static_cast<std::size_t>(i)] = value / sum;
    }
    return result;
}

std::size_t indexOfMax(const std::vector<double> &values, std::size_t from, std::size_t to)
{
    std::size_t top = from;
    for (std::size_t i = from; i <= to; ++i) {
        if (values[i] > values[top]) {
            top = i;
        }
    }
    return top;
}

// Where a fold has the beat, in steps from its start (to a fraction).
//
// Not at its highest point: a short sharp hit between the beats (a rim
// shot, the first click of a clap) has all its bands rise in the same
// few milliseconds and folds into one tall spike, and a kick, whose
// bands rise one after the other over ten milliseconds and more, into a
// hump that is lower and holds more. The place is chosen where the fold
// has the most within 30 ms either way, and the beat is the peak of the
// finely smoothed fold inside that.
double heaviestPlace(const std::vector<double> &folded)
{
    const std::vector<double> fine = smoothed(folded);
    const auto steps = static_cast<std::ptrdiff_t>(fine.size());
    constexpr std::ptrdiff_t WeighedOverMs = 30;
    // Too few steps for a hump and a spike to be told apart: the highest.
    if (steps < 8 * WeighedOverMs) {
        return parabolaPeak(fine, indexOfMax(fine, 0, fine.size() - 1));
    }
    const std::vector<double> broad = smoothed(folded, WeighedOverMs);
    const auto centre = static_cast<std::ptrdiff_t>(indexOfMax(broad, 0, broad.size() - 1));
    std::ptrdiff_t top = centre;
    for (std::ptrdiff_t k = -WeighedOverMs; k <= WeighedOverMs; ++k) {
        const std::ptrdiff_t i = ((centre + k) % steps + steps) % steps;
        if (fine[static_cast<std::size_t>(i)] > fine[static_cast<std::size_t>(top)]) {
            top = i;
        }
    }
    return parabolaPeak(fine, static_cast<std::size_t>(top));
}

// Where in the beat the whole track's fold has the beat, in ms from 0.
double wholeTrackPhase(const std::vector<double> &onsets, double period)
{
    const auto beats = static_cast<std::size_t>(static_cast<double>(onsets.size() - 1) / period);
    std::size_t used = 0;
    const std::vector<double> folded = fold(onsets, 0.0, period, beats, used);
    return heaviestPlace(folded) * (period / static_cast<double>(folded.size()));
}

// ------------------------------------------------------------------ tempo

// How much a tempo is preferred for being a likely one: a bell around
// `centreBpm`, `octaves` wide. Between a tempo and its double the audio
// often cannot say; this can.
double preference(double periodMs, double centreBpm, double octaves)
{
    const double away = std::log2(60000.0 / periodMs / centreBpm) / octaves;
    return std::exp(-0.5 * away * away);
}

// The period made finer on the repeats further away: the repeat 4 beats
// off is at 4 periods, known to the same millisecond, which is the period
// four times finer. Then 16, then 64, while the function still repeats
// that far away (`floor`).
double sharpened(const std::vector<double> &ac, double period, double floor)
{
    for (const std::size_t beats : {std::size_t{4}, std::size_t{16}, std::size_t{64}}) {
        const double centre = period * static_cast<double>(beats);
        // A hundredth either way, and never as far as a quarter of a beat:
        // where sections of a track answer each other half a beat apart,
        // the function repeats 63.5 and 64.5 beats away as well as 64.
        const auto reach = std::max<std::size_t>(3, static_cast<std::size_t>(std::min(0.01 * centre, 0.25 * period)));
        const auto middle = static_cast<std::size_t>(centre);
        if (middle < reach + 1 || middle + reach + 1 >= ac.size()) {
            break;
        }
        const std::size_t top = indexOfMax(ac, middle - reach, middle + reach);
        if (ac[top] < floor) {
            break;
        }
        period = parabolaPeak(ac, top) / static_cast<double>(beats);
    }
    return period;
}

// How sharply the track folds at `period` when every 8 s of it may fold at
// a period of its own, up to 5 percent off: the mean of those folds'
// peaks. This is what the tempo's candidates are compared by, and not the
// fold of the whole track: a drummer whose tempo wanders by a few percent
// smears that at every period alike, and then the preference for 128 BPM
// decides alone. On 368 CD tracks of rock and pop the whole-track fold had
// 114 at another tempo than rekordbox's, most of them at four thirds or
// two thirds of it and every one of those on a track whose tempo wanders
// by over a percent; window by window it is 86.
//
// One half of each window says where the beat is and the other how much
// is there, and the other way round. The highest point of a fold of few
// beats is partly chance, more so the fewer the beats, which a plain
// window peak turns into a liking for slow tempos (it took seven of those
// 368 tracks from the right tempo to half of it); chance does not come
// back in the other half. Not all of it is gone: the best of the eleven
// periods tried is still a best of eleven.
double localPeak(const std::vector<double> &onsets, double period)
{
    // A track shorter than a window is one window, as long as it is.
    const std::size_t WindowMs = std::min<std::size_t>(8000, onsets.size() - 1);
    constexpr std::size_t HopMs = 4000;
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t start = 0; start + WindowMs < onsets.size(); start += HopMs) {
        double best = 0.0;
        for (int k = -5; k <= 5; ++k) {
            const double local = period * (1.0 + 0.01 * k);
            const auto beats = static_cast<std::size_t>(static_cast<double>(WindowMs) / local);
            const std::size_t first = beats / 2;
            if (first < 2) {
                continue;
            }
            std::size_t usedA = 0;
            std::size_t usedB = 0;
            const std::vector<double> a = smoothed(fold(onsets, static_cast<double>(start), local, first, usedA));
            const std::vector<double> b = smoothed(
                fold(onsets, static_cast<double>(start) + static_cast<double>(first) * local, local, beats - first, usedB));
            if (usedA == 0 || usedB == 0 || a.size() != b.size()) {
                continue;
            }
            best = std::max(best, 0.5 * (b[indexOfMax(a, 0, a.size() - 1)] + a[indexOfMax(b, 0, b.size() - 1)]));
        }
        sum += best;
        ++count;
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

// The beat period in ms, or nothing when the onset function repeats at no
// lag and none of its multiples.
std::optional<double> beatPeriod(const std::vector<double> &onsets, const Periods &allowed)
{
    constexpr std::size_t FarthestBeats = 64;
    const std::size_t longest = std::max(LongestPeriodMs, static_cast<std::size_t>(std::ceil(allowed.longest)));
    const std::vector<double> ac = autocorrelation(onsets, FarthestBeats * (longest + 100));
    // A lag is a period when the function repeats there and at twice,
    // three and four times it, the further ones counting less.
    constexpr std::array<double, 4> MultipleWeights{1.0, 0.5, 0.33, 0.25};
    double bestScore = 0.0;
    std::size_t best = 0;
    for (std::size_t lag = ShortestPeriodMs; lag <= LongestPeriodMs; ++lag) {
        double score = 0.0;
        for (std::size_t m = 0; m < MultipleWeights.size(); ++m) {
            score += MultipleWeights[m] * ac[lag * (m + 1)];
        }
        score *= preference(static_cast<double>(lag), 128.0, 0.45);
        if (best == 0 || score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    // The lag that scores best need not repeat itself: a kick alone at
    // 70 BPM scores best at half its period, by the multiples, with
    // nothing at that lag. What counts is the strongest of its repeats.
    double strongest = 0.0;
    for (std::size_t m = 0; best != 0 && m < MultipleWeights.size(); ++m) {
        strongest = std::max(strongest, ac[best * (m + 1)]);
    }
    if (best == 0 || !(bestScore > 0.0) || !(strongest > 0.0)) {
        return std::nullopt;
    }
    constexpr double StillRepeats = 0.15;
    const double floor = StillRepeats * strongest;
    const double base = parabolaPeak(ac, best);

    // That lag is a level of the rhythm, not always the beat: a dotted
    // rhythm at 95 BPM repeats well at three quarters of its beat, which
    // the scoring above takes for 127. So the lag's relatives are tried:
    // the track folded at each, window by window (localPeak above), and
    // the one that puts the most of the onsets into one moment of its
    // folds is the beat, a wide
    // preference around 128 BPM deciding between a period and its double
    // where the tempo range has room for both (in a range of one octave
    // it has not, and nothing is preferred). A relative outside the range
    // is tried as its double or half inside it.
    //
    // That choice is not fair to the faster tempo: where strong and weak
    // beats take turns (a kick, then a snare), the fold at the double
    // period holds only the strong ones and peaks higher, and drum and
    // bass comes out at 87. A score that counts the height for less the
    // longer the period was fitted to rekordbox's choices on the tuning
    // halves of two collections and did not hold: what it won on varied
    // music it lost on house and techno, where rekordbox itself takes the
    // slower tempo, and on the held-back half of the varied music it lost
    // two tracks more. Which of a tempo and its double rekordbox takes is
    // not in the fold.
    constexpr std::array<double, 7> Relatives{0.5, 2.0 / 3.0, 0.75, 1.0, 4.0 / 3.0, 1.5, 2.0};
    double chosen = base;
    double chosenScore = -1.0;
    double chosenPeak = 0.0;
    std::array<double, Relatives.size()> tried{};
    std::size_t triedCount = 0;
    // The repeat nearest a period, within a few milliseconds, made finer.
    constexpr std::size_t Near = 6;
    const auto repeatNear = [&ac, floor](double about) {
        const auto middle = static_cast<std::size_t>(std::llround(about));
        return sharpened(ac, parabolaPeak(ac, indexOfMax(ac, middle - Near, middle + Near)), floor);
    };
    for (const double relative : Relatives) {
        double about = base * relative;
        while (about > allowed.longest) {
            about /= 2.0;
        }
        while (about < allowed.shortest) {
            about *= 2.0;
        }
        if (about > allowed.longest) {
            continue;
        }
        // Half, whole and double come to the same period inside a range
        // of under two octaves: once is enough.
        if (std::any_of(tried.begin(), tried.begin() + static_cast<std::ptrdiff_t>(triedCount),
                        [about](double before) { return std::abs(before - about) < 0.5; })) {
            continue;
        }
        tried[triedCount++] = about;
        const double period = repeatNear(about);
        const double peak = localPeak(onsets, period);
        const double score = peak * (allowed.prefersNear128 ? preference(period, 128.0, 1.0) : 1.0);
        if (score > chosenScore) {
            chosenScore = score;
            chosen = period;
            chosenPeak = peak;
        }
    }

    // A tempo or its double, in doubt the double: more beat lines help
    // the DJ, a grid at half the tempo has none where the other beats are
    // (Sebastian, 2026-11-25). Where the double of the chosen tempo lies
    // inside the range, it is taken when the track folds there at least
    // three quarters as sharply. By the fold alone the slower tempo wins
    // too often (see above): on a playlist of 280 indie tracks it had half
    // of rekordbox's tempo on 18, and by ear rekordbox's was the right one
    // on 17 of them.
    //
    // The double is measured here and not looked up among the relatives
    // tried: of a relative at three quarters or two thirds of the lag,
    // the loop above tries one octave only.
    //
    // A track that folds nowhere (every peak nothing) is no doubt between
    // two tempos, and keeps what it had.
    //
    // Three quarters is where it stops paying, with rekordbox's tempo as
    // the right one and his ear where he listened to that playlist: on
    // the tuning halves 26 tracks come to the right tempo and 6 leave it,
    // on the held-back halves 14 and 4. From 0.65 it loses 15 for 42.
    constexpr double DoubleTakenFrom = 0.75;
    if (chosenPeak > 0.0 && chosen / 2.0 >= allowed.shortest) {
        const double doubled = repeatNear(chosen / 2.0);
        if (localPeak(onsets, doubled) >= DoubleTakenFrom * chosenPeak) {
            return doubled;
        }
    }
    return chosen;
}

// ---------------------------------------------------------------- windows

constexpr std::size_t WindowBeats = 8;
constexpr std::size_t WindowHopBeats = 4;

struct WindowReading
{
    double centreMs = 0.0;
    // How far the window's own peak is from the place the grid has the
    // beat, in ms; late is positive. Not a number where the window has no
    // peak within a quarter of a beat of that place.
    double offsetMs = std::numeric_limits<double>::quiet_NaN();
    double strength = 0.0;
    // The highest point of the window's fold, wherever it is. 0 where the
    // window lies partly outside the track.
    double highest = 0.0;
};

std::vector<WindowReading> readWindows(const std::vector<double> &onsets, double period, double phase)
{
    std::vector<WindowReading> readings;
    const double room = (static_cast<double>(onsets.size() - 1) - phase) / period;
    if (room < static_cast<double>(WindowBeats + 1)) {
        return readings;
    }
    const auto beats = static_cast<std::size_t>(room);
    for (std::size_t start = 0; start + WindowBeats < beats; start += WindowHopBeats) {
        WindowReading reading;
        reading.centreMs = phase + (static_cast<double>(start) + static_cast<double>(WindowBeats) / 2.0) * period;
        // Folded from half a beat before the grid's place, so that place
        // is the middle of the fold and the search either side of it does
        // not run over the fold's end.
        std::size_t used = 0;
        const std::vector<double> profile =
            smoothed(fold(onsets, phase + (static_cast<double>(start) - 0.5) * period, period, WindowBeats, used));
        const std::size_t middle = profile.size() / 2;
        const std::size_t reach = profile.size() / 4;
        const std::size_t top = indexOfMax(profile, middle - reach, middle + reach);
        const double highest = profile[indexOfMax(profile, 0, profile.size() - 1)];
        reading.highest = used == WindowBeats ? highest : 0.0;
        // No reading where beats of the window lie outside the track,
        // where the peak near the grid's place is not the window's peak
        // or near it in height (the beat is somewhere else here), or
        // where it is at the edge of the search (it is a slope, not a peak).
        constexpr double NearTheHighest = 0.5;
        if (used == WindowBeats && profile[top] >= NearTheHighest * highest && top != middle - reach
            && top != middle + reach) {
            const double step = period / static_cast<double>(profile.size());
            reading.offsetMs = (parabolaPeak(profile, top) - static_cast<double>(profile.size()) / 2.0) * step;
            reading.strength = profile[top];
        }
        readings.push_back(reading);
    }
    return readings;
}

struct Line
{
    double slope = 0.0;      // ms of offset a ms of track
    double atZeroMs = 0.0;   // the offset at 0 ms
    double spreadMs = 0.0;   // root mean square of what the kept windows leave
    std::size_t kept = 0;
};

// A line through the windows' offsets, weighted by their strength, twice:
// the second time without the windows far off the first line.
std::optional<Line> fitLine(const std::vector<WindowReading> &readings)
{
    std::vector<bool> keep(readings.size());
    for (std::size_t i = 0; i < readings.size(); ++i) {
        keep[i] = std::isfinite(readings[i].offsetMs);
    }
    // A window counts by its strength, up to three times the middle
    // window's: one thump must not carry the line away from the rest.
    std::vector<double> strengths;
    for (std::size_t i = 0; i < readings.size(); ++i) {
        if (keep[i]) {
            strengths.push_back(readings[i].strength);
        }
    }
    const double mostWeight = 3.0 * quantile(strengths, 0.5);
    const auto weightOf = [&](std::size_t i) { return std::min(readings[i].strength, mostWeight); };
    Line line;
    for (int round = 0; round < 2; ++round) {
        double weight = 0.0;
        double meanTime = 0.0;
        double meanOffset = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < readings.size(); ++i) {
            if (keep[i]) {
                weight += weightOf(i);
                meanTime += weightOf(i) * readings[i].centreMs;
                meanOffset += weightOf(i) * readings[i].offsetMs;
                ++count;
            }
        }
        if (count < 3 || !(weight > 0.0)) {
            return std::nullopt;
        }
        meanTime /= weight;
        meanOffset /= weight;
        double covariance = 0.0;
        double variance = 0.0;
        for (std::size_t i = 0; i < readings.size(); ++i) {
            if (keep[i]) {
                const double dt = readings[i].centreMs - meanTime;
                covariance += weightOf(i) * dt * (readings[i].offsetMs - meanOffset);
                variance += weightOf(i) * dt * dt;
            }
        }
        if (!(variance > 0.0)) {
            return std::nullopt;
        }
        line.slope = covariance / variance;
        line.atZeroMs = meanOffset - line.slope * meanTime;

        // What each window leaves, and how wide the kept ones scatter:
        // from their median, so the ones to be thrown out do not set the
        // bound that throws them out. Half a millisecond more, or windows
        // that agree to a hair would throw out all but themselves.
        std::vector<double> left(readings.size(), 0.0);
        std::vector<double> keptLeft;
        for (std::size_t i = 0; i < readings.size(); ++i) {
            if (std::isfinite(readings[i].offsetMs)) {
                left[i] = readings[i].offsetMs - (line.atZeroMs + line.slope * readings[i].centreMs);
                if (keep[i]) {
                    keptLeft.push_back(left[i]);
                }
            }
        }
        const double middle = quantile(keptLeft, 0.5);
        std::vector<double> deviations;
        deviations.reserve(keptLeft.size());
        double squares = 0.0;
        for (const double value : keptLeft) {
            deviations.push_back(std::abs(value - middle));
            squares += value * value;
        }
        line.spreadMs = std::sqrt(squares / static_cast<double>(keptLeft.size()));
        line.kept = keptLeft.size();
        const double bound = 4.0 * (1.4826 * quantile(deviations, 0.5) + 0.5);
        for (std::size_t i = 0; i < readings.size(); ++i) {
            keep[i] = std::isfinite(readings[i].offsetMs) && std::abs(left[i] - middle) <= bound;
        }
    }
    return line;
}

// ----------------------------------------------------- a tempo that moves

// The onset function over its own level of the 4 s around: a quiet part
// of the track then says as much about the tempo as a loud one.
std::vector<double> levelled(const std::vector<double> &onsets)
{
    constexpr std::ptrdiff_t Half = 2000;
    const auto n = static_cast<std::ptrdiff_t>(onsets.size());
    std::vector<double> sums(onsets.size() + 1, 0.0);
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        sums[i + 1] = sums[i] + onsets[i];
    }
    const double mean = onsets.empty() ? 0.0 : sums.back() / static_cast<double>(onsets.size());
    std::vector<double> result(onsets.size());
    for (std::ptrdiff_t i = 0; i < n; ++i) {
        const std::ptrdiff_t from = std::max<std::ptrdiff_t>(0, i - Half);
        const std::ptrdiff_t to = std::min(n, i + Half);
        const double level = (sums[static_cast<std::size_t>(to)] - sums[static_cast<std::size_t>(from)])
            / static_cast<double>(std::max<std::ptrdiff_t>(1, to - from));
        result[static_cast<std::size_t>(i)] = onsets[static_cast<std::size_t>(i)] / (level + 0.1 * mean + 1e-12);
    }
    return result;
}

// The beat period along the track: a value every 2 s.
struct TempoCurve
{
    std::vector<double> centresMs;
    std::vector<double> periodsMs;
    // Whether the 8 s around each centre repeat at their lag at all: a
    // rhythm, at whatever tempo near the track's. A breakdown of pads, an
    // intro without drums and a held chord do not.
    std::vector<bool> rhythm;

    bool rhythmAt(double timeMs) const
    {
        if (centresMs.empty()) {
            return false;
        }
        const double spacing = centresMs.size() > 1 ? centresMs[1] - centresMs[0] : 1.0;
        const double place = std::round((timeMs - centresMs.front()) / spacing);
        const auto i = static_cast<std::size_t>(std::clamp(place, 0.0, static_cast<double>(centresMs.size() - 1)));
        return rhythm[i];
    }

    double at(double timeMs) const
    {
        if (timeMs <= centresMs.front()) {
            return periodsMs.front();
        }
        if (timeMs >= centresMs.back()) {
            return periodsMs.back();
        }
        const auto above = std::upper_bound(centresMs.begin(), centresMs.end(), timeMs);
        const auto i = static_cast<std::size_t>(above - centresMs.begin());
        const double part = (timeMs - centresMs[i - 1]) / (centresMs[i] - centresMs[i - 1]);
        return periodsMs[i - 1] + part * (periodsMs[i] - periodsMs[i - 1]);
    }
};

// Every 2 s the autocorrelation of the 8 s around it, at the lags from
// 0.78 to 1.28 of `guess` (a quarter either way: wide enough for a ramp
// from 100 to 125 BPM, and short of the lags at two thirds, three
// quarters, four thirds and three halves of the beat, which a rhythm
// repeats at too and which are no change of tempo); through those the
// path of lags that scores best without jumping. The lags stay within
// what `allowed` holds and a tenth beyond, and in any case may go as far
// as 280 and 1100 ms. A window is scored against the track's strong windows
// and not against its own best: one with no rhythm in it (a breakdown)
// then has no say, and the path goes straight through it. Left to its
// own best it would pick whatever its noise repeats at, and a grid
// carried through the breakdown at that period comes out beside the beat.
std::optional<TempoCurve> tempoCurve(const std::vector<double> &onsets, double guess, const Periods &allowed)
{
    const auto firstLag = std::max(std::min<std::size_t>(280, static_cast<std::size_t>(0.93 * allowed.shortest)),
                                   static_cast<std::size_t>(0.78 * guess));
    const auto lastLag = std::min(std::max<std::size_t>(1100, static_cast<std::size_t>(1.1 * allowed.longest)),
                                  static_cast<std::size_t>(1.28 * guess));
    constexpr std::size_t HopMs = 2000;
    constexpr std::size_t HalfMs = 4000;
    constexpr std::size_t Transform = 16384;
    if (lastLag <= firstLag + 2 || onsets.size() < 2 * HalfMs + 2 * HopMs) {
        return std::nullopt;
    }
    const std::size_t lags = lastLag - firstLag + 1;
    TempoCurve curve;
    std::vector<std::vector<double>> scores;
    std::vector<double> sizes;
    std::vector<std::complex<double>> z(Transform);
    for (std::size_t centre = HalfMs; centre + HalfMs < onsets.size(); centre += HopMs) {
        double mean = 0.0;
        for (std::size_t i = centre - HalfMs; i < centre + HalfMs; ++i) {
            mean += onsets[i];
        }
        mean /= static_cast<double>(2 * HalfMs);
        std::fill(z.begin(), z.end(), std::complex<double>(0.0, 0.0));
        for (std::size_t i = 0; i < 2 * HalfMs; ++i) {
            z[i] = onsets[centre - HalfMs + i] - mean;
        }
        fft(z, false);
        for (std::complex<double> &value : z) {
            value = std::norm(value);
        }
        fft(z, true);
        std::vector<double> row(lags);
        for (std::size_t k = 0; k < lags; ++k) {
            const std::size_t lag = firstLag + k;
            // The lag and its double, so that half a beat does not win.
            row[k] = std::max(z[lag].real() + 0.5 * z[2 * lag].real(), 0.0);
        }
        // What the window would score at a lag where it repeated itself
        // exactly.
        sizes.push_back(1.5 * z[0].real());
        curve.centresMs.push_back(static_cast<double>(centre));
        scores.push_back(std::move(row));
    }
    if (scores.size() < 2) {
        return std::nullopt;
    }
    std::vector<double> tops;
    tops.reserve(scores.size());
    for (const std::vector<double> &row : scores) {
        tops.push_back(row[indexOfMax(row, 0, row.size() - 1)]);
    }
    const double scale = quantile(tops, 0.8);
    if (!(scale > 0.0)) {
        return std::nullopt;
    }
    for (std::vector<double> &row : scores) {
        for (double &value : row) {
            value = std::min(value / scale, 1.5);
        }
    }

    // The best path: a change of lag costs by the square of its size (in
    // log), so much that only a real change of tempo pays for one.
    constexpr double ChangeCost = 400.0;
    std::vector<double> logLag(lags);
    for (std::size_t k = 0; k < lags; ++k) {
        logLag[k] = std::log(static_cast<double>(firstLag + k));
    }
    std::vector<double> best = scores[0];
    std::vector<double> next(lags);
    std::vector<std::vector<std::uint32_t>> from(scores.size(), std::vector<std::uint32_t>(lags, 0));
    for (std::size_t w = 1; w < scores.size(); ++w) {
        for (std::size_t j = 0; j < lags; ++j) {
            double top = -std::numeric_limits<double>::infinity();
            std::uint32_t topFrom = 0;
            for (std::size_t k = 0; k < lags; ++k) {
                const double d = logLag[j] - logLag[k];
                const double value = best[k] - ChangeCost * d * d;
                if (value > top) {
                    top = value;
                    topFrom = static_cast<std::uint32_t>(k);
                }
            }
            next[j] = top + scores[w][j];
            from[w][j] = topFrom;
        }
        best.swap(next);
    }
    std::vector<std::size_t> path(scores.size());
    path.back() = indexOfMax(best, 0, lags - 1);
    for (std::size_t w = scores.size() - 1; w > 0; --w) {
        path[w - 1] = from[w][path[w]];
    }

    // Finer than a millisecond where the window has a peak at its lag;
    // elsewhere between the windows that have. A peak: three tenths of
    // the track's strong windows, and a fifth of what the window would
    // score if it repeated itself exactly. The second is what noise does
    // not reach: its strong windows are no stronger than its weak ones,
    // and none of them repeats.
    constexpr double HasAPeak = 0.3;
    constexpr double RepeatsItself = 0.2;
    curve.periodsMs.assign(scores.size(), std::numeric_limits<double>::quiet_NaN());
    curve.rhythm.assign(scores.size(), false);
    std::vector<std::size_t> known;
    for (std::size_t w = 0; w < scores.size(); ++w) {
        const std::size_t k = path[w];
        if (scores[w][k] >= HasAPeak && scores[w][k] * scale >= RepeatsItself * sizes[w] && k > 0 && k + 1 < lags) {
            curve.periodsMs[w] = static_cast<double>(firstLag) + parabolaPeak(scores[w], k);
            curve.rhythm[w] = true;
            known.push_back(w);
        }
    }
    // No window with a rhythm (noise, speech): the curve is flat at the
    // guess and says of every window that it has none. That is not the
    // same as no curve, which is a track too short to have one.
    if (known.empty()) {
        std::fill(curve.periodsMs.begin(), curve.periodsMs.end(), guess);
        return curve;
    }
    for (std::size_t w = 0; w < scores.size(); ++w) {
        if (std::isfinite(curve.periodsMs[w])) {
            continue;
        }
        const auto after = std::lower_bound(known.begin(), known.end(), w);
        if (after == known.begin()) {
            curve.periodsMs[w] = curve.periodsMs[*after];
        } else if (after == known.end()) {
            curve.periodsMs[w] = curve.periodsMs[*std::prev(after)];
        } else {
            const std::size_t a = *std::prev(after);
            const std::size_t b = *after;
            const double part = static_cast<double>(w - a) / static_cast<double>(b - a);
            curve.periodsMs[w] = curve.periodsMs[a] + part * (curve.periodsMs[b] - curve.periodsMs[a]);
        }
    }
    return curve;
}

// The beats of a track whose tempo moves, by following windows of eight
// beats from the strongest one outwards, four beats at a time.
//
// The next window is folded at the period the tempo curve has there. Its
// peak corrects the grid only when it is within an eighth of a beat of
// where the grid already is, at least half as high as the window's
// highest point, and the window a quarter as strong as the strongest:
// then it is this beat, heard clearly. Otherwise the grid goes on at the
// curve's period and nothing corrects it (a breakdown, a fill, a hit
// between the beats). After three windows that have drums and no such
// peak the grid is no longer on the track's beat, and the beat is taken
// where those drums have it.
std::vector<double> followWindows(const std::vector<double> &onsets, const TempoCurve &curve, double straightPeriod,
                                  double straightPhase, std::size_t &windowsSure, std::size_t &windowsTried)
{
    windowsSure = 0;
    windowsTried = 0;
    const double last = static_cast<double>(onsets.size() - 1);
    constexpr double Reach = 0.125;
    constexpr double NearTheHighest = 0.5;
    constexpr double HasDrums = 0.25;
    constexpr double DrumsElsewhere = 0.3;
    constexpr int WindowsLost = 3;

    // The anchor: of windows every four beats, the strongest, and its
    // peak wherever in the beat it is.
    double strongest = -1.0;
    double anchor = 0.0;
    for (double t = 0.0; t + 9.0 * curve.at(t) < last;) {
        const double tau = curve.at(t);
        std::size_t used = 0;
        const std::vector<double> folded = fold(onsets, t, tau, WindowBeats, used);
        if (used == WindowBeats) {
            const std::vector<double> profile = smoothed(folded);
            const double height = profile[indexOfMax(profile, 0, profile.size() - 1)];
            if (height > strongest) {
                strongest = height;
                anchor = t + heaviestPlace(folded) * (tau / static_cast<double>(folded.size()));
            }
        }
        t += static_cast<double>(WindowHopBeats) * tau;
    }
    if (!(strongest > 0.0)) {
        return {};
    }
    // Which moment of the beat is the beat, the whole track says better
    // than its strongest eight beats: where the straight grid has a beat
    // within reach of this window and the window has a clear peak on it,
    // the following starts there and not on the window's own heaviest
    // place, which may be the beat between.
    if (straightPeriod > 0.0) {
        const double tau = curve.at(anchor);
        const double onStraight = straightPhase + std::round((anchor - straightPhase) / straightPeriod) * straightPeriod;
        if (std::abs(onStraight - anchor) > Reach * tau && onStraight - tau / 2.0 >= 0.0) {
            std::size_t used = 0;
            const std::vector<double> profile = smoothed(fold(onsets, onStraight - tau / 2.0, tau, WindowBeats, used));
            const std::size_t middle = profile.size() / 2;
            const auto reach = std::max<std::size_t>(2, static_cast<std::size_t>(Reach * static_cast<double>(profile.size())));
            if (used == WindowBeats && middle > reach && middle + reach < profile.size()) {
                const std::size_t top = indexOfMax(profile, middle - reach, middle + reach);
                const double highest = profile[indexOfMax(profile, 0, profile.size() - 1)];
                if (profile[top] >= NearTheHighest * highest && top != middle - reach && top != middle + reach) {
                    anchor = onStraight
                        + (parabolaPeak(profile, top) - static_cast<double>(profile.size()) / 2.0)
                            * (tau / static_cast<double>(profile.size()));
                }
            }
        }
    }

    const auto run = [&](double direction) {
        std::vector<double> beats;
        double b = anchor;
        int lost = 0;
        // The last beat a window was sure of, and how many beats were
        // given out before it. The beats since are only as good as the
        // curve's period: when the next window is sure, they are spaced
        // evenly between the two, so that a grid carried through a long
        // breakdown arrives where the drums come back and does not bend
        // on its last four beats.
        double sure = anchor;
        std::size_t beatsBeforeSure = 0;
        for (;;) {
            const double tau = curve.at(b);
            const double next = b + direction * static_cast<double>(WindowHopBeats) * tau;
            if (next < 0.0 || next > last) {
                // The edge of the track: on to it at the curve's period.
                for (double step = b; step >= 0.0 && step <= last; step += direction * curve.at(step)) {
                    beats.push_back(step);
                }
                break;
            }
            // The eight beats around the beat to be corrected, four before
            // it and four from it on: its own error, not the error the
            // grid will have four beats later. Near the edge the window
            // does not fit and corrects nothing.
            const double here = curve.at(next);
            std::size_t used = 0;
            const std::vector<double> folded =
                fold(onsets, next - (static_cast<double>(WindowBeats) / 2.0 + 0.5) * here, here, WindowBeats, used);
            const std::vector<double> profile = smoothed(folded);
            const double step = here / static_cast<double>(profile.size());
            const std::size_t middle = profile.size() / 2;
            const auto reach = std::max<std::size_t>(2, static_cast<std::size_t>(Reach * static_cast<double>(profile.size())));
            double correction = 0.0;
            bool corrects = false;
            if (used == WindowBeats && middle > reach && middle + reach < profile.size()) {
                ++windowsTried;
                const std::size_t highestAt = indexOfMax(profile, 0, profile.size() - 1);
                const std::size_t top = indexOfMax(profile, middle - reach, middle + reach);
                const bool clearly = profile[top] >= NearTheHighest * profile[highestAt] && top != middle - reach
                    && top != middle + reach && profile[top] >= HasDrums * strongest;
                if (clearly) {
                    correction = (parabolaPeak(profile, top) - static_cast<double>(profile.size()) / 2.0) * step;
                    lost = 0;
                    corrects = true;
                    ++windowsSure;
                } else if (profile[highestAt] >= DrumsElsewhere * strongest && ++lost >= WindowsLost) {
                    correction = (heaviestPlace(folded) - static_cast<double>(profile.size()) / 2.0) * step;
                    lost = 0;
                    corrects = true;
                }
            }
            const double corrected = next + correction;
            for (std::size_t k = 0; k < WindowHopBeats; ++k) {
                beats.push_back(b + (corrected - b) * static_cast<double>(k) / static_cast<double>(WindowHopBeats));
            }
            if (corrects) {
                const std::size_t since = beats.size() - beatsBeforeSure;
                for (std::size_t k = 0; k < since; ++k) {
                    beats[beatsBeforeSure + k] = sure + (corrected - sure) * static_cast<double>(k) / static_cast<double>(since);
                }
                sure = corrected;
                beatsBeforeSure = beats.size();
            }
            b = corrected;
        }
        return beats;
    };

    std::vector<double> beats = run(-1.0);
    const std::vector<double> forward = run(1.0);
    beats.insert(beats.end(), forward.begin(), forward.end());
    std::sort(beats.begin(), beats.end());
    // The anchor is in both runs, and two beats closer than a fifth of
    // the shortest period are one.
    std::vector<double> distinct;
    for (const double beat : beats) {
        if (distinct.empty() || beat - distinct.back() > static_cast<double>(ShortestPeriodMs) / 5.0) {
            distinct.push_back(beat);
        }
    }
    return distinct;
}

// The beats as the one path through the track that scores best (Ellis,
// "Beat Tracking by Dynamic Programming", 2007): a beat scores what the
// onset function has at its place, and the step from one beat to the next
// costs by the square of how far (in log) it is from the period the tempo
// curve has there. This follows a drummer, whose every beat is a little
// off where the last ones said it would be, as the window follower does
// not. `sureShare` is the share of the path's beats that had something to
// go by, measured against the path's own stronger beats: it says how much
// of the track has a beat to be on, not whether the track has a rhythm.
// Nothing comes back where the path has under eight beats or under four
// sure ones.
//
// It is for a tempo that wanders and not for one that holds (see where it
// is called): given a track from a drum machine, the path leaves the beat
// for whatever is loudest, and where a section has its bass or its chords
// between the beats it sits there for the length of the section.
std::vector<double> trackBeats(const std::vector<double> &onsets, const TempoCurve &curve, double &sureShare)
{
    sureShare = 0.0;
    // How much a step off the curve's period costs. 100 is Ellis's; 400
    // and 1000 follow the varied music no better and no worse.
    constexpr double Tightness = 100.0;
    constexpr std::size_t Hop = 2;
    // The onset function is taken over 15 ms either side: over 8 a tick
    // between the beats outweighs a kick, whose bands rise one after the
    // other, and over 30 the beats land too far from their hits.
    constexpr std::ptrdiff_t Smooth = 15;
    const std::size_t n = onsets.size() / Hop;
    if (n < 16) {
        return {};
    }
    // The onset function a little broader, so that a beat a few ms off a
    // hit still has most of it, and every second value of that.
    std::vector<double> sums(onsets.size() + 1, 0.0);
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        sums[i + 1] = sums[i] + onsets[i];
    }
    std::vector<double> local(n);
    double mean = 0.0;
    for (std::size_t t = 0; t < n; ++t) {
        const auto centre = static_cast<std::ptrdiff_t>(t * Hop);
        const auto from = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, centre - Smooth));
        const auto to = static_cast<std::size_t>(
            std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(onsets.size()), centre + Smooth + 1));
        local[t] = (sums[to] - sums[from]) / static_cast<double>(to - from);
        mean += local[t];
    }
    mean /= static_cast<double>(n);
    double variance = 0.0;
    for (const double value : local) {
        variance += (value - mean) * (value - mean);
    }
    const double deviation = std::sqrt(variance / static_cast<double>(n));
    if (!(deviation > 0.0)) {
        return {};
    }
    for (double &value : local) {
        value /= deviation;
    }
    std::vector<double> logOf(4096);
    for (std::size_t k = 1; k < logOf.size(); ++k) {
        logOf[k] = std::log(static_cast<double>(k));
    }
    std::vector<double> score(n, 0.0);
    std::vector<std::int32_t> before(n, -1);
    for (std::size_t t = 0; t < n; ++t) {
        const double period = curve.at(static_cast<double>(t * Hop)) / static_cast<double>(Hop);
        const double logPeriod = std::log(period);
        const auto farthest = std::min<std::size_t>(static_cast<std::size_t>(2.0 * period), logOf.size() - 1);
        const auto nearest = std::max<std::size_t>(static_cast<std::size_t>(0.5 * period), 1);
        double best = 0.0;
        std::int32_t bestBefore = -1;
        for (std::size_t back = nearest; back <= farthest && back <= t; ++back) {
            const double d = logOf[back] - logPeriod;
            const double value = score[t - back] - Tightness * d * d;
            if (value > best) {
                best = value;
                bestBefore = static_cast<std::int32_t>(t - back);
            }
        }
        score[t] = local[t] + best;
        before[t] = bestBefore;
    }
    // The last beat: the best score in the last beat and a half.
    const double lastPeriod = curve.at(static_cast<double>((n - 1) * Hop)) / static_cast<double>(Hop);
    const std::size_t tail = n - 1 - std::min<std::size_t>(n - 1, static_cast<std::size_t>(1.5 * lastPeriod));
    std::size_t at = indexOfMax(score, tail, n - 1);
    std::vector<double> beats;
    std::vector<double> strengths;
    for (std::int32_t i = static_cast<std::int32_t>(at); i >= 0; i = before[static_cast<std::size_t>(i)]) {
        beats.push_back(static_cast<double>(static_cast<std::size_t>(i) * Hop));
        strengths.push_back(local[static_cast<std::size_t>(i)]);
    }
    std::reverse(beats.begin(), beats.end());
    std::reverse(strengths.begin(), strengths.end());
    if (beats.size() < 8) {
        return {};
    }
    // Where the path had nothing to go by (a breakdown, a start without
    // drums) it wanders to whatever is there at next to no cost. A beat
    // is sure when it has a quarter of what the track's stronger beats
    // have; two and more unsure beats in a row are spaced evenly between
    // the sure ones around them.
    constexpr double SureShare = 0.25;
    const double typical = quantile(strengths, 0.75);
    std::vector<std::size_t> sure;
    for (std::size_t i = 0; i < beats.size(); ++i) {
        if (strengths[i] >= SureShare * typical) {
            sure.push_back(i);
        }
    }
    if (sure.size() < 4) {
        return {};
    }
    sureShare = static_cast<double>(sure.size()) / static_cast<double>(beats.size());
    // Before the first sure beat and after the last the grid goes on at
    // the period the curve has there, to the start and the end.
    std::vector<double> whole;
    for (double time = beats[sure.front()] - curve.at(beats[sure.front()]); time >= 0.0; time -= curve.at(time)) {
        whole.push_back(time);
    }
    std::reverse(whole.begin(), whole.end());
    whole.push_back(beats[sure.front()]);
    for (std::size_t k = 0; k + 1 < sure.size(); ++k) {
        const std::size_t a = sure[k];
        const std::size_t b = sure[k + 1];
        if (b - a < 3) {
            whole.insert(whole.end(), beats.begin() + static_cast<std::ptrdiff_t>(a) + 1,
                         beats.begin() + static_cast<std::ptrdiff_t>(b) + 1);
            continue;
        }
        // How many beats the stretch holds is the curve's to say, not the
        // path's: over a long breakdown the path gains or loses one for
        // the price of a percent a beat.
        constexpr double StepMs = 10.0;
        double counted = 0.0;
        for (double time = beats[a]; time < beats[b]; time += StepMs) {
            counted += std::min(StepMs, beats[b] - time) / curve.at(time);
        }
        const auto count = static_cast<std::size_t>(std::max(1.0, std::round(counted)));
        for (std::size_t i = 1; i <= count; ++i) {
            whole.push_back(beats[a] + (beats[b] - beats[a]) * static_cast<double>(i) / static_cast<double>(count));
        }
    }
    const double end = static_cast<double>(onsets.size() - 1);
    for (double time = whole.back() + curve.at(whole.back()); time <= end; time += curve.at(time)) {
        whole.push_back(time);
    }
    return whole;
}

// How tightly a grid sits on the hits. A beat counts when the onset
// function has a strong onset within 50 ms of it: at least half as strong
// as the beat a quarter of the way down this grid's own list of strongest
// (its 75th percentile). Of those beats, the share whose onset lies within
// 12 ms of where this grid's onsets usually lie against its beats. A grid
// that follows the playing has its hits at one place all through the
// track; one the playing runs away from has them wandering. A beat with no
// onset near it says nothing either way. Nothing where under 16 of the
// beats lie 50 ms or more inside the track, the grid has no strong onsets,
// or under 8 of its beats have one.
std::optional<double> tightness(const std::vector<double> &beats, const std::vector<double> &onsets)
{
    constexpr std::ptrdiff_t ReachMs = 50;
    constexpr double NearMs = 12.0;
    const auto length = static_cast<std::ptrdiff_t>(onsets.size());
    std::vector<double> peaks;
    std::vector<double> places;
    for (const double beat : beats) {
        if (!std::isfinite(beat) || beat < 0.0 || beat >= static_cast<double>(length)) {
            continue;
        }
        const auto centre = static_cast<std::ptrdiff_t>(std::llround(beat));
        if (centre - ReachMs < 0 || centre + ReachMs >= length) {
            continue;
        }
        double best = 0.0;
        std::ptrdiff_t at = 0;
        for (std::ptrdiff_t k = -ReachMs; k <= ReachMs; ++k) {
            const double value = onsets[static_cast<std::size_t>(centre + k)];
            if (value > best) {
                best = value;
                at = k;
            }
        }
        peaks.push_back(best);
        places.push_back(static_cast<double>(at));
    }
    if (peaks.size() < 16) {
        return std::nullopt;
    }
    const double strong = quantile(peaks, 0.75);
    if (!(strong > 0.0)) {
        return std::nullopt;
    }
    std::vector<double> kept;
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        if (peaks[i] >= 0.5 * strong) {
            kept.push_back(places[i]);
        }
    }
    if (kept.size() < 8) {
        return std::nullopt;
    }
    const double usual = quantile(kept, 0.5);
    std::size_t near = 0;
    for (const double place : kept) {
        near += std::abs(place - usual) <= NearMs ? 1 : 0;
    }
    return static_cast<double>(near) / static_cast<double>(kept.size());
}

// How far the tempo wanders: the spread of the curve's periods (5th to
// 95th percentile) over their middle, in the windows that have a rhythm.
// A drum machine holds to a thousandth; a drummer wanders by percents.
// So does a machine whose tempo is changed on purpose (a step from 124 to
// 126, a ramp): this tells a tempo that moves from one that holds, not
// who plays. 0 where under eight windows have a rhythm: too few to say.
double wander(const TempoCurve &curve)
{
    std::vector<double> periods;
    for (std::size_t w = 0; w < curve.periodsMs.size(); ++w) {
        if (curve.rhythm[w]) {
            periods.push_back(curve.periodsMs[w]);
        }
    }
    if (periods.size() < 8) {
        return 0.0;
    }
    return (quantile(periods, 0.95) - quantile(periods, 0.05)) / quantile(periods, 0.5);
}

// Each followed beat set on its own onset: the peak of the onset
// function within 25 ms of it, where there is a clear one. The windows
// give the beat as eight beats have it on average, which on a drummer is
// the pulse and not the hits; rekordbox's beat-by-beat grids are on the
// hits (their beats lie 3 to 9 ms off the line through their
// neighbours), and a loop is cut on a hit. A beat without a clear onset
// near it stays where the windows or the path put it.
void setOnOnsets(std::vector<double> &beats, const std::vector<double> &onsets)
{
    if (beats.empty() || onsets.size() < 64) {
        return;
    }
    // The onset function smoothed over 4 ms either way, along the track.
    constexpr std::ptrdiff_t Smooth = 4;
    std::array<double, 2 * Smooth + 1> window{};
    double sum = 0.0;
    for (std::ptrdiff_t k = 0; k <= 2 * Smooth; ++k) {
        window[static_cast<std::size_t>(k)] =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(2 * Smooth));
        sum += window[static_cast<std::size_t>(k)];
    }
    const auto n = static_cast<std::ptrdiff_t>(onsets.size());
    std::vector<double> smooth(onsets.size(), 0.0);
    for (std::ptrdiff_t i = Smooth; i + Smooth < n; ++i) {
        double value = 0.0;
        for (std::ptrdiff_t k = -Smooth; k <= Smooth; ++k) {
            value += window[static_cast<std::size_t>(k + Smooth)] * onsets[static_cast<std::size_t>(i + k)];
        }
        smooth[static_cast<std::size_t>(i)] = value / sum;
    }
    // Clear: three tenths of what the track's strongest hundredth of
    // moments reach.
    const double clear = 0.3 * quantile(smooth, 0.99);
    constexpr std::ptrdiff_t ReachMs = 25;
    for (double &beat : beats) {
        const auto centre = static_cast<std::ptrdiff_t>(std::llround(beat));
        const std::ptrdiff_t from = centre - ReachMs;
        const std::ptrdiff_t to = centre + ReachMs;
        if (from < 1 || to + 1 >= n) {
            continue;
        }
        const std::size_t top = indexOfMax(smooth, static_cast<std::size_t>(from), static_cast<std::size_t>(to));
        if (smooth[top] >= clear && static_cast<std::ptrdiff_t>(top) != from && static_cast<std::ptrdiff_t>(top) != to) {
            beat = parabolaPeak(smooth, top);
        }
    }
    // Two beats set on one onset are one beat.
    std::sort(beats.begin(), beats.end());
    std::vector<double> distinct;
    for (const double beat : beats) {
        if (distinct.empty() || beat - distinct.back() > static_cast<double>(ShortestPeriodMs) / 5.0) {
            distinct.push_back(beat);
        }
    }
    beats.swap(distinct);
}

}  // namespace

TempoRange usableTempoRange(TempoRange range)
{
    // From 40 BPM, to 240: a beat every second and a half is as slow as
    // the tempo curve's windows of 8 s can follow, and a beat every
    // quarter of a second as fast as a fold tells a kick from a tick. At
    // least an octave. What is no range at all is the default one.
    constexpr double Lowest = 40.0;
    constexpr double Highest = 240.0;
    if (!(range.lowBpm > 0.0) || !(range.highBpm >= range.lowBpm) || !std::isfinite(range.highBpm)) {
        range = TempoRange{};
    }
    range.lowBpm = std::clamp(range.lowBpm, Lowest, Highest / 2.0);
    range.highBpm = std::clamp(range.highBpm, 2.0 * range.lowBpm, Highest);
    return range;
}

namespace
{

// The grid of a track from its onsets: all of the analysis after the
// listening.
GridAnalysis gridFromOnsets(const Onsets &both, const TempoRange &range)
{
    GridAnalysis analysis;
    const std::vector<double> &onsets = both.forBeat;
    const Periods allowed = periodsOf(range);
    // Under four beats of the slowest tempo there is nothing to repeat.
    if (static_cast<double>(onsets.size()) < 4.0 * std::max(static_cast<double>(LongestPeriodMs), allowed.longest)) {
        return analysis;
    }
    const std::optional<double> coarse = beatPeriod(both.forTempo, allowed);
    if (!coarse) {
        return analysis;
    }
    double period = *coarse;
    if (!(period >= allowed.shortest * 0.9 && period <= allowed.longest * 1.1)) {
        return analysis;
    }
    double phase = wholeTrackPhase(onsets, period);

    // The windows correct the period and the place, three times over: a
    // better period makes sharper folds, which make a better line.
    std::size_t windowsRead = 0;
    std::size_t windowsKept = 0;
    for (int round = 0; round < 3; ++round) {
        std::vector<WindowReading> readings = readWindows(onsets, period, phase);
        std::vector<double> strengths;
        strengths.reserve(readings.size());
        for (const WindowReading &reading : readings) {
            strengths.push_back(reading.strength);
        }
        // A window a quarter as strong as the track's strong ones has no
        // drums in it to speak of.
        const double floor = 0.25 * quantile(strengths, 0.8);
        for (WindowReading &reading : readings) {
            if (reading.strength < floor) {
                reading.offsetMs = std::numeric_limits<double>::quiet_NaN();
            }
        }
        const std::optional<Line> line = fitLine(readings);
        if (!line) {
            break;
        }
        // A line that would change the tempo by a hundredth is no
        // correction: the windows are following something else.
        constexpr double MostCorrection = 0.01;
        if (!(std::abs(line->slope) <= MostCorrection) || !std::isfinite(line->atZeroMs)) {
            break;
        }
        // A window at t ms is late by atZero + slope * t: the beat that
        // the grid has at phase + k * period is at that plus this.
        phase += line->atZeroMs + line->slope * phase;
        period *= 1.0 + line->slope;
        windowsRead = readings.size();
        windowsKept = line->kept;
    }

    const double last = static_cast<double>(onsets.size() - 1);
    double first = std::fmod(phase, period);
    if (first < 0.0) {
        first += period;
    }
    std::vector<double> straight;
    for (std::size_t k = 0;; ++k) {
        const double time = first + static_cast<double>(k) * period;
        if (time > last) {
            break;
        }
        straight.push_back(time);
    }

    // Is the tempo constant? The windows say. Of those in a stretch that
    // has a rhythm (the tempo curve knows: its 8 s there repeat, at
    // whatever tempo near the track's), how many have their own peak
    // within 10 ms of the straight grid (of the grid's middle distance
    // from them, so that which moment of the beat it sits on does not
    // count)? On a track made with a drum machine nearly all: on the
    // collection the grid explains 99 percent of the windows of the
    // middle track. Where it explains fewer than three quarters, the
    // tempo is taken to move and the track is followed; between three
    // quarters and 85 percent the two grids are compared (below). Above
    // that the followed grid is not asked: on real music it strays from a
    // straight grid that is right more often than it finds a tempo that
    // moves.
    bool moves = false;
    std::vector<double> followed;
    // How sure the grid is, 0 to 1: of the windows with a rhythm, the
    // share the straight grid explains; of a followed grid, the share of
    // its windows that had a clear beat where it expected one, or of its
    // path's beats that had something to go by. Nothing
    // regular in the audio at all (noise, speech) leaves it at 0, with a
    // grid that means nothing.
    double sure = 0.0;
    if (const std::optional<TempoCurve> curve = tempoCurve(levelled(both.forTempo), period, allowed)) {
        const std::vector<WindowReading> readings = readWindows(onsets, period, phase);
        std::vector<double> offsets;
        std::size_t withRhythm = 0;
        for (const WindowReading &reading : readings) {
            if (reading.highest > 0.0 && curve->rhythmAt(reading.centreMs)) {
                ++withRhythm;
                if (std::isfinite(reading.offsetMs)) {
                    offsets.push_back(reading.offsetMs);
                }
            }
        }
        const double middle = quantile(offsets, 0.5);
        std::size_t explained = 0;
        constexpr double OnTheGridWithinMs = 10.0;
        for (const double offset : offsets) {
            explained += std::abs(offset - middle) <= OnTheGridWithinMs ? 1 : 0;
        }
        constexpr double ExplainsTheTrack = 0.75;
        // Between three quarters and 85 percent the windows do not say: a
        // drummer who keeps close to one tempo is there, and so is a drum
        // machine under sounds that lie off its beats. There the two grids
        // are asked themselves, each as it would be given out: the track is
        // followed if the followed grid sits on the hits more tightly than
        // the straight one, by a tenth of its beats.
        //
        // The followed grid has been set on its onsets by then and is tight
        // on most tracks (0.97 and over on three in four), so this asks
        // above all whether the straight grid's hits wander. The straight
        // grid's tightness alone decides nearly as well.
        //
        // Judged by Sebastian's decisions where he made one and by
        // rekordbox elsewhere, on the 1327 tracks of the four tuning
        // halves: three quarters alone is right on 1061, this on 1068 with
        // all 732 of the collection kept, the straight grid's tightness
        // alone on 1066 at best, following wherever the followed grid is
        // tighter (at any share) on 1068 with one of the collection lost,
        // following every track on 937. Margins from a tenth to a fifth
        // come to the same within a track or two.
        constexpr double NearlyExplainsTheTrack = 0.85;
        constexpr double TighterBy = 0.10;
        sure = withRhythm > 0 ? static_cast<double>(explained) / static_cast<double>(withRhythm) : 0.0;
        if (withRhythm > 0 && static_cast<double>(explained) < NearlyExplainsTheTrack * static_cast<double>(withRhythm)) {
            std::size_t windowsSure = 0;
            std::size_t windowsTried = 0;
            followed = followWindows(onsets, *curve, period, phase, windowsSure, windowsTried);
            double followedSure =
                windowsTried > 0 ? static_cast<double>(windowsSure) / static_cast<double>(windowsTried) : 0.0;
            // A drummer or a drum machine? Where the tempo wanders by over
            // a percent the path follows it; where it holds, the windows
            // do, which keep to the beat through a section whose loudest
            // sounds lie between the beats. With the path for every track
            // that is followed, 15 of the 72 collection tracks whose grid
            // Sebastian had looked at and found right came out with
            // another grid; the varied music was the better for it by ten
            // tracks in 368. Neither how well a straight grid fits the
            // path's beats nor how sure the window follower is of itself
            // told the two kinds of track apart as well as the curve does.
            constexpr double Wanders = 0.01;
            if (wander(*curve) > Wanders) {
                double pathSure = 0.0;
                std::vector<double> path = trackBeats(onsets, *curve, pathSure);
                if (path.size() >= 8) {
                    // A path finds beats in anything, speech and noise
                    // included, and is sure of them by its own measure:
                    // it is sure of the track only as far as the curve
                    // found a rhythm in it.
                    std::size_t withARhythm = 0;
                    for (const bool rhythm : curve->rhythm) {
                        withARhythm += rhythm ? 1 : 0;
                    }
                    followed = std::move(path);
                    followedSure = pathSure * static_cast<double>(withARhythm) / static_cast<double>(curve->rhythm.size());
                }
            }
            setOnOnsets(followed, onsets);
            bool taken = static_cast<double>(explained) < ExplainsTheTrack * static_cast<double>(withRhythm);
            if (!taken && followed.size() >= 8) {
                const std::optional<double> ofFollowed = tightness(followed, onsets);
                const std::optional<double> ofStraight = ofFollowed ? tightness(straight, onsets) : std::nullopt;
                taken = ofFollowed && ofStraight && *ofFollowed - *ofStraight > TighterBy;
            }
            moves = taken && followed.size() >= 8;
            if (moves) {
                sure = followedSure;
            }
        }
    } else {
        // Too short for a tempo curve: what the windows kept of the line.
        sure = windowsRead > 0 ? static_cast<double>(windowsKept) / static_cast<double>(windowsRead) : 0.0;
    }
    if (moves) {
        for (const double time : followed) {
            if (time - FollowedGridEarlierMs >= 0.0) {
                analysis.beats.push_back({time - FollowedGridEarlierMs, 0});
            }
        }
    } else {
        for (const double time : straight) {
            if (time + StraightGridLaterMs <= last) {
                analysis.beats.push_back({time + StraightGridLaterMs, 0});
            }
        }
    }
    analysis.confidence = sure;
    analysis.followsTempo = moves;
    return analysis;
}

}  // namespace

// A minute over the middle gap. The middle, not the mean: a followed
// grid's few long gaps over a breakdown would pull a mean down.
double usualBpm(const std::vector<Beat> &beats)
{
    if (beats.size() < 2) {
        return 0.0;
    }
    std::vector<double> gaps;
    gaps.reserve(beats.size() - 1);
    for (std::size_t i = 1; i < beats.size(); ++i) {
        gaps.push_back(beats[i].timeMs - beats[i - 1].timeMs);
    }
    const auto middle = gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2);
    std::nth_element(gaps.begin(), middle, gaps.end());
    return *middle > 0.0 ? 60000.0 / *middle : 0.0;
}

GridAnalysis gridFromSamples(std::span<const float> samples, int sampleRateHz, TempoRange range)
{
    if (sampleRateHz <= 0 || samples.empty()) {
        GridAnalysis failed;
        failed.error = "no samples";
        return failed;
    }
    return gridFromOnsets(onsetFunction(samples, sampleRateHz), usableTempoRange(range));
}

}  // namespace musicanalyze
