// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/engine.hpp>

#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/scratch_dir_guard.hpp"

namespace seabass::infrastructure::engine
{

namespace fs = std::filesystem;

namespace
{

constexpr int HotCueSlotCount = 8;

djinterop::engine::engine_schema schemaFor(EngineSchemaGeneration generation)
{
    switch (generation) {
    case EngineSchemaGeneration::V1: return djinterop::engine::latest_v1_schema;
    case EngineSchemaGeneration::V2: return djinterop::engine::latest_v2_schema;
    case EngineSchemaGeneration::V3: return djinterop::engine::latest_v3_schema;
    }
    return djinterop::engine::latest_v2_schema;
}

// A default assumed when a track's own sample rate isn't known -- same
// fallback convention (and same reasoning) as
// LibdjinteropEngineCueWriter's own sample-rate handling.
constexpr double DefaultSampleRate = 44100.0;

// Same conversion LibdjinteropEngineCueWriter::writeHotCues() uses,
// duplicated rather than shared: that writer works against an *existing*
// track by reopening the whole database on every call (fine for the
// handful of stragglers Sync/Library Health/Add Cue write one at a time,
// but reopening the entire database once per track for a bulk import of
// an entire library -- confirmed on real data to be well over a thousand
// tracks -- is what actually caused the reported hang: it isn't an
// infinite loop, just an enormous, avoidable amount of per-track I/O
// against what may well be slow removable media). Setting hot_cues/
// main_cue directly on the snapshot before create_track() writes cues as
// part of the same single insert, no reopen at all.
djinterop::pad_color parseColor(const std::string &color)
{
    if (color.size() == 7 && color[0] == '#') {
        auto hexByte = [&](size_t pos) {
            return static_cast<std::uint8_t>(std::stoi(color.substr(pos, 2), nullptr, 16));
        };
        return djinterop::pad_color{hexByte(1), hexByte(3), hexByte(5), 0xFF};
    }
    return djinterop::pad_color{};
}

// Returns the number of cues actually represented in the snapshot (hot
// cue slots filled, plus one if a memory cue was set) -- not simply
// cues.size(), since Engine has exactly one memory-style cue slot, so
// all but the earliest memory cue are unavoidably lost in this
// direction (see libdjinterop_engine_cue_writer.cpp's own comment).
int applyCuesToSnapshot(djinterop::track_snapshot &snapshot, const std::vector<domain::CuePoint> &cues)
{
    if (cues.empty()) {
        return 0;
    }
    std::vector<std::optional<djinterop::hot_cue>> slots(HotCueSlotCount);
    std::optional<double> earliestMemoryCueMs;
    int hotCuesSet = 0;
    for (const auto &cue : cues) {
        if (cue.kind == domain::CuePoint::Kind::Memory) {
            if (!earliestMemoryCueMs || cue.positionMs < *earliestMemoryCueMs) {
                earliestMemoryCueMs = cue.positionMs;
            }
            continue;
        }
        int slot = cue.hotCueNumber - 1;
        if (slot < 0 || slot >= HotCueSlotCount) {
            continue;
        }
        double sampleOffset = cue.positionMs / 1000.0 * DefaultSampleRate;
        slots[static_cast<size_t>(slot)] = djinterop::hot_cue{cue.comment, sampleOffset, parseColor(cue.color)};
        hotCuesSet++;
    }
    snapshot.hot_cues = std::move(slots);
    if (earliestMemoryCueMs) {
        snapshot.main_cue = *earliestMemoryCueMs / 1000.0 * DefaultSampleRate;
    }
    return hotCuesSet + (earliestMemoryCueMs ? 1 : 0);
}

}  // namespace

using infrastructure::ScratchDirGuard;

EngineLibraryCreationResult EngineLibraryCreator::create(const std::string &directory,
                                                           const std::vector<domain::Track> &tracks,
                                                           EngineSchemaGeneration schemaGeneration,
                                                           application::ProgressReporter &reporter,
                                                           const application::CancellationToken &cancel)
{
    EngineLibraryCreationResult result;
    result.tracksTotal = static_cast<int>(tracks.size());

    if (djinterop::engine::database_exists(directory)) {
        result.errorMessage = "An Engine Library already exists at " + directory + " -- refusing to overwrite it.";
        return result;
    }

    // Every track insert is its own implicit SQLite transaction (libdjinterop
    // exposes no public transaction control), which means its own fsync.
    // On fast local storage that's cheap enough to hide; on the slow
    // removable media this library is actually destined for, thousands of
    // individual fsyncs is what makes a large library take a very long
    // time. Rather than patching the vendored library to add transaction
    // batching, the whole database is instead built in a scratch directory
    // on local storage (temp_directory_path() -- /tmp is tmpfs on most
    // Linux setups, and even when it isn't, it's still far faster than a
    // USB stick or SD card), then copied onto the real target in one pass
    // at the end -- a handful of large sequential writes instead of one
    // fsync per track. As a side effect, this also means a crash or a
    // yanked cable during the (now fast) build phase leaves the real stick
    // completely untouched, instead of the half-written "Engine Library"
    // folder this feature used to leave behind before this change.
    fs::path scratchDir = fs::temp_directory_path() /
                           ("seabass-engine-build-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanupBeforeStart;
    fs::remove_all(scratchDir, cleanupBeforeStart);
    ScratchDirGuard scratchGuard{scratchDir};

    try {
        {
            auto db = djinterop::engine::create_database(scratchDir.string(), schemaFor(schemaGeneration));

            reporter.start("Creating Engine Library", tracks.size());
            size_t processed = 0;

            for (const auto &track : tracks) {
                if (cancel.cancelled()) {
                    // The scratch copy dies with scratchGuard; the stick
                    // has not been touched yet.
                    result.cancelled = true;
                    reporter.finish();
                    return result;
                }
                // Streaming tracks have no local file by design (see
                // Track::streamingSource's own doc comment) and rows with no
                // resolved file path can't be referenced from a fresh
                // library at all -- both are skipped, not fabricated.
                if (!track.streamingSource.empty() || track.filePath.empty()) {
                    result.tracksSkipped++;
                    reporter.tick(++processed);
                    continue;
                }

                djinterop::track_snapshot snapshot;
                snapshot.title = track.title.empty() ? std::nullopt : std::optional(track.title);
                snapshot.artist = track.artist.empty() ? std::nullopt : std::optional(track.artist);
                if (track.bpm > 0.0) {
                    snapshot.bpm = track.bpm;
                }
                if (auto key = parseRekordboxKey(track.key)) {
                    snapshot.key = *key;
                }
                if (track.durationSeconds > 0.0) {
                    snapshot.duration =
                        std::chrono::milliseconds(static_cast<int64_t>(track.durationSeconds * 1000.0));
                }
                if (track.bitrate > 0) {
                    snapshot.bitrate = track.bitrate;
                }
                if (track.rating.has_value()) {
                    // Track::rating is 0-5 stars; Engine's own scale is 0-100.
                    snapshot.rating = *track.rating * 20;
                }
                if (!track.comment.empty()) {
                    snapshot.comment = track.comment;
                }
                if (track.fileSizeBytes > 0) {
                    snapshot.file_bytes = track.fileSizeBytes;
                }

                std::error_code relError;
                fs::path relative = fs::relative(track.filePath, directory, relError);
                snapshot.relative_path = relError ? track.filePath : relative.generic_string();

                // Simple, approximate two-point beatgrid: assumes the track
                // starts exactly on a downbeat at sample 0, then a second
                // marker far enough out to cover the whole track at a
                // constant BPM. Not a substitute for a real per-beat grid --
                // see this class's own doc comment for why that's out of
                // scope for now -- but gives Engine something to quantize/
                // sync against rather than nothing at all.
                if (track.bpm > 0.0 && track.durationSeconds > 0.0) {
                    double secondsPerBeat = 60.0 / track.bpm;
                    double totalBeats = track.durationSeconds / secondsPerBeat;
                    int64_t sampleCount = static_cast<int64_t>(track.durationSeconds * DefaultSampleRate);
                    std::vector<djinterop::beatgrid_marker> grid = {
                        {0, 0.0},
                        {static_cast<int>(totalBeats), totalBeats * secondsPerBeat * DefaultSampleRate},
                    };
                    snapshot.beatgrid = djinterop::engine::normalize_beatgrid(grid, sampleCount);
                    snapshot.sample_rate = DefaultSampleRate;
                    snapshot.sample_count = static_cast<unsigned long long>(sampleCount);
                }

                result.cuesCopied += applyCuesToSnapshot(snapshot, track.cues);

                db.create_track(snapshot);
                result.tracksCreated++;
                reporter.tick(++processed);
            }
            reporter.finish();
            // db goes out of scope here, closing its SQLite connection (and
            // with it, any pending journal) before the raw files underneath
            // are copied below -- copying while the connection is still open
            // would risk copying an inconsistent file.
        }

        // One pass of large sequential writes onto the real target, instead
        // of the many small fsync'd writes the per-track loop above would
        // otherwise have done directly against it.
        reporter.start("Copying to stick", 1);
        fs::copy(scratchDir, directory, fs::copy_options::recursive);
        reporter.tick(1);
        reporter.finish();
    } catch (const std::exception &e) {
        result.errorMessage = e.what();
    }

    return result;
}

}  // namespace seabass::infrastructure::engine
