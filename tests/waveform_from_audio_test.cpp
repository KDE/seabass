// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// waveformFromSamples() and friends: a waveform measured from audio, 150
// entries a second, three bands and an overall level. Every signal here
// is generated; nothing is read from disk.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "domain/pcm_source.hpp"
#include "domain/waveform_from_audio.hpp"

namespace domain = seabass::domain;

namespace
{

constexpr int Rate = 44100;

std::vector<float> sine(double hz, double amplitude, double seconds, int rate = Rate)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    }
    return out;
}

// A sine whose frequency rises exponentially from `fromHz` to `toHz`.
// Returns the samples; `frequencyAt` gives the frequency at a time.
std::vector<float> sweep(double fromHz, double toHz, double seconds, double amplitude)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * Rate));
    const double k = std::log(toHz / fromHz) / seconds;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double t = static_cast<double>(i) / Rate;
        // phase = 2 pi f0 (e^(k t) - 1) / k
        const double phase = 2.0 * std::numbers::pi * fromHz * (std::exp(k * t) - 1.0) / k;
        out[i] = static_cast<float>(amplitude * std::sin(phase));
    }
    return out;
}

double sweepFrequency(double fromHz, double toHz, double seconds, double t)
{
    return fromHz * std::exp(std::log(toHz / fromHz) / seconds * t);
}

// Hands its samples out a few at a time, in chunks of changing size, so
// the measurement has to carry its state across reads.
class TrickleSource : public domain::PcmSource
{
public:
    TrickleSource(std::vector<float> samples, std::string failAtEnd = {})
        : m_samples(std::move(samples)), m_failAtEnd(std::move(failAtEnd))
    {
    }
    int sampleRate() const override { return Rate; }
    std::size_t read(std::span<float> out) override
    {
        const std::size_t want = std::min<std::size_t>({out.size(), 1 + (m_reads++ * 37) % 701,
                                                        m_samples.size() - m_position});
        std::copy_n(m_samples.begin() + static_cast<std::ptrdiff_t>(m_position), want, out.begin());
        m_position += want;
        return want;
    }
    std::string error() const override { return m_position == m_samples.size() ? m_failAtEnd : std::string(); }

private:
    std::vector<float> m_samples;
    std::string m_failAtEnd;
    std::size_t m_position = 0;
    std::size_t m_reads = 0;
};

// The mean of one band over the entries from `from` on (past the filters'
// settling), for a steady signal.
struct Means
{
    double low = 0, mid = 0, high = 0, overall = 0;
};

Means steadyMeans(const domain::AudioWaveform &w, std::size_t from = 30)
{
    Means m;
    std::size_t n = 0;
    for (std::size_t i = from; i + 1 < w.entries.size(); ++i) {
        m.low += w.entries[i].low;
        m.mid += w.entries[i].mid;
        m.high += w.entries[i].high;
        m.overall += w.entries[i].overall;
        ++n;
    }
    assert(n > 0);
    m.low /= n;
    m.mid /= n;
    m.high /= n;
    m.overall /= n;
    return m;
}

}  // namespace

