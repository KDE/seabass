// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "application/ports/track_metadata_probe.hpp"
#include "infrastructure/local/metadata_cache.hpp"

namespace seabass::infrastructure::local
{

// The sample rate of an audio file on a stick, as
// LibdjinteropEngineReader asks for it for a row that has cues and
// records no rate: from the stick's MetadataCache when it holds the file
// at its current size and mtime with a rate, otherwise from the probe,
// once, with the answer stored in the cache. save() writes the cache
// back; a caller calls it at the end of its pass, also when the pass
// ends early, since what was probed stays true.
//
// The cache is loaded on the first question, so a pass that asks nothing
// does not read it. An entry with no rate (a missing "samplerate" field
// reads as 0) is probed again rather than trusted.
//
// One thread per object: MetadataCache is not synchronised. A pass makes
// its own and drops it at the end.
class CachedSampleRates
{
public:
    CachedSampleRates(std::string stickRoot, application::TrackMetadataProbe &probe)
        : m_stickRoot(std::move(stickRoot)), m_probe(probe)
    {
    }

    std::optional<double> rateOf(const std::string &absoluteFilePath)
    {
        if (!m_cache) {
            m_cache.emplace(m_stickRoot);
        }
        if (const auto cached = m_cache->lookup(absoluteFilePath); cached && cached->sampleRate > 0) {
            return static_cast<double>(cached->sampleRate);
        }
        ++m_probes;
        const auto read = m_probe.read(absoluteFilePath);
        if (!read) {
            return std::nullopt;
        }
        m_cache->store(absoluteFilePath, *read);
        if (read->sampleRate <= 0) {
            return std::nullopt;
        }
        return static_cast<double>(read->sampleRate);
    }

    // False when the cache could not be written (a read-only or full
    // stick): that costs a probe next time, never the pass.
    bool save() { return !m_cache || m_cache->save(); }

    // Files handed to the probe so far.
    std::size_t probes() const { return m_probes; }

private:
    std::string m_stickRoot;
    application::TrackMetadataProbe &m_probe;
    std::optional<MetadataCache> m_cache;
    std::size_t m_probes = 0;
};

}  // namespace seabass::infrastructure::local
