// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the audio alignment makes of each pair of copies, and why: every
// window's shift, coefficient and ambiguity as the product computes them,
// the verdict measureShiftMs() gives, and which rule made a pair unknown.
// Beside that, diagnostic only and computed here (the product's
// correlationPeak() does not expose them): every window's best rival and
// every local peak within PEAK_SHARE of the best, so other rules can be
// tried on the same numbers without decoding again.
//
//   alignment_dump PAIRS_FILE [PEAK_SHARE]
//
// PAIRS_FILE: one "A<TAB>B" per line (absolute paths; '#' lines skipped).
// PEAK_SHARE defaults to 0.8. Output, one record per line, key=value:
//
//   PAIR i verdict=shift|unknown shiftMs= reason= decodeMs= corrMs= diagMs= ...
//   WIN  i w name= shiftMs= q= amb= edge= outRange= rivalAmb= rivalShiftMs= rivalShare= rivalDistMs=
//   PEAK i w shiftMs= coef= share= inRange=
//   PART i shiftMs= heard= minCoef=      (only for pairs whose windows share samples)
//
// Windows 0..2 are the product's (start, middle, end); further windows
// (extra-*) are placed differently, for comparison only, where the kept
// samples hold them. Needs a QCoreApplication: the decoder runs an event loop.

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "domain/audio_alignment.hpp"
#include "domain/matching_policy.hpp"
#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"