int main()
{
    // ---- how many entries -------------------------------------------------
    {
        assert(domain::waveformEntryCount(0, Rate) == 0);
        assert(domain::waveformEntryCount(1, Rate) == 1);
        assert(domain::waveformEntryCount(294, Rate) == 1);
        assert(domain::waveformEntryCount(295, Rate) == 2);
        assert(domain::waveformEntryCount(10 * Rate, Rate) == 1500);
        assert(domain::waveformEntryCount(48000, 48000) == 150);
        assert(domain::waveformEntryCount(48001, 48000) == 151);
        assert(domain::waveformEntryCount(1000, 0) == 0);

        // The measurement makes exactly that many, the last one short.
        const auto w = domain::waveformFromSamples(sine(440, 0.5, 1.0 + 100.0 / Rate), Rate);
        assert(w.entries.size() == domain::waveformEntryCount(Rate + 100, Rate));
        assert(w.entries.size() == 151);
        assert(w.sampleCount == static_cast<std::uint64_t>(Rate + 100));
        assert(w.sampleRateHz == Rate);
        assert(domain::waveformFromSamples(sine(440, 0.5, 1.0), 0).entries.empty());
        assert(domain::waveformFromSamples({}, Rate).entries.empty());
        std::cout << "case 1 (entry count: every started 1/150 s, 151 for a second and 100 samples) OK\n";
    }

    // ---- one tone in each band --------------------------------------------
    //
    // A full-scale sine reads 1 in its own band, and the other two stay
    // low: the crossovers are fourth order, 24 dB an octave.
    {
        struct Tone
        {
            double hz;
            int band;  // 0 low, 1 mid, 2 high
        };
        for (const Tone tone : {Tone{50, 0}, Tone{80, 0}, Tone{500, 1}, Tone{700, 1}, Tone{6000, 2}, Tone{10000, 2}}) {
            const auto w = domain::waveformFromSamples(sine(tone.hz, 1.0, 2.0), Rate);
            const Means m = steadyMeans(w);
            const double bands[3] = {m.low, m.mid, m.high};
            assert(bands[tone.band] > 0.9 && bands[tone.band] <= 1.0);
            for (int b = 0; b < 3; ++b) {
                if (b != tone.band) {
                    assert(bands[b] < 0.25);
                }
            }
            // An entry is 6.7 ms: a tone with a shorter period peaks
            // in every one.
            if (tone.hz >= 500) {
                assert(std::fabs(m.overall - 1.0) < 0.01);
            }
        }
        // Half the amplitude, half the level: linear, not decibels.
        const Means half = steadyMeans(domain::waveformFromSamples(sine(700, 0.5, 2.0), Rate));
        assert(std::fabs(half.mid - 0.5) < 0.05);
        assert(std::fabs(half.overall - 0.5) < 0.01);
        std::cout << "case 2 (one tone a band: its band near 1, the others below 0.25, linear in amplitude) OK\n";
    }

    // ---- a sine sweep lands in the right band ------------------------------
    //
    // 30 Hz to 15 kHz over 12 seconds. At every entry whose frequency is
    // more than half an octave from an edge, the strongest band is the
    // one the frequency is in.
    {
        constexpr double From = 30.0, To = 15000.0, Seconds = 12.0;
        const auto w = domain::waveformFromSamples(sweep(From, To, Seconds, 0.8), Rate);
        std::size_t checked = 0;
        std::size_t perBand[3] = {0, 0, 0};
        for (std::size_t i = 10; i < w.entries.size(); ++i) {
            const double t = (static_cast<double>(i) + 0.5) / domain::WaveformEntriesPerSecond;
            const double f = sweepFrequency(From, To, Seconds, t);
            const double fromEdge = std::min(std::fabs(std::log2(f / domain::WaveformLowMidEdgeHz)),
                                             std::fabs(std::log2(f / domain::WaveformMidHighEdgeHz)));
            if (fromEdge < 0.5 || f > To) {
                continue;
            }
            const int expected = f < domain::WaveformLowMidEdgeHz ? 0 : (f < domain::WaveformMidHighEdgeHz ? 1 : 2);
            const auto &e = w.entries[i];
            const float bands[3] = {e.low, e.mid, e.high};
            int strongest = 0;
            for (int b = 1; b < 3; ++b) {
                if (bands[b] > bands[strongest]) {
                    strongest = b;
                }
            }
            if (strongest != expected) {
                std::cerr << "entry " << i << " at " << f << " Hz: low " << e.low << " mid " << e.mid << " high "
                          << e.high << ", expected band " << expected << "\n";
                assert(false && "the sweep landed in the wrong band");
            }
            ++checked;
            ++perBand[expected];
        }
        assert(perBand[0] > 50 && perBand[1] > 300 && perBand[2] > 300);
        std::cout << "case 3 (sweep 30 Hz to 15 kHz: " << checked << " entries, " << perBand[0] << " low, " << perBand[1]
                  << " mid, " << perBand[2] << " high, each in its band) OK\n";
    }

    // ---- overall is the peak, whatever the frequency ------------------------
    {
        std::vector<float> square(Rate);
        for (std::size_t i = 0; i < square.size(); ++i) {
            square[i] = (i / 100) % 2 == 0 ? 0.8f : -0.8f;
        }
        const auto w = domain::waveformFromSamples(square, Rate);
        for (const auto &e : w.entries) {
            assert(std::fabs(e.overall - 0.8f) < 1e-6f);
        }
        // Silence is zero everywhere.
        const auto quiet = domain::waveformFromSamples(std::vector<float>(Rate, 0.0f), Rate);
        for (const auto &e : quiet.entries) {
            assert(e.low == 0.0f && e.mid == 0.0f && e.high == 0.0f && e.overall == 0.0f);
        }
        // A click shows in its own entry only.
        std::vector<float> click(Rate, 0.0f);
        click[294 * 40 + 7] = 0.9f;
        const auto c = domain::waveformFromSamples(click, Rate);
        for (std::size_t i = 0; i < c.entries.size(); ++i) {
            assert((i == 40) == (c.entries[i].overall > 0.0f));
        }
        std::cout << "case 4 (overall is the entry's peak; silence is zero; a click lands in its entry) OK\n";
    }

    // ---- read from a source in pieces: the same as one span -----------------
    {
        const auto samples = sweep(40, 12000, 3.0, 0.7);
        const auto whole = domain::waveformFromSamples(samples, Rate);
        TrickleSource source(samples);
        std::string error = "not cleared";
        const auto pieces = domain::waveformFromPcm(source, &error);
        assert(error.empty());
        assert(pieces.entries.size() == whole.entries.size());
        assert(pieces.sampleCount == whole.sampleCount);
        for (std::size_t i = 0; i < whole.entries.size(); ++i) {
            assert(pieces.entries[i].low == whole.entries[i].low);
            assert(pieces.entries[i].mid == whole.entries[i].mid);
            assert(pieces.entries[i].high == whole.entries[i].high);
            assert(pieces.entries[i].overall == whole.entries[i].overall);
        }
        // A source that stops on an error says so, and what it read counts.
        TrickleSource failing(sine(440, 0.5, 1.0), "decode error at 1 s");
        const auto cut = domain::waveformFromPcm(failing, &error);
        assert(error == "decode error at 1 s");
        assert(cut.entries.size() == 150);
        // No sample rate: nothing, and why.
        domain::InMemoryPcmSource broken({0.1f, 0.2f}, 0);
        const auto none = domain::waveformFromPcm(broken, &error);
        assert(none.entries.empty() && !error.empty());
        std::cout << "case 5 (a source read in pieces gives the same entries; its error is passed on) OK\n";
    }

    // ---- reduced to a preview ------------------------------------------------
    {
        std::vector<domain::WaveformEntry> e{{0.1f, 0.2f, 0.3f, 0.4f},
                                             {0.3f, 0.4f, 0.5f, 0.6f},
                                             {0.5f, 0.6f, 0.7f, 0.8f},
                                             {0.7f, 0.8f, 0.9f, 1.0f}};
        const auto two = domain::reduceWaveform(e, 2);
        assert(two.size() == 2);
        assert(std::fabs(two[0].low - 0.2f) < 1e-6f && std::fabs(two[0].overall - 0.5f) < 1e-6f);
        assert(std::fabs(two[1].high - 0.8f) < 1e-6f && std::fabs(two[1].mid - 0.7f) < 1e-6f);
        // More columns than entries: each entry repeated in order.
        const auto eight = domain::reduceWaveform(e, 8);
        assert(eight.size() == 8);
        for (std::size_t j = 0; j < 8; ++j) {
            assert(eight[j].low == e[j / 2].low);
        }
        const auto same = domain::reduceWaveform(e, 4);
        for (std::size_t j = 0; j < 4; ++j) {
            assert(same[j].mid == e[j].mid);
        }
        // Every entry is in exactly one column: the means of 1200 columns
        // of a long waveform average back to the waveform's own mean.
        const auto w = domain::waveformFromSamples(sweep(40, 12000, 30.0, 0.7), Rate);
        const auto wide = domain::reduceWaveform(w.entries, 1200);
        assert(wide.size() == 1200);
        double all = 0, cols = 0;
        for (const auto &x : w.entries) {
            all += x.overall;
        }
        for (const auto &x : wide) {
            cols += x.overall;
        }
        assert(std::fabs(all / w.entries.size() - cols / wide.size()) < 0.01);
        assert(domain::reduceWaveform({}, 400).empty());
        assert(domain::reduceWaveform(e, 0).empty());
        std::cout << "case 6 (reduced to N columns: means, repeats when short, nothing lost) OK\n";
    }

    std::cout << "waveform_from_audio_test: all cases passed\n";
    return 0;
}
