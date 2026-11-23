// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace seabass::domain
{

// A track's audio as an analyser sees it: one channel of float samples
// (the mean of the file's channels, -1 to 1), pulled in pieces from the
// start to the end. Sample 0 is the first sample the decoder gives,
// which for a file Qt Multimedia decodes is Engine's timeline
// (infrastructure/audio/playback_timeline.hpp), so a time an analyser
// finds here is a time on that timeline.
//
// Pulled rather than handed over whole, so a source that decodes a file
// holds a few buffers at a time and not a six-minute track; an analyser
// that wants the whole track keeps it itself.
class PcmSource
{
public:
    virtual ~PcmSource() = default;

    // Samples per second. Known from the start: a source that has to
    // decode before it knows sets it up front or fails (error()). 0 only
    // when error() says why.
    virtual int sampleRate() const = 0;

    // Fills the front of `out` with the next samples and returns how many
    // it wrote. 0 means the end of the audio, or a failure when error()
    // is not empty; a short count before that is allowed, so a caller
    // loops until 0.
    virtual std::size_t read(std::span<float> out) = 0;

    // Empty while all is well. A source that fails part of the way
    // through keeps what it read valid and stops there, so a caller must
    // ask this after the last read: a track cut short by a decode error
    // is not the whole track.
    virtual std::string error() const = 0;
};

// Samples already in memory: generated tracks, and the tests of every
// analyser, which then link no decoder.
class InMemoryPcmSource : public PcmSource
{
public:
    InMemoryPcmSource(std::vector<float> samples, int sampleRate)
        : m_samples(std::move(samples)), m_sampleRate(sampleRate)
    {
        if (m_sampleRate <= 0) {
            m_error = "sample rate must be positive";
            m_sampleRate = 0;
        }
    }

    int sampleRate() const override { return m_sampleRate; }

    std::size_t read(std::span<float> out) override
    {
        if (!m_error.empty()) {
            return 0;
        }
        const std::size_t count = std::min(out.size(), m_samples.size() - m_position);
        std::copy_n(m_samples.begin() + static_cast<std::ptrdiff_t>(m_position), count, out.begin());
        m_position += count;
        return count;
    }

    std::string error() const override { return m_error; }

private:
    std::vector<float> m_samples;
    int m_sampleRate = 0;
    std::size_t m_position = 0;
    std::string m_error;
};

// Every sample left in `source`, in one vector. For analysers that work
// on the whole track and for tests; the rest of the source is consumed.
inline std::vector<float> readAllSamples(PcmSource &source)
{
    std::vector<float> all;
    std::vector<float> chunk(16384);
    for (;;) {
        const std::size_t got = source.read(chunk);
        if (got == 0) {
            break;
        }
        all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return all;
}

}  // namespace seabass::domain
