// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/v2/engine_library.hpp>
#include <djinterop/engine/v3/engine_library.hpp>

#include "domain/engine_overview.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/engine/engine_cue_timeline.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/work_counters.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

constexpr int HotCueSlotCount = 8;

// Engine's own colour for a pad, for a cue or loop that brings none.
// Measured on WHALESHARK2 (2026-10-02) from the cues Engine OS itself
// labelled "Cue N" / "Loop N": pad 1 yellow, 2 orange, 3 purple, 4 red.
// No sample for pads 5 to 8 on that stick, so those repeat the four:
// visible is what matters here, not which shade.
djinterop::pad_color defaultPadColor(int slot)
{
    static const djinterop::pad_color palette[4] = {
        {0xF4, 0xD3, 0x38, 0xFF},
        {0xEF, 0x81, 0x30, 0xFF},
        {0xAA, 0x55, 0xC4, 0xFF},
        {0xCE, 0x32, 0x39, 0xFF},
    };
    return palette[((slot - 1) % 4 + 4) % 4];
}

// A pad's colour, never without one. The Prime 4 does not show a saved
// loop whose colour has alpha 0, and a hot cue is the same kind of
// record: on WHALESHARK2 (2026-10-02) two hot loops synced from rekordbox
// without a colour sat in the blob, byte for byte like the player's own
// but for the colour, and the Saved Loops bank stayed empty; the same
// loops with the pad's default colour showed. So a cue with no colour,
// or rekordbox's black (its "no colour"), takes Engine's default for the
// pad. The reader maps alpha 0 back to no colour, so nothing here is
// compared against anything (colour never is).
djinterop::pad_color padColor(const std::string &color, int slot)
{
    if (color.size() == 7 && color[0] == '#' && color != "#000000") {
        auto hexByte = [&](size_t pos) {
            return static_cast<std::uint8_t>(std::stoi(color.substr(pos, 2), nullptr, 16));
        };
        return djinterop::pad_color{hexByte(1), hexByte(3), hexByte(5), 0xFF};
    }
    return defaultPadColor(slot);
}

}  // namespace

LibdjinteropEngineCueWriter::LibdjinteropEngineCueWriter(std::string engineLibraryPath)
    : m_engineLibraryPath(std::move(engineLibraryPath))
    , m_leadInSource([](const std::string &file) { return audio::readMp3LeadIn(file); })
{
}

// One of the two, by the library's schema: 2.x keeps beat data in the
// Track table, 3.x in PerformanceData.
struct LibdjinteropEngineCueWriter::Tables
{
    std::optional<djinterop::engine::v2::engine_library> v2;
    std::optional<djinterop::engine::v3::engine_library> v3;
};

djinterop::database &LibdjinteropEngineCueWriter::database()
{
    if (!m_database) {
        WorkCounters::instance().noteEngineDatabaseOpen();
        // What load_database() does, with the library kept: its tables
        // and the database below then share one connection, so a beat
        // grid written through one is what the other reads. Engine 1.x
        // has no such library and is opened as before.
        namespace dje = djinterop::engine;
        if (dje::v3::engine_library::exists(m_engineLibraryPath)) {
            auto library = dje::v3::engine_library::load(m_engineLibraryPath);
            if (library.schema() >= dje::engine_schema::schema_3_0_0) {
                m_database = library.database();
                m_tables = std::make_shared<Tables>();
                m_tables->v3 = std::move(library);
            } else if (library.schema() >= dje::engine_schema::schema_2_18_0) {
                // Opened again as what it is. Only on 2.x, once per writer.
                auto v2 = dje::v2::engine_library::load(m_engineLibraryPath);
                m_database = v2.database();
                m_tables = std::make_shared<Tables>();
                m_tables->v2 = std::move(v2);
            }
        }
        if (!m_database) {
            m_database = djinterop::engine::load_database(m_engineLibraryPath);
        }
    }
    return *m_database;
}

void LibdjinteropEngineCueWriter::writeAnnotation(const std::string &trackSourceId,
                                                    const std::optional<int> &stars,
                                                    const std::optional<std::string> &comment)
{
    if (!stars && !comment) {
        return;
    }

    auto &db = database();
    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }

    if (stars && *stars > 0) {
        // Engine stores 0 to 100; domain::Track carries 0 to 5, which is
        // what every reader normalises to. Twenty per star, so three
        // stars is 60 and the value round-trips through the reader's own
        // division unchanged.
        track->set_rating(*stars * 20);
    }
    // A zero is deliberately not written. Engine has one value for
    // "unrated" and for "rated zero stars", and the reader maps anything
    // <= 0 back to no rating at all -- so writing a 0 would not store a
    // zero-star rating, it would clear the field and then be offered
    // again by the next scan, for ever. Same distinction export.pdb
    // cannot make; see docs/metadata-backup-plan.md. No reader stores a
    // 0 today (they all guard on > 0), so this is a guard against a
    // future one rather than a live case.
    if (comment) {
        track->set_comment(*comment);
    }
}

void LibdjinteropEngineCueWriter::writeHotCues(const std::string &trackSourceId,
                                                const std::vector<domain::CuePoint> &cues)
{
    writeCues(trackSourceId, cues, std::nullopt);
}

