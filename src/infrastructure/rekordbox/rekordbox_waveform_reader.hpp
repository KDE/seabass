// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/track_analysis.hpp"
#include "domain/waveform.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"

namespace seabass::infrastructure::rekordbox
{

// Reads the low-resolution monochrome waveform preview (the "PWAV" tag,
// which lives in the track's .DAT file, not the .EXT sibling used for
// cues) for a single track (~400 points, one per column). Best-effort:
// returns an empty vector if the track, its analysis file, or the tag
// itself can't be found -- a missing waveform should never block playback.
//
// Byte layout (documented at
// https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/anlz.html):
// each byte packs a 5-bit height (0-31) in the low bits and a 3-bit
// "whiteness" in the high bits. This tag has no true per-band split (that
// needs the far less documented color/3-band tags), so all three of the
// returned column's bands share the same height, tinted brighter with the
// whiteness bits -- an honest rendering of what this tag actually carries,
// not a fabricated multi-band split.
// `anlzSource` null (the default) reads the analysis file from
// pioneerRoot, as this always did; pass one to serve it from somewhere
// else, such as a stick backup being browsed in place. export.pdb is
// still read from pioneerRoot either way -- findAnlzPathForTrackId()
// needs a seekable file.
std::vector<domain::WaveformColumn> readWaveformPreview(const std::string &pioneerRoot,
                                                          const std::string &trackSourceId,
                                                          std::shared_ptr<AnlzByteSource> anlzSource = nullptr);

// The waveform preview and the beat grid (the PQTZ section) from one read
// and one parse of the track's ANLZ .DAT file. Either half is empty where
// the file has not got it; both are where there is no file, or it cannot
// be read -- a missing analysis is a display that falls back, never an
// error.
//
// `pathIndex` answers "which analysis file is this track's" from memory.
// Without one, that question parses the whole export.pdb: about 20 ms on
// a 1,400-track stick, for every track asked about. A caller reading many
// tracks' analyses in a row (a list of waveforms) should hold an index
// built from the same export.pdb and pass it here.
domain::TrackAnalysis readTrackAnalysis(const std::string &pioneerRoot, const std::string &trackSourceId,
                                        std::shared_ptr<AnlzByteSource> anlzSource = nullptr,
                                        const AnlzPathIndex *pathIndex = nullptr);

// The parts of rekordbox's newer waveforms a Denon overview is made from
// (see domain/engine_overview.hpp), as raw section bodies: each one
// absent where the file or the section is not there.
struct RekordboxWaveformSections
{
    // PWV6, from the .2EX: the three-band preview, 3 bytes an entry.
    std::optional<std::string> threeBandPreview;
    // PWV4, from the .EXT: the colour preview, 6 bytes an entry.
    std::optional<std::string> colourPreview;
    // How many entries the scrolling waveform has (150 a second): PWV7
    // from the .2EX, else PWV5, else PWV3, both from the .EXT. They have
    // had the same count on every track measured.
    std::optional<std::uint32_t> scrollEntries;
};

// Reads those from the .EXT and .2EX siblings of `analyzePath` (the
// path export.pdb gives, ending in .DAT) through `source`, the way the
// cue reader reads the .EXT. A missing file, a missing section, or a
// file that does not parse leaves its parts absent; nothing throws.
RekordboxWaveformSections readWaveformSections(AnlzByteSource &source, const std::string &analyzePath);

// The same for a track of the export under `pioneerRoot`, with the same
// `anlzSource` and `pathIndex` choices as readTrackAnalysis(). All
// absent for a track the export does not hold or an id that is not a
// number.
RekordboxWaveformSections readWaveformSections(const std::string &pioneerRoot, const std::string &trackSourceId,
                                               std::shared_ptr<AnlzByteSource> anlzSource = nullptr,
                                               const AnlzPathIndex *pathIndex = nullptr);

}  // namespace seabass::infrastructure::rekordbox
