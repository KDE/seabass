// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/seabass_grid_analyser.hpp"

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

namespace seabass::domain
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

void run(const Biquad &f, std::vector<double> &x)
{
    double z1 = 0.0;
    double z2 = 0.0;
    for (double &sample : x) {
        const double in = sample;
        const double out = f.b0 * in + z1;
        z1 = f.b1 * in - f.a1 * out + z2;
        z2 = f.b2 * in - f.a2 * out;
        sample = out;
    }
}

// Forwards and then backwards, so what the filters delay one way they
// advance the other: an onset stays where it is in time. All of them one
// way and then all of them the other, which is the same as each both
// ways in turn and turns the track round twice and not twice a filter.
void runBothWays(std::initializer_list<Biquad> filters, std::vector<double> &x)
{
    for (const Biquad &f : filters) {
        run(f, x);
    }
    std::reverse(x.begin(), x.end());
    for (const Biquad &f : filters) {
        run(f, x);
    }
    std::reverse(x.begin(), x.end());
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
// Takes the samples and lets go of them as soon as it has its own copy:
// a track is held whole, and twice is once too many.
Onsets onsetFunction(std::vector<float> samples, int sampleRate)
{
    // A sample that is no number (a decoder's slip) is silence: left in,
    // the filters would spread it over the whole track.
    std::vector<double> audio(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        audio[i] = std::isfinite(samples[i]) ? static_cast<double>(samples[i]) : 0.0;
    }
    samples = std::vector<float>();
    // Half the rate is enough for 5 kHz and halves the work: every other
    // sample of the audio, low-passed both ways first so nothing folds
    // down and nothing moves in time.
    double rate = static_cast<double>(sampleRate);
    if (rate >= 40000.0) {
        runBothWays({lowpass(0.2 * rate, rate, ButterworthFourthQ[0]), lowpass(0.2 * rate, rate, ButterworthFourthQ[1])},
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
        runBothWays({highpass(edges[0], rate, ButterworthFourthQ[0]), highpass(edges[0], rate, ButterworthFourthQ[1]),
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
        runBothWays({lowpass(EnvelopeSmoothingHz, powerRate, std::numbers::sqrt2 / 2.0)}, band);
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

// The tempos looked for, as beat periods in ms: 200 BPM to 60 BPM. (The
// preference below leaves little chance above 180: with folds alike, the
// half of such a tempo wins.)
constexpr std::size_t ShortestPeriodMs = 300;
constexpr std::size_t LongestPeriodMs = 1000;

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
        for (std::ptrdiff_t k = -reach; k <= reach; ++k) {
            value += window[static_cast<std::size_t>(k + reach)] * profile[static_cast<std::size_t>(((i + k) % n + n) % n)];
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

// The beat period in ms, or nothing when the onset function repeats at no
// lag and none of its multiples.
std::optional<double> beatPeriod(const std::vector<double> &onsets)
{
    constexpr std::size_t FarthestBeats = 64;
    const std::vector<double> ac = autocorrelation(onsets, FarthestBeats * (LongestPeriodMs + 100));
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
    // the whole track folded at each, and the one that puts the most of
    // the track's onsets into one moment of its fold is the beat, a wide
    // preference around 128 BPM deciding between a period and its double.
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
    for (const double relative : Relatives) {
        const double about = base * relative;
        if (about < static_cast<double>(ShortestPeriodMs) || about > static_cast<double>(LongestPeriodMs)) {
            continue;
        }
        // The repeat nearest that period, within a few milliseconds.
        constexpr std::size_t Near = 6;
        const auto middle = static_cast<std::size_t>(std::llround(about));
        const double period = sharpened(ac, parabolaPeak(ac, indexOfMax(ac, middle - Near, middle + Near)), floor);
        const auto beats = static_cast<std::size_t>(static_cast<double>(onsets.size() - 1) / period);
        std::size_t used = 0;
        const std::vector<double> profile = smoothed(fold(onsets, 0.0, period, beats, used));
        const double score = profile[indexOfMax(profile, 0, profile.size() - 1)] * preference(period, 128.0, 1.0);
        if (score > chosenScore) {
            chosenScore = score;
            chosen = period;
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
// path of lags that scores best without jumping. A window is scored against the track's strong windows
// and not against its own best: one with no rhythm in it (a breakdown)
// then has no say, and the path goes straight through it. Left to its
// own best it would pick whatever its noise repeats at, and a grid
// carried through the breakdown at that period comes out beside the beat.
std::optional<TempoCurve> tempoCurve(const std::vector<double> &onsets, double guess)
{
    const auto firstLag = std::max<std::size_t>(280, static_cast<std::size_t>(0.78 * guess));
    const auto lastLag = std::min<std::size_t>(1100, static_cast<std::size_t>(1.28 * guess));
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

// Each followed beat set on its own onset: the peak of the onset
// function within 25 ms of it, where there is a clear one. The windows
// give the beat as eight beats have it on average, which on a drummer is
// the pulse and not the hits; rekordbox's beat-by-beat grids are on the
// hits (their beats lie 3 to 9 ms off the line through their
// neighbours), and a loop is cut on a hit. A beat without a clear onset
// near it stays where the windows put it.
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

GridAnalysis SeabassGridAnalyser::analyse(PcmSource &source)
{
    GridAnalysis analysis;
    const int rate = source.sampleRate();
    std::vector<float> samples = readAllSamples(source);
    if (!source.error().empty()) {
        analysis.error = source.error();
        return analysis;
    }
    if (rate <= 0 || samples.empty()) {
        analysis.error = "no samples";
        return analysis;
    }

    const Onsets both = onsetFunction(std::move(samples), rate);
    const std::vector<double> &onsets = both.forBeat;
    // Under four beats of the slowest tempo there is nothing to repeat.
    if (onsets.size() < 4 * LongestPeriodMs) {
        return analysis;
    }
    const std::optional<double> coarse = beatPeriod(both.forTempo);
    if (!coarse) {
        return analysis;
    }
    double period = *coarse;
    if (!(period >= static_cast<double>(ShortestPeriodMs) * 0.9 && period <= static_cast<double>(LongestPeriodMs) * 1.1)) {
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
    // tempo is taken to move and the track is followed. The followed grid
    // is not asked first: on real music it strays from a straight grid
    // that is right more often than it finds a tempo that moves.
    bool moves = false;
    std::vector<double> followed;
    // How sure the grid is, 0 to 1: of the windows with a rhythm, the
    // share the straight grid explains; of a followed grid, the share of
    // its windows that had a clear beat where it expected one. Nothing
    // regular in the audio at all (noise, speech) leaves it at 0, with a
    // grid that means nothing.
    double sure = 0.0;
    if (const std::optional<TempoCurve> curve = tempoCurve(levelled(both.forTempo), period)) {
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
        sure = withRhythm > 0 ? static_cast<double>(explained) / static_cast<double>(withRhythm) : 0.0;
        if (withRhythm > 0 && static_cast<double>(explained) < ExplainsTheTrack * static_cast<double>(withRhythm)) {
            std::size_t windowsSure = 0;
            std::size_t windowsTried = 0;
            followed = followWindows(onsets, *curve, period, phase, windowsSure, windowsTried);
            setOnOnsets(followed, onsets);
            moves = followed.size() >= 8;
            if (moves) {
                sure = windowsTried > 0 ? static_cast<double>(windowsSure) / static_cast<double>(windowsTried) : 0.0;
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
    return analysis;
}

}  // namespace seabass::domain
