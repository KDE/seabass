// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <bit>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "application/ports/analysis_writer.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

namespace seabass::infrastructure::local
{

namespace detail
{

// The key and tempo of `item` onto `analysis`, as both readers below give
// them.
inline void addKeyAndTempo(application::RekordboxAnalysis &analysis, const application::BeatGridCopyItem &item)
{
    if (!item.key.empty()) {
        auto key = engine::parseRekordboxKey(item.key);
        if (!key) {
            key = engine::parseCamelotKey(item.key);
        }
        if (key) {
            analysis.key = static_cast<int>(*key);
        }
    }
    if (item.bpm > 0.0) {
        analysis.bpm = item.bpm;
    }
}

}  // namespace detail

// rekordbox's analysis of an item, as Beat Grids & Waveforms reads it for
// the page, and the syncs and Create Engine Library for theirs: the grid, the
// waveform sections, the key in Engine's numbering and the tempo. `index`
// (one pass over export.pdb) may be null; each track then finds its
// analysis file the slow way, once. The .DAT, .EXT and .2EX are each read
// once. A track the export does not hold, or an id that is not a number,
// has none of it. Throws what the readers throw.
//
// `beats` and `waveforms` say which parts are read: a caller copying only
// one leaves the other's file unread (over USB, each is a read per track),
// and the part not read stays empty, in the digest below too.
inline application::RekordboxAnalysis rekordboxAnalysisOf(const std::string &pioneerRoot,
                                                          const application::BeatGridCopyItem &item,
                                                          const rekordbox::AnlzPathIndex *index, bool beats = true,
                                                          bool waveforms = true)
{
    namespace rb = infrastructure::rekordbox;
    std::optional<std::string> analyzePath;
    try {
        const auto trackId = static_cast<std::uint32_t>(std::stoul(item.rekordboxTrackId));
        analyzePath = index != nullptr ? index->pathFor(trackId) : rb::findAnlzPathForTrackId(pioneerRoot, trackId);
    } catch (const std::exception &) {
        // Not a number, or an export that cannot be read: no analysis, as
        // the readers say for it.
    }
    application::RekordboxAnalysis analysis;
    if (analyzePath) {
        try {
            const auto source = rb::anlzSourceForPioneerRoot(pioneerRoot);
            if (beats) {
                analysis.beats = rb::readTrackAnalysis(*source, *analyzePath).beats;
            }
            if (waveforms) {
                const auto sections = rb::readWaveformSections(*source, *analyzePath);
                analysis.threeBandPreview = sections.threeBandPreview;
                analysis.colourPreview = sections.colourPreview;
                analysis.scrollEntries = sections.scrollEntries;
            }
        } catch (const std::exception &) {
            // No source for the export: no analysis, as the readers say.
        }
    }
    detail::addKeyAndTempo(analysis, item);
    return analysis;
}

// Everything in `analysis` as one digest: equal digests, the same analysis
// (to a 64-bit hash's chance). For a save that reads rekordbox's analysis
// again to see that it is still the one its preview worked from, which
// only has to notice a change, so a fast hash rather than a SHA-256: a
// preview takes this of every track it would write.
inline std::string rekordboxAnalysisDigest(const application::RekordboxAnalysis &analysis)
{
    std::string bytes;
    // In this machine's byte order: the digest is compared in the process
    // that made it, never stored.
    const auto put = [&bytes](std::uint64_t v) { bytes.append(reinterpret_cast<const char *>(&v), sizeof v); };
    const auto text = [&](const std::optional<std::string> &part) {
        put(part ? part->size() + 1 : 0);
        if (part) {
            bytes += *part;
        }
    };
    bytes.reserve(analysis.beats.size() * 16 + 64 + (analysis.threeBandPreview ? analysis.threeBandPreview->size() : 0)
                  + (analysis.colourPreview ? analysis.colourPreview->size() : 0));
    bytes += "rekordbox analysis 1";
    put(analysis.beats.size());
    std::vector<std::uint64_t> beats;
    beats.reserve(analysis.beats.size() * 2);
    for (const domain::Beat &beat : analysis.beats) {
        beats.push_back(std::bit_cast<std::uint64_t>(beat.timeMs));
        beats.push_back(static_cast<std::uint64_t>(static_cast<std::int64_t>(beat.beatInBar)));
    }
    bytes.append(reinterpret_cast<const char *>(beats.data()), beats.size() * sizeof(std::uint64_t));
    text(analysis.threeBandPreview);
    text(analysis.colourPreview);
    put(analysis.scrollEntries ? *analysis.scrollEntries + 1ull : 0);
    put(analysis.key ? static_cast<std::uint64_t>(static_cast<std::int64_t>(*analysis.key)) + 1 : 0);
    put(analysis.bpm ? std::bit_cast<std::uint64_t>(*analysis.bpm) : 0);
    put(analysis.bpm ? 1 : 0);
    char out[40];
    std::snprintf(out, sizeof out, "%016zx-%zx", std::hash<std::string_view>{}(bytes), bytes.size());
    return out;
}

// The same from the analysis file a row names (`analyzePath`, as
// exportLibrary.db's content.analysisDataFilePath spells it), for a
// OneLibrary row, whose id is not export.pdb's.
inline application::RekordboxAnalysis rekordboxAnalysisAt(const std::string &pioneerRoot,
                                                          const std::string &analyzePath,
                                                          const application::BeatGridCopyItem &item,
                                                          bool beats = true, bool waveforms = true)
{
    namespace rb = infrastructure::rekordbox;
    const auto source = rb::anlzSourceForPioneerRoot(pioneerRoot);
    application::RekordboxAnalysis analysis;
    if (beats) {
        analysis.beats = rb::readTrackAnalysis(*source, analyzePath).beats;
    }
    if (waveforms) {
        const auto sections = rb::readWaveformSections(*source, analyzePath);
        analysis.threeBandPreview = sections.threeBandPreview;
        analysis.colourPreview = sections.colourPreview;
        analysis.scrollEntries = sections.scrollEntries;
    }
    detail::addKeyAndTempo(analysis, item);
    return analysis;
}

}  // namespace seabass::infrastructure::local
