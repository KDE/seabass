// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/analysis_writer.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

namespace seabass::infrastructure::local
{

// rekordbox's analysis of an item, as Beat Grids & Waveforms reads it for
// the page and Sync after Rekordbox Export for its save: the grid, the
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
    return analysis;
}

}  // namespace seabass::infrastructure::local
