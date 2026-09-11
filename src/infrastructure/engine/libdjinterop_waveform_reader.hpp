// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/waveform.hpp"

namespace seabass::infrastructure::engine
{

// Reads an Engine Library track's waveform (via libdjinterop's
// track::waveform(), which is typically full audio resolution -- one
// entry per few dozen samples) and downsamples it to ~400 points by
// averaging each bucket's low/mid/high bands separately, normalized to
// 0..1 -- Engine's format genuinely carries a true 3-band split, unlike
// rekordbox's basic preview tag. Best-effort: returns an empty vector if
// the track or its waveform data can't be found -- a missing waveform
// should never block playback.
std::vector<domain::WaveformColumn> readWaveformPreview(const std::string &engineLibraryPath,
                                                          const std::string &trackSourceId);

}  // namespace seabass::infrastructure::engine
