// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

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
std::vector<domain::Beat> beatGridOf(const djinterop::track &track);

// A track's beat grid from an Engine library. Engine stores markers, not
// beats (see domain::BeatGridMarker), in samples; this gives the beats
// they imply, in milliseconds. Empty when the track is not analysed, or
// anything about it cannot be read.
std::vector<domain::Beat> readBeatGrid(const std::string &engineLibraryPath, const std::string &trackSourceId);

}  // namespace seabass::infrastructure::engine
