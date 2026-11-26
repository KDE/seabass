// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "musicanalyze/key.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace musicanalyze
{

namespace
{

// The rate the picture is made at, about: every Nth sample of the audio
// with N the whole number that comes nearest to it (4 at 44.1 kHz, which
// is 11025 exactly; 4 at 48 kHz, which is 12000). Frequencies are worked
// out from the rate that results, so the notes land where they are.
constexpr double WorkingRateHz = 11025.0;
// A frame and the step to the next, in samples at the working rate: a
// second and a half, 0.37 s apart. The window is what tells two notes a
// semitone apart at C2 (65 Hz, 3.9 Hz to the next): 0.67 Hz a bin.
constexpr std::size_t FrameSamples = 16384;
constexpr std::size_t FrameStep = 4096;
constexpr int Octaves = 5;
constexpr int ThirdsPerOctave = 36;
constexpr int Thirds = Octaves * ThirdsPerOctave;

// One weight an octave (C2 upwards, five of them) and a note, the tonic first: major, then minor.
constexpr std::array<double, 60> MajorWeights{
    1.6509014232128216, -0.3685065190278356, -0.43078591980711473, -1.0528351935728819, 0.24767249258430193, 0.13080599835354739, -0.22843625934893533, 0.71897245087059591, -0.91329570554950146, -0.3474945196669898, -0.52717792737338387, -0.18538644162339993,
    2.3571661665021377, -0.25841369616695514, -0.51600565442465351, -0.80261851065713263, 1.0353383727074716, -0.10715535500168402, -0.56435876432688858, 0.96461067079047424, -0.99373937283662339, -0.62342175347469153, -0.53191379351277479, 0.22002032981591546,
    1.9867652101630373, -0.90594240109544577, 0.10219112946410633, -1.5143633370217675, 1.6448188316838761, 0.089018385823367088, -0.91424769338283607, 1.6254924965146815, -1.0420321916352506, -0.43955413987755781, -0.65383863559150734, -0.2369328687276504,
    1.5927376579597272, -1.0738723225215898, -0.11957959854634456, -1.3527771907967694, 1.6398149245918106, -0.24660405549368689, -0.62296902115823749, 2.1643817076403322, -1.0312449169382214, -0.20167560652182293, -1.311146423755543, 0.53745872314468679,
    1.3016931671799377, -1.1111253662918632, 0.4696178755640622, -1.2464765206131889, 1.0202958756288709, -0.74690833887634411, -0.48296846670245175, 1.0929889910697674, -0.52823487616268727, 0.28352137614907308, -0.85211295898986483, 0.37267206559871019,
};
constexpr std::array<double, 60> MinorWeights{
    2.127912551690982, -0.073726638595304622, -0.57131223521405694, 0.203216152399913, -0.3233953121550705, -0.639131377824389, 0.31955838082292404, 0.45084605341631162, 0.14138437598716577, -0.10178618818094472, -0.091369018389326151, -0.1366306230094248,
    2.6176496493989401, -0.17082128308304742, -0.44983362457646109, 0.028014374603914358, -0.36322453357881673, -0.87461522328803176, 0.041335113254806162, 1.5912228680990039, -0.50561943764583406, -1.6087880113805111, -0.36006296536740889, -0.12476556585114111,
    2.6634459837667741, -0.63820173660754886, 0.074510245083934848, 1.482243665607857, -0.03284059491540782, -1.0351137582275556, -0.71498151223956219, 1.8367256731576007, -0.77644192529210287, -1.6129426063925139, -0.34477386492923628, -0.64300435532927769,
    2.5167324402434148, -0.69064042319508578, 0.36538658687486791, 1.5080168146882764, -0.94466589415025914, -0.39226130017665534, -0.57393016990031709, 1.576808709462963, -0.87598092508809933, -1.5016226292527726, -0.070604248437498879, -0.89176283867318507,
    1.4274186891571641, -0.30782759825408573, 0.35192613533249029, 0.57444312211726933, -0.46680755441940691, -0.34771080257680897, -0.28844708296599691, 1.1937878041661061, -0.57544571242561449, -1.1085417781379354, 0.92441255998527128, -0.95017060553249255,
};
constexpr double MajorBias = 0.50694057695700812;
constexpr double MinorBias = -0.50694057695700745;

// A second-order low-pass (Audio EQ Cookbook), transposed direct form II.
class LowPass
{
public:
    LowPass(double cornerHz, double rateHz, double q)
    {
        const double w0 = 2.0 * std::numbers::pi * cornerHz / rateHz;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        m_b0 = (1.0 - std::cos(w0)) / 2.0 / a0;
        m_b1 = (1.0 - std::cos(w0)) / a0;
        m_a1 = -2.0 * std::cos(w0) / a0;
        m_a2 = (1.0 - alpha) / a0;
    }
    double process(double x)
    {
        const double y = m_b0 * x + m_z1;
        m_z1 = m_b1 * x - m_a1 * y + m_z2;
        m_z2 = m_b0 * x - m_a2 * y;
        // After the sound stops the state would shrink for ever without
        // reaching zero, and arithmetic on numbers that small is many
        // times slower: digital silence would be the slowest part of a
        // track. Far below anything that counts, it is zero.
        if (std::abs(m_z1) < Negligible) {
            m_z1 = 0.0;
        }
        if (std::abs(m_z2) < Negligible) {
            m_z2 = 0.0;
        }
        return y;
    }

private:
    static constexpr double Negligible = 1e-30;
    double m_b0 = 0.0, m_b1 = 0.0, m_a1 = 0.0, m_a2 = 0.0;
    double m_z1 = 0.0, m_z2 = 0.0;
};

// In place, a power of two long.
void transform(std::vector<std::complex<double>> &data, const std::vector<std::complex<double>> &twiddle)
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
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const std::size_t half = length / 2;
        const std::size_t stride = n / length;
        for (std::size_t start = 0; start < n; start += length) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::complex<double> odd = data[start + k + half] * twiddle[k * stride];
                const std::complex<double> even = data[start + k];
                data[start + k] = even + odd;
                data[start + k + half] = even - odd;
            }
        }
    }
}

