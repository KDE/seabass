// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <optional>
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
//
// `detailUpTo` also hands over every entry of the waveform, not averaged
// down, as TrackAnalysis::detail (the zoomed popup, #104), when there are
// no more than that many (0, the default: never; the player keeps only an
// overview-sized one and reads a larger one again when it is asked for): Engine 2.x and
// 3.x libraries keep only the overview the player draws across the whole
// track (about a thousand entries, all libdjinterop reads from them),
// 1.x libraries the high-resolution waveform as well. Its spacing comes
// from the row's sample count and rate, 0 where either is missing.
//
// A 2.x or 3.x row's waveform, beat and track data blobs are checked
// before libdjinterop decodes them (engineBlobDecodable(), as the cue
// writer does since 347a95da): one too short or with a stream cut short,
// which its decoder would read past or loop on for ever, is not handed
// over. A waveform that does not decode gives no waveform; beat data or
// track data (where the sample rate and count are) that does not gives
// no grid and no recorded rate; whatever does decode is read all the
// same. A 1.x library keeps these blobs in p.db, which this check does
// not read: there the decoder is still handed them unchecked.
domain::TrackAnalysis readTrackAnalysis(const std::string &engineLibraryPath, const std::string &trackSourceId,
                                        std::size_t detailUpTo = 0);

// readTrackAnalysis() for a caller on the GUI thread, which must not copy
// a whole database (#110): nothing (nullopt) when m.db is read from a copy
// and none is ready yet. prepareReadCopy(), off that thread, makes it;
// false when it cannot be made.
std::optional<domain::TrackAnalysis> readTrackAnalysisIfReady(const std::string &engineLibraryPath,
                                                              const std::string &trackSourceId,
                                                              std::size_t detailUpTo = 0);
bool prepareReadCopy(const std::string &engineLibraryPath);

}  // namespace seabass::infrastructure::engine
