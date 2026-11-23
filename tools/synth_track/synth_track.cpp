// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "synth_track/synth_track.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>

namespace seabass::synth
{

namespace
{

constexpr double TwoPi = 2.0 * std::numbers::pi;

std::uint64_t splitMix64(std::uint64_t &x)
{
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

double midiToHz(double note)
{
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
}

// A sawtooth with the step at each wrap smoothed over a sample either
// side (polyBLEP), so a pad of them does not alias into a hiss.
double polyBlepSaw(double phase, double increment)
{
    double value = 2.0 * phase - 1.0;
    if (phase < increment) {
        const double t = phase / increment;
        value -= t + t - t * t - 1.0;
    } else if (phase > 1.0 - increment) {
        const double t = (phase - 1.0) / increment;
        value -= t * t + t + t + 1.0;
    }
    return value;
}

// The second-order filters of the RBJ cookbook, direct form I.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    static Biquad make(double sampleRate, double hz, double q, bool highPass, bool bandPass = false)
    {
        const double w = TwoPi * hz / sampleRate;
        const double cosw = std::cos(w);
        const double alpha = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad f;
        if (bandPass) {
            f.b0 = alpha / a0;
            f.b1 = 0.0;
            f.b2 = -alpha / a0;
        } else if (highPass) {
            f.b0 = (1.0 + cosw) / 2.0 / a0;
            f.b1 = -(1.0 + cosw) / a0;
            f.b2 = f.b0;
        } else {
            f.b0 = (1.0 - cosw) / 2.0 / a0;
            f.b1 = (1.0 - cosw) / a0;
            f.b2 = f.b0;
        }
        f.a1 = -2.0 * cosw / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    double process(double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// Where a fraction `f` of beat k falls, on a grid that may move: a
// straight line between beat k and beat k + 1.
double timeInBeat(const std::vector<double> &beatTimes, int k, double f)
{
    const double start = beatTimes[static_cast<std::size_t>(k)];
    const double next = beatTimes[static_cast<std::size_t>(k) + 1];
    return start + f * (next - start);
}

class Renderer
{
public:
    Renderer(const SynthTrackSpec &spec, std::size_t length)
        : m_rate(static_cast<double>(spec.sampleRate)), m_random(spec.seed), m_drums(length, 0.0),
          m_send(length, 0.0), m_music(length, 0.0)
    {
    }

    Random &random() { return m_random; }
    std::vector<double> &drums() { return m_drums; }
    std::vector<double> &send() { return m_send; }
    std::vector<double> &music() { return m_music; }

    // The first sample at or after `timeMs`, and that sample's time from
    // it in seconds: every voice is sampled from its true start, between
    // samples or not, so a hit is where the grid says to the sample's
    // fraction.
    struct Start
    {
        std::size_t index = 0;
        double offsetSeconds = 0.0;
    };
    Start startOf(double timeMs) const
    {
        const double exact = std::max(0.0, timeMs) * m_rate / 1000.0;
        const double first = std::ceil(exact);
        return {static_cast<std::size_t>(first), (first - exact) / m_rate};
    }

    void kick(double timeMs, double gain)
    {
        const Start s = startOf(timeMs);
        const auto length = static_cast<std::size_t>(0.45 * m_rate);
        for (std::size_t i = 0; i < length && s.index + i < m_drums.size(); ++i) {
            const double t = s.offsetSeconds + static_cast<double>(i) / m_rate;
            // A sine swept from 155 Hz down to 45, started on its crest so
            // that the hit begins at full strength on the beat.
            const double phase = TwoPi * (45.0 * t + 110.0 * 0.025 * (1.0 - std::exp(-t / 0.025)));
            double value = std::cos(phase) * std::exp(-t / 0.16);
            // The beater's click.
            value += 0.25 * m_random.bipolar() * std::exp(-t / 0.0008);
            m_drums[s.index + i] += gain * value;
        }
    }

    void snare(double timeMs, double gain)
    {
        const Start s = startOf(timeMs);
        const auto length = static_cast<std::size_t>(0.4 * m_rate);
        Biquad highPass = Biquad::make(m_rate, 900.0, 0.7, true);
        for (std::size_t i = 0; i < length && s.index + i < m_drums.size(); ++i) {
            const double t = s.offsetSeconds + static_cast<double>(i) / m_rate;
            const double tone = std::sin(TwoPi * 185.0 * t) * std::exp(-t / 0.045);
            const double noise = highPass.process(m_random.bipolar()) * std::exp(-t / 0.11);
            const double value = gain * (0.45 * tone + 0.6 * noise);
            m_drums[s.index + i] += value;
            m_send[s.index + i] += 0.6 * value;
        }
    }

    void clap(double timeMs, double gain)
    {
        const Start s = startOf(timeMs);
        const auto length = static_cast<std::size_t>(0.35 * m_rate);
        Biquad bandPass = Biquad::make(m_rate, 1400.0, 1.2, false, true);
        for (std::size_t i = 0; i < length && s.index + i < m_drums.size(); ++i) {
            const double t = s.offsetSeconds + static_cast<double>(i) / m_rate;
            // Three hands a few ms apart, then the room.
            double envelope = 0.0;
            for (const double at : {0.0, 0.009, 0.018}) {
                if (t >= at) {
                    envelope += std::exp(-(t - at) / 0.005);
                }
            }
            if (t >= 0.022) {
                envelope += 0.5 * std::exp(-(t - 0.022) / 0.07);
            }
            const double value = gain * 1.6 * bandPass.process(m_random.bipolar()) * envelope;
            m_drums[s.index + i] += value;
            m_send[s.index + i] += 0.7 * value;
        }
    }

    void hat(double timeMs, double gain, bool open)
    {
        const Start s = startOf(timeMs);
        const double decay = open ? 0.16 : 0.03;
        const auto length = static_cast<std::size_t>(decay * 6.0 * m_rate);
        Biquad highPass = Biquad::make(m_rate, 7000.0, 0.8, true);
        for (std::size_t i = 0; i < length && s.index + i < m_drums.size(); ++i) {
            const double t = s.offsetSeconds + static_cast<double>(i) / m_rate;
            const double value = gain * 0.35 * highPass.process(m_random.bipolar()) * std::exp(-t / decay);
            m_drums[s.index + i] += value;
            m_send[s.index + i] += 0.3 * value;
        }
    }

    void bass(double timeMs, double lengthMs, double midiNote, double gain)
    {
        const Start s = startOf(timeMs);
        const double hz = midiToHz(midiNote);
        const double seconds = lengthMs / 1000.0;
        const auto length = static_cast<std::size_t>((seconds + 0.04) * m_rate);
        Biquad lowPass = Biquad::make(m_rate, 420.0, 0.9, false);
        for (std::size_t i = 0; i < length && s.index + i < m_music.size(); ++i) {
            const double t = s.offsetSeconds + static_cast<double>(i) / m_rate;
            double cycles = hz * t;
            cycles -= std::floor(cycles);
            const double saw = polyBlepSaw(cycles, hz / m_rate);
            const double attack = std::min(1.0, t / 0.004);
            const double release = t <= seconds ? 1.0 : std::exp(-(t - seconds) / 0.012);
            const double value = 0.55 * std::sin(TwoPi * cycles) + 0.35 * saw;
            m_music[s.index + i] += gain * lowPass.process(value) * attack * release * std::exp(-t / 0.6);
        }
    }

    void padNote(double timeMs, double lengthMs, double midiNote, double gain)
    {
        const Start s = startOf(timeMs);
        const double seconds = lengthMs / 1000.0;
        const auto length = static_cast<std::size_t>((seconds + 0.6) * m_rate);
        // Two saws a few cents apart, for the slow beating of a pad.
        const double hzA = midiToHz(midiNote - 0.07);
        const double hzB = midiToHz(midiNote + 0.07);
        Biquad lowPass = Biquad::make(m_rate, 1500.0, 0.7, false);
        for (std::size_t i = 0; i < length && s.index + i < m_music.size(); ++i) {
            const double t = s.offsetSeconds + static_cast<double>(i) / m_rate;
            double a = hzA * t;
            double b = hzB * t + 0.37;
            a -= std::floor(a);
            b -= std::floor(b);
            const double attack = std::min(1.0, t / 0.3);
            const double release = t <= seconds ? 1.0 : std::exp(-(t - seconds) / 0.15);
            const double value = polyBlepSaw(a, hzA / m_rate) + polyBlepSaw(b, hzB / m_rate);
            const double out = gain * lowPass.process(value) * attack * release;
            m_music[s.index + i] += out;
            m_send[s.index + i] += 0.5 * out;
        }
    }

private:
    double m_rate;
    Random m_random;
    std::vector<double> m_drums;
    std::vector<double> m_send;
    std::vector<double> m_music;
};

// Schroeder's reverb: four combs side by side (damped, as Freeverb does),
// then two allpasses in a row. Delays from Freeverb's, scaled to the rate.
std::vector<double> schroederReverb(const std::vector<double> &input, double sampleRate)
{
    const double scale = sampleRate / 44100.0;
    struct Comb
    {
        std::vector<double> line;
        std::size_t at = 0;
        double store = 0.0;
    };
    struct Allpass
    {
        std::vector<double> line;
        std::size_t at = 0;
    };
    std::array<Comb, 4> combs;
    const std::array<int, 4> combDelays{1557, 1617, 1491, 1422};
    for (std::size_t i = 0; i < combs.size(); ++i) {
        combs[i].line.assign(static_cast<std::size_t>(combDelays[i] * scale), 0.0);
    }
    std::array<Allpass, 2> allpasses;
    const std::array<int, 2> allpassDelays{556, 225};
    for (std::size_t i = 0; i < allpasses.size(); ++i) {
        allpasses[i].line.assign(static_cast<std::size_t>(allpassDelays[i] * scale), 0.0);
    }
    constexpr double Feedback = 0.8;
    constexpr double Damping = 0.3;
    std::vector<double> out(input.size(), 0.0);
    for (std::size_t n = 0; n < input.size(); ++n) {
        double sum = 0.0;
        for (Comb &comb : combs) {
            const double delayed = comb.line[comb.at];
            comb.store = delayed * (1.0 - Damping) + comb.store * Damping;
            comb.line[comb.at] = input[n] + comb.store * Feedback;
            comb.at = (comb.at + 1) % comb.line.size();
            sum += delayed;
        }
        double value = sum * 0.25;
        for (Allpass &allpass : allpasses) {
            const double delayed = allpass.line[allpass.at];
            const double stored = value + delayed * 0.5;
            allpass.line[allpass.at] = stored;
            allpass.at = (allpass.at + 1) % allpass.line.size();
            value = delayed - stored * 0.5;
        }
        out[n] = value;
    }
    return out;
}

// The chord of each bar, four bars round: i VI III VII in a minor key,
// I V vi IV in a major one. Semitones above the tonic.
std::array<int, 3> chordOfBar(int bar, bool minor)
{
    static constexpr std::array<std::array<int, 3>, 4> minorChords{{{0, 3, 7}, {8, 12, 15}, {3, 7, 10}, {10, 14, 17}}};
    static constexpr std::array<std::array<int, 3>, 4> majorChords{{{0, 4, 7}, {7, 11, 14}, {9, 12, 16}, {5, 9, 12}}};
    return minor ? minorChords[static_cast<std::size_t>(bar % 4)] : majorChords[static_cast<std::size_t>(bar % 4)];
}

struct Step
{
    int sixteenth;  // 0 to 15 in the bar
    double gain;
};

std::vector<Step> kickSteps(DrumPattern pattern)
{
    switch (pattern) {
    case DrumPattern::FourOnTheFloor:
        return {{0, 1.0}, {4, 1.0}, {8, 1.0}, {12, 1.0}};
    case DrumPattern::Rock:
        return {{0, 1.0}, {8, 1.0}, {10, 0.8}};
    case DrumPattern::Breakbeat:
        return {{0, 1.0}, {6, 0.85}, {10, 0.9}};
    case DrumPattern::HalfTime:
        return {{0, 1.0}, {10, 0.9}};
    }
    return {};
}

std::vector<Step> snareSteps(DrumPattern pattern)
{
    switch (pattern) {
    case DrumPattern::FourOnTheFloor:
    case DrumPattern::Rock:
        return {{4, 0.9}, {12, 0.9}};
    case DrumPattern::Breakbeat:
        return {{4, 0.9}, {7, 0.25}, {12, 0.9}, {15, 0.3}};
    case DrumPattern::HalfTime:
        return {{8, 1.0}};
    }
    return {};
}

std::vector<Step> hatSteps(DrumPattern pattern)
{
    std::vector<Step> steps;
    switch (pattern) {
    case DrumPattern::FourOnTheFloor:
        for (int s = 0; s < 16; s += 2) {
            steps.push_back({s, s % 4 == 2 ? 0.9 : 0.35});
        }
        break;
    case DrumPattern::Rock:
    case DrumPattern::HalfTime:
        for (int s = 0; s < 16; s += 2) {
            steps.push_back({s, s % 4 == 0 ? 0.7 : 0.5});
        }
        break;
    case DrumPattern::Breakbeat:
        for (int s = 0; s < 16; ++s) {
            steps.push_back({s, s % 2 == 0 ? 0.6 : 0.3});
        }
        break;
    }
    return steps;
}

}  // namespace

Random::Random(std::uint64_t seed)
{
    std::uint64_t x = seed;
    for (std::uint64_t &word : m_state) {
        word = splitMix64(x);
    }
}

// xoshiro256** (Blackman and Vigna).
std::uint64_t Random::next()
{
    const std::uint64_t result = rotl(m_state[1] * 5, 7) * 9;
    const std::uint64_t t = m_state[1] << 17;
    m_state[2] ^= m_state[0];
    m_state[3] ^= m_state[1];
    m_state[1] ^= m_state[2];
    m_state[0] ^= m_state[3];
    m_state[2] ^= t;
    m_state[3] = rotl(m_state[3], 45);
    return result;
}

double Random::uniform()
{
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
}

double Random::normal()
{
    if (m_haveSpare) {
        m_haveSpare = false;
        return m_spare;
    }
    // 1 - uniform() is in (0, 1], so the logarithm is finite.
    const double radius = std::sqrt(-2.0 * std::log(1.0 - uniform()));
    const double angle = TwoPi * uniform();
    m_spare = radius * std::sin(angle);
    m_haveSpare = true;
    return radius * std::cos(angle);
}

double bpmOfInterval(const TempoProgramme &tempo, int k)
{
    double bpm = 120.0;
    int first = 0;
    bool found = false;
    for (const TempoSegment &segment : tempo.segments) {
        if (k < first + segment.beats) {
            const double along = static_cast<double>(k - first) / static_cast<double>(segment.beats);
            bpm = segment.startBpm + (segment.endBpm - segment.startBpm) * along;
            found = true;
            break;
        }
        first += segment.beats;
    }
    if (!found && !tempo.segments.empty()) {
        bpm = tempo.segments.back().endBpm;
    }
    if (tempo.driftBpm != 0.0 && tempo.driftPeriodBeats > 0.0) {
        bpm += tempo.driftBpm * std::sin(TwoPi * static_cast<double>(k) / tempo.driftPeriodBeats);
    }
    return bpm;
}

const char *drumPatternName(DrumPattern pattern)
{
    switch (pattern) {
    case DrumPattern::FourOnTheFloor:
        return "four-on-the-floor";
    case DrumPattern::Rock:
        return "rock";
    case DrumPattern::Breakbeat:
        return "breakbeat";
    case DrumPattern::HalfTime:
        return "half-time";
    }
    return "?";
}

const char *instrumentName(Instrument instrument)
{
    switch (instrument) {
    case Instrument::Kick:
        return "kick";
    case Instrument::Snare:
        return "snare";
    case Instrument::Clap:
        return "clap";
    case Instrument::Hat:
        return "hat";
    case Instrument::OpenHat:
        return "open-hat";
    case Instrument::Bass:
        return "bass";
    case Instrument::Pad:
        return "pad";
    }
    return "?";
}

int totalBars(const SynthTrackSpec &spec)
{
    int bars = 0;
    for (const Section &section : spec.sections) {
        bars += section.bars;
    }
    return bars;
}

namespace
{

// Every beat time, one more than the bars hold: the end of the last bar,
// so that the last beat has a length to divide.
std::vector<double> beatTimesWithEnd(const SynthTrackSpec &spec)
{
    const int beats = totalBars(spec) * 4;
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(beats) + 1);
    double time = spec.firstBeatMs;
    for (int k = 0; k <= beats; ++k) {
        times.push_back(time);
        time += 60000.0 / bpmOfInterval(spec.tempo, k);
    }
    return times;
}

}  // namespace

std::vector<domain::Beat> synthGrid(const SynthTrackSpec &spec)
{
    const std::vector<double> times = beatTimesWithEnd(spec);
    std::vector<domain::Beat> beats;
    beats.reserve(times.size());
    for (std::size_t k = 0; k + 1 < times.size(); ++k) {
        beats.push_back({times[k], static_cast<int>(k % 4) + 1});
    }
    return beats;
}

SynthTrack renderSynthTrack(const SynthTrackSpec &spec)
{
    SynthTrack track;
    track.sampleRate = spec.sampleRate;
    track.beats = synthGrid(spec);
    const std::vector<double> times = beatTimesWithEnd(spec);
    const double endMs = times.back() + spec.tailMs;
    const auto length = static_cast<std::size_t>(std::ceil(endMs * spec.sampleRate / 1000.0));
    Renderer renderer(spec, length);
    Random &random = renderer.random();

    const auto stepTime = [&](int bar, int sixteenth) {
        const int beat = bar * 4 + sixteenth / 4;
        const int sub = sixteenth % 4;
        double fraction = 0.0;
        if (spec.swingSixteenths) {
            const double eighth = sub < 2 ? 0.0 : 0.5;
            fraction = eighth + (sub % 2 == 1 ? spec.swing * 0.5 : 0.0);
        } else {
            fraction = sub * 0.25;
            if (sub == 2) {
                fraction = spec.swing;
            }
        }
        return timeInBeat(times, beat, fraction);
    };
    const auto jitter = [&]() {
        if (spec.jitterMs <= 0.0) {
            return 0.0;
        }
        return std::clamp(random.normal(), -3.0, 3.0) * spec.jitterMs;
    };
    const auto beatOf = [](int bar, int sixteenth) { return sixteenth % 4 == 0 ? bar * 4 + sixteenth / 4 : -1; };

    std::vector<double> kickTimes;
    int bar = 0;
    for (const Section &section : spec.sections) {
        for (int inSection = 0; inSection < section.bars; ++inSection, ++bar) {
            const bool lastBar = inSection == section.bars - 1;
            if (section.kick) {
                for (const Step &step : kickSteps(spec.pattern)) {
                    const double at = stepTime(bar, step.sixteenth) + jitter();
                    renderer.kick(at, 0.95 * step.gain);
                    kickTimes.push_back(at);
                    track.hits.push_back({at, Instrument::Kick, beatOf(bar, step.sixteenth)});
                }
            }
            if (section.snare) {
                for (const Step &step : snareSteps(spec.pattern)) {
                    const double at = stepTime(bar, step.sixteenth) + jitter();
                    const Instrument which = spec.clap && step.gain > 0.5 ? Instrument::Clap : Instrument::Snare;
                    if (which == Instrument::Clap) {
                        renderer.clap(at, 0.7 * step.gain);
                    } else {
                        renderer.snare(at, 0.7 * step.gain);
                    }
                    track.hits.push_back({at, which, beatOf(bar, step.sixteenth)});
                }
            }
            if (section.fill && lastBar) {
                for (int sixteenth = 12; sixteenth < 16; ++sixteenth) {
                    const double at = stepTime(bar, sixteenth) + jitter();
                    renderer.snare(at, 0.35 + 0.1 * (sixteenth - 12));
                    track.hits.push_back({at, Instrument::Snare, beatOf(bar, sixteenth)});
                }
            }
            if (section.hats) {
                for (const Step &step : hatSteps(spec.pattern)) {
                    const double at = stepTime(bar, step.sixteenth) + jitter();
                    // An open hat on the last off-beat of every second bar.
                    const bool open = step.sixteenth == 14 && bar % 2 == 1;
                    renderer.hat(at, 0.6 * step.gain, open);
                    track.hits.push_back({at, open ? Instrument::OpenHat : Instrument::Hat, beatOf(bar, step.sixteenth)});
                }
            }
            const std::array<int, 3> chord = chordOfBar(bar, spec.minor);
            if (section.bass) {
                const double beatMs = times[static_cast<std::size_t>(bar) * 4 + 1] - times[static_cast<std::size_t>(bar) * 4];
                const double root = spec.keyRootMidi + chord[0] - (chord[0] > 7 ? 12 : 0);
                if (spec.pattern == DrumPattern::FourOnTheFloor) {
                    // The off-beat bass of house.
                    for (int sixteenth : {2, 6, 10, 14}) {
                        const double at = stepTime(bar, sixteenth);
                        renderer.bass(at, 0.42 * beatMs, root, 0.5);
                        track.hits.push_back({at, Instrument::Bass, -1});
                    }
                } else {
                    for (const auto [sixteenth, beats] : {std::pair{0, 1.6}, std::pair{10, 1.2}}) {
                        const double at = stepTime(bar, sixteenth);
                        renderer.bass(at, beats * beatMs, root + (sixteenth == 10 ? 7 : 0), 0.5);
                        track.hits.push_back({at, Instrument::Bass, beatOf(bar, sixteenth)});
                    }
                }
            }
            if (section.pad) {
                const double at = times[static_cast<std::size_t>(bar) * 4];
                const double lengthMs = times[static_cast<std::size_t>(bar) * 4 + 4] - at;
                for (const int interval : chord) {
                    renderer.padNote(at, lengthMs, spec.keyRootMidi + 12 + interval, 0.05);
                }
                track.hits.push_back({at, Instrument::Pad, bar * 4});
            }
        }
    }
    std::sort(track.hits.begin(), track.hits.end(),
              [](const SynthHit &a, const SynthHit &b) { return a.timeMs < b.timeMs; });
    std::sort(kickTimes.begin(), kickTimes.end());

    // The bass and pad ducked under each kick: down by 70 percent at the
    // hit, back in about a quarter of a second.
    std::vector<double> &music = renderer.music();
    std::size_t nextKick = 0;
    double lastKickSeconds = -1e9;
    const double rate = static_cast<double>(spec.sampleRate);
    for (std::size_t n = 0; n < music.size(); ++n) {
        const double seconds = static_cast<double>(n) / rate;
        while (nextKick < kickTimes.size() && kickTimes[nextKick] / 1000.0 <= seconds) {
            lastKickSeconds = kickTimes[nextKick] / 1000.0;
            ++nextKick;
        }
        const double since = seconds - lastKickSeconds;
        music[n] *= 1.0 - 0.7 * std::exp(-since / 0.09);
    }

    const std::vector<double> wet = schroederReverb(renderer.send(), rate);
    const std::vector<double> &drums = renderer.drums();
    std::vector<double> mix(length);
    double peak = 0.0;
    for (std::size_t n = 0; n < length; ++n) {
        mix[n] = drums[n] + music[n] + 0.35 * wet[n];
        peak = std::max(peak, std::abs(mix[n]));
    }
    // A soft limiter: the loudest moment taken to 1, then a tanh curve
    // that leaves quiet parts nearly as they were and rounds the peaks,
    // ending at -1 dBFS.
    const double toUnit = peak > 0.0 ? 1.0 / peak : 0.0;
    constexpr double Drive = 1.6;
    const double ceiling = std::pow(10.0, -1.0 / 20.0) / std::tanh(Drive);
    track.samples.resize(length);
    for (std::size_t n = 0; n < length; ++n) {
        track.samples[n] = static_cast<float>(ceiling * std::tanh(Drive * mix[n] * toUnit));
    }
    return track;
}

namespace
{

Section section(const char *name, int bars, const char *parts, bool fill = false)
{
    // `parts`: k kick, s snare, h hats, b bass, p pad.
    const std::string p(parts);
    Section s;
    s.name = name;
    s.bars = bars;
    s.kick = p.find('k') != std::string::npos;
    s.snare = p.find('s') != std::string::npos;
    s.hats = p.find('h') != std::string::npos;
    s.bass = p.find('b') != std::string::npos;
    s.pad = p.find('p') != std::string::npos;
    s.fill = fill;
    return s;
}

// Intro, main part, breakdown, drop, outro.
std::vector<Section> clubArrangement(int breakdownBars = 8)
{
    return {section("intro", 8, "kh"),
            section("main", 16, "kshbp"),
            section("breakdown", breakdownBars, "hp", true),
            section("drop", 16, "kshbp"),
            section("outro", 8, "khb")};
}

SynthTrackSpec base(const char *name, const char *description, double bpm, std::uint64_t seed)
{
    SynthTrackSpec spec;
    spec.name = name;
    spec.description = description;
    spec.seed = seed;
    spec.tempo.segments = {{1, bpm, bpm}};
    spec.sections = clubArrangement();
    return spec;
}

std::vector<SynthTrackSpec> presets()
{
    std::vector<SynthTrackSpec> all;

    all.push_back(base("house-124", "constant 124 BPM", 124.0, 101));

    {
        SynthTrackSpec s = base("trance-137.98", "constant 137.98 BPM with a clap", 137.98, 102);
        s.clap = true;
        s.keyRootMidi = 48;
        s.keyName = "C minor";
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("drift-122", "122 BPM drifting 0.5 BPM either way over 96 beats", 122.0, 103);
        s.tempo.driftBpm = 0.5;
        s.tempo.driftPeriodBeats = 96.0;
        s.keyRootMidi = 43;
        s.keyName = "G minor";
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("ramp-100-125", "100 BPM for 16 beats, a ramp to 125 over 128 beats, then 125", 100.0, 104);
        s.tempo.segments = {{16, 100.0, 100.0}, {128, 100.0, 125.0}, {1, 125.0, 125.0}};
        s.keyRootMidi = 50;
        s.minor = false;
        s.keyName = "D major";
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("step-120-126", "120 BPM for 64 beats, then 126", 120.0, 105);
        s.tempo.segments = {{64, 120.0, 120.0}, {1, 126.0, 126.0}};
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("alternating-127.5-128", "runs of 32 beats at 127.5 and 128 by turns", 128.0, 106);
        s.tempo.segments.clear();
        for (int run = 0; run < 7; ++run) {
            const double bpm = run % 2 == 0 ? 127.5 : 128.0;
            s.tempo.segments.push_back({32, bpm, bpm});
        }
        s.clap = true;
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("live-drummer-96", "a drummer at 96 BPM, hits scattered 8 ms around a gently drifting grid",
                                96.0, 107);
        s.pattern = DrumPattern::Rock;
        s.jitterMs = 8.0;
        s.tempo.driftBpm = 0.8;
        s.tempo.driftPeriodBeats = 128.0;
        s.keyRootMidi = 40;
        s.minor = false;
        s.keyName = "E major";
        s.sections = {section("intro", 4, "kh"), section("verse", 16, "kshb"), section("chorus", 16, "kshbp", true),
                      section("verse", 8, "kshb")};
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("breakbeat-swing-132", "a broken beat at 132 BPM, sixteenths swung to 58 percent", 132.0, 108);
        s.pattern = DrumPattern::Breakbeat;
        s.swing = 0.58;
        s.swingSixteenths = true;
        s.keyRootMidi = 41;
        s.keyName = "F minor";
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("long-breakdown-126", "126 BPM with 32 bars of breakdown and no kick in them", 126.0, 109);
        s.sections = clubArrangement(32);
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("drumless-intro-128", "16 bars of pad alone, the first beat at 2.345 s", 128.0, 110);
        s.firstBeatMs = 2345.0;
        s.sections = {section("intro", 16, "p"), section("build", 8, "hbp"), section("main", 16, "kshbp"),
                      section("outro", 8, "kh")};
        s.minor = false;
        s.keyRootMidi = 43;
        s.keyName = "G major";
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("house-124-48k", "constant 124 BPM at 48 kHz", 124.0, 111);
        s.sampleRate = 48000;
        all.push_back(s);
    }
    {
        SynthTrackSpec s = base("halftime-174", "drum and bass at 174 BPM played half time: snare on 3 only", 174.0, 112);
        s.pattern = DrumPattern::HalfTime;
        s.keyRootMidi = 38;
        s.keyName = "D minor";
        all.push_back(s);
    }
    return all;
}

}  // namespace

std::vector<std::string> synthPresetNames()
{
    std::vector<std::string> names;
    for (const SynthTrackSpec &spec : presets()) {
        names.push_back(spec.name);
    }
    return names;
}

std::optional<SynthTrackSpec> synthPreset(const std::string &name)
{
    for (SynthTrackSpec &spec : presets()) {
        if (spec.name == name) {
            return spec;
        }
    }
    return std::nullopt;
}

namespace
{

void putLe(std::string &out, std::uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

}  // namespace

bool writeWav16(const std::string &path, const std::vector<float> &samples, int sampleRate, std::string *error)
{
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::string bytes;
    bytes.reserve(44 + dataBytes);
    bytes += "RIFF";
    putLe(bytes, 36 + dataBytes, 4);
    bytes += "WAVEfmt ";
    putLe(bytes, 16, 4);
    putLe(bytes, 1, 2);  // PCM
    putLe(bytes, 1, 2);  // one channel
    putLe(bytes, static_cast<std::uint32_t>(sampleRate), 4);
    putLe(bytes, static_cast<std::uint32_t>(sampleRate) * 2, 4);
    putLe(bytes, 2, 2);
    putLe(bytes, 16, 2);
    bytes += "data";
    putLe(bytes, dataBytes, 4);
    for (const float sample : samples) {
        const double clamped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
        const auto value = static_cast<std::int16_t>(std::lround(clamped * 32767.0));
        putLe(bytes, static_cast<std::uint16_t>(value), 2);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error != nullptr) {
            *error = "cannot open " + path + " for writing";
        }
        return false;
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        if (error != nullptr) {
            *error = "cannot write " + path;
        }
        return false;
    }
    return true;
}

bool writeGridTsv(const std::string &path, const SynthTrackSpec &spec, const std::vector<domain::Beat> &beats,
                  std::string *error)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        if (error != nullptr) {
            *error = "cannot open " + path + " for writing";
        }
        return false;
    }
    file << "# index\ttime_ms\tbeat_in_bar\tbpm_to_next\n";
    char line[128];
    for (std::size_t k = 0; k < beats.size(); ++k) {
        std::snprintf(line, sizeof line, "%zu\t%.6f\t%d\t%.6f\n", k, beats[k].timeMs, beats[k].beatInBar,
                      bpmOfInterval(spec.tempo, static_cast<int>(k)));
        file << line;
    }
    file.close();
    if (!file) {
        if (error != nullptr) {
            *error = "cannot write " + path;
        }
        return false;
    }
    return true;
}

}  // namespace seabass::synth
