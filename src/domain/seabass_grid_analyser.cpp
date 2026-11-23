// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/seabass_grid_analyser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
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

// Forwards and then backwards, so what the filter delays one way it
// advances the other: an onset stays where it is in time.
void runBothWays(const Biquad &f, std::vector<double> &x)
{
    run(f, x);
    std::reverse(x.begin(), x.end());
    run(f, x);
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
    // ones: a twiddle built by repeated multiplication drifts.
    std::vector<std::complex<double>> twiddle(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) {
        twiddle[k] = std::polar(1.0, sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n));
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
// The envelope is the band's power smoothed at this frequency: slow
// enough to be an envelope, fast enough that a kick's rise takes a few
// milliseconds of it and not twenty.
constexpr double EnvelopeSmoothingHz = 60.0;

// One value a millisecond: value i is at i ms on the source's timeline.
std::vector<double> onsetFunction(const std::vector<float> &samples, int sampleRate)
{
    // Half the rate is enough for 5 kHz and halves the work: every other
    // sample of the audio, low-passed both ways first so nothing folds
    // down and nothing moves in time.
    std::vector<double> audio(samples.begin(), samples.end());
    double rate = static_cast<double>(sampleRate);
    if (rate >= 40000.0) {
        for (const double q : ButterworthFourthQ) {
            runBothWays(lowpass(0.2 * rate, rate, q), audio);
        }
        std::vector<double> halved((audio.size() + 1) / 2);
        for (std::size_t i = 0; i < halved.size(); ++i) {
            halved[i] = audio[2 * i];
        }
        audio = std::move(halved);
        rate /= 2.0;
    }
    if (audio.size() < 2) {
        return {};
    }

    const auto milliseconds = static_cast<std::size_t>(static_cast<double>(audio.size() - 1) * 1000.0 / rate);
    std::vector<double> onsets(milliseconds, 0.0);
    std::vector<double> band;
    std::vector<double> envelope(milliseconds);
    for (const auto &edges : Bands) {
        band = audio;
        const double top = std::min(edges[1], 0.45 * rate);
        for (const double q : ButterworthFourthQ) {
            runBothWays(highpass(edges[0], rate, q), band);
            runBothWays(lowpass(top, rate, q), band);
        }
        for (double &value : band) {
            value *= value;
        }
        runBothWays(lowpass(EnvelopeSmoothingHz, rate, std::numbers::sqrt2 / 2.0), band);
        for (std::size_t ms = 0; ms < milliseconds; ++ms) {
            const double at = static_cast<double>(ms) * rate / 1000.0;
            const auto below = static_cast<std::size_t>(at);
            const double part = at - static_cast<double>(below);
            const double power = band[below] * (1.0 - part) + band[below + 1] * part;
            envelope[ms] = std::sqrt(std::max(power, 0.0));
        }
        // The rises only, and each band counted alike whatever its level.
        double sum = 0.0;
        double level = 0.0;
        for (std::size_t ms = 1; ms < milliseconds; ++ms) {
            sum += std::max(envelope[ms] - envelope[ms - 1], 0.0);
            level += envelope[ms];
        }
        // A band that holds a level and hardly ever rises (a tone, a pad,
        // the hum under a recording) has no onsets, only the ripple of
        // its own smoothing; scaled up like the others it would be taken
        // for a rhythm. Drums rise by several times the band's mean level
        // every second, a held note by next to nothing.
        constexpr double RisesASecondOfTheLevel = 0.2;
        if (!(sum > RisesASecondOfTheLevel * level / 1000.0)) {
            continue;
        }
        const double scale = static_cast<double>(milliseconds) / sum;
        for (std::size_t ms = 1; ms < milliseconds; ++ms) {
            onsets[ms] += scale * std::max(envelope[ms] - envelope[ms - 1], 0.0);
        }
    }
    return onsets;
}

// ------------------------------------------------------------- the limits

// The tempos looked for, as beat periods in ms: 200 BPM to 60 BPM.
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
        const auto reach = std::max<std::size_t>(3, static_cast<std::size_t>(0.01 * centre));
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
// lag at all.
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
    if (best == 0 || !(ac[best] > 0.0)) {
        return std::nullopt;
    }
    constexpr double StillRepeats = 0.15;
    const double floor = StillRepeats * ac[best];
    const double base = parabolaPeak(ac, best);

    // That lag is a level of the rhythm, not always the beat: a dotted
    // rhythm at 95 BPM repeats well at three quarters of its beat, which
    // the scoring above takes for 127. So the lag's relatives are tried:
    // the whole track folded at each, and the one that puts the most of
    // the track's onsets into one moment of its fold is the beat, the
    // preference for likely tempos deciding between a period and its
    // double, which fold alike.
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
            reading.offsetMs = (parabolaPeak(profile, top) - static_cast<double>(middle)) * step;
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
    Line line;
    for (int round = 0; round < 2; ++round) {
        double weight = 0.0;
        double meanTime = 0.0;
        double meanOffset = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 0; i < readings.size(); ++i) {
            if (keep[i]) {
                weight += readings[i].strength;
                meanTime += readings[i].strength * readings[i].centreMs;
                meanOffset += readings[i].strength * readings[i].offsetMs;
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
                covariance += readings[i].strength * dt * (readings[i].offsetMs - meanOffset);
                variance += readings[i].strength * dt * dt;
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
            keep[i] = std::isfinite(readings[i].offsetMs) && std::abs(left[i]) <= bound;
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
            / static_cast<double>(2 * Half);
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
            row[k] = std::max(z[lag].real() + 0.5 * z[std::min(2 * lag, Transform / 2 - 1)].real(), 0.0);
        }
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
    // elsewhere between the windows that have.
    constexpr double HasAPeak = 0.3;
    curve.periodsMs.assign(scores.size(), std::numeric_limits<double>::quiet_NaN());
    curve.rhythm.assign(scores.size(), false);
    std::vector<std::size_t> known;
    for (std::size_t w = 0; w < scores.size(); ++w) {
        const std::size_t k = path[w];
        if (scores[w][k] >= HasAPeak && k > 0 && k + 1 < lags) {
            curve.periodsMs[w] = static_cast<double>(firstLag) + parabolaPeak(scores[w], k);
            curve.rhythm[w] = true;
            known.push_back(w);
        }
    }
    if (known.empty()) {
        return std::nullopt;
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
std::vector<double> followWindows(const std::vector<double> &onsets, const TempoCurve &curve)
{
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
            if (next < 0.0 || next + 9.0 * tau >= last) {
                // No room for another window: on at the curve's period
                // to the edge of the track.
                for (double step = b; step >= 0.0 && step <= last; step += direction * curve.at(step)) {
                    beats.push_back(step);
                }
                break;
            }
            const double here = curve.at(next);
            std::size_t used = 0;
            const std::vector<double> folded = fold(onsets, next - here / 2.0, here, WindowBeats, used);
            const std::vector<double> profile = smoothed(folded);
            const double step = here / static_cast<double>(profile.size());
            const std::size_t middle = profile.size() / 2;
            const auto reach = std::max<std::size_t>(2, static_cast<std::size_t>(Reach * static_cast<double>(profile.size())));
            double correction = 0.0;
            bool corrects = false;
            if (used == WindowBeats && middle > reach && middle + reach < profile.size()) {
                const std::size_t highestAt = indexOfMax(profile, 0, profile.size() - 1);
                const std::size_t top = indexOfMax(profile, middle - reach, middle + reach);
                const bool clearly = profile[top] >= NearTheHighest * profile[highestAt] && top != middle - reach
                    && top != middle + reach && profile[top] >= HasDrums * strongest;
                if (clearly) {
                    correction = (parabolaPeak(profile, top) - static_cast<double>(middle)) * step;
                    lost = 0;
                    corrects = true;
                } else if (profile[highestAt] >= DrumsElsewhere * strongest && ++lost >= WindowsLost) {
                    correction = (heaviestPlace(folded) - static_cast<double>(middle)) * step;
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

}  // namespace

GridAnalysis SeabassGridAnalyser::analyse(PcmSource &source)
{
    GridAnalysis analysis;
    const int rate = source.sampleRate();
    const std::vector<float> samples = readAllSamples(source);
    if (!source.error().empty()) {
        analysis.error = source.error();
        return analysis;
    }
    if (rate <= 0 || samples.empty()) {
        analysis.error = "no samples";
        return analysis;
    }

    const std::vector<double> onsets = onsetFunction(samples, rate);
    // Under four beats of the slowest tempo there is nothing to repeat.
    if (onsets.size() < 4 * LongestPeriodMs) {
        return analysis;
    }
    const std::optional<double> coarse = beatPeriod(onsets);
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
    if (const std::optional<TempoCurve> curve = tempoCurve(levelled(onsets), period)) {
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
        if (withRhythm > 0 && static_cast<double>(explained) < ExplainsTheTrack * static_cast<double>(withRhythm)) {
            followed = followWindows(onsets, *curve);
            moves = followed.size() >= 8;
        }
    }
    const std::vector<double> &grid = moves ? followed : straight;
    for (const double time : grid) {
        analysis.beats.push_back({time, 0});
    }
    analysis.confidence = windowsRead > 0 ? static_cast<double>(windowsKept) / static_cast<double>(windowsRead) : 0.0;
    return analysis;
}

}  // namespace seabass::domain