// How much of each third of a semitone a track has, from C2 up five
// octaves, read as the track comes: feed() its audio in pieces, in
// order, then finish(). What is held meanwhile is one frame of the
// thinned audio (a second and a half) and the sums.
//
// The audio is thinned first: every `step`th sample, after an
// eighth-order Butterworth low-pass at four tenths of the new rate, so
// that what would fold down into the notes (at 11 kHz, everything from
// 8.9 kHz up) is 45 dB down and more. Then frame by frame, each a third
// of a second after the one before: the peaks of its spectrum, each at
// its true frequency (a parabola through the peak and its neighbors on
// a log scale), gathered into thirds of a semitone, and every frame
// counted the same, loud or quiet.
class ThirdsReader
{
public:
    // `sampleRateHz` is KeyLowestSampleRateHz at the least.
    explicit ThirdsReader(int sampleRateHz)
        : m_step(static_cast<std::size_t>(std::max(1.0, std::round(sampleRateHz / WorkingRateHz)))),
          m_rate(static_cast<double>(sampleRateHz) / static_cast<double>(m_step)),
          // The four sections of an eighth-order Butterworth.
          m_sections{LowPass(0.4 * m_rate, sampleRateHz, Q[0]), LowPass(0.4 * m_rate, sampleRateHz, Q[1]),
                     LowPass(0.4 * m_rate, sampleRateHz, Q[2]), LowPass(0.4 * m_rate, sampleRateHz, Q[3])},
          m_binHz(m_rate / static_cast<double>(FrameSamples)), m_window(FrameSamples), m_twiddle(FrameSamples / 2),
          m_frame(FrameSamples)
    {
        const auto firstBinAt = [&](double hz) { return static_cast<std::size_t>(std::ceil(hz / m_binHz)); };
        m_low = std::max<std::size_t>(firstBinAt(m_c2 * std::pow(2.0, -0.5 / 12.0)), 2);
        m_high = std::min(firstBinAt(m_c2 * 32.0 * std::pow(2.0, 0.5 / 12.0)), FrameSamples / 2 - 1);
        for (std::size_t i = 0; i < FrameSamples; ++i) {
            m_window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(FrameSamples - 1));
        }
        for (std::size_t k = 0; k < FrameSamples / 2; ++k) {
            m_twiddle[k] = std::polar(1.0, -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(FrameSamples));
        }
        m_magnitude.resize(m_high + 2);
        m_audio.reserve(FrameSamples);
    }

    void feed(std::span<const float> samples)
    {
        if (m_high <= m_low) {
            return;
        }
        for (const float sample : samples) {
            // A sample that is no number (a decoder's slip) is silence.
            double x = std::isfinite(sample) ? static_cast<double>(sample) : 0.0;
            if (m_step > 1) {
                for (LowPass &section : m_sections) {
                    x = section.process(x);
                }
            }
            if (m_seen % m_step == 0) {
                m_audio.push_back(x);
                if (m_audio.size() == FrameSamples) {
                    frame();
                    m_audio.erase(m_audio.begin(), m_audio.begin() + FrameStep);
                }
            }
            ++m_seen;
        }
    }

    // Empty where there is nothing to read a key from: not one whole
    // frame of audio.
    std::optional<std::array<double, Thirds>> finish() const
    {
        if (m_frames == 0) {
            return std::nullopt;
        }
        return m_total;
    }

