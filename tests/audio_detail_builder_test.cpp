// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::AudioDetailWaveformBuilder on its own, no decoder: sample s is
// in column floor(s * 150 / rate) at any rate (also one 150 does not
// divide), the last column is kept however short, silence stays silence,
// and no samples give no columns.

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
