// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "infrastructure/rekordbox/anlz_file.hpp"

namespace seabass::infrastructure::rekordbox
{

// A track's own analysis files given a new analysis (Browse's Analyse
// again): the beat grid and every waveform taken from files
// buildAnlzFiles() made for the track, the rest of the old file kept byte
// for byte. The cue lists are the DJ's work and are never rebuilt.
//
// Replaced: the grid (PQTZ, PQT2) and the waveforms (PWAV, PWV2, PWV3,
// PWV4, PWV5, PWV6, PWV7, PWVC), each in the old file's place. One the
// old file lacks goes where the built file has it: after the nearest
// section before it in the built file that the result holds, else before
// the nearest one after it, else at the end. One the built file lacks is
// taken out of the old: a grid of one analysis beside the waveforms of
// another would be neither.
//
// Kept: the file header, PPTH, PVBR, every cue list (PCOB, PCO2), and
// every section not named above, unknown ones included, in their order.
// PSSI (the phrases) counts beats of the grid it was made with, so it is
// kept only with `keepPhrases` (phrasesFitNewGrid()).
//
// The result is a copy of `old`: writeRaw() to old's path still refuses
// a file that changed since it was read.
AnlzFile mergeAnlzAnalysis(const AnlzFile &old, const AnlzFile &built, bool keepPhrases);

// Whether `fourcc` is a section mergeAnlzAnalysis() replaces.
bool isReplacedAnalysisSection(std::uint32_t fourcc);

// Every beat's time in ms, as the PQTZ section of a .DAT holds it; empty
// when there is none, or it cannot be read whole.
std::vector<double> pqtzBeatTimes(const AnlzFile &dat);

// Whether phrases counted on the grid `oldBeats` still fit the grid
// `newBeats` (times in ms): its first beat, and the last beat both grids
// have by number, each within a quarter of a beat of the old grid's. A
// phrase is a count of beats from the first, so a grid that starts
// elsewhere or runs at another tempo moves every phrase off its music.
// False when either grid has fewer than two beats.
bool phrasesFitNewGrid(const std::vector<double> &oldBeats, const std::vector<double> &newBeats);

// A track's three analysis files as the stick has them (nullopt: not
// there), and as they are to be written.
struct TrackAnlzFiles
{
    std::optional<AnlzFile> dat;
    std::optional<AnlzFile> ext;
    std::optional<AnlzFile> twoEx;
};

struct MergedTrackAnlzFiles
{
    TrackAnlzFiles files;
    // The .EXT had phrases, and they were kept or taken out.
    bool phrasesKept = false;
    bool phrasesRemoved = false;
};

// The new analysis (the three files buildAnlzFiles() made, as bytes) into
// a track's files, each by mergeAnlzAnalysis(), the phrases by
// phrasesFitNewGrid() between the old .DAT's grid and the new one. A
// .2EX the track lacks is the built one whole: it holds only the path and
// waveforms, nothing of the DJ's. A missing .EXT stays missing: it holds
// the cue lists of newer players, and a track that has none is left as
// rekordbox exported it. nullopt when the .DAT is missing: the row names
// a file that is not there, a fault to repair, not to write around.
// Throws std::runtime_error when a built file is not a valid ANLZ file.
std::optional<MergedTrackAnlzFiles> mergeTrackAnlzFiles(const TrackAnlzFiles &old, const std::string &builtDat,
                                                        const std::string &builtExt, const std::string &builtTwoEx);

}  // namespace seabass::infrastructure::rekordbox
