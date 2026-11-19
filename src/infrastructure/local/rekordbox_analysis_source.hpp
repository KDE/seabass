// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/analysis_writer.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
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
// analysis file the slow way. Throws what the readers throw.
inline application::RekordboxAnalysis rekordboxAnalysisOf(const std::string &pioneerRoot,
                                                          const application::BeatGridCopyItem &item,
                                                          const rekordbox::AnlzPathIndex *index)
{
    namespace rb = infrastructure::rekordbox;
    application::RekordboxAnalysis analysis;
    analysis.beats = rb::readTrackAnalysis(pioneerRoot, item.rekordboxTrackId, nullptr, index).beats;
    const auto sections = rb::readWaveformSections(pioneerRoot, item.rekordboxTrackId, nullptr, index);
    analysis.threeBandPreview = sections.threeBandPreview;
    analysis.colourPreview = sections.colourPreview;
    analysis.scrollEntries = sections.scrollEntries;
    detail::addKeyAndTempo(analysis, item);
    return analysis;
}

// The same from the analysis file a row names (`analyzePath`, as
// exportLibrary.db's content.analysisDataFilePath spells it), for a
// OneLibrary row, whose id is not export.pdb's.
inline application::RekordboxAnalysis rekordboxAnalysisAt(const std::string &pioneerRoot,
                                                          const std::string &analyzePath,
                                                          const application::BeatGridCopyItem &item)
{
    namespace rb = infrastructure::rekordbox;
    const auto source = rb::anlzSourceForPioneerRoot(pioneerRoot);
    application::RekordboxAnalysis analysis;
    analysis.beats = rb::readTrackAnalysis(*source, analyzePath).beats;
    const auto sections = rb::readWaveformSections(*source, analyzePath);
    analysis.threeBandPreview = sections.threeBandPreview;
    analysis.colourPreview = sections.colourPreview;
    analysis.scrollEntries = sections.scrollEntries;
    detail::addKeyAndTempo(analysis, item);
    return analysis;
}

}  // namespace seabass::infrastructure::local