private:
    static constexpr std::array<double, 4> Q{0.50979557910415917, 0.60134488693504529, 0.89997622313641570, 2.5629154477415055};

    // The frame that m_audio holds, added to the sums.
    void frame()
    {
        for (std::size_t i = 0; i < FrameSamples; ++i) {
            m_frame[i] = m_audio[i] * m_window[i];
        }
        transform(m_frame, m_twiddle);
        for (std::size_t k = m_low - 2; k <= m_high + 1; ++k) {
            m_magnitude[k] = std::abs(m_frame[k]);
        }
        std::array<double, Thirds> inFrame{};
        double sum = 0.0;
        for (std::size_t k = m_low; k < m_high; ++k) {
            const double below = m_magnitude[k - 1], here = m_magnitude[k], above = m_magnitude[k + 1];
            if (!(here > below && here >= above)) {
                continue;
            }
            const double la = std::log(below + 1e-9), lb = std::log(here + 1e-9), lc = std::log(above + 1e-9);
            const double offset = std::clamp(0.5 * (la - lc) / (la - 2.0 * lb + lc - 1e-12), -0.5, 0.5);
            const double hz = (static_cast<double>(k) + offset) * m_binHz;
            const double third = std::floor(36.0 * std::log2(hz / m_c2) + 1.5);
            if (third >= 0.0 && third < static_cast<double>(Thirds)) {
                inFrame[static_cast<std::size_t>(third)] += here;
                sum += here;
            }
        }
        for (int i = 0; i < Thirds; ++i) {
            m_total[static_cast<std::size_t>(i)] += inFrame[static_cast<std::size_t>(i)] / (sum + 1e-6);
        }
        ++m_frames;
    }

    std::size_t m_step;
    double m_rate;
    std::array<LowPass, 4> m_sections;
    double m_binHz;
    // C2, the lowest note read.
    double m_c2 = 440.0 * std::pow(2.0, -33.0 / 12.0);
    std::size_t m_low = 0;
    std::size_t m_high = 0;
    std::vector<double> m_window;
    std::vector<std::complex<double>> m_twiddle;
    std::vector<std::complex<double>> m_frame;
    std::vector<double> m_magnitude;
    // The thinned audio from the start of the next frame on.
    std::vector<double> m_audio;
    std::size_t m_seen = 0;
    std::size_t m_frames = 0;
    std::array<double, Thirds> m_total{};
};

}  // namespace

