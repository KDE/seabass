// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <stdexcept>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/v2/engine_library.hpp>
#include <djinterop/engine/v3/engine_library.hpp>

#include "infrastructure/audio/mp3_lead_in_reader.hpp"
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

    const auto converted = [&](double sampleRate, double samples) {
        const domain::MarkersFromBeats fromBeats = domain::markersFromBeats(beats, samples / sampleRate * 1000.0);
        const auto markers = domain::engineNumbered(fromBeats.markers);
        return std::make_pair(markerBlobs(markers, sampleRate, samples), fromBeats.barBreaks);
    };

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
        BeatGridWriteResult result{Outcome::Written, static_cast<int>(grid.size()), barBreaks};
        // Before the checks for the user's own edits: a grid written here
        // before is not the default one either, and writing it again
        // would change nothing but the row's edit time.
        if (existing->is_beatgrid_set == 1 && sameGrid(existing->adjusted_beat_grid, grid)) {
            result.outcome = Outcome::AlreadySame;
            result.gridFingerprint = gridFingerprint(*existing);
            return result;
        }
        if (!options.overwriteEngineEdits) {
            if (locked) {
                result.outcome = Outcome::RowLocked;
                return result;
            }
            // An adjusted grid that is not Engine's own was set on the
            // player, unless it is the one Seabass wrote last time: then
            // the DJ corrected the grid in rekordbox and this carries the
            // correction over, which is what the conversion is for.
            const bool ours = options.ownGridFingerprint && *options.ownGridFingerprint == gridFingerprint(*existing);
            if (existing->adjusted_beat_grid != existing->default_beat_grid && !ours) {
                result.outcome = Outcome::RowEditedOnEngine;
                return result;
            }
        }
        // Only the adjusted grid: the default one is Engine's analysis,
        // which the player offers to go back to. Rate, length and the
        // bytes after the grids are the row's and stay as they are.
        BeatDataBlob blob = *existing;
        blob.is_beatgrid_set = 1;
        blob.adjusted_beat_grid = grid;
        if (options.previewOnly) {
            result.gridFingerprint = gridFingerprint(blob);
            return result;
        }
        if (m_beforeBeatGridWrite) {
            m_beforeBeatGridWrite();
        }
        writeBeatData(blob);
        result.gridFingerprint = gridFingerprint(blob);
        return result;
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

}  // namespace seabass::infrastructure::engine
