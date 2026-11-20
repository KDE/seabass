// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/track_analysis.hpp"
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

// The waveform preview and the beat grid from one opening of the
// database. Either half is empty where the track has not got it.
domain::TrackAnalysis readTrackAnalysis(const std::string &engineLibraryPath, const std::string &trackSourceId);

// Every entry of the waveform libdjinterop reads for the track, not
// downsampled, for a view zoomed in to a few bars (#104). Engine 2.x and
// 3.x libraries keep only the overview the player draws across the whole
// track (about a thousand entries: libdjinterop reads nothing finer from
// them), Engine 1.x libraries the high-resolution waveform as well, and
// that is what libdjinterop hands over there. msPerColumn from the row's
// sample count and rate; 0 where either is missing. Empty where the track
// or its waveform cannot be read; nothing throws.
domain::DetailWaveform readDetailWaveform(const std::string &engineLibraryPath, const std::string &trackSourceId);

}  // namespace seabass::infrastructure::engine
