// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

namespace seabass::infrastructure::rekordbox
{

std::vector<domain::Beat> readBeatGrid(const std::string &pioneerRoot, const std::string &trackSourceId,
                                       std::shared_ptr<AnlzByteSource> anlzSource)
{
    return readTrackAnalysis(pioneerRoot, trackSourceId, std::move(anlzSource)).beats;
}

}  // namespace seabass::infrastructure::rekordbox
