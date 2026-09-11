// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "domain/waveform.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"

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

}  // namespace seabass::infrastructure::rekordbox