void LibdjinteropEngineCueWriter::writeHotCuesAtSampleRate(const std::string &trackSourceId,
                                                            const std::vector<domain::CuePoint> &cues,
                                                            double sampleRateHz)
{
    if (!(sampleRateHz > 0.0)) {
        throw std::invalid_argument("writeHotCuesAtSampleRate: no sample rate for track id=" + trackSourceId);
    }
    writeCues(trackSourceId, cues, sampleRateHz);
}

double LibdjinteropEngineCueWriter::shiftMsOf(djinterop::track &track)
{
    std::string relative;
    try {
        relative = track.relative_path();
    } catch (const std::exception &) {
        // No path names no file: nothing to shift by.
    }
    if (relative.empty() || !m_leadInSource) {
        return 0.0;
    }
    const std::string &audioRoot = m_audioRoot.empty() ? m_engineLibraryPath : m_audioRoot;
    const std::string filePath = pathToUtf8((pathFromUtf8(audioRoot) / pathFromUtf8(relative)).lexically_normal());
    return shiftsCueTimeline(filePath, {}) ? engineCueShiftMs(m_leadInSource(filePath)) : 0.0;
}

double LibdjinteropEngineCueWriter::cueTimelineShiftMs(const std::string &trackSourceId)
{
    auto track = database().track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    return shiftMsOf(*track);
}

void LibdjinteropEngineCueWriter::writeCues(const std::string &trackSourceId,
                                             const std::vector<domain::CuePoint> &cues,
                                             std::optional<double> knownSampleRate)
{
    auto &db = database();

    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }

    // The caller's rate when it read one from the file. Otherwise the
    // row's: some tracks' sample_rate() throws even though set_hot_cues()
    // below works fine (same libdjinterop decoder quirk documented in the
    // Engine reader: it lives in a different blob than the one hot cues
    // use). Fall back to 44.1kHz; see libdjinterop_engine_reader.cpp
    // for the same reasoning.
    double sampleRate = 44100.0;
    if (knownSampleRate) {
        sampleRate = *knownSampleRate;
    } else {
        try {
            if (auto rate = track->sample_rate()) {
                sampleRate = *rate;
            }
        } catch (const std::exception &) {
            // fall back to the default above
        }
    }

    // The cues arrive on rekordbox's timeline; an MP3's are moved back
    // onto Engine's (#50, engine_cue_timeline.hpp), by the shift the
    // reader adds for the same file. What the shift puts before Engine's
    // 0:
    //   - a hot cue is clamped to 0;
    //   - a loop's start is clamped to 0 and its end stays at the same
    //     audio instant, so the loop keeps its musical end and is that
    //     much shorter;
    //   - a loop that ends there too would be [0, 0], no loop at all (the
    //     waveform draws a loop only when its end is after its start),
    //     so it is left out and counted (loopsDroppedByLeadInShift());
    //   - a memory cue is left out and counted (see below), because
    //     Engine reads a main cue at 0 as no cue at all.
    // Create Engine Library applies the same rule (applyCuesToSnapshot).
    const double shiftMs = shiftMsOf(*track);
    const auto toSamples = [&](double rekordboxMs) { return std::max(0.0, (rekordboxMs - shiftMs) / 1000.0 * sampleRate); };

    std::vector<std::optional<djinterop::hot_cue>> slots(HotCueSlotCount);
    // Engine's hot loops are a separate 8-slot array (loop_at()/loops()),
    // indexed the same 1-8 way as hot_cues() but never the same array --
    // see libdjinterop_engine_reader.cpp's read-side comment on why a
    // slot can't hold both at once by Seabass's own design, enforced
    // here by construction: each cue below writes into exactly one of
    // these two arrays, and both are written in full (via set_hot_cues/
    // set_loops below), so a slot that used to be a hot cue and is now a
    // loop (or vice versa) is correctly cleared on its old side too.
    std::vector<std::optional<djinterop::loop>> loopSlots(HotCueSlotCount);
    // Engine has exactly one memory-style cue point ("Cue"), a plain
    // sample offset with no color/comment/multiplicity -- unlike
    // rekordbox's unlimited, independently colored/commented memory
    // cues. When more than one memory cue is being written, only the
    // earliest (by position) can be represented at all; the rest are
    // unavoidably lost in this direction, a real format limitation, not
    // a bug -- see libdjinterop_engine_reader.cpp's read-side comment.
    // Engine has no memory-loop concept at all (unlike rekordbox's ANLZ),
    // so a Kind::Memory cue with isLoop set can't be represented here
    // either -- treated the same as any other memory cue, its loop-out
    // is simply dropped, matching this format's genuine limitations.
    //
    // The earliest Engine can hold, not the earliest given: a memory cue
    // at 0:00 would be stored as offset 0, which Engine reads as no cue
    // (domain::engineCanHoldMemoryCue), so taking it would throw away a
    // later cue that could have been kept. The sync planner leaves such
    // cues out before they get here; this is the same rule, kept for
    // every other caller.
    std::optional<double> earliestMemoryCueMs;
    for (const auto &cue : cues) {
        if (cue.kind == domain::CuePoint::Kind::Memory) {
            // And one the lead-in shift puts at or before Engine's 0 is
            // as unrepresentable as one at 0:00. Not clamped like a hot
            // cue: clamped, it would be the "no cue" offset 0. Counted
            // instead (memoryCuesDroppedByLeadInShift()), since unlike a
            // cue at 0:00 it is a real cue the user set.
            if (!domain::engineCanHoldMemoryCue(cue)) {
                continue;
            }
            if (cue.positionMs - shiftMs <= 0.0) {
                ++m_memoryCuesDroppedByLeadInShift;
                if (m_drops) {
                    ++m_drops->memoryCues;
                }
                continue;
            }
            if (!earliestMemoryCueMs || cue.positionMs < *earliestMemoryCueMs) {
                earliestMemoryCueMs = cue.positionMs;
            }
            continue;
        }
        int slot = cue.hotCueNumber - 1;
        if (slot < 0 || slot >= HotCueSlotCount) {
            continue;
        }
        double startOffset = toSamples(cue.positionMs);
        if (cue.isLoop) {
            double endOffset = toSamples(cue.loopEndMs);
            // Only a loop the shift emptied: one handed in empty is
            // written as given, as before.
            if (endOffset <= startOffset && cue.loopEndMs > cue.positionMs) {
                ++m_loopsDroppedByLeadInShift;
                if (m_drops) {
                    ++m_drops->loops;
                }
                continue;
            }
            loopSlots[slot] = djinterop::loop{domain::withoutTrailingNuls(cue.comment), startOffset, endOffset, padColor(cue.color, slot + 1)};
        } else {
            slots[slot] = djinterop::hot_cue{domain::withoutTrailingNuls(cue.comment), startOffset, padColor(cue.color, slot + 1)};
        }
    }

    // set_loops() writes the whole 8-slot array without reading the old
    // one. The reader falls back to "no loops" when a track's loop blob
    // does not decode, so a cue list built from that read would erase
    // every hot loop on the track here. Refuse instead: a write that
    // cannot see what it is replacing must not replace it.
    try {
        (void)track->loops();
    } catch (const std::exception &e) {
        throw std::runtime_error("the hot loops on track id=" + trackSourceId
                                 + " could not be read (" + e.what()
                                 + "); refusing to write cues, which would erase them");
    }
    track->set_hot_cues(slots);
    track->set_loops(loopSlots);
    // `cues` is the complete replacement set, the same contract every
    // CueWriter has, so no memory cue in it means the track has no main
    // cue any more -- not "leave whatever was there". Skipping the call
    // when the set was empty left the old main cue in place, which read
    // back as a memory cue the caller had just removed. Engine stores
    // "no main cue" as sample offset 0 (see the reader: main_cue() maps 0
    // back to nothing), which is what set_main_cue(nullopt) writes.
    track->set_main_cue(earliestMemoryCueMs
                            ? std::optional<double>(toSamples(*earliestMemoryCueMs))
                            : std::nullopt);
}

