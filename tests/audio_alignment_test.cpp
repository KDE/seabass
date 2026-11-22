// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The arithmetic that lines two copies of a song up (domain/
// audio_alignment), against signals made here: noise with a known delay,
// a known different tail, a section cut out, another signal entirely.
// No decoder is involved; alignment_probe_test runs the same rule on
// files the real one decodes.

#include <cassert>
#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <random>
#include <utility>
#include <vector>

#include "domain/audio_alignment.hpp"

using namespace seabass::domain::alignment;

namespace
{

std::vector<float> noise(size_t samples, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, 0.3f);
    std::vector<float> out(samples);
    for (auto &value : out) {
        value = dist(rng);
    }
    return out;
}

std::vector<float> concat(std::vector<float> a, const std::vector<float> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<float> silence(size_t samples)
{
    return std::vector<float>(samples, 0.0f);
}

// The default search: MatchingPolicy's default 10 s plus the margin.
const double Search = shiftSearchSeconds(10.0);

bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

}  // namespace

int main()
{
    // The transform against a plain DFT, and back again.
    {
        std::vector<std::complex<double>> data(16);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = {std::sin(0.7 * static_cast<double>(i)) + 0.1 * static_cast<double>(i), std::cos(1.3 * i)};
        }
        const auto original = data;
        fft(data, false);
        for (size_t k = 0; k < data.size(); ++k) {
            std::complex<double> expected;
            for (size_t n = 0; n < original.size(); ++n) {
                expected += original[n] * std::polar(1.0, -2.0 * std::numbers::pi * static_cast<double>(k * n) / 16.0);
            }
            assert(std::abs(data[k] - expected) < 1e-9 && "the forward transform is a DFT");
        }
        fft(data, true);
        for (size_t n = 0; n < data.size(); ++n) {
            assert(std::abs(data[n] - original[n]) < 1e-12 && "the inverse undoes it");
        }
    }

    // Resampling keeps time exact. At 44.1 kHz to 22.05 kHz an impulse at
    // input sample 2m stays at output sample m.
    {
        Resampler r(44100.0, 22050.0);
        for (int k = 0; k < 2000; ++k) {
            r.push(k == 1234 ? 1.0f : 0.0f);
        }
        const auto out = r.finish();
        size_t peak = 0;
        for (size_t n = 0; n < out.size(); ++n) {
            if (out[n] > out[peak]) {
                peak = n;
            }
        }
        assert(peak == 617 && "an impulse stays where it was");
        assert(out.size() >= 999 && out.size() <= 1001);
    }
    // At a ratio that is no integer (48 kHz) every output sample still
    // stands for its own instant: a ramp whose value is its time comes out
    // as a ramp whose value is the output's time. Half an output sample
    // late would be 23 us off; this holds it to 2 us.
    {
        Resampler r(48000.0, 22050.0);
        for (int k = 0; k < 48000; ++k) {
            r.push(static_cast<float>(k / 48000.0));
        }
        const auto out = r.finish();
        assert(out.size() >= 22049 && out.size() <= 22051);
        for (size_t n = 2; n + 2 < out.size(); ++n) {
            assert(near(out[n], static_cast<double>(n) / 22050.0, 2e-6) && "the 48 kHz output is on time");
        }
    }
    // Upsampling (a 16 kHz file) too.
    {
        Resampler r(16000.0, 22050.0);
        for (int k = 0; k < 16000; ++k) {
            r.push(static_cast<float>(k / 16000.0));
        }
        const auto out = r.finish();
        for (size_t n = 3; n + 3 < out.size(); ++n) {
            assert(near(out[n], static_cast<double>(n) / 22050.0, 2e-5) && "the 16 kHz output is on time");
        }
    }

    // One window's peak: the lag is where a sound sits in b minus where
    // it sits in a, either way round.
    {
        const auto a = noise(5000, 1);
        auto peak = correlationPeak(a, concat(silence(137), a), 500);
        assert(peak && peak->lag == 137 && peak->normalised > 0.99 && "b starts 137 samples later");
        const std::vector<float> trimmed(a.begin() + 200, a.end());
        peak = correlationPeak(a, trimmed, 500);
        assert(peak && peak->lag == -200 && peak->normalised > 0.99 && "b starts 200 samples earlier");
        peak = correlationPeak(a, noise(5000, 2), 500);
        assert(peak && peak->normalised < 0.2 && "another signal correlates with nothing");
        assert(!correlationPeak(a, a, 6000) && "less overlap than asked for is no answer");
        assert(!correlationPeak(silence(3000), a, 500) && "silence has nothing to correlate");
    }

    // The rule.
    {
        auto clean = cleanShiftMs({{10.0, 0.9}, {10.5, 0.8}, {11.0, 0.95}});
        assert(clean && near(*clean, 10.5, 1e-9) && "the median of three that agree");
        clean = cleanShiftMs({{10.0, 0.9}, {12.0, 0.8}, {11.0, 0.95}});
        assert(clean && near(*clean, 11.0, 1e-9) && "2 ms apart still agrees");
        assert(!cleanShiftMs({{10.0, 0.9}, {12.01, 0.8}, {11.0, 0.95}}) && "just over 2 ms apart does not");
        assert(!cleanShiftMs({{10.0, 0.9}, {10.0, 0.49}, {10.0, 0.95}}) && "one weak window is no answer");
        clean = cleanShiftMs({{10.0, 0.5}, {10.0, 0.5}, {10.0, 0.5}});
        assert(clean && "0.5 is enough");
        assert(!cleanShiftMs({}) && "no windows, no answer");
        // Rivals: a beat-only intro and outro have a rival a beat away at
        // nearly the true lag's coefficient, the middle none. Windows of
        // their own resolve each other, when every window's own best is
        // the same lag and one has no rival at ResolvingRivalShare.
        clean = cleanShiftMs({{21.3, 0.99, 0.9836}, {21.3, 0.98, 0.83}, {21.3, 0.99, 0.9689}});
        assert(clean && near(*clean, 21.3, 1e-9) && "the clear middle settles the beat-only start and end");
        assert(!cleanShiftMs({{21.3, 0.99, 0.9836}, {21.3, 0.98, 0.93}, {21.3, 0.99, 0.9689}})
               && "a window with a rival at 0.93 settles nothing");
        assert(!cleanShiftMs({{21.3, 0.99, 0.99}, {21.3, 0.98, 0.97}, {21.3, 0.99, 0.96}}) && "every window torn: unknown");
        assert(!cleanShiftMs({{21.3, 0.99, 0.99}, {521.3, 0.98, 0.5}, {21.3, 0.99, 0.96}})
               && "the windows' own bests must agree, a rival is never the answer");
        assert(!cleanShiftMs({{21.3, 0.99, 0.5, false}, {21.3, 0.98, 0.5}, {21.3, 0.99, 0.5}})
               && "a best on the edge or past the search is no answer");
        // Windows sharing samples (a short file): one measurement cannot
        // resolve itself, so any rival at RivalShare is unknown.
        assert(!cleanShiftMs({{21.3, 0.99, 0.96}, {21.3, 0.99, 0.5}, {21.3, 0.99, 0.5}}, false));
        assert(cleanShiftMs({{21.3, 0.99, 0.94}, {21.3, 0.99, 0.94}, {21.3, 0.99, 0.94}}, false)
               && "under RivalShare, a short file is known as before");
    }

    // Whole files, at a low analysis rate so the test stays quick: the
    // windows are as long in seconds as ever, only fewer samples. 100 s,
    // so start, middle and end are three different stretches.
    {
        constexpr double rate = 2000.0;
        const auto a = noise(200000, 7);
        const double sampleMs = 1000.0 / rate;

        auto shift = measureShiftMs(a, a, rate, Search);
        assert(shift && near(*shift, 0.0, 1e-9) && "a file lines up with itself");

        // Leading silence: the music in b starts 431 samples later.
        shift = measureShiftMs(a, concat(silence(431), a), rate, Search);
        assert(shift && near(*shift, 431 * sampleMs, 1e-6) && "prepended silence is the shift");
        shift = measureShiftMs(concat(silence(431), a), a, rate, Search);
        assert(shift && near(*shift, -431 * sampleMs, 1e-6) && "and the other way round");

        // A different tail moves nothing: the end window is counted from
        // each file's own end and turned back into a shift from the start.
        shift = measureShiftMs(a, concat(a, noise(14000, 8)), rate, Search);
        assert(shift && near(*shift, 0.0, 1e-6) && "a longer tail is not a shift");
        shift = measureShiftMs(a, concat(concat(silence(300), a), noise(9000, 9)), rate, Search);
        assert(shift && near(*shift, 300 * sampleMs, 1e-6) && "lead-in and tail together");

        // Not the same music.
        assert(!measureShiftMs(a, noise(200000, 11), rate, Search) && "another signal does not line up");

        // A section cut from the middle of b: the start agrees, the end
        // is 150 ms early, and no one shift is right for every cue.
        std::vector<float> cut(a.begin(), a.begin() + 100000);
        cut.insert(cut.end(), a.begin() + 100300, a.end());
        assert(!measureShiftMs(a, cut, rate, Search) && "start and end disagree");

        // Shorter than a window: the windows are the whole files.
        const std::vector<float> shortA(a.begin(), a.begin() + 20000);
        shift = measureShiftMs(shortA, concat(silence(50), shortA), rate, Search);
        assert(shift && near(*shift, 50 * sampleMs, 1e-6) && "a 10 s file still lines up");

        // Under a second: too little to say.
        const std::vector<float> tiny(a.begin(), a.begin() + 1500);
        assert(!measureShiftMs(tiny, tiny, rate, Search) && "under a second is unknown");
    }

    // Music made of a repeated bar or beat matches itself one period away
    // nearly as well as at the true shift, and better by the raw sum, which
    // grows with the overlap: the old peak picking answered +575 ms, with
    // all three windows agreeing at 0.99, for copies 1300 ms apart. The
    // true shift or no answer; never another period's.
    {
        // 400 Hz keeps this cheap on a slow runner; the windows are as
        // long in seconds as ever.
        constexpr double rate = 400.0;
        constexpr size_t length = 40000;  // 100 s
        // `share` of the energy not periodic. a is the track from its
        // start, b the same track starting `lead` samples into it, so a
        // sound sits `lead` samples EARLIER in b: shift -lead.
        auto loopPair = [&](size_t period, size_t lead, double share, unsigned seed) {
            const auto loop = noise(period, seed);
            const auto extra = noise(length + lead, seed + 1);
            const float periodic = static_cast<float>(std::sqrt(1.0 - share));
            const float other = static_cast<float>(std::sqrt(share));
            std::vector<float> track(length + lead);
            for (size_t i = 0; i < track.size(); ++i) {
                track[i] = periodic * loop[i % period] + other * extra[i];
            }
            std::vector<float> a(track.begin(), track.begin() + length);
            std::vector<float> b(track.begin() + static_cast<long>(lead), track.end());
            return std::pair{a, b};
        };
        const double sampleMs = 1000.0 / rate;
        auto trueOrUnknown = [&](const std::optional<double> &shift, double expectedMs) {
            return !shift || near(*shift, expectedMs, sampleMs);
        };

        // One bar at 128 BPM (1.875 s), 1% not periodic, 1300 ms apart.
        {
            const auto [a, b] = loopPair(750, 520, 0.01, 21);
            const auto shift = measureShiftMs(a, b, rate, Search);
            assert(trueOrUnknown(shift, -1300.0) && "a bar loop lines up at its true shift or not at all");
            const auto windows = windowShifts(a, b, rate, Search);
            assert(windows && windows->size() == 3);
            for (const auto &window : *windows) {
                assert((window.ambiguous() || near(window.shiftMs, -1300.0, sampleMs))
                       && "no window settles on a bar away");
            }
        }
        // One beat (0.47 s), 300 ms apart: the next beat is 169 ms the
        // other way.
        {
            const auto [a, b] = loopPair(188, 120, 0.01, 31);
            assert(trueOrUnknown(measureShiftMs(a, b, rate, Search), -300.0)
                   && "a beat loop lines up at its true shift or not at all");
        }
        // Purely periodic: every period matches exactly as well, and there
        // is nothing to tell them apart by.
        {
            const auto [a, b] = loopPair(750, 520, 0.0, 41);
            assert(!measureShiftMs(a, b, rate, Search) && "a purely periodic pair is unknown");
            const auto peak = correlationPeak(a, b, 400);
            assert(peak && peak->ambiguous && "its peak has rivals");
        }
        // Enough that is not periodic, and the true shift is found: the
        // rival rule does not make every loop-based track unknown.
        {
            const auto [a, b] = loopPair(750, 520, 0.3, 51);
            const auto shift = measureShiftMs(a, b, rate, Search);
            assert(shift && near(*shift, -1300.0, 1e-6) && "a loop with 30% else in it lines up");
        }
        // Further apart than the search reaches: not lined up, even
        // though the same music is in both.
        {
            const auto a = noise(length, 61);
            const auto b = concat(silence(static_cast<size_t>((Search + 3.0) * rate)), a);
            assert(!measureShiftMs(a, b, rate, Search) && "a shift past the search is unknown");
            assert(measureShiftMs(a, b, rate, Search + 5.0) && "and found when the search reaches it");
        }
        // A phrase that comes back every 14 s, with 30% else in it, and the
        // second copy 15 s later: past the search. Its alias a phrase
        // nearer, 1 s, is the only one inside it (the next is at -13 s),
        // and must not be taken for the shift because the true peak was
        // not looked at.
        {
            const size_t phrase = 14 * 400;
            const auto loop = noise(phrase, 91);
            const auto extra = noise(length, 92);
            std::vector<float> song(length);
            for (size_t i = 0; i < length; ++i) {
                song[i] = static_cast<float>(std::sqrt(0.7)) * loop[i % phrase]
                    + static_cast<float>(std::sqrt(0.3)) * extra[i];
            }
            const auto later = concat(silence(15 * 400), song);
            const auto shift = measureShiftMs(song, later, rate, Search);
            assert(!shift && "an alias inside the search of a shift past it is unknown, never 1 s");
            const auto found = measureShiftMs(song, later, rate, 20.0);
            assert(found && near(*found, 15000.0, 1e-6) && "and with the search reaching it, 15 s");
        }
        // A 3.75 s clip pasted at the start and again at 37.5 s, in music
        // that otherwise never repeats, the copies 300 ms apart: seen over
        // two seconds of overlap at the far end of the start window, the
        // second clip matches the first exactly. Too little overlap to be a
        // rival; the pair lines up.
        {
            auto song = noise(length, 101);
            const size_t clip = 1500;
            std::copy(song.begin(), song.begin() + clip, song.begin() + 15000);
            const auto later = concat(silence(120), song);
            const auto shift = measureShiftMs(song, later, rate, Search);
            assert(shift && near(*shift, 300.0, 1e-6) && "a pasted clip seen over two seconds is no rival");
        }
        // A DJ track: a beat-only loop for its first and last 45 s (1% of
        // it not repeating), music that does not repeat between, 135 s in
        // all so the three windows are their own. The start and end
        // windows each have a rival a bar away; the middle settles them.
        {
            const size_t total = 135 * 400;
            const size_t bar = 750;
            const auto loop = noise(bar, 111);
            const auto varied = noise(total + 2000, 112);
            const auto drift = noise(total + 2000, 113);
            auto track = [&](size_t samples, bool loopAllThrough) {
                std::vector<float> out(samples);
                for (size_t i = 0; i < samples; ++i) {
                    const bool inLoop = loopAllThrough || i < 45 * 400 || i >= samples - 45 * 400;
                    out[i] = inLoop ? static_cast<float>(std::sqrt(0.99)) * loop[i % bar]
                            + static_cast<float>(std::sqrt(0.01)) * drift[i]
                                    : varied[i];
                }
                return out;
            };
            const auto song = track(total, false);
            for (const size_t lead : {size_t{8}, size_t{520}}) {  // 20 ms, and 1.3 s: more than a beat
                const auto later = concat(silence(lead), song);
                const auto shift = measureShiftMs(song, later, rate, Search);
                assert(shift && near(*shift, lead * 2.5, 1e-6) && "loop intro and outro, settled by the middle");
                const auto windows = windowShifts(song, later, rate, Search);
                assert(windows && (*windows)[0].ambiguous() && (*windows)[2].ambiguous()
                       && !(*windows)[1].ambiguous() && "the start and end were torn, the middle was not");
            }
            // A loop all through: every window torn, unknown.
            const auto loopy = track(total, true);
            assert(!measureShiftMs(loopy, concat(silence(8), loopy), rate, Search) && "a loop all through is unknown");
            // A bar missing from the middle of one copy: the start says 0,
            // the end a bar early. Unknown.
            std::vector<float> missing(song.begin(), song.begin() + 67 * 400);
            missing.insert(missing.end(), song.begin() + 67 * 400 + bar, song.end());
            assert(!measureShiftMs(song, missing, rate, Search) && "a bar cut from the middle is unknown");
        }
        assert(near(shiftSearchSeconds(10.0), 12.0, 1e-12) && near(shiftSearchSeconds(0.0), 5.0, 1e-12)
               && "the search is the compare window plus 2 s, at least 5 s");
    }

    // A file shorter than three windows gives all three the same samples,
    // so their agreeing proves nothing: the shift must hold in each third
    // of the overlap too. A cut anywhere is unknown; a clean shift and a
    // silent intro are not.
    {
        constexpr double rate = 2000.0;
        const auto song = noise(9000, 81);  // 4.5 s
        const double sampleMs = 1000.0 / rate;
        // 400 ms cut out at 1.6 s: sounds before it 0 apart, after it 400.
        std::vector<float> cut(song.begin(), song.begin() + 3200);
        cut.insert(cut.end(), song.begin() + 4000, song.end());
        assert(!measureShiftMs(song, cut, rate, Search) && "a cut short file is unknown");
        assert(!measureShiftMs(cut, song, rate, Search) && "either way round");
        // 100 ms cut out near the end: the start lines up nearly all over.
        std::vector<float> nicked(song.begin(), song.begin() + 8400);
        nicked.insert(nicked.end(), song.begin() + 8600, song.end());
        assert(!measureShiftMs(song, nicked, rate, Search) && "a small cut near the end is unknown too");
        // Shifted whole, and with a silent intro: lined up.
        const auto shift = measureShiftMs(song, concat(silence(1300), song), rate, Search);
        assert(shift && near(*shift, 1300 * sampleMs, 1e-6) && "a short file with 650 ms in front lines up");
        const auto intro = concat(silence(3000), song);
        const auto later = measureShiftMs(intro, concat(silence(1350), intro), rate, Search);
        assert(later && near(*later, 1350 * sampleMs, 1e-6) && "a silent intro is passed over, not held against it");
    }

    // What is kept of a decoded file: never a two hour mix whole. At 50 Hz
    // so the test is cheap; the windows and the middle's reach are as
    // long in seconds as ever.
    {
        constexpr double rate = 50.0;
        constexpr size_t hours2 = 7200 * 50;
        const auto mix = noise(hours2, 71);
        auto keep = [&](const std::vector<float> &whole, size_t &mostHeld) {
            KeptSamplesBuilder builder(rate);
            for (size_t at = 0; at < whole.size(); at += 997) {
                builder.append(std::span<const float>(whole).subspan(at, std::min<size_t>(997, whole.size() - at)));
            }
            mostHeld = builder.mostHeld();
            return builder.finish();
        };
        size_t mostHeld = 0;
        const KeptSamples kept = keep(mix, mostHeld);
        assert(kept.total == hours2);
        // The first window, the middle and its reach on both sides, the
        // last window: 40 + 220 + 40 s, give or take a sample.
        const size_t expected = static_cast<size_t>((40.0 + 40.0 + 40.0 + 2 * MiddleReachSeconds) * rate) + 2;
        assert(kept.bytes() <= expected * sizeof(float) && "five minutes of a two hour mix kept, held at its size");
        assert(kept.pieces.size() == 3);
        // Half of what has arrived, the first window, and half a window
        // and the reach before the middle, give or take a block.
        assert(mostHeld <= hours2 / 2 + static_cast<size_t>((60.0 + MiddleReachSeconds) * rate) + 3 * 997
               && "never more than about half of it held while it arrives");

        // Lined up from what was kept exactly as from the whole files.
        std::vector<float> later(431, 0.0f);
        later.insert(later.end(), mix.begin(), mix.end() - 431);
        size_t ignored = 0;
        const KeptSamples keptLater = keep(later, ignored);
        assert(alignmentCovers(kept, keptLater, rate));
        const auto fromKept = measureShiftMs(kept, keptLater, rate, Search);
        const auto fromWhole = measureShiftMs(mix, later, rate, Search);
        assert(fromKept && fromWhole && near(*fromKept, *fromWhole, 1e-9) && near(*fromKept, 431 * 1000.0 / rate, 1e-6)
               && "the kept stretches give the whole files' answer");

        // Copies further apart in length than the middle's reach covers
        // are not measured at all.
        std::vector<float> longer = mix;
        const auto extra = noise(static_cast<size_t>((2 * MiddleReachSeconds + 10.0) * rate), 72);
        longer.insert(longer.end(), extra.begin(), extra.end());
        const KeptSamples keptLonger = keep(longer, ignored);
        assert(!alignmentCovers(kept, keptLonger, rate) && "a middle window outside what was kept is no measurement");
        std::vector<float> bitLonger = mix;
        bitLonger.insert(bitLonger.end(), extra.begin(), extra.begin() + static_cast<long>(60 * rate));
        assert(alignmentCovers(kept, keep(bitLonger, ignored), rate) && "a minute longer is within reach");

        // A file shorter than all of that is kept whole, in one piece.
        const std::vector<float> song(mix.begin(), mix.begin() + static_cast<long>(200 * rate));
        const KeptSamples keptSong = keep(song, ignored);
        assert(keptSong.pieces.size() == 1 && keptSong.pieces[0].samples == song);
        assert(keptSong.pieces[0].samples.capacity() == song.size());
    }

    std::cout << "audio_alignment_test: all assertions passed\n";
    return 0;
}