namespace
{

namespace al = seabass::domain::alignment;
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Peak
{
    long long lag = 0;
    double coef = 0.0;
    bool inRange = false;
};

// The product's correlationPeak(), copied, keeping what it throws away:
// the whole coefficient curve's local peaks.
struct Diag
{
    bool ok = false;
    long long bestLag = 0;
    double bestCoef = 0.0;
    bool edge = false;
    bool outRange = false;
    bool rivalAmb = false;  // a rival at RivalShare, as the product decides it
    bool hasRival = false;
    long long rivalLag = 0;  // the highest local peak beyond the separation
    double rivalCoef = 0.0;
    std::vector<Peak> peaks;  // local peaks beyond the separation, >= share * best
};

Diag diagnose(std::span<const float> a, std::span<const float> b, size_t minOverlap, al::LagRange range,
              size_t rivalSeparation, double peakShare)
{
    Diag d;
    const size_t na = a.size();
    const size_t nb = b.size();
    if (na == 0 || nb == 0 || std::min(na, nb) < minOverlap) {
        return d;
    }
    size_t n = 1;
    while (n < na + nb - 1) {
        n <<= 1;
    }
    std::vector<std::complex<double>> z(n);
    for (size_t i = 0; i < na; ++i) {
        z[i].real(a[i]);
    }
    for (size_t i = 0; i < nb; ++i) {
        z[i].imag(b[i]);
    }
    al::fft(z, false);
    for (size_t k = 0; k <= n / 2; ++k) {
        const size_t m = (n - k) % n;
        const std::complex<double> zk = z[k];
        const std::complex<double> zm = z[m];
        const std::complex<double> ak = (zk + std::conj(zm)) * 0.5;
        const std::complex<double> bk = (zk - std::conj(zm)) * std::complex<double>(0.0, -0.5);
        const std::complex<double> am = (zm + std::conj(zk)) * 0.5;
        const std::complex<double> bm = (zm - std::conj(zk)) * std::complex<double>(0.0, -0.5);
        z[k] = std::conj(ak) * bk;
        z[m] = std::conj(am) * bm;
    }
    al::fft(z, true);

    const long long lna = static_cast<long long>(na);
    const long long lnb = static_cast<long long>(nb);
    const long long need = std::max(static_cast<long long>(minOverlap), std::min(lna, lnb) / 2);
    auto overlapAt = [&](long long lag) { return std::min(lna, lnb - lag) - std::max(0LL, -lag); };
    std::vector<double> eA(na + 1, 0.0);
    std::vector<double> eB(nb + 1, 0.0);
    for (size_t i = 0; i < na; ++i) {
        eA[i + 1] = eA[i] + static_cast<double>(a[i]) * static_cast<double>(a[i]);
    }
    for (size_t i = 0; i < nb; ++i) {
        eB[i + 1] = eB[i] + static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    constexpr double SilentShare = 1e-9;
    const double silentA = eA[na] * SilentShare;
    const double silentB = eB[nb] * SilentShare;
    auto coefficientAt = [&](long long lag) {
        const long long from = std::max(0LL, -lag);
        const long long to = std::min(lna, lnb - lag);
        const double energyA = eA[static_cast<size_t>(to)] - eA[static_cast<size_t>(from)];
        const double energyB = eB[static_cast<size_t>(to + lag)] - eB[static_cast<size_t>(from + lag)];
        if (energyA <= silentA || energyB <= silentB || energyA <= 0.0 || energyB <= 0.0) {
            return 0.0;
        }
        const size_t index = lag >= 0 ? static_cast<size_t>(lag) : n - static_cast<size_t>(-lag);
        return z[index].real() / std::sqrt(energyA * energyB);
    };
    long long lowest = -(lna - 1);
    long long highest = lnb - 1;
    while (lowest <= highest && overlapAt(lowest) < need) {
        ++lowest;
    }
    while (highest >= lowest && overlapAt(highest) < need) {
        --highest;
    }
    if (lowest > highest) {
        return d;
    }
    std::vector<double> c(static_cast<size_t>(highest - lowest + 1));
    size_t best = 0;
    for (size_t k = 0; k < c.size(); ++k) {
        c[k] = coefficientAt(lowest + static_cast<long long>(k));
        if (c[k] > c[best]) {
            best = k;
        }
    }
    d.ok = true;
    d.bestLag = lowest + static_cast<long long>(best);
    d.bestCoef = c[best];
    d.edge = c.size() > 1 && (best == 0 || best + 1 == c.size());
    d.outRange = d.bestLag < range.lowest || d.bestLag > range.highest;
    const double rivalLevel = al::RivalShare * c[best];
    const double listLevel = peakShare * c[best];
    for (size_t k = 0; k < c.size(); ++k) {
        const size_t distance = k > best ? k - best : best - k;
        if (distance <= rivalSeparation) {
            continue;
        }
        const bool rises = k == 0 || c[k] >= c[k - 1];
        const bool falls = k + 1 == c.size() || c[k] >= c[k + 1];
        if (!(rises && falls)) {
            continue;
        }
        const long long lag = lowest + static_cast<long long>(k);
        if (!d.hasRival || c[k] > d.rivalCoef) {
            d.hasRival = true;
            d.rivalLag = lag;
            d.rivalCoef = c[k];
        }
        if (c[k] >= rivalLevel) {
            d.rivalAmb = true;
        }
        if (c[k] >= listLevel) {
            d.peaks.push_back({lag, c[k], lag >= range.lowest && lag <= range.highest});
        }
    }
    // Loop-built music has a peak every beat: keep the strongest few hundred.
    if (d.peaks.size() > 300) {
        std::partial_sort(d.peaks.begin(), d.peaks.begin() + 300, d.peaks.end(),
                          [](const Peak &x, const Peak &y) { return x.coef > y.coef; });
        d.peaks.resize(300);
    }
    return d;
}

struct Window
{
    std::string name;
    size_t startA, lengthA, startB, lengthB;
};

// The product's windowsFor(), copied, plus windows placed differently.
std::vector<Window> windowsFor(size_t na, size_t nb, double rate)
{
    const size_t window = static_cast<size_t>(std::llround(al::WindowSeconds * rate));
    const size_t shorter = std::min(na, nb);
    const size_t middle = shorter / 2 > window / 2 ? shorter / 2 - window / 2 : 0;
    std::vector<Window> windows{
        {"start", 0, std::min(window, na), 0, std::min(window, nb)},
        {"middle", middle, std::min(window, na - middle), middle, std::min(window, nb - middle)},
        {"end", na - std::min(window, na), std::min(window, na), nb - std::min(window, nb), std::min(window, nb)},
    };
    // Same index in both, like the middle window: the shift is the lag.
    if (shorter >= 4 * window) {
        for (const double offset : {-60.0, -30.0, 30.0, 60.0}) {
            const long long start = static_cast<long long>(middle) + std::llround(offset * rate);
            if (start > static_cast<long long>(window) && static_cast<size_t>(start) + window + window < shorter) {
                windows.push_back({"mid" + std::string(offset > 0 ? "+" : "") + std::to_string(int(offset)),
                                   static_cast<size_t>(start), window, static_cast<size_t>(start), window});
            }
        }
        const size_t longWindow = 2 * window;
        const size_t longStart = shorter / 2 - longWindow / 2;
        windows.push_back({"mid80", longStart, longWindow, longStart, longWindow});
    }
    return windows;
}

bool overlaps(size_t sx, size_t lx, size_t sy, size_t ly)
{
    return sx < sy + ly && sy < sx + lx;
}

bool windowsShareSamples(size_t na, size_t nb, double rate)
{
    std::vector<Window> w = windowsFor(na, nb, rate);
    w.resize(3);
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = i + 1; j < 3; ++j) {
            if (overlaps(w[i].startA, w[i].lengthA, w[j].startA, w[j].lengthA)
                || overlaps(w[i].startB, w[i].lengthB, w[j].startB, w[j].lengthB)) {
                return true;
            }
        }
    }
    return false;
}

