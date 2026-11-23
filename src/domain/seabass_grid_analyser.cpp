// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/seabass_grid_analyser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
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
// the parabola through the value there and its two neighbours.
double parabolaPeak(const std::vector<double> &values, std::size_t i)
{
    if (i == 0 || i + 1 >= values.size()) {
        return static_cast<double>(i);
    }
    const double a = values[i - 1];
    const double b = values[i];
    const double c = values[i + 1];
    const double d = a - 2.0 * b + c;
    return d == 0.0 ? static_cast<double>(i) : static_cast<double>(i) + 0.5 * (a - c) / d;
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

// ------------------------------------------------------------------ tempo

// The tempos looked for, as beat periods in ms: 200 BPM to 60 BPM.
constexpr std::size_t ShortestPeriodMs = 300;
constexpr std::size_t LongestPeriodMs = 1000;
// Between a tempo and its double or its half the audio often cannot
// say; this can: a bell around 128 BPM, 0.45 octaves wide.
constexpr double PreferredBpm = 128.0;
constexpr double PreferenceOctaves = 0.45;

// The beat period in ms, or nothing when the onset function repeats at no
// lag at all.
std::optional<double> coarsePeriod(const std::vector<double> &onsets)
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
        const double octaves = std::log2(60000.0 / static_cast<double>(lag) / PreferredBpm) / PreferenceOctaves;
        score *= std::exp(-0.5 * octaves * octaves);
        if (best == 0 || score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    if (best == 0 || !(ac[best] > 0.0)) {
        return std::nullopt;
    }
    double period = parabolaPeak(ac, best);
    // The repeat 4 beats away is at 4 periods, known to the same
    // millisecond: that is the period four times finer. Then 16, then 64,
    // while the function still repeats that far away.
    for (const std::size_t beats : {std::size_t{4}, std::size_t{16}, std::size_t{64}}) {
        const double centre = period * static_cast<double>(beats);
        const auto reach = std::max<std::size_t>(3, static_cast<std::size_t>(0.01 * centre));
        const auto middle = static_cast<std::size_t>(centre);
        if (middle < reach || middle + reach + 1 >= ac.size()) {
            break;
        }
        std::size_t top = middle - reach;
        for (std::size_t lag = middle - reach; lag <= middle + reach; ++lag) {
            if (ac[lag] > ac[top]) {
                top = lag;
            }
        }
        constexpr double StillRepeats = 0.15;
        if (ac[top] < StillRepeats * ac[best]) {
            break;
        }
        period = parabolaPeak(ac, top) / static_cast<double>(beats);
    }
    return period;
}

// ------------------------------------------------------------------- folds

// The onset function over `beats` beats from `startMs`, summed onto one
// beat: about one step a millisecond. `used` is how many of the beats lay
// inside the track.
std::vector<double> fold(const std::vector<double> &onsets, double startMs, double period, std::size_t beats,
                         std::size_t &used)
{
    const auto steps = static_cast<std::size_t>(std::llround(period));
    const double step = period / static_cast<double>(steps);
    std::vector<double> total(steps, 0.0);
    used = 0;
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
// last is the first), with a raised-cosine window 9 ms wide, so a peak is
// one peak and not the ripple on it.
std::vector<double> smoothed(const std::vector<double> &profile)
{
    constexpr std::ptrdiff_t Reach = 5;
    std::array<double, 2 * Reach + 1> window{};
    double sum = 0.0;
    for (std::ptrdiff_t k = 0; k <= 2 * Reach; ++k) {
        window[static_cast<std::size_t>(k)] =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(2 * Reach));
        sum += window[static_cast<std::size_t>(k)];
    }
    const auto n = static_cast<std::ptrdiff_t>(profile.size());
    std::vector<double> result(profile.size(), 0.0);
    for (std::ptrdiff_t i = 0; i < n; ++i) {
        double value = 0.0;
        for (std::ptrdiff_t k = -Reach; k <= Reach; ++k) {
            value += window[static_cast<std::size_t>(k + Reach)] * profile[static_cast<std::size_t>(((i + k) % n + n) % n)];
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

// Where in the beat the whole track's fold peaks, in ms from 0.
double wholeTrackPhase(const std::vector<double> &onsets, double period)
{
    const auto beats = static_cast<std::size_t>(static_cast<double>(onsets.size() - 1) / period);
    std::size_t used = 0;
    const std::vector<double> profile = smoothed(fold(onsets, 0.0, period, beats, used));
    const double step = period / static_cast<double>(profile.size());
    return parabolaPeak(profile, indexOfMax(profile, 0, profile.size() - 1)) * step;
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
    const std::optional<double> coarse = coarsePeriod(onsets);
    if (!coarse) {
        return analysis;
    }
    double period = *coarse;
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
    for (std::size_t k = 0;; ++k) {
        const double time = first + static_cast<double>(k) * period;
        if (time > last) {
            break;
        }
        analysis.beats.push_back({time, 0});
    }
    analysis.confidence = windowsRead > 0 ? static_cast<double>(windowsKept) / static_cast<double>(windowsRead) : 0.0;
    return analysis;
}

}  // namespace seabass::domain
