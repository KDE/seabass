// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/database_read_copy.hpp"

#include <chrono>
#include <cmath>
#include <optional>

#include <djinterop/djinterop.hpp>

#include "infrastructure/engine/engine_blob_check.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

// The markers are in samples, so the grid is only as good as the sample
// rate. Engine records it in the track's data blob -- but libdjinterop
// cannot read every shape of that blob and throws on the ones it cannot
// (a fifth of the committed fixture's analysed tracks). The grid itself
// then gives the rate away: samples per beat, against the tempo Engine
// recorded, is samples per second. Taken only when it lands within a
// hundredth of a rate audio files really have; anything else is a guess,
// and a grid that drifts is worse than none. A damaged trackData
// (EngineBlobGuard) is not read at all (`readRecorded` false): its rate is
// worked out the same way.
double sampleRateOf(const djinterop::track &track, const std::vector<djinterop::beatgrid_marker> &markers,
                    bool readRecorded)
{
    try {
        if (const std::optional<double> recorded = readRecorded ? track.sample_rate() : std::nullopt;
            recorded && *recorded > 0.0) {
            return *recorded;
        }
    } catch (const std::exception &) {
        // Fall through to working it out.
    }
    const std::optional<double> bpm = track.bpm();
    const auto &first = markers.front();
    const auto &last = markers.back();
    if (!bpm || *bpm <= 0.0 || last.index <= first.index) {
        return 0.0;
    }
    const double samplesPerBeat = (last.sample_offset - first.sample_offset) / static_cast<double>(last.index - first.index);
    const double implied = samplesPerBeat * *bpm / 60.0;
    for (const double rate : {44100.0, 48000.0, 88200.0, 96000.0, 22050.0, 32000.0, 176400.0, 192000.0}) {
        if (std::abs(implied - rate) <= rate * 0.01) {
            return rate;
        }
    }
    return 0.0;
}

}  // namespace

std::vector<domain::Beat> beatGridOf(const djinterop::track &track, bool readRecordedRate)
{
    const std::vector<djinterop::beatgrid_marker> engineMarkers = track.beatgrid();
    if (engineMarkers.size() < 2) {
        return {};
    }
    const double sampleRate = sampleRateOf(track, engineMarkers, readRecordedRate);
    if (sampleRate <= 0.0) {
        return {};
    }
    std::vector<domain::BeatGridMarker> markers;
    for (const auto &marker : engineMarkers) {
        markers.push_back({marker.index, marker.sample_offset / sampleRate * 1000.0});
    }
    double durationMs = 0.0;
    if (const auto duration = track.duration()) {
        durationMs = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(*duration).count());
    }
    return domain::beatsInOrder(domain::beatsFromMarkers(std::move(markers), durationMs));
}

std::vector<domain::Beat> readBeatGrid(const std::string &engineLibraryPath, const std::string &trackSourceId)
{
    try {
        const std::int64_t id = std::stoll(trackSourceId);
        // From a copy when a read would write beside m.db (#110).
        const StickDatabaseRead read = StickDatabaseRead::engine(engineLibraryPath);
        // Not from a damaged blob (EngineBlobGuard), nor while one may be
        // half written; read unchecked only where the check cannot run.
        // Checked in the database the decode reads (the copy, when there
        // is one), before anything opens it read-write, which would roll
        // a hot journal back and then read what it left.
        std::string error;
        const std::optional<EngineBlobs> damaged = EngineBlobGuard(read.rootUtf8()).damaged(id, &error);
        // A damaged beatData leaves no grid; a damaged trackData only its
        // recorded rate, which the markers then give, as for a record
        // libdjinterop cannot read.
        if (damaged ? contains(*damaged, EngineBlob::BeatData) : !engineBlobGuardFailureReadsUnchecked(error)) {
            return {};
        }
        const bool rateReadable = !damaged || !contains(*damaged, EngineBlob::TrackData);
        if (!djinterop::engine::database_exists(read.rootUtf8())) {
            return {};
        }
        auto db = djinterop::engine::load_database(read.rootUtf8());
        std::optional<djinterop::track> track = db.track_by_id(id);
        return track ? beatGridOf(*track, rateReadable) : std::vector<domain::Beat>{};
    } catch (const std::exception &) {
        return {};
    }
}

}  // namespace seabass::infrastructure::engine