// The product's holdsInEveryPart(), copied, returning the numbers it
// compares instead of the comparison: how many parts had sound, and the
// lowest coefficient among them.
struct PartResult
{
    bool ok = false;
    int heard = 0;
    double minCoef = 1.0;
};

PartResult partsAt(const al::KeptSamples &ka, const al::KeptSamples &kb, double shiftMs, double rate)
{
    PartResult r;
    const auto a = ka.slice(0, ka.total);
    const auto b = kb.slice(0, kb.total);
    if (!a || !b) {
        return r;
    }
    const long long na = static_cast<long long>(ka.total);
    const long long nb = static_cast<long long>(kb.total);
    const long long lag = std::llround(shiftMs * rate / 1000.0);
    const long long from = std::max(0LL, -lag);
    const long long to = std::min(na, nb - lag);
    const long long partLength = std::max(1LL, std::llround(al::PartSeconds * rate));
    const long long parts = std::max(3LL, (to - from) / partLength);
    if (to - from < parts) {
        return r;
    }
    struct Sums
    {
        double cross = 0.0, energyA = 0.0, energyB = 0.0;
    };
    std::vector<Sums> sums(static_cast<size_t>(parts));
    Sums all;
    for (long long k = 0; k < parts; ++k) {
        const long long begin = from + k * (to - from) / parts;
        const long long end = from + (k + 1) * (to - from) / parts;
        Sums &part = sums[static_cast<size_t>(k)];
        for (long long i = begin; i < end; ++i) {
            const double x = (*a)[static_cast<size_t>(i)];
            const double y = (*b)[static_cast<size_t>(i + lag)];
            part.cross += x * y;
            part.energyA += x * x;
            part.energyB += y * y;
        }
        all.energyA += part.energyA;
        all.energyB += part.energyB;
    }
    constexpr double SilentShare = 0.01;
    const double share = 1.0 / static_cast<double>(parts);
    r.ok = true;
    for (const Sums &part : sums) {
        if (part.energyA <= SilentShare * share * all.energyA || part.energyB <= SilentShare * share * all.energyB) {
            continue;
        }
        ++r.heard;
        r.minCoef = std::min(r.minCoef, part.cross / std::sqrt(part.energyA * part.energyB));
    }
    return r;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // QCoreApplication takes the locale from the environment: a decimal
    // comma would make the output unparseable.
    std::setlocale(LC_NUMERIC, "C");
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s PAIRS_FILE [PEAK_SHARE]\n", argv[0]);
        return 2;
    }
    const double peakShare = argc > 2 ? std::stod(argv[2]) : 0.8;
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    const double rate = al::AnalysisRate;
    const double maxShiftSeconds = al::shiftSearchSeconds(seabass::domain::MatchingPolicy::compareAudioSeconds());
    const long long maxShift = std::llround(maxShiftSeconds * rate);
    const size_t minOverlap = static_cast<size_t>(std::ceil(al::MinOverlapSeconds * rate));
    const size_t rivalSeparation = static_cast<size_t>(std::ceil(al::RivalSeparationMs * rate / 1000.0));
    std::printf("# maxShiftSeconds=%.3f rate=%.0f rivalSeparation=%zu peakShare=%.3f\n", maxShiftSeconds, rate,
                rivalSeparation, peakShare);

    seabass::infrastructure::audio::QtMultimediaAlignmentProbe probe;
    std::string line;
    int index = -1;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        ++index;
        const std::string pathA = line.substr(0, tab);
        const std::string pathB = line.substr(tab + 1);
        std::printf("# %d\t%s\t%s\n", index, pathA.c_str(), pathB.c_str());

        auto t = Clock::now();
        const auto ka = probe.decoded(pathA);
        const auto kb = probe.decoded(pathB);
        const double decodeMs = msSince(t);
        if (!ka || !kb) {
            std::printf("PAIR %d verdict=unknown shiftMs=nan reason=decode-failed-%s decodeMs=%.1f corrMs=0 diagMs=0\n",
                        index, !ka ? "a" : "b", decodeMs);
            std::fflush(stdout);
            continue;
        }
        if (!al::alignmentCovers(*ka, *kb, rate)) {
            std::printf("PAIR %d verdict=unknown shiftMs=nan reason=coverage decodeMs=%.1f corrMs=0 diagMs=0 "
                        "na=%zu nb=%zu\n",
                        index, decodeMs, ka->total, kb->total);
            std::fflush(stdout);
            continue;
        }
        t = Clock::now();
        const std::optional<double> verdict = al::measureShiftMs(*ka, *kb, rate, maxShiftSeconds);
        const double corrMs = msSince(t);
        const auto product = al::windowShifts(*ka, *kb, rate, maxShiftSeconds);

        t = Clock::now();
        const std::vector<Window> windows = windowsFor(ka->total, kb->total, rate);
        std::vector<Diag> diags;
        for (const Window &w : windows) {
            const auto sa = ka->slice(w.startA, w.lengthA);
            const auto sb = kb->slice(w.startB, w.lengthB);
            if (!sa || !sb) {
                diags.push_back({});
                continue;
            }
            const long long startDifference = static_cast<long long>(w.startB) - static_cast<long long>(w.startA);
            const al::LagRange range{-maxShift - startDifference, maxShift - startDifference};
            diags.push_back(diagnose(*sa, *sb, minOverlap, range, rivalSeparation, peakShare));
        }
        const double diagMs = msSince(t);

        // Which rule: as cleanShiftMs() and measureOver() apply them.
        std::string reason;
        bool consistent = true;
        if (!product) {
            reason = "window-unmeasurable";
        } else {
            std::vector<std::string> parts;
            for (size_t k = 0; k < product->size() && k < 3; ++k) {
                const auto &p = (*product)[k];
                const Diag &d = diags[k];
                const double diagShift = (static_cast<double>(d.bestLag) + static_cast<double>(windows[k].startB)
                                          - static_cast<double>(windows[k].startA))
                    * 1000.0 / rate;
                if (!d.ok || std::abs(diagShift - p.shiftMs) > 1e-6
                    || p.ambiguous != (d.edge || d.outRange || d.rivalAmb)) {
                    consistent = false;
                }
                if (p.ambiguous) {
                    if (d.edge) {
                        parts.push_back("edge" + std::to_string(k));
                    }
                    if (d.outRange) {
                        parts.push_back("outRange" + std::to_string(k));
                    }
                    if (d.rivalAmb) {
                        parts.push_back("rival" + std::to_string(k));
                    }
                }
                if (!(p.normalised >= al::MinNormalisedPeak)) {
                    parts.push_back("lowPeak" + std::to_string(k));
                }
            }
            for (const auto &part : parts) {
                reason += (reason.empty() ? "" : "+") + part;
            }
            if (reason.empty()) {
                double lo = 1e300, hi = -1e300;
                for (const auto &p : *product) {
                    lo = std::min(lo, p.shiftMs);
                    hi = std::max(hi, p.shiftMs);
                }
                if (hi - lo > al::MaxDisagreementMs) {
                    reason = "disagree";
                } else if (!verdict) {
                    reason = "partCheck";
                } else {
                    reason = "ok";
                }
            }
        }
        const bool share = windowsShareSamples(ka->total, kb->total, rate);
        std::printf("PAIR %d verdict=%s shiftMs=%.4f reason=%s decodeMs=%.1f corrMs=%.1f diagMs=%.1f na=%zu nb=%zu "
                    "shareSamples=%d consistent=%d\n",
                    index, verdict ? "shift" : "unknown", verdict ? *verdict : std::nan(""), reason.c_str(), decodeMs,
                    corrMs, diagMs, ka->total, kb->total, share ? 1 : 0, consistent ? 1 : 0);
        for (size_t k = 0; k < windows.size(); ++k) {
            const Window &w = windows[k];
            const Diag &d = diags[k];
            if (!d.ok) {
                std::printf("WIN %d %zu name=%s ok=0\n", index, k, w.name.c_str());
                continue;
            }
            const double offset = static_cast<double>(w.startB) - static_cast<double>(w.startA);
            const double shiftMs = (static_cast<double>(d.bestLag) + offset) * 1000.0 / rate;
            const double q = k < 3 && product ? (*product)[k].normalised : d.bestCoef;
            const bool amb = d.edge || d.outRange || d.rivalAmb;
            std::printf("WIN %d %zu name=%s ok=1 shiftMs=%.4f q=%.5f fftQ=%.5f amb=%d edge=%d outRange=%d rivalAmb=%d "
                        "rivalShiftMs=%.4f rivalShare=%.5f rivalDistMs=%.4f\n",
                        index, k, w.name.c_str(), shiftMs, q, d.bestCoef, amb ? 1 : 0, d.edge ? 1 : 0,
                        d.outRange ? 1 : 0, d.rivalAmb ? 1 : 0,
                        d.hasRival ? (static_cast<double>(d.rivalLag) + offset) * 1000.0 / rate : std::nan(""),
                        d.hasRival && d.bestCoef > 0 ? d.rivalCoef / d.bestCoef : std::nan(""),
                        d.hasRival ? static_cast<double>(d.rivalLag - d.bestLag) * 1000.0 / rate : std::nan(""));
            for (const Peak &p : d.peaks) {
                std::printf("PEAK %d %zu shiftMs=%.4f coef=%.5f share=%.5f inRange=%d\n", index, k,
                            (static_cast<double>(p.lag) + offset) * 1000.0 / rate, p.coef, p.coef / d.bestCoef,
                            p.inRange ? 1 : 0);
            }
        }
        if (share) {
            // The part check at every candidate the product windows offer.
            std::vector<double> candidates;
            for (size_t k = 0; k < 3 && k < diags.size(); ++k) {
                const Diag &d = diags[k];
                if (!d.ok) {
                    continue;
                }
                const double offset = static_cast<double>(windows[k].startB) - static_cast<double>(windows[k].startA);
                candidates.push_back((static_cast<double>(d.bestLag) + offset) * 1000.0 / rate);
                for (const Peak &p : d.peaks) {
                    candidates.push_back((static_cast<double>(p.lag) + offset) * 1000.0 / rate);
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                         [](double x, double y) { return std::abs(x - y) < 1e-6; }),
                             candidates.end());
            for (const double c : candidates) {
                const PartResult r = partsAt(*ka, *kb, c, rate);
                std::printf("PART %d shiftMs=%.4f ok=%d heard=%d minCoef=%.5f\n", index, c, r.ok ? 1 : 0, r.heard,
                            r.minCoef);
            }
        }
        std::fflush(stdout);
    }
    return 0;
}