void LibdjinteropEngineCueWriter::setLastPlayedAt(const std::string &trackSourceId,
                                                  std::chrono::system_clock::time_point when)
{
    auto &db = database();
    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    track->set_last_played_at(when);
}

void LibdjinteropEngineCueWriter::propagateMissingFields(const std::string &trackSourceId,
                                                           std::optional<double> bpm, std::optional<std::string> key)
{
    if (!bpm && !key) {
        return;
    }
    auto &db = database();
    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    if (bpm) {
        track->set_bpm(*bpm);
    }
    if (key) {
        // Reuses the exact same string->musical_key parser
        // EngineLibraryCreator already relies on, so a key propagated
        // from a rekordbox-sourced donor round-trips through the same
        // pitch-class/mode matching a fresh rekordbox->Engine sync
        // would've used -- not a second, divergent parsing path.
        if (auto parsed = parseRekordboxKey(*key)) {
            track->set_key(*parsed);
        }
    }
}

namespace
{

using BeatDataBlob = djinterop::engine::v3::beat_data_blob;
using GridMarkerBlob = djinterop::engine::v3::beat_grid_marker_blob;

// Engine's markers in samples, numbered and counted the way Denon writes
// its own: each says how many beats on the next one is, the last 0.
std::vector<GridMarkerBlob> markerBlobs(const std::vector<domain::BeatGridMarker> &markers, double sampleRate,
                                        double samples)
{
    std::vector<GridMarkerBlob> blobs;
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const int toNext = i + 1 < markers.size() ? markers[i + 1].index - markers[i].index : 0;
        blobs.push_back({markers[i].timeMs / 1000.0 * sampleRate, markers[i].index, toNext, 0});
    }
    // The last marker is the first beat at or after the end. It was put
    // there in milliseconds; the way back to samples can leave it a
    // fraction of a sample short, which a player could take for a grid
    // that stops before the track does.
    if (!blobs.empty() && blobs.back().sample_offset < samples && samples - blobs.back().sample_offset < 0.5) {
        blobs.back().sample_offset = samples;
    }
    return blobs;
}

// The same grid: the same beats numbered the same, each within half a
// sample. Closer than that is the same moment in the audio.
bool sameGrid(const std::vector<GridMarkerBlob> &a, const std::vector<GridMarkerBlob> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].beat_number != b[i].beat_number || !(std::abs(a[i].sample_offset - b[i].sample_offset) <= 0.5)) {
            return false;
        }
    }
    return true;
}

