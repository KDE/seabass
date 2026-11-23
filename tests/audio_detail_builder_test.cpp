// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::AudioDetailWaveformBuilder on its own, no decoder: sample s is
// in column floor(s * 150 / rate) at any rate (also one 150 does not
// divide), a click's bands then decay at their release, the last column
// is kept however short, silence stays silence, and no samples give no
// columns.

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "domain/audio_detail_waveform.hpp"

using seabass::domain::AudioDetailWaveformBuilder;
using seabass::domain::DetailWaveform;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

DetailWaveform build(int rate, const std::vector<float> &samples, std::size_t chunk = 1000)
{
    AudioDetailWaveformBuilder builder(rate);
    for (std::size_t at = 0; at < samples.size(); at += chunk) {
        builder.push(samples.data() + at, std::min(chunk, samples.size() - at));
    }
    return builder.finish();
}

double loudest(const DetailWaveform &detail, std::size_t c)
{
    const auto &col = detail.columns[c];
    return std::max({col.low, col.mid, col.high});
}

}  // namespace

int main()
{
    for (const int rate : {44100, 48000, 32000}) {
        // Each sample at and around a column boundary, and some way in.
        const long perColumn = rate / 150;
        for (const long s : {0L, perColumn - 1, perColumn, perColumn + 1, 2 * perColumn, 2 * perColumn + 1,
                             rate - 1L, static_cast<long>(rate), 7L * rate / 3}) {
            std::vector<float> samples(static_cast<std::size_t>(3 * rate), 0.0F);
            samples[static_cast<std::size_t>(s)] = 1.0F;
            const DetailWaveform detail = build(rate, samples, 777);
            const long expected = static_cast<long>(std::floor(static_cast<double>(s) * 150.0 / rate));
            long first = -1;
            for (std::size_t c = 0; c < detail.columns.size(); ++c) {
                if (loudest(detail, c) > 0.0) {
                    first = static_cast<long>(c);
                    break;
                }
            }
            check(first == expected, "rate " + std::to_string(rate) + ": an impulse at sample " + std::to_string(s)
                                         + " lights column " + std::to_string(first) + ", expected "
                                         + std::to_string(expected));
            check(std::abs(loudest(detail, static_cast<std::size_t>(expected)) - 1.0) < 1e-12,
                  "rate " + std::to_string(rate) + ": the impulse's column is the loudest, scaled to 1");
            check(detail.columns.size() == 450, "rate " + std::to_string(rate) + ": three seconds, 450 columns, got "
                                                    + std::to_string(detail.columns.size()));
            check(std::abs(detail.msPerColumn - 1000.0 / 150.0) < 1e-12, "150 columns a second");
        }
    }

    // After an onset each band falls away as a swell, not at once: the
    // columns after a click shrink step by step (no drop to 0 in the next
    // column), at the band's release, and are gone half a second later.
    // The click's own column is unmoved (the impulse checks above).
    for (const int rate : {44100, 48000}) {
        std::vector<float> samples(static_cast<std::size_t>(rate), 0.0F);
        const std::size_t at = static_cast<std::size_t>(rate / 4);
        samples[at] = 1.0F;
        const DetailWaveform detail = build(rate, samples);
        const std::size_t c = at * 150 / static_cast<std::size_t>(rate);
        const std::string r = "rate " + std::to_string(rate) + ": ";
        check(detail.columns[c - 1].high == 0.0 && detail.columns[c].high == 1.0, r + "the click lights its column only");
        // The high band, 40 ms: 6.67 ms a column is a factor of about
        // exp(-6.67 / 40) = 0.85 a column.
        const double perColumn = std::exp(-(1000.0 / 150.0) / AudioDetailWaveformBuilder::HighReleaseMs);
        for (std::size_t k = 1; k <= 6; ++k) {
            const double before = detail.columns[c + k - 1].high;
            const double now = detail.columns[c + k].high;
            check(now > 0.0 && now < before, r + "column " + std::to_string(k) + " after the click is lower but not 0");
            // From one whole column to the next; the click's own column
            // holds it partway in, so the first step is shorter.
            check(k == 1 || std::abs(now / before - perColumn) < 0.01,
                  r + "and falls at the release: " + std::to_string(now / before) + " against "
                      + std::to_string(perColumn));
        }
        check(loudest(detail, c + 75) < 0.001, r + "half a second later it is gone");
    }

    // A last column that is short: 294 samples a column at 44.1 kHz, then 6
    // more, with an impulse in those 6.
    {
        std::vector<float> samples(300, 0.0F);
        samples[297] = 0.5F;
        const DetailWaveform detail = build(44100, samples);
        check(detail.columns.size() == 2, "a short last column is kept: " + std::to_string(detail.columns.size()));
        check(detail.columns.size() == 2 && loudest(detail, 1) == 1.0 && loudest(detail, 0) == 0.0,
              "and holds what was in it");
    }
    // Exactly whole columns: no empty column after them.
    {
        const DetailWaveform detail = build(48000, std::vector<float>(640, 0.25F));
        check(detail.columns.size() == 2, "640 samples at 48 kHz are two columns");
    }

    // All silence: every column 0, and no division by a loudest of 0.
    {
        const DetailWaveform detail = build(44100, std::vector<float>(44100, 0.0F));
        check(detail.columns.size() == 150, "a second of silence, 150 columns");
        bool silent = true;
        for (std::size_t c = 0; c < detail.columns.size(); ++c) {
            silent = silent && loudest(detail, c) == 0.0;
        }
        check(silent, "every column of silence is 0");
    }

    // Nothing at all.
    {
        AudioDetailWaveformBuilder builder(44100);
        builder.reserveSeconds(10);
        const DetailWaveform detail = builder.finish();
        check(detail.columns.empty() && detail.msPerColumn == 0.0, "no samples, no columns and no spacing");
    }

    if (failures > 0) {
        std::cerr << failures << " checks FAILED\n";
        return 1;
    }
    std::cout << "audio_detail_builder_test: all passed\n";
    return 0;
}
