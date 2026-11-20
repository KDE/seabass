// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#include <djinterop/djinterop.hpp>

#include "application/ports/kept_engine_analysis.hpp"
#include "infrastructure/engine/engine_blob_check.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

constexpr size_t TargetPoints = 400;

// Whether the row records the sample rate its beat markers count in:
// the same blob field the catalog reader turns cues into times with,
// where an unrecorded rate is Track::cueRateUnverified. A record
// libdjinterop cannot read records nothing either.
bool sampleRateRecorded(const djinterop::track &track)
{
    try {
        const std::optional<double> rate = track.sample_rate();
        return rate && *rate > 0.0;
    } catch (const std::exception &) {
        return false;
    }
}

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

namespace
{

// Which of a 2.x or 3.x row's blobs libdjinterop may be handed (see
// engineBlobDecodable()). A library without Database2/m.db is 1.x: its
// blobs are elsewhere and are not checked. A row that cannot be read
// here is not handed over at all: unsure means leave it.
struct DecodableBlobs
{
    bool waveform = true;
    bool beats = true;
    // trackData: the sample rate and count are read from it.
    bool trackData = true;
};

DecodableBlobs decodableBlobs(const std::string &engineLibraryPath, std::int64_t trackId)
{
    const std::filesystem::path mainDb = pathFromUtf8(engineLibraryPath) / Database2Dir / MainDbName;
    std::error_code ec;
    if (!std::filesystem::exists(mainDb, ec)) {
        return {};
    }
    std::string error;
    auto row = readEngineAnalysisRow(pathToUtf8(mainDb), trackId, &error);
    if (!row) {
        // Not a damaged blob: the row could not be read at all (the
        // database locked or being journalled by a write, a moment ago).
        // Once more; then nothing is handed over. That is exactly when a
        // blob may be half written, and a decoder that reads past it or
        // loops on it hangs whoever called (the player's load() runs on
        // the GUI thread). The track shows no waveform or grid for now,
        // and every view reads it again once the write is over.
        row = readEngineAnalysisRow(pathToUtf8(mainDb), trackId, &error);
        if (!row) {
            return {false, false, false};
        }
    }
    const auto ok = [](const application::EngineSqlValue &value) {
        return value.isNull() || internal::engineBlobDecodable(value.bytes);
    };
    return {ok(row->columns.overviewWaveFormData), ok(row->columns.beatData), ok(row->columns.trackData)};
}

}  // namespace

domain::TrackAnalysis readTrackAnalysis(const std::string &engineLibraryPath, const std::string &trackSourceId,
                                        std::size_t detailUpTo)
{
    domain::TrackAnalysis analysis;
    try {
        if (!djinterop::engine::database_exists(engineLibraryPath)) {
            return {};
        }
        const std::int64_t trackId = std::stoll(trackSourceId);
        const DecodableBlobs decodable = decodableBlobs(engineLibraryPath, trackId);
        auto db = djinterop::engine::load_database(engineLibraryPath);
        std::optional<djinterop::track> track = db.track_by_id(trackId);
        if (!track) {
            return {};
        }
        // Each half in its own try: a waveform libdjinterop cannot read
        // is no reason to throw the grid away with it, nor the other way.
        if (decodable.waveform) {
            try {
                const std::vector<djinterop::waveform_entry> entries = track->waveform();
                analysis.waveform = previewOf(entries);
                if (!entries.empty() && entries.size() <= detailUpTo) {
                    analysis.detail.columns.reserve(entries.size());
                    for (const djinterop::waveform_entry &entry : entries) {
                        analysis.detail.columns.push_back(
                            {entry.low.value / 255.0, entry.mid.value / 255.0, entry.high.value / 255.0});
                    }
                    // The spacing from trackData, only where it decodes,
                    // and in a try of its own: a trackData libdjinterop
                    // throws on leaves the detail unspaced (msPerColumn
                    // 0), never takes the overview away with it.
                    std::optional<unsigned long long> samples;
                    std::optional<double> rate;
                    if (decodable.trackData) {
                        try {
                            samples = track->sample_count();
                            rate = track->sample_rate();
                        } catch (const std::exception &) {
                            samples.reset();
                            rate.reset();
                        }
                    }
                    if (samples && *samples > 0 && rate && *rate > 0.0) {
                        analysis.detail.msPerColumn = static_cast<double>(*samples) / *rate * 1000.0
                            / static_cast<double>(entries.size());
                    }
                }
            } catch (const std::exception &) {
                analysis.waveform.clear();
                analysis.detail = {};
            }
        }
        // The grid is placed by the rate trackData records (or works one
        // out), so it needs both blobs. A trackData that does not decode
        // is not read at all, so no rate is known from it, and a grid whose
        // rate is not known is not drawn (#40). (A row whose trackData is
        // NULL is read as ever: libdjinterop gives no rate, and the beat
        // reader works one out from the markers.)
        if (decodable.beats && decodable.trackData) {
            try {
                analysis.beats = beatGridOf(*track);
            } catch (const std::exception &) {
                analysis.beats.clear();
            }
        }
        analysis.beatRateRecorded = decodable.trackData && sampleRateRecorded(*track);
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