// The row's adjusted grid as it stands, to the whole sample: the same
// stored grid always gives the same text, and an edit on the player that
// moves the grid by a sample or more gives another. The rate and length
// are in it too, since a grid in samples means nothing without them.
std::string gridFingerprint(const BeatDataBlob &blob)
{
    std::string text = "seabass-beat-grid 1\n";
    char line[128];
    std::snprintf(line, sizeof line, "rate %.3f\nsamples %lld\n", blob.sample_rate,
                  static_cast<long long>(std::llround(blob.samples)));
    text += line;
    for (const GridMarkerBlob &marker : blob.adjusted_beat_grid) {
        std::snprintf(line, sizeof line, "%lld %lld\n", static_cast<long long>(marker.beat_number),
                      static_cast<long long>(std::llround(marker.sample_offset)));
        text += line;
    }
    return hashing::toHex(hashing::Sha256::of(std::string_view(text)));
}

// rekordbox's beats (already on Engine's timeline) as Engine's markers at
// a rate and length, with the bars rekordbox breaks and Engine cannot.
std::pair<std::vector<GridMarkerBlob>, int> convertedGrid(const std::vector<domain::Beat> &beats, double sampleRate,
                                                          double samples)
{
    const domain::MarkersFromBeats fromBeats = domain::markersFromBeats(beats, samples / sampleRate * 1000.0);
    const auto markers = domain::engineNumbered(fromBeats.markers);
    return {markerBlobs(markers, sampleRate, samples), fromBeats.barBreaks};
}

// What a grid write does to a row the player has analysed: the outcome,
// and the blob to write when there is one to write.
struct AnalysedGridPlan
{
    BeatGridWriteResult result;
    std::optional<BeatDataBlob> write;
};

AnalysedGridPlan planAnalysedGrid(const BeatDataBlob &existing, bool locked, const std::vector<GridMarkerBlob> &grid,
                                  int barBreaks, bool overwriteEngineEdits,
                                  const std::optional<std::string> &ownGridFingerprint)
{
    using Outcome = BeatGridWriteResult::Outcome;
    AnalysedGridPlan plan{{Outcome::Written, static_cast<int>(grid.size()), barBreaks}, std::nullopt};
    // Before the checks for the user's own edits: a grid written here
    // before is not the default one either, and writing it again
    // would change nothing but the row's edit time.
    if (existing.is_beatgrid_set == 1 && sameGrid(existing.adjusted_beat_grid, grid)) {
        plan.result.outcome = Outcome::AlreadySame;
        plan.result.gridFingerprint = gridFingerprint(existing);
        return plan;
    }
    if (!overwriteEngineEdits) {
        if (locked) {
            plan.result.outcome = Outcome::RowLocked;
            return plan;
        }
        // An adjusted grid that is not Engine's own was set on the
        // player, unless it is the one Seabass wrote last time: then
        // the DJ corrected the grid in rekordbox and this carries the
        // correction over, which is what the conversion is for.
        const bool ours = ownGridFingerprint && *ownGridFingerprint == gridFingerprint(existing);
        if (existing.adjusted_beat_grid != existing.default_beat_grid && !ours) {
            plan.result.outcome = Outcome::RowEditedOnEngine;
            return plan;
        }
    }
    // Only the adjusted grid: the default one is Engine's analysis,
    // which the player offers to go back to. Rate, length and the
    // bytes after the grids are the row's and stay as they are.
    BeatDataBlob blob = existing;
    blob.is_beatgrid_set = 1;
    blob.adjusted_beat_grid = grid;
    plan.result.gridFingerprint = gridFingerprint(blob);
    plan.write = std::move(blob);
    return plan;
}

}  // namespace

