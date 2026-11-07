// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "application/ports/track_metadata_probe.hpp"
#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
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
        loadCache();
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

    // The lead-in of an MP3 (#50), the same way: from the cache when it
    // holds the unchanged file with its lead-in read, otherwise from the
    // probe, which reads it with the rest and has it cached. When the
    // probe gives no lead-in (no TagLib in this build, or no playing
    // length TagLib could find) the file's start is read directly, the
    // way the Engine cue writer reads it, so a cue read through here and
    // written back lands where it was; that answer is cached too, with
    // whatever the probe or the cache already held for the file, or on
    // its own (MetadataCache: an entry with a lead-in and no length), so
    // the next pass does not read the file again. A read that finds no
    // lead-in is not cached: it may be a read that failed.
    std::optional<domain::Mp3LeadIn> leadInOf(const std::string &absoluteFilePath)
    {
        loadCache();
        if (const auto cached = m_cache->lookupLeadIn(absoluteFilePath); cached && cached->mp3LeadInRead) {
            return cached->mp3LeadIn;
        }
        ++m_probes;
        const auto read = m_probe.read(absoluteFilePath);
        if (read && read->mp3LeadInRead) {
            m_cache->store(absoluteFilePath, *read);
            return read->mp3LeadIn;
        }
        const auto direct = audio::readMp3LeadIn(absoluteFilePath);
        if (direct) {
            application::FileMetadata entry;
            if (read && read->durationSeconds > 0.0) {
                entry = *read;
            } else if (const auto known = m_cache->lookup(absoluteFilePath)) {
                entry = *known;
            }
            entry.mp3LeadInRead = true;
            entry.mp3LeadIn = direct;
            m_cache->store(absoluteFilePath, entry);
        }
        return direct;
    }

    // The same for a pass that must not stat every file: the cached
    // lead-in taken by path, unchecked, or the file's start read directly
    // when the cache has none. No probe and nothing stored; a later
    // leadInOf() pass checks what was taken here.
    std::optional<domain::Mp3LeadIn> leadInByPathOf(const std::string &absoluteFilePath)
    {
        loadCache();
        if (const auto cached = m_cache->lookupLeadIn(absoluteFilePath, false); cached && cached->mp3LeadInRead) {
            return cached->mp3LeadIn;
        }
        return audio::readMp3LeadIn(absoluteFilePath);
    }

    // False when the cache could not be written (a read-only or full
    // stick): that costs a probe next time, never the pass.
    bool save() { return !m_cache || m_cache->save(); }

    // Files handed to the probe so far.
    std::size_t probes() const { return m_probes; }

private:
    void loadCache()
    {
        if (!m_cache) {
            m_cache.emplace(m_stickRoot);
        }
    }

    std::string m_stickRoot;
    application::TrackMetadataProbe &m_probe;
    std::optional<MetadataCache> m_cache;
    std::size_t m_probes = 0;
};

}  // namespace seabass::infrastructure::local
