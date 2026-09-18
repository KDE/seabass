// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>

#include <djinterop/djinterop.hpp>

#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

constexpr size_t TargetPoints = 400;

}  // namespace

namespace
{

std::vector<domain::WaveformColumn> previewOf(const std::vector<djinterop::waveform_entry> &entries)
{
    if (entries.empty()) {
        return {};
    }

    // Downsample to ~TargetPoints columns by averaging each band separately
    // within each bucket, keeping the low/mid/high split intact.
    std::vector<domain::WaveformColumn> waveform;
    waveform.reserve(TargetPoints);
    size_t bucketSize = std::max<size_t>(1, entries.size() / TargetPoints);
    for (size_t start = 0; start < entries.size(); start += bucketSize) {
        size_t end = std::min(entries.size(), start + bucketSize);
        double lowSum = 0.0, midSum = 0.0, highSum = 0.0;
        size_t count = 0;
        for (size_t i = start; i < end; ++i) {
            lowSum += entries[i].low.value;
            midSum += entries[i].mid.value;
            highSum += entries[i].high.value;
            count++;
        }
        domain::WaveformColumn col;
        if (count > 0) {
            col.low = (lowSum / count) / 255.0;
            col.mid = (midSum / count) / 255.0;
            col.high = (highSum / count) / 255.0;
        }
        waveform.push_back(col);
    }
    return waveform;
}

}  // namespace

domain::TrackAnalysis readTrackAnalysis(const std::string &engineLibraryPath, const std::string &trackSourceId)
{
    domain::TrackAnalysis analysis;
    try {
        if (!djinterop::engine::database_exists(engineLibraryPath)) {
            return {};
        }
        auto db = djinterop::engine::load_database(engineLibraryPath);
        std::optional<djinterop::track> track = db.track_by_id(std::stoll(trackSourceId));
        if (!track) {
            return {};
        }
        analysis.waveform = previewOf(track->waveform());
        // Its own try: a grid libdjinterop cannot read is no reason to
        // throw the waveform away with it.
        try {
            analysis.beats = beatGridOf(*track);
        } catch (const std::exception &) {
            analysis.beats.clear();
        }
    } catch (const std::exception &) {
        return {};
    }
    return analysis;
}

std::vector<domain::WaveformColumn> readWaveformPreview(const std::string &engineLibraryPath,
                                                          const std::string &trackSourceId)
{
    return readTrackAnalysis(engineLibraryPath, trackSourceId).waveform;
}

}  // namespace seabass::infrastructure::engine
