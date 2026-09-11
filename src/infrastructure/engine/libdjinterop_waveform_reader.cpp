// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>

#include <djinterop/djinterop.hpp>

namespace seabass::infrastructure::engine
{

namespace
{

constexpr size_t TargetPoints = 400;

}  // namespace

std::vector<domain::WaveformColumn> readWaveformPreview(const std::string &engineLibraryPath,
                                                          const std::string &trackSourceId)
{
    // Best-effort per the header contract -- load_database()/track_by_id()
    // can throw (e.g. the stick was unmounted right as playback was
    // requested), which callers -- notably PlaybackController::load(),
    // invoked directly from every Play click -- don't guard against.
    std::optional<djinterop::track> track;
    std::vector<djinterop::waveform_entry> entries;
    try {
        if (!djinterop::engine::database_exists(engineLibraryPath)) {
            return {};
        }
        auto db = djinterop::engine::load_database(engineLibraryPath);
        track = db.track_by_id(std::stoll(trackSourceId));
        if (!track) {
            return {};
        }
        entries = track->waveform();
    } catch (const std::exception &) {
        return {};
    }
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

}  // namespace seabass::infrastructure::engine