BeatGridWriteResult LibdjinteropEngineCueWriter::writeBeatGrid(const std::string &trackSourceId,
                                                               const std::vector<domain::Beat> &rekordboxBeats,
                                                               const BeatGridWriteOptions &options)
{
    using Outcome = BeatGridWriteResult::Outcome;
    auto &db = database();
    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    if (!m_tables) {
        return {Outcome::UnsupportedSchema};
    }
    const std::int64_t id = track->id();

    // Each step below reads or writes one column through libdjinterop,
    // each in its own statement, the way writeCues() does: the save that
    // owns this writer works on a copy it backs up first.
    const auto readBeatData = [&]() -> std::optional<BeatDataBlob> {
        try {
            return m_tables->v3 ? m_tables->v3->performance_data().get_beat_data(id)
                                : m_tables->v2->track().get_beat_data(id);
        } catch (const std::exception &) {
            // A blob that does not decode is no grid to keep or compare.
            return std::nullopt;
        }
    };
    const auto writeBeatData = [&](const BeatDataBlob &blob) {
        if (m_tables->v3) {
            m_tables->v3->performance_data().set_beat_data(id, blob);
        } else {
            m_tables->v2->track().set_beat_data(id, blob);
        }
    };
    // A flag that cannot be read is taken as `unreadable`.
    const auto flag = [&](bool (djinterop::engine::v2::track_table::*v2)(std::int64_t),
                          bool (djinterop::engine::v3::track_table::*v3)(std::int64_t), bool unreadable) {
        try {
            if (m_tables->v3) {
                auto table = m_tables->v3->track();
                return (table.*v3)(id);
            }
            auto table = m_tables->v2->track();
            return (table.*v2)(id);
        } catch (const std::exception &) {
            return unreadable;
        }
    };

    // The grid comes on rekordbox's timeline, as the cues do, and moves
    // onto Engine's by the same shift. A beat the shift puts before the
    // start is dropped; the line through the others still reaches back
    // past it.
    const double shiftMs = shiftMsOf(*track);
    std::vector<domain::Beat> beats = rekordboxBeats;
    for (domain::Beat &beat : beats) {
        beat.timeMs -= shiftMs;
    }
    if (domain::markersFromBeats(beats, 0.0).markers.size() < 2) {
        return {Outcome::NoSourceGrid};
    }

    const auto converted = [&](double sampleRate, double samples) { return convertedGrid(beats, sampleRate, samples); };

    // Analysed is what the player reads off the flag, with the blob to
    // say what rate and length the audio has. A row with a grid but the
    // flag down is one the player will analyse again, so it counts as
    // not analysed.
    const std::optional<BeatDataBlob> existing = readBeatData();
    // An analysed flag that cannot be read leaves the row alone (Skip) or
    // filled as a row the player is still to analyse. A lock that cannot
    // be read counts as a lock: unsure is a refusal, never a write over
    // what the user may have pinned on the player.
    const bool analysed = flag(&djinterop::engine::v2::track_table::get_is_analyzed,
                               &djinterop::engine::v3::track_table::get_is_analyzed, false)
        && existing && existing->sample_rate > 0.0 && existing->samples > 0.0 && std::isfinite(existing->sample_rate)
        && std::isfinite(existing->samples);
    const bool locked = flag(&djinterop::engine::v2::track_table::get_is_beat_grid_locked,
                             &djinterop::engine::v3::track_table::get_is_beat_grid_locked, true);

    if (analysed) {
        const auto [grid, barBreaks] = converted(existing->sample_rate, existing->samples);
        AnalysedGridPlan plan =
            planAnalysedGrid(*existing, locked, grid, barBreaks, options.overwriteEngineEdits, options.ownGridFingerprint);
        if (!plan.write || options.previewOnly) {
            return plan.result;
        }
        if (m_beforeBeatGridWrite) {
            m_beforeBeatGridWrite();
        }
        writeBeatData(*plan.write);
        return plan.result;
    }

    if (options.unanalysed == UnanalysedRowGrid::Skip) {
        return {Outcome::RowNotAnalysed};
    }
    const double rate = options.fileSampleRateHz.value_or(0.0);
    const double durationMs = options.fileDurationMs.value_or(0.0);
    if (!(rate > 0.0) || !(durationMs > 0.0) || !std::isfinite(rate) || !std::isfinite(durationMs)) {
        return {Outcome::NoSampleRate};
    }
    // A row the player has not analysed: the whole blob is made here, at
    // the file's rate and length, both grids the converted one, and the
    // nine trailing bytes Denon writes as zeros. The analysed flag and
    // the waveform stay as they are: the player is still to analyse it.
    const double samples = std::round(durationMs / 1000.0 * rate);
    const auto [grid, barBreaks] = converted(rate, samples);
    BeatGridWriteResult result{Outcome::Written, static_cast<int>(grid.size()), barBreaks};
    const bool lock = options.unanalysed == UnanalysedRowGrid::FillAndLock;
    if (existing && existing->sample_rate == rate && existing->samples == samples && existing->is_beatgrid_set == 1
        && sameGrid(existing->adjusted_beat_grid, grid) && sameGrid(existing->default_beat_grid, grid)
        && (!lock || locked)) {
        result.outcome = Outcome::AlreadySame;
        result.gridFingerprint = gridFingerprint(*existing);
        return result;
    }
    // A lock is the user's word whether or not the player has analysed
    // the row. After the check above: a row FillAndLock filled before is
    // locked by this very writer, and is the same, not refused.
    if (locked && !options.overwriteEngineEdits) {
        result.outcome = Outcome::RowLocked;
        return result;
    }
    BeatDataBlob blob;
    blob.sample_rate = rate;
    blob.samples = samples;
    blob.is_beatgrid_set = 1;
    blob.default_beat_grid = grid;
    blob.adjusted_beat_grid = grid;
    blob.extra_data = std::vector<std::byte>(9, std::byte{0});
    if (options.previewOnly) {
        result.gridFingerprint = gridFingerprint(blob);
        return result;
    }
    if (m_beforeBeatGridWrite) {
        m_beforeBeatGridWrite();
    }
    writeBeatData(blob);
    result.gridFingerprint = gridFingerprint(blob);
    if (lock && !locked) {
        // After the grid: should this fail, the row has a grid the player
        // may replace, not a lock on no grid at all.
        if (m_tables->v3) {
            m_tables->v3->track().set_is_beat_grid_locked(id, true);
        } else {
            m_tables->v2->track().set_is_beat_grid_locked(id, true);
        }
    }
    return result;
}

