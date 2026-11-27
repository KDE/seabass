// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "domain/beat_grid.hpp"

namespace djinterop
{
class track;
}

namespace seabass::infrastructure::engine
{

// The same, of a track of a database that is open already.
// readRecordedRate false: the row's trackData is not read (it is damaged,
// EngineBlobGuard), and the rate is worked out from the markers and the
// tempo, as for a record libdjinterop cannot read.
std::vector<domain::Beat> beatGridOf(const djinterop::track &track, bool readRecordedRate = true);

// A track's beat grid from an Engine library. Engine stores markers, not
// beats (see domain::BeatGridMarker), in samples; this gives the beats
// they imply, in milliseconds. Empty when the track is not analysed, or
// anything about it cannot be read.
std::vector<domain::Beat> readBeatGrid(const std::string &engineLibraryPath, const std::string &trackSourceId);

// The same for many tracks, the library opened once: a scan of every
// track's grid (Align Cue Points) otherwise opens the database, and
// checks whether a read copy is needed, once a track, which on a stick
// is most of its time. Each grid as readBeatGrid() gives it, by the id
// asked for; a track with none, or that cannot be read, is left out.
// `stop`, when set, is asked between tracks and ends the read early.
std::map<std::string, std::vector<domain::Beat>> readBeatGrids(const std::string &engineLibraryPath,
                                                              const std::vector<std::string> &trackSourceIds,
                                                              const std::function<bool()> &stop = {});

}  // namespace seabass::infrastructure::engine
