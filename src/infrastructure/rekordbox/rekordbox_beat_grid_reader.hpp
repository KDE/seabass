// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "domain/beat_grid.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"

namespace seabass::infrastructure::rekordbox
{

// A track's beat grid, from the PQTZ section of its ANLZ .DAT file:
// rekordbox writes every beat out, with its place in the bar. Empty when
// the track has no analysis file, or no grid in it, or it cannot be read
// -- a missing grid is a display that falls back, never an error.
// `anlzSource` as for readWaveformPreview().
std::vector<domain::Beat> readBeatGrid(const std::string &pioneerRoot, const std::string &trackSourceId,
                                       std::shared_ptr<AnlzByteSource> anlzSource = nullptr);

}  // namespace seabass::infrastructure::rekordbox