std::string keyName(MusicalKey key)
{
    static constexpr std::array<const char *, 12> Names{"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    const int tonic = ((key.tonic % 12) + 12) % 12;
    return std::string(Names[static_cast<std::size_t>(tonic)]) + (key.minor ? "m" : "");
}

namespace
{

// The thirds of a semitone made into notes: how much of each note there
// is in each of five octaves.
std::optional<std::array<double, 60>> pictureOf(const std::optional<std::array<double, Thirds>> &thirds)
{
    if (!thirds) {
        return std::nullopt;
    }
    // Which third of a semitone the notes are tuned on: the one that has
    // the most in it over all notes and octaves. A third under, on, or a
    // third over concert pitch, tried in that order; a tie keeps the first.
    std::array<double, ThirdsPerOctave> perThird{};
    for (int i = 0; i < Thirds; ++i) {
        perThird[static_cast<std::size_t>(i % ThirdsPerOctave)] += (*thirds)[static_cast<std::size_t>(i)];
    }
    int shift = -1;
    double best = -1.0;
    for (const int candidate : {-1, 0, 1}) {
        double centers = 0.0;
        for (int note = 0; note < 12; ++note) {
            centers += perThird[static_cast<std::size_t>((3 * note + 1 + candidate + ThirdsPerOctave) % ThirdsPerOctave)];
        }
        if (centers > best) {
            best = centers;
            shift = candidate;
        }
    }
    // A note: its center third and half of the one on either side.
    std::array<double, 60> picture{};
    double all = 0.0;
    for (int octave = 0; octave < Octaves; ++octave) {
        const auto at = [&](int third) {
            return (*thirds)[static_cast<std::size_t>(octave * ThirdsPerOctave
                                                      + (third + shift + ThirdsPerOctave) % ThirdsPerOctave)];
        };
        for (int note = 0; note < 12; ++note) {
            const double value = at(3 * note + 1) + 0.5 * (at(3 * note) + at(3 * note + 2));
            picture[static_cast<std::size_t>(octave * 12 + note)] = value;
            all += value;
        }
    }
    if (!(all > 0.0)) {
        return std::nullopt;
    }
    for (double &value : picture) {
        value /= all + 1e-9;
    }
    return picture;
}

// The key a picture of the notes is in.
KeyAnalysis keyOf(const std::optional<std::array<double, 60>> &picture)
{
    KeyAnalysis analysis;
    if (!picture) {
        return analysis;
    }
    // Each key sees the picture with its own tonic first: the twelve
    // major keys, then the twelve minor ones.
    std::array<double, 24> scores{};
    for (int minor = 0; minor < 2; ++minor) {
        const std::array<double, 60> &weights = minor == 1 ? MinorWeights : MajorWeights;
        for (int tonic = 0; tonic < 12; ++tonic) {
            double score = minor == 1 ? MinorBias : MajorBias;
            for (int octave = 0; octave < Octaves; ++octave) {
                for (int note = 0; note < 12; ++note) {
                    const double amount = (*picture)[static_cast<std::size_t>(octave * 12 + (note + tonic) % 12)];
                    score += std::log1p(60.0 * amount) * weights[static_cast<std::size_t>(octave * 12 + note)];
                }
            }
            scores[static_cast<std::size_t>(minor * 12 + tonic)] = score;
        }
    }
    // The first of the best, should two tie.
    const auto top = static_cast<std::size_t>(std::max_element(scores.begin(), scores.end()) - scores.begin());
    const double best = scores[top];
    double second = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < scores.size(); ++i) {
        if (i != top) {
            second = std::max(second, scores[i]);
        }
    }
    analysis.key = MusicalKey{static_cast<int>(top % 12), top >= 12};
    analysis.margin = best - second;
    return analysis;
}

}  // namespace

std::optional<std::array<double, 60>> pitchPicture(std::span<const float> samples, int sampleRateHz)
{
    if (sampleRateHz < KeyLowestSampleRateHz) {
        return std::nullopt;
    }
    ThirdsReader reader(sampleRateHz);
    reader.feed(samples);
    return pictureOf(reader.finish());
}

class KeyAnalyzer::State
{
public:
    explicit State(int sampleRateHz) : hasRate(sampleRateHz > 0)
    {
        if (sampleRateHz >= KeyLowestSampleRateHz) {
            reader.emplace(sampleRateHz);
        }
    }
    // Empty where the sample rate is too low.
    std::optional<ThirdsReader> reader;
    // False where what was given as the sample rate is none (0, or less).
    bool hasRate;
    bool any = false;
};

KeyAnalyzer::KeyAnalyzer(int sampleRateHz) : m_state(std::make_unique<State>(sampleRateHz)) {}

KeyAnalyzer::~KeyAnalyzer() = default;
KeyAnalyzer::KeyAnalyzer(KeyAnalyzer &&) noexcept = default;
KeyAnalyzer &KeyAnalyzer::operator=(KeyAnalyzer &&) noexcept = default;

void KeyAnalyzer::feed(std::span<const float> samples)
{
    if (m_state && !samples.empty()) {
        m_state->any = true;
        if (m_state->reader) {
            m_state->reader->feed(samples);
        }
    }
}

KeyAnalysis KeyAnalyzer::finish()
{
    KeyAnalysis analysis;
    if (m_state && !m_state->hasRate) {
        analysis.error = "no sample rate";
    } else if (!m_state || !m_state->any) {
        analysis.error = "no samples";
    } else if (!m_state->reader) {
        analysis.error = "the sample rate is too low to read a key";
    } else {
        analysis = keyOf(pictureOf(m_state->reader->finish()));
    }
    m_state.reset();
    return analysis;
}

KeyAnalysis keyFromSamples(std::span<const float> samples, int sampleRateHz)
{
    KeyAnalyzer analyzer(sampleRateHz);
    analyzer.feed(samples);
    return analyzer.finish();
}

}  // namespace musicanalyze