namespace
{

using TrackDataBlob = djinterop::engine::v3::track_data_blob;
using OverviewBlob = djinterop::engine::v3::overview_waveform_data_blob;

// One row's analysis columns, wherever its schema keeps them: 3.x in
// PerformanceData, 2.x in Track. Each read or write is one statement
// through libdjinterop, as in writeBeatGrid().
class RowColumns
{
public:
    RowColumns(std::optional<djinterop::engine::v2::engine_library> &v2,
               std::optional<djinterop::engine::v3::engine_library> &v3, std::int64_t id)
        : m_v2(v2)
        , m_v3(v3)
        , m_id(id)
    {
    }

    bool isV3() const { return m_v3.has_value(); }

    // A blob that does not decode is none to keep or compare.
    std::optional<BeatDataBlob> beatData() const
    {
        try {
            return m_v3 ? m_v3->performance_data().get_beat_data(m_id) : m_v2->track().get_beat_data(m_id);
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }
    std::optional<TrackDataBlob> trackData() const
    {
        try {
            return m_v3 ? m_v3->performance_data().get_track_data(m_id) : m_v2->track().get_track_data(m_id);
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }
    std::optional<OverviewBlob> overview() const
    {
        try {
            return m_v3 ? m_v3->performance_data().get_overview_waveform_data(m_id)
                        : m_v2->track().get_overview_waveform_data(m_id);
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    void setBeatData(const BeatDataBlob &blob)
    {
        m_v3 ? m_v3->performance_data().set_beat_data(m_id, blob) : m_v2->track().set_beat_data(m_id, blob);
    }
    void setTrackData(const TrackDataBlob &blob)
    {
        m_v3 ? m_v3->performance_data().set_track_data(m_id, blob) : m_v2->track().set_track_data(m_id, blob);
    }
    void setOverview(const OverviewBlob &blob)
    {
        m_v3 ? m_v3->performance_data().set_overview_waveform_data(m_id, blob)
             : m_v2->track().set_overview_waveform_data(m_id, blob);
    }
    // Track.bpmAnalyzed as given, Track.bpm (an integer column) rounded,
    // as the player leaves both after its analysis.
    void setBpm(double bpm)
    {
        const auto rounded = static_cast<std::int64_t>(std::llround(bpm));
        if (m_v3) {
            m_v3->track().set_bpm_analyzed(m_id, bpm);
            m_v3->track().set_bpm(m_id, rounded);
        } else {
            m_v2->track().set_bpm_analyzed(m_id, bpm);
            m_v2->track().set_bpm(m_id, rounded);
        }
    }
    void setKey(int key) { m_v3 ? m_v3->track().set_key(m_id, key) : m_v2->track().set_key(m_id, key); }
    void setIsAnalyzed(bool analysed)
    {
        m_v3 ? m_v3->track().set_is_analyzed(m_id, analysed) : m_v2->track().set_is_analyzed(m_id, analysed);
    }

    // Information.uuid of this database: the name of its folder under
    // Database2/OverviewData. Read from the one row the table has,
    // whatever its id.
    std::string uuid() const { return m_v3 ? m_v3->information().get().uuid : m_v2->information().get().uuid; }

private:
    std::optional<djinterop::engine::v2::engine_library> &m_v2;
    std::optional<djinterop::engine::v3::engine_library> &m_v3;
    std::int64_t m_id;
};

// The overview as the player stores it: 1024 points, then the last triple,
// with how many samples each point covers.
OverviewBlob overviewBlob(const domain::EngineOverview &overview, double samples)
{
    OverviewBlob blob;
    blob.samples_per_waveform_point = samples / static_cast<double>(domain::EngineOverview::PointCount);
    blob.waveform_points.reserve(overview.points.size());
    for (const domain::BandTriple &point : overview.points) {
        blob.waveform_points.push_back({point.low, point.mid, point.high});
    }
    blob.maximum_point = {overview.finalTriple.low, overview.finalTriple.mid, overview.finalTriple.high};
    return blob;
}

// The overview file holds the same container as the overview column (a
// four-byte big-endian length, then the zlib'd layout; measured on a
// Prime 4's own files), so libdjinterop's encoder writes it and its
// decoder reads one back.
std::string fileBytesOf(const OverviewBlob &blob)
{
    const std::vector<std::byte> bytes = blob.to_blob();
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

// What the overview file at `path` holds, or nothing when there is none
// or it does not decode (a player can leave a truncated one behind).
std::optional<OverviewBlob> readOverviewFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(text.size());
    std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    try {
        return OverviewBlob::from_blob(bytes);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

// The file's place under the Engine Library folder. The uuid comes out of
// the database and goes into a path, so it is held to what a uuid is.
std::string overviewFileRelativePath(const std::string &uuid, std::int64_t trackId)
{
    const bool plain = !uuid.empty() && std::all_of(uuid.begin(), uuid.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
    });
    if (!plain) {
        throw std::runtime_error("the Engine library's Information.uuid \"" + uuid
                                 + "\" cannot name its overview folder");
    }
    return "Database2/OverviewData/" + uuid + "/" + std::to_string(trackId) + ".rgb";
}

bool usable(double sampleRate, double samples)
{
    return sampleRate > 0.0 && samples > 0.0 && std::isfinite(sampleRate) && std::isfinite(samples);
}

}  // namespace

AnalysisImportResult LibdjinteropEngineCueWriter::importAnalysis(const std::string &trackSourceId,
                                                                 const RekordboxAnalysis &analysis,
                                                                 const AnalysisImportOptions &options)
{
    using Outcome = BeatGridWriteResult::Outcome;
    using Waveform = AnalysisImportResult::Waveform;
    AnalysisImportResult result;
    result.grid.outcome = Outcome::NotRequested;
    result.waveform = Waveform::NotRequested;

    auto &db = database();
    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    if (!m_tables) {
        if (options.grids) {
            result.grid.outcome = Outcome::UnsupportedSchema;
        }
        if (options.waveforms) {
            result.waveform = Waveform::UnsupportedSchema;
        }
        return result;
    }
    const std::int64_t id = track->id();
    RowColumns row(m_tables->v2, m_tables->v3, id);

    // The grid on Engine's timeline, as writeBeatGrid() puts it. The shift
    // can mean reading the file's start, so only when a grid is wanted.
    std::vector<domain::Beat> beats = analysis.beats;
    if (options.grids) {
        const double shiftMs = shiftMsOf(*track);
        for (domain::Beat &beat : beats) {
            beat.timeMs -= shiftMs;
        }
    }
    const bool hasGrid = domain::markersFromBeats(beats, 0.0).markers.size() >= 2;

    // The three-band preview fits the player's overview better; the colour
    // one is there for an export without a .2EX.
    const bool threeBand = analysis.threeBandPreview && !analysis.threeBandPreview->empty();
    const bool hasPreview = threeBand || (analysis.colourPreview && !analysis.colourPreview->empty());
    const auto overviewOver = [&](double sourceSeconds, double targetSeconds) {
        return threeBand ? domain::overviewFromThreeBandPreview(*analysis.threeBandPreview, sourceSeconds, targetSeconds)
                         : domain::overviewFromColourPreview(*analysis.colourPreview, sourceSeconds, targetSeconds);
    };
    const auto scrollSeconds = [&]() -> std::optional<double> {
        if (!analysis.scrollEntries || *analysis.scrollEntries == 0) {
            return std::nullopt;
        }
        return static_cast<double>(*analysis.scrollEntries) / 150.0;
    };

    // As in writeBeatGrid(): the flag with a usable grid blob is analysed,
    // and a lock that cannot be read is a lock.
    const auto flag = [&](bool (djinterop::engine::v2::track_table::*v2)(std::int64_t),
                          bool (djinterop::engine::v3::track_table::*v3)(std::int64_t), bool unreadable) {
        try {
            if (m_tables->v3) {
                auto table = m_tables->v3->track();
                return (table.*v3)(id);
            }
            auto table = m_tables->v2->track();
            return (table.*v2)(id);
        } catch (const std::exception &) {
            return unreadable;
        }
    };
    const std::optional<BeatDataBlob> existing = row.beatData();
    const bool analysed = flag(&djinterop::engine::v2::track_table::get_is_analyzed,
                               &djinterop::engine::v3::track_table::get_is_analyzed, false)
        && existing && usable(existing->sample_rate, existing->samples);
    const bool locked = flag(&djinterop::engine::v2::track_table::get_is_beat_grid_locked,
                             &djinterop::engine::v3::track_table::get_is_beat_grid_locked, true);

    const std::filesystem::path fileRoot = pathFromUtf8(m_overviewFileRoot.empty() ? m_engineLibraryPath : m_overviewFileRoot);
    const auto writeFile = [&](const std::string &relative, const std::string &bytes) {
        if (m_overviewFileSink) {
            m_overviewFileSink(relative, bytes);
            return;
        }
        const std::filesystem::path path = fileRoot / pathFromUtf8(relative);
        std::filesystem::create_directories(path.parent_path());
        if (!writeFileDurablyAtomic(pathToUtf8(path), bytes)) {
            throw std::runtime_error("could not write the overview file " + pathToUtf8(path));
        }
    };
    const auto beforeWrite = [&] {
        if (m_beforeBeatGridWrite) {
            m_beforeBeatGridWrite();
        }
    };

    if (analysed) {
        std::optional<BeatDataBlob> gridWrite;
        if (options.grids) {
            if (!hasGrid) {
                result.grid.outcome = Outcome::NoSourceGrid;
            } else {
                const auto [grid, barBreaks] = convertedGrid(beats, existing->sample_rate, existing->samples);
                AnalysedGridPlan plan = planAnalysedGrid(*existing, locked, grid, barBreaks, options.overwriteEngineEdits,
                                                         options.ownGridFingerprint);
                result.grid = plan.result;
                gridWrite = std::move(plan.write);
            }
        }

        std::optional<OverviewBlob> overviewWrite;
        std::optional<std::string> fileWrite;
        if (options.waveforms) {
            // The row's own length: its analysis record's, or the beat
            // data's where the record says nothing usable.
            double rate = existing->sample_rate;
            double samples = existing->samples;
            if (const auto record = row.trackData();
                record && usable(record->sample_rate, static_cast<double>(record->samples))) {
                rate = record->sample_rate;
                samples = static_cast<double>(record->samples);
            }
            const double targetSeconds = samples / rate;
            const domain::EngineOverview overview =
                hasPreview ? overviewOver(scrollSeconds().value_or(targetSeconds), targetSeconds) : domain::EngineOverview{};
            if (overview.empty()) {
                // No preview, or one that is not whole entries.
                result.waveform = Waveform::NoSourceWaveform;
            } else {
                const OverviewBlob blob = overviewBlob(overview, samples);
                if (const auto current = row.overview(); !current || *current != blob) {
                    overviewWrite = blob;
                }
                if (!options.skipOverviewFile) {
                    const std::string relative = overviewFileRelativePath(row.uuid(), id);
                    const OverviewBlob second = overviewBlob(domain::secondWaveformFromOverview(overview), samples);
                    if (const auto current = readOverviewFile(fileRoot / pathFromUtf8(relative));
                        !current || *current != second) {
                        fileWrite = fileBytesOf(second);
                        result.overviewFileRelativePath = relative;
                    }
                }
                result.waveform = overviewWrite || fileWrite ? Waveform::Written : Waveform::AlreadySame;
            }
        }

        if (options.previewOnly || (!gridWrite && !overviewWrite && !fileWrite)) {
            return result;
        }
        beforeWrite();
        if (gridWrite) {
            row.setBeatData(*gridWrite);
        }
        if (overviewWrite) {
            row.setOverview(*overviewWrite);
        }
        if (fileWrite) {
            writeFile(result.overviewFileRelativePath, *fileWrite);
        }
        return result;
    }

    // A row the player has not analysed. Each part says what it lacks;
    // a part that lacks nothing is RowNotAnalysed when the other part
    // keeps the row from being completed.
    const double rate = options.fileSampleRateHz.value_or(0.0);
    const std::uint64_t sampleCount =
        analysis.scrollEntries && rate > 0.0 && std::isfinite(rate)
            ? domain::engineSampleCount(*analysis.scrollEntries, rate, options.fileIsMp3)
            : 0;
    if (options.grids) {
        result.grid.outcome = hasGrid ? Outcome::RowNotAnalysed : Outcome::NoSourceGrid;
    }
    if (options.waveforms) {
        result.waveform = !hasPreview ? Waveform::NoSourceWaveform
                          : sampleCount == 0 ? Waveform::NoSampleCount
                                             : Waveform::RowNotAnalysed;
    }
    if (!options.grids || !options.waveforms || !hasGrid || !hasPreview || sampleCount == 0) {
        return result;
    }
    // A lock is the user's word whether or not the player has analysed
    // the row.
    if (locked && !options.overwriteEngineEdits) {
        result.grid.outcome = Outcome::RowLocked;
        return result;
    }
    const auto samples = static_cast<double>(sampleCount);
    const domain::EngineOverview overview = overviewOver(*scrollSeconds(), samples / rate);
    if (overview.empty()) {
        result.waveform = Waveform::NoSourceWaveform;
        return result;
    }
    const auto [grid, barBreaks] = convertedGrid(beats, rate, samples);

    // The analysis record as the player writes it: rate, length, key (0
    // for none, as libdjinterop writes no key) and the three loudness
    // values; on 3.x 24 zero bytes follow, as on every row a Prime 4
    // analysed (68 bytes in all). 2.x records end after the loudness.
    TrackDataBlob record{};
    record.sample_rate = rate;
    record.samples = static_cast<std::int64_t>(sampleCount);
    record.key = analysis.key.value_or(0);
    const std::array<double, 3> loudness =
        options.zeroLoudness ? std::array<double, 3>{0.0, 0.0, 0.0} : domain::loudnessFromOverview(overview);
    record.average_loudness_low = loudness[0];
    record.average_loudness_mid = loudness[1];
    record.average_loudness_high = loudness[2];
    if (row.isV3()) {
        record.extra_data = std::vector<std::byte>(24, std::byte{0});
    }

    // Both grids rekordbox's: there is no analysis of Engine's own to keep.
    BeatDataBlob beatData;
    beatData.sample_rate = rate;
    beatData.samples = samples;
    beatData.is_beatgrid_set = 1;
    beatData.default_beat_grid = grid;
    beatData.adjusted_beat_grid = grid;
    beatData.extra_data = std::vector<std::byte>(9, std::byte{0});

    const OverviewBlob overviewData = overviewBlob(overview, samples);
    std::string file;
    if (!options.skipOverviewFile) {
        result.overviewFileRelativePath = overviewFileRelativePath(row.uuid(), id);
        file = fileBytesOf(overviewBlob(domain::secondWaveformFromOverview(overview), samples));
    }

    result.grid = {Outcome::Written, static_cast<int>(grid.size()), barBreaks, gridFingerprint(beatData)};
    result.waveform = Waveform::Written;
    result.completedRow = true;
    if (options.previewOnly) {
        return result;
    }
    beforeWrite();
    // The flag last: until it is up the player takes the row for one it is
    // still to analyse and analyses it, so a failure anywhere before leaves
    // a row that plays, never one flagged analysed with a piece missing.
    row.setTrackData(record);
    row.setOverview(overviewData);
    row.setBeatData(beatData);
    if (!options.skipOverviewFile) {
        writeFile(result.overviewFileRelativePath, file);
    }
    if (analysis.bpm && *analysis.bpm > 0.0 && std::isfinite(*analysis.bpm)) {
        row.setBpm(*analysis.bpm);
    }
    if (analysis.key) {
        row.setKey(*analysis.key);
    }
    row.setIsAnalyzed(true);
    return result;
}

}  // namespace seabass::infrastructure::engine
