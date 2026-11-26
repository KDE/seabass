// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <djinterop/djinterop.hpp>
#include <zlib.h>
#include <djinterop/engine/v2/engine_library.hpp>
#include <djinterop/engine/v3/engine_library.hpp>
#include <djinterop/engine/v3/loops_blob.hpp>
#include <djinterop/engine/v3/quick_cues_blob.hpp>

#include "domain/engine_overview.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/engine/engine_blob_check.hpp"
#include "infrastructure/engine/engine_cue_timeline.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/work_counters.hpp"

namespace seabass::infrastructure::engine
{


using internal::engineBlobDecodable;

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

// The rules for what the lead-in shift (#50) puts at or before Engine's 0,
// shared by writeCues() and the move of a marked row's stored cues
// (movedOntoEngineTimeline()). Each takes a position already moved by the
// shift, in milliseconds or in samples alike.
//
// A hot cue, and a loop's start, is clamped to 0.
double clampedToStart(double moved)
{
    return std::max(0.0, moved);
}

// A loop whose clamped ends meet or cross was emptied by the shift: it is
// left out. Only one the shift emptied: a loop that came empty stays.
bool loopEmptiedByShift(double movedStart, double movedEnd, double start, double end)
{
    return clampedToStart(movedEnd) <= clampedToStart(movedStart) && end > start;
}

// A main cue moved to 0 or before is left out: Engine reads one at 0 as no
// main cue at all.
bool mainCueSurvivesShift(double moved)
{
    return moved > 0.0;
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

void LibdjinteropEngineCueWriter::writeRating(const std::string &trackSourceId, std::optional<int> stars)
{
    if (stars && (*stars < 1 || *stars > 5)) {
        throw std::invalid_argument("engine: a rating is 1 to 5 stars or none, got " + std::to_string(*stars));
    }
    auto track = database().track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    track->set_rating(stars ? std::optional<int>(*stars * 20) : std::nullopt);
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

void LibdjinteropEngineCueWriter::writeEngineStoredCues(const std::string &trackSourceId,
                                                         const std::vector<domain::CuePoint> &cues)
{
    writeCues(trackSourceId, cues, std::nullopt, CuePositions::EngineStored);
}

std::optional<double> LibdjinteropEngineCueWriter::knownShiftMsOf(djinterop::track &track)
{
    std::string relative;
    try {
        relative = track.relative_path();
    } catch (const std::exception &) {
        // No path names no file: what it is cannot be told.
    }
    if (relative.empty()) {
        return std::nullopt;
    }
    // A file that is not an MP3 has no lead-in to correct for: none,
    // whatever the setting.
    if (!domain::hasMp3Extension(relative)) {
        return 0.0;
    }
    if (!m_leadInSource) {
        return std::nullopt;
    }
    const std::string &audioRoot = m_audioRoot.empty() ? m_engineLibraryPath : m_audioRoot;
    const std::string filePath = pathToUtf8((pathFromUtf8(audioRoot) / pathFromUtf8(relative)).lexically_normal());
    // An MP3 with the correction off is moved by nothing, but its true
    // shift is not known.
    if (!shiftsCueTimeline(filePath, {})) {
        return std::nullopt;
    }
    const std::optional<domain::Mp3LeadIn> leadIn = m_leadInSource(filePath);
    // A lead-in with no rate says nothing (engineToRekordboxOffsetMs()
    // gives 0 for it); one with a rate may say 0, and that is known.
    if (!leadIn || leadIn->sampleRate <= 0) {
        return std::nullopt;
    }
    return engineCueShiftMs(leadIn);
}

double LibdjinteropEngineCueWriter::shiftMsOf(djinterop::track &track)
{
    // What is taken off positions: 0 when the shift is not known, as
    // before the difference was told apart.
    return knownShiftMsOf(track).value_or(0.0);
}

double LibdjinteropEngineCueWriter::cueTimelineShiftMs(const std::string &trackSourceId)
{
    auto track = database().track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    return shiftMsOf(*track);
}

std::optional<std::vector<domain::CuePoint>> LibdjinteropEngineCueWriter::storedCues(const std::string &trackSourceId)
{
    auto track = database().track_by_id(std::stoll(trackSourceId));
    if (!track) {
        return std::nullopt;
    }
    // The rule writeCues() takes a rate by when it is given none.
    double sampleRate = 44100.0;
    try {
        if (auto rate = track->sample_rate(); rate && *rate > 0.0) {
            sampleRate = *rate;
        }
    } catch (const std::exception &) {
        // the default above
    }
    return engineRowCues(*track, sampleRate);
}

void LibdjinteropEngineCueWriter::writeCues(const std::string &trackSourceId,
                                             const std::vector<domain::CuePoint> &cues,
                                             std::optional<double> knownSampleRate, CuePositions positions)
{
    auto &db = database();

    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    // set_hot_cues() and set_main_cue() read the quick cues they replace,
    // and the loops are read below before they are replaced.
    const EngineBlobs damaged =
        refuseDamagedBlobs(*track, engine_blobs_for::quickCues | engine_blobs_for::loops, "cues not written");

    // The caller's rate when it read one from the file. Otherwise the
    // row's: some tracks' sample_rate() throws even though set_hot_cues()
    // below works fine (same libdjinterop decoder quirk documented in the
    // Engine reader: it lives in a different blob than the one hot cues
    // use). Fall back to 44.1kHz; see libdjinterop_engine_reader.cpp
    // for the same reasoning. A damaged trackData is not read at all, and
    // falls back the same way.
    double sampleRate = 44100.0;
    if (knownSampleRate) {
        sampleRate = *knownSampleRate;
    } else if (!contains(damaged, EngineBlob::TrackData)) {
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
    // Asked once: the shift, and whether it is known at all (for the mark,
    // below). Positions Engine stored are taken as they are, and the file
    // is not asked.
    const bool asStored = positions == CuePositions::EngineStored;
    const std::optional<double> knownShiftMs = asStored ? std::nullopt : knownShiftMsOf(*track);
    const double shiftMs = knownShiftMs.value_or(0.0);
    const auto toSamples = [&](double rekordboxMs) { return clampedToStart((rekordboxMs - shiftMs) / 1000.0 * sampleRate); };

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
            if (!mainCueSurvivesShift(cue.positionMs - shiftMs)) {
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
            if (loopEmptiedByShift(startOffset, endOffset, cue.positionMs, cue.loopEndMs)) {
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
    // Every cue on the row is on Engine's timeline now, so the mark of
    // Engine's rekordbox import goes: left on, the player would move what
    // was just written about 52 ms earlier on the row's first load. After
    // the cues, as libdjinterop gives no transaction over the three
    // columns above; a failure here says the cues are in and the mark is
    // not, rather than pass for a write that did all it should.
    //
    // Only when the shift taken off is known (a file that is not an MP3,
    // or an MP3 whose lead-in was read with the correction on). Unknown,
    // nothing was taken off and the cues sit where they were given, on
    // rekordbox's timeline: the mark stays, and the player moves them.
    // Positions Engine stored are no answer to whose timeline they are on:
    // the mark stays as it was.
    try {
        if (!asStored && knownShiftMs) {
            takeOffRekordboxImportMark(track->id());
        }
    } catch (const std::exception &e) {
        throw std::runtime_error("the cues of track id=" + trackSourceId
                                 + " were written, but the mark of Engine's rekordbox import could not be taken off ("
                                 + e.what() + "): the player will move them on its first load");
    }
}

EngineBlobs LibdjinteropEngineCueWriter::refuseDamagedBlobs(djinterop::track &track, EngineBlobs needed,
                                                            const char *what)
{
    if (!m_blobGuard) {
        m_blobGuard = std::make_shared<EngineBlobGuard>(m_engineLibraryPath);
    }
    const std::int64_t id = track.id();
    // The row as a DJ knows it: its title (a column, not a blob).
    const auto named = [&] {
        std::string title;
        try {
            title = track.title().value_or("");
        } catch (const std::exception &) {
        }
        return "track id=" + std::to_string(id) + (title.empty() ? std::string() : " (\"" + title + "\")");
    };
    std::string error;
    const std::optional<EngineBlobs> damaged = m_blobGuard->damaged(id, &error);
    if (!damaged) {
        if (engineBlobGuardFailureReadsUnchecked(error)) {
            return 0;
        }
        throw std::runtime_error(named() + ": " + what + ", its blobs could not be checked (" + error + ")");
    }
    if ((*damaged & needed) != 0) {
        throw DamagedEngineRowError(named() + " has a damaged " + engineBlobNames(*damaged & needed) + " blob: "
                                    + what);
    }
    return *damaged;
}

void LibdjinteropEngineCueWriter::takeOffRekordboxImportMark(std::int64_t id)
{
    (void)database();
    if (!m_tables) {
        return;
    }
    // Only the value 1 is the mark; any other is left as it is. Read
    // first: a row without the mark is not written, so its edit time (a
    // trigger on PerformanceData updates it) moves only with its cues.
    if (m_tables->v3) {
        auto performance = m_tables->v3->performance_data();
        if (performance.get_third_party_source_id(id) == 1) {
            performance.set_third_party_source_id(id, std::nullopt);
        }
    } else if (m_tables->v2) {
        auto rows = m_tables->v2->track();
        if (rows.get_third_party_source_id(id) == 1) {
            rows.set_third_party_source_id(id, std::nullopt);
        }
    }
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
    // Reuses the exact same string->musical_key parser
    // EngineLibraryCreator already relies on, so a key propagated
    // from a rekordbox-sourced donor round-trips through the same
    // pitch-class/mode matching a fresh rekordbox->Engine sync
    // would've used -- not a second, divergent parsing path.
    const std::optional<djinterop::musical_key> parsed = key ? parseRekordboxKey(*key) : std::nullopt;
    // set_key() rewrites the key in trackData too, read first. Asked before
    // anything is written, so a refused row is left whole, its bpm too.
    if (parsed) {
        (void)refuseDamagedBlobs(*track, engine_blobs_for::trackData, "bpm and key not written");
    }
    if (bpm) {
        track->set_bpm(*bpm);
    }
    if (parsed) {
        track->set_key(*parsed);
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
// The tempo a converted grid plays at, from its first marker to its last;
// nothing when it does not say one.
std::optional<double> gridTempo(const std::vector<GridMarkerBlob> &grid, double sampleRate)
{
    if (grid.size() < 2 || !(sampleRate > 0.0)) {
        return std::nullopt;
    }
    const double seconds = (grid.back().sample_offset - grid.front().sample_offset) / sampleRate;
    const double beats = static_cast<double>(grid.back().beat_number - grid.front().beat_number);
    const double bpm = seconds > 0.0 ? beats / seconds * 60.0 : 0.0;
    if (!(bpm > 0.0) || !std::isfinite(bpm)) {
        return std::nullopt;
    }
    return bpm;
}

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

// The grid the player plays: the adjusted one once set, else its own
// analysis. Fewer than two markers is no grid: an adjusted grid set but
// empty is empty, whatever the default one holds.
const std::vector<GridMarkerBlob> &playedGrid(const BeatDataBlob &existing)
{
    return existing.is_beatgrid_set == 1 ? existing.adjusted_beat_grid : existing.default_beat_grid;
}

bool emptyGrid(const BeatDataBlob &existing)
{
    return playedGrid(existing).size() < 2;
}

// Under differingIsConflict (#136): the row the player analysed holds a
// grid of Engine's own that is not `grid`. An empty one (emptyGrid) never
// differs. Equal within half a sample (sameGrid) does not differ. The
// ledger's grid is Seabass's earlier copy of rekordbox's, not Engine's
// own, unless the DJ locked it since: a lock is the DJ's word on the grid
// as it is.
bool gridDiffersFromEngines(const BeatDataBlob &existing, bool locked, const std::vector<GridMarkerBlob> &grid,
                            const std::optional<std::string> &ownGridFingerprint)
{
    if (emptyGrid(existing)) {
        return false;
    }
    const auto &played = playedGrid(existing);
    if (sameGrid(played, grid)) {
        return false;
    }
    const bool ours = ownGridFingerprint && *ownGridFingerprint == gridFingerprint(existing);
    return !ours || locked;
}


using TrackDataBlob = djinterop::engine::v3::track_data_blob;
using OverviewBlob = djinterop::engine::v3::overview_waveform_data_blob;
using application::EngineAnalysisColumns;
using application::EngineSqlValue;

std::vector<std::byte> bytesOf(const std::string &text)
{
    std::vector<std::byte> bytes(text.size());
    std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

std::string textOf(const std::vector<std::byte> &bytes)
{
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

// The column each blob type is stored in, for its check.
template<typename Blob>
constexpr EngineBlob columnOf();
template<>
constexpr EngineBlob columnOf<TrackDataBlob>()
{
    return EngineBlob::TrackData;
}
template<>
constexpr EngineBlob columnOf<OverviewBlob>()
{
    return EngineBlob::OverviewWaveFormData;
}
template<>
constexpr EngineBlob columnOf<djinterop::engine::v3::beat_data_blob>()
{
    return EngineBlob::BeatData;
}
template<>
constexpr EngineBlob columnOf<djinterop::engine::v3::quick_cues_blob>()
{
    return EngineBlob::QuickCues;
}

// Stored bytes through libdjinterop's decoder, nothing when they would not
// decode, or are not safe to try (engineBlobDecodable).
template<typename Blob>
std::optional<Blob> fromEngineBlob(const std::string &bytes)
{
    if (!engineBlobDecodable(columnOf<Blob>(), bytes)) {
        return std::nullopt;
    }
    try {
        return Blob::from_blob(bytesOf(bytes));
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

// A column's blob through libdjinterop's decoder, nothing when it is not a
// blob or does not decode: no analysis to keep or compare.
template<typename Blob>
std::optional<Blob> decoded(const EngineSqlValue &value)
{
    if (value.type != EngineSqlValue::Type::Blob) {
        return std::nullopt;
    }
    return fromEngineBlob<Blob>(value.bytes);
}

// A flag as the player reads it; nothing when the column holds no number
// (NULL, text, a blob), which is not knowing.
std::optional<bool> flagOf(const EngineSqlValue &v)
{
    switch (v.type) {
    case EngineSqlValue::Type::Integer:
        return v.integer != 0;
    case EngineSqlValue::Type::Real:
        return v.real != 0.0;
    default:
        return std::nullopt;
    }
}

bool usable(double sampleRate, double samples)
{
    return sampleRate > 0.0 && samples > 0.0 && std::isfinite(sampleRate) && std::isfinite(samples);
}

// What the row's flags say, read the way the player reads them.
struct RowFlags
{
    // The row has the player's analysis: the flag up and a beat data blob
    // with a usable rate and length. Meaningless when unreadable.
    bool analysed = false;
    // Unsure whether the player has analysed the row: its flag is not a
    // number, or it is up over beat data that does not decode or has no
    // usable rate and length. Such a row is left alone, never filled as
    // one the player is still to analyse.
    bool unreadable = false;
    // A lock that cannot be read counts as a lock.
    bool locked = true;
    // The flag is up, but the row holds no analysis at all: no beat data,
    // no overview, no analysis record and no overview file. Engine DJ 5.1
    // left rows so on sticks it analysed (2026-11-23, 30 of 174). There is
    // nothing of the player's to keep or misread, so the row counts as not
    // analysed and is completed like one, with both parts or not at all.
    bool analysedNothingStored = false;
};

// A column with nothing in it: NULL, or a blob of no bytes.
bool holdsNothing(const EngineSqlValue &v)
{
    return v.isNull() || (v.type == EngineSqlValue::Type::Blob && v.bytes.empty());
}

// overviewFileThere: whether the row's overview file is on the stick,
// asked only of a row whose columns hold nothing; true when unsure.
RowFlags flagsOf(const EngineAnalysisColumns &c, const std::optional<BeatDataBlob> &existing,
                 const std::function<bool()> &overviewFileThere)
{
    RowFlags flags;
    flags.locked = flagOf(c.isBeatGridLocked).value_or(true);
    const std::optional<bool> analysed = flagOf(c.isAnalyzed);
    if (!analysed) {
        flags.unreadable = true;
        return flags;
    }
    if (*analysed && holdsNothing(c.beatData) && holdsNothing(c.overviewWaveFormData) && holdsNothing(c.trackData)
        && !overviewFileThere()) {
        flags.analysedNothingStored = true;
        return flags;
    }
    const bool usableGrid = existing && usable(existing->sample_rate, existing->samples);
    flags.unreadable = *analysed && !usableGrid;
    flags.analysed = *analysed && usableGrid;
    return flags;
}

using QuickCuesBlob = djinterop::engine::v3::quick_cues_blob;
using LoopsBlob = djinterop::engine::v3::loops_blob;

// A cue column that holds something: not NULL, not an empty blob.
bool holdsCues(const EngineSqlValue &value)
{
    return !value.isNull() && !(value.type == EngineSqlValue::Type::Blob && value.bytes.empty());
}

// The loops column decoded, nothing when it is not a blob or does not
// decode. Not compressed, so decoded as it is, once its count is checked.
std::optional<LoopsBlob> decodedLoops(const EngineSqlValue &value)
{
    if (value.type != EngineSqlValue::Type::Blob || !engineBlobDecodable(EngineBlob::Loops, value.bytes)) {
        return std::nullopt;
    }
    try {
        return LoopsBlob::from_blob(bytesOf(value.bytes));
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

// Whether the row holds a cue of any kind: a hot cue, a loop, or a main
// cue. "No main cue" is sample -1 as the player writes it and 0 as
// writeCues() does, so only one past the start counts. Cues that cannot be
// read (a damaged blob) count as cues: nothing is known about them.
bool carriesCues(const EngineCueBlobs &blobs)
{
    if (holdsCues(blobs.quickCues)) {
        // Through the guard every stored blob goes through: libdjinterop's
        // inflate does not come back from every damaged one.
        const auto quickCues = decoded<QuickCuesBlob>(blobs.quickCues);
        if (!quickCues || quickCues->adjusted_main_cue > 0.0
            || std::any_of(quickCues->quick_cues.begin(), quickCues->quick_cues.end(), [](const auto &cue) {
                   return cue.sample_offset != djinterop::engine::v3::QUICK_CUE_SAMPLE_OFFSET_EMPTY;
               })) {
            return true;
        }
    }
    if (holdsCues(blobs.loops)) {
        const auto loops = decodedLoops(blobs.loops);
        return !loops || std::any_of(loops->loops.begin(), loops->loops.end(), [](const auto &loop) {
            return loop.is_start_set || loop.is_end_set;
        });
    }
    return false;
}

// A marked row's stored cues, on rekordbox's timeline, moved onto Engine's:
// `shiftSamples` earlier, under writeCues()' rules (clampedToStart() and the
// two beside it). Labels, colours and empty slots stay as they are; only a
// blob whose cues move is encoded again. Nothing when a blob that holds
// cues does not decode.
struct MovedCues
{
    EngineCueBlobs blobs;
    int memoryCuesDropped = 0;
    int loopsDropped = 0;
};

std::optional<MovedCues> movedOntoEngineTimeline(const EngineCueBlobs &stored, double shiftSamples)
{
    MovedCues moved{stored, 0, 0};
    if (holdsCues(stored.quickCues)) {
        std::optional<QuickCuesBlob> quickCues = decoded<QuickCuesBlob>(stored.quickCues);
        if (!quickCues) {
            return std::nullopt;
        }
        const QuickCuesBlob before = *quickCues;
        for (auto &cue : quickCues->quick_cues) {
            // A set hot cue; -1 is an empty slot.
            if (cue.sample_offset >= 0.0) {
                cue.sample_offset = clampedToStart(cue.sample_offset - shiftSamples);
            }
        }
        // Both main cues as writeCues() sets them: one past the start moves
        // or, moved to the start or before, is none (0).
        for (double *mainCue : {&quickCues->adjusted_main_cue, &quickCues->default_main_cue}) {
            if (*mainCue > 0.0) {
                const double to = *mainCue - shiftSamples;
                if (!mainCueSurvivesShift(to) && mainCue == &quickCues->adjusted_main_cue) {
                    ++moved.memoryCuesDropped;
                }
                *mainCue = mainCueSurvivesShift(to) ? to : 0.0;
            }
        }
        if (!(*quickCues == before)) {
            moved.blobs.quickCues = EngineSqlValue::ofBlob(textOf(quickCues->to_blob()));
        }
    }
    if (holdsCues(stored.loops)) {
        std::optional<LoopsBlob> loops = decodedLoops(stored.loops);
        if (!loops) {
            return std::nullopt;
        }
        const LoopsBlob before = *loops;
        for (auto &loop : loops->loops) {
            const double start = loop.start_sample_offset;
            const double end = loop.end_sample_offset;
            if (loop.is_start_set && loop.is_end_set && start >= 0.0 && end >= 0.0
                && loopEmptiedByShift(start - shiftSamples, end - shiftSamples, start, end)) {
                loop = djinterop::engine::v3::loop_blob::empty();
                ++moved.loopsDropped;
                continue;
            }
            if (loop.is_start_set && start >= 0.0) {
                loop.start_sample_offset = clampedToStart(start - shiftSamples);
            }
            if (loop.is_end_set && end >= 0.0) {
                loop.end_sample_offset = clampedToStart(end - shiftSamples);
            }
        }
        if (!(*loops == before)) {
            moved.blobs.loops = EngineSqlValue::ofBlob(textOf(loops->to_blob()));
        }
    }
    return moved;
}

}  // namespace

// The row's analysis as stored, and, read only where it is wanted, its
// overview file.
struct LibdjinteropEngineCueWriter::RowNow
{
    EngineAnalysisColumns columns;
    std::string trackFile;
    std::string libraryUuid;
};

std::string LibdjinteropEngineCueWriter::databaseFile() const
{
    return pathToUtf8(pathFromUtf8(m_engineLibraryPath) / "Database2" / "m.db");
}

std::string LibdjinteropEngineCueWriter::overviewFileRoot() const
{
    return m_overviewFileRoot.empty() ? m_engineLibraryPath : m_overviewFileRoot;
}

EngineAnalysisRowAccess &LibdjinteropEngineCueWriter::rowAccess()
{
    if (!m_rows) {
        m_rows = std::make_shared<EngineAnalysisRowAccess>(databaseFile());
    }
    return *m_rows;
}

namespace
{

std::optional<std::string> readFileBytes(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + pathToUtf8(path));
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw std::runtime_error("cannot read " + pathToUtf8(path));
    }
    return text;
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

}  // namespace

LibdjinteropEngineCueWriter::RowNow LibdjinteropEngineCueWriter::readRowNow(std::int64_t id, bool withOverviewFile)
{
    std::string error;
    auto row = rowAccess().read(id, &error);
    if (!row) {
        throw std::runtime_error(error);
    }
    RowNow now{std::move(row->columns), std::move(row->trackFile), std::move(row->libraryUuid)};
    if (withOverviewFile) {
        now.columns.overviewFileRelativePath = overviewFileRelativePath(now.libraryUuid, id);
        now.columns.overviewFile =
            readFileBytes(pathFromUtf8(overviewFileRoot()) / pathFromUtf8(now.columns.overviewFileRelativePath));
    }
    return now;
}

bool LibdjinteropEngineCueWriter::overviewFileThere(const RowNow &now, std::int64_t id)
{
    try {
        std::error_code ec;
        const bool there = std::filesystem::exists(
            pathFromUtf8(overviewFileRoot()) / pathFromUtf8(overviewFileRelativePath(now.libraryUuid, id)), ec);
        return there || ec;
    } catch (const std::exception &) {
        // No folder to name: unsure, so taken to be there.
        return true;
    }
}

struct LibdjinteropEngineCueWriter::GridMarkPlan
{
    bool takeOffMark = false;
    std::optional<EngineCueRewrite> cues;
    int memoryCuesDropped = 0;
    int loopsDropped = 0;
};

LibdjinteropEngineCueWriter::GridMarkPlan
LibdjinteropEngineCueWriter::planMarkWithGrid(std::int64_t id, const EngineAnalysisColumns &after,
                                              std::optional<double> knownShift)
{
    GridMarkPlan plan;
    std::string error;
    const auto blobs = rowAccess().readCueBlobs(id, &error);
    if (!blobs) {
        // Nothing known about the row's cues: they and the mark stay.
        return plan;
    }
    if (!carriesCues(*blobs)) {
        plan.takeOffMark = true;
        return plan;
    }
    if (!blobs->rekordboxImportMark) {
        // No mark (or one whose meaning is not known): the cues are left
        // exactly as they are.
        return plan;
    }
    // The rate the row's cues are counted at, as the reader takes it: the
    // analysis record's, else the beat data's (the file's, for a row the
    // grid was made for at the file's rate). Neither: not moved.
    double rate = 0.0;
    if (const auto record = decoded<TrackDataBlob>(after.trackData);
        record && record->sample_rate > 0.0 && std::isfinite(record->sample_rate)) {
        rate = record->sample_rate;
    } else if (const auto beatData = decoded<BeatDataBlob>(after.beatData);
               beatData && beatData->sample_rate > 0.0 && std::isfinite(beatData->sample_rate)) {
        rate = beatData->sample_rate;
    }
    if (!(rate > 0.0)) {
        return plan;
    }
    // What writeCues() takes off this row's positions, as the caller
    // asked it with the grid. Not known (an MP3 whose lead-in cannot be
    // read, or the correction off): the cues and the mark stay, and the
    // player moves them.
    if (!knownShift || !std::isfinite(*knownShift)) {
        return plan;
    }
    const double shiftMs = *knownShift;
    const auto moved = movedOntoEngineTimeline(*blobs, shiftMs / 1000.0 * rate);
    if (!moved) {
        return plan;
    }
    plan.takeOffMark = true;
    // A shift of 0 moves nothing, and nothing is written but the mark.
    if (!(moved->blobs.quickCues == blobs->quickCues) || !(moved->blobs.loops == blobs->loops)) {
        plan.cues = EngineCueRewrite{*blobs, moved->blobs};
        plan.memoryCuesDropped = moved->memoryCuesDropped;
        plan.loopsDropped = moved->loopsDropped;
    }
    return plan;
}

void LibdjinteropEngineCueWriter::writeColumns(std::int64_t id, const EngineAnalysisColumns &before,
                                               const EngineAnalysisColumns &after, std::optional<double> knownShiftMs)
{
    if (before.isAnalyzed == after.isAnalyzed && before.isBeatGridLocked == after.isBeatGridLocked
        && before.bpm == after.bpm && before.bpmAnalyzed == after.bpmAnalyzed && before.key == after.key
        && before.trackData == after.trackData && before.overviewWaveFormData == after.overviewWaveFormData
        && before.beatData == after.beatData) {
        return;
    }
    const bool gridWritten = !after.beatData.isNull() && !(before.beatData == after.beatData);
    const GridMarkPlan plan = gridWritten ? planMarkWithGrid(id, after, knownShiftMs) : GridMarkPlan{};
    // The grid, the moved cues and the mark in one transaction: a failure
    // leaves the row as it was, cues, mark and all.
    std::string error;
    if (!rowAccess().write(id, after, &error, plan.takeOffMark, plan.cues ? &*plan.cues : nullptr)) {
        throw std::runtime_error("could not write the row's analysis: " + error);
    }
    m_memoryCuesDroppedByLeadInShift += plan.memoryCuesDropped;
    m_loopsDroppedByLeadInShift += plan.loopsDropped;
    if (m_drops) {
        m_drops->memoryCues += plan.memoryCuesDropped;
        m_drops->loops += plan.loopsDropped;
    }
}

void LibdjinteropEngineCueWriter::putOverviewFile(const std::string &relative, const std::optional<std::string> &bytes)
{
    const std::filesystem::path path = pathFromUtf8(overviewFileRoot()) / pathFromUtf8(relative);
    if (bytes) {
        if (m_overviewFileSink) {
            m_overviewFileSink(relative, *bytes);
            return;
        }
        std::filesystem::create_directories(path.parent_path());
        if (!writeFileDurablyAtomic(pathToUtf8(path), *bytes)) {
            throw std::runtime_error("could not write the overview file " + pathToUtf8(path));
        }
        return;
    }
    if (m_overviewFileRemover) {
        m_overviewFileRemover(relative);
        return;
    }
    std::string failure;
    if (!removeEntry(path, failure)) {
        throw std::runtime_error("could not remove the overview file " + pathToUtf8(path) + ": " + failure);
    }
}

void LibdjinteropEngineCueWriter::beforeWrite()
{
    if (m_beforeBeatGridWrite) {
        m_beforeBeatGridWrite();
    }
}

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

    // The grid comes on rekordbox's timeline, as the cues do, and moves
    // onto Engine's by the same shift. A beat the shift puts before the
    // start is dropped; the line through the others still reaches back
    // past it. Whether the shift is known goes with the grid to
    // writeColumns(), for the row's mark.
    const std::optional<double> knownShiftMs = knownShiftMsOf(*track);
    const double shiftMs = knownShiftMs.value_or(0.0);
    std::vector<domain::Beat> beats = rekordboxBeats;
    for (domain::Beat &beat : beats) {
        beat.timeMs -= shiftMs;
    }
    if (domain::markersFromBeats(beats, 0.0).markers.size() < 2) {
        return {Outcome::NoSourceGrid};
    }

    const auto converted = [&](double sampleRate, double samples) { return convertedGrid(beats, sampleRate, samples); };

    // The row as stored, read once: its flags and its beat data. Analysed
    // is what the player reads off the flag, with the blob to say what rate
    // and length the audio has. A row with a grid but the flag down is one
    // the player will analyse again, so it counts as not analysed. Every
    // write below is one transaction over the row's columns.
    const RowNow now = readRowNow(id, false);
    const std::optional<BeatDataBlob> existing = decoded<BeatDataBlob>(now.columns.beatData);
    const RowFlags flags = flagsOf(now.columns, existing, [&] { return overviewFileThere(now, id); });
    if (flags.unreadable) {
        return {Outcome::RowUnreadable};
    }
    const bool locked = flags.locked;

    if (flags.analysed) {
        const auto [grid, barBreaks] = converted(existing->sample_rate, existing->samples);
        AnalysedGridPlan plan =
            planAnalysedGrid(*existing, locked, grid, barBreaks, options.overwriteEngineEdits, options.ownGridFingerprint);
        if (!plan.write) {
            return plan.result;
        }
        if (m_keptStore && options.keepKey.empty()) {
            return {Outcome::CannotKeepEngines};
        }
        const std::string written = textOf(plan.write->to_blob());
        const auto apply = [&](EngineAnalysisColumns &after) { after.beatData = EngineSqlValue::ofBlob(written); };
        const KeepFacts keep{true, false, options.ownGridFingerprint};
        // What the write would refuse, a preview refuses too.
        if (keepBeforeWrite(id, options.keepKey, keep, now, apply, true) == Keep::Refused) {
            return {Outcome::CannotKeepEngines};
        }
        if (options.previewOnly) {
            return plan.result;
        }
        beforeWrite();
        keepBeforeWrite(id, options.keepKey, keep, now, apply, false);
        EngineAnalysisColumns after = now.columns;
        apply(after);
        writeColumns(id, now.columns, after, knownShiftMs);
        return plan.result;
    }

    // A row marked analysed with nothing stored is completed with both
    // parts or not at all: a grid alone under its raised flag would pass
    // for the player's own analysis later.
    if (options.unanalysed == UnanalysedRowGrid::Skip || flags.analysedNothingStored) {
        BeatGridWriteResult left{Outcome::RowNotAnalysed};
        left.analysedNothingStored = flags.analysedNothingStored;
        return left;
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
    result.gridFingerprint = gridFingerprint(blob);
    if (options.previewOnly) {
        return result;
    }
    beforeWrite();
    // The grid and the lock in one transaction: never a lock on no grid.
    const std::string written = textOf(blob.to_blob());
    const auto apply = [&](EngineAnalysisColumns &after) {
        after.beatData = EngineSqlValue::ofBlob(written);
        if (lock && !locked) {
            after.isBeatGridLocked = EngineSqlValue::ofInteger(1);
        }
    };
    keepBeforeWrite(id, options.keepKey, KeepFacts{}, now, apply, false);
    EngineAnalysisColumns after = now.columns;
    apply(after);
    writeColumns(id, now.columns, after, knownShiftMs);
    return result;
}

namespace
{

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
    return textOf(blob.to_blob());
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
    return fromEngineBlob<OverviewBlob>(text);
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

    // The grid on Engine's timeline, as writeBeatGrid() puts it. The shift
    // can mean reading the file's start, so only when a grid is wanted.
    std::vector<domain::Beat> beats = analysis.beats;
    std::optional<double> knownShiftMs;
    if (options.grids) {
        knownShiftMs = knownShiftMsOf(*track);
        const double shiftMs = knownShiftMs.value_or(0.0);
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

    // The row as stored, read once, flags as writeBeatGrid() reads them.
    const RowNow now = readRowNow(id, false);
    const std::optional<BeatDataBlob> existing = decoded<BeatDataBlob>(now.columns.beatData);
    const RowFlags flags = flagsOf(now.columns, existing, [&] { return overviewFileThere(now, id); });
    if (flags.unreadable) {
        if (options.grids) {
            result.grid.outcome = Outcome::RowUnreadable;
        }
        if (options.waveforms) {
            result.waveform = Waveform::RowUnreadable;
        }
        return result;
    }
    result.analysedNothingStored = flags.analysedNothingStored;
    result.grid.analysedNothingStored = flags.analysedNothingStored;
    const bool locked = flags.locked;
    result.rowLocked = locked;
    // respectLock: the lock stays even where the edits may be written over.
    const bool overwriteEdits = options.overwriteEngineEdits && !(options.respectLock && locked);
    const std::filesystem::path fileRoot = pathFromUtf8(overviewFileRoot());

    if (flags.analysed) {
        std::optional<BeatDataBlob> gridWrite;
        if (options.grids) {
            if (!hasGrid) {
                result.grid.outcome = Outcome::NoSourceGrid;
            } else {
                const auto [grid, barBreaks] = convertedGrid(beats, existing->sample_rate, existing->samples);
                if (options.differingIsConflict && !overwriteEdits && !options.overwriteEngineEdits
                    && gridDiffersFromEngines(*existing, locked, grid, options.ownGridFingerprint)) {
                    result.grid.outcome = Outcome::DiffersFromEngine;
                } else {
                    // An empty grid on Engine's side never wins (#136): it is
                    // no edit of the DJ's to keep, only a lock is.
                    const bool overEmpty = options.differingIsConflict && emptyGrid(*existing) && !locked;
                    AnalysedGridPlan plan = planAnalysedGrid(*existing, locked, grid, barBreaks,
                                                             overwriteEdits || overEmpty, options.ownGridFingerprint);
                    result.grid = plan.result;
                    gridWrite = std::move(plan.write);
                }
            }
        }

        std::optional<OverviewBlob> overviewWrite;
        std::optional<std::string> fileWrite;
        if (options.waveforms) {
            // The row's own length: its analysis record's, or the beat
            // data's where the record says nothing usable.
            double rate = existing->sample_rate;
            double samples = existing->samples;
            if (const auto record = decoded<TrackDataBlob>(now.columns.trackData);
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
                if (const auto current = decoded<OverviewBlob>(now.columns.overviewWaveFormData);
                    !current || *current != blob) {
                    overviewWrite = blob;
                }
                if (!options.skipOverviewFile) {
                    const std::string relative = overviewFileRelativePath(now.libraryUuid, id);
                    const OverviewBlob second = overviewBlob(domain::secondWaveformFromOverview(overview), samples);
                    if (const auto current = readOverviewFile(fileRoot / pathFromUtf8(relative));
                        !current || *current != second) {
                        fileWrite = fileBytesOf(second);
                        result.overviewFileRelativePath = relative;
                    }
                }
                result.waveform = overviewWrite || fileWrite ? Waveform::Written : Waveform::AlreadySame;
                // The waveform the player drew stays unless asked for. An
                // overview column already rekordbox's is Seabass's: only
                // its file is put right.
                if (overviewWrite && !options.overwritePlayerWaveforms
                    && holdsPlayersWaveform(id, options.keepKey, now, options.ownGridFingerprint)) {
                    overviewWrite.reset();
                    fileWrite.reset();
                    result.overviewFileRelativePath.clear();
                    result.waveform =
                        options.differingIsConflict ? Waveform::DiffersFromEngine : Waveform::PlayersWaveform;
                }
            }
        }

        if (!gridWrite && !overviewWrite && !fileWrite) {
            return result;
        }
        // The player's analysis of a row whose file is not under the stick
        // root has no key to be kept by: it is not replaced, in a preview
        // as in a write, and the parts that would have been say why.
        const auto cannotKeep = [&]() {
            if (gridWrite) {
                result.grid = {Outcome::CannotKeepEngines};
            }
            if (overviewWrite || fileWrite) {
                result.waveform = Waveform::CannotKeepEngines;
            }
            result.overviewFileRelativePath.clear();
            return result;
        };
        if (m_keptStore && options.keepKey.empty()) {
            return cannotKeep();
        }
        const auto apply = [&](EngineAnalysisColumns &after) {
            if (gridWrite) {
                after.beatData = EngineSqlValue::ofBlob(textOf(gridWrite->to_blob()));
                if (options.tempoWithGrid && analysis.bpm && *analysis.bpm > 0.0 && std::isfinite(*analysis.bpm)) {
                    after.bpm = EngineSqlValue::ofInteger(std::llround(*analysis.bpm));
                    after.bpmAnalyzed = EngineSqlValue::ofReal(*analysis.bpm);
                }
            }
            if (overviewWrite) {
                after.overviewWaveFormData = EngineSqlValue::ofBlob(textOf(overviewWrite->to_blob()));
            }
            if (fileWrite) {
                after.overviewFile = *fileWrite;
            }
        };
        const KeepFacts keep{true, overviewWrite.has_value() || fileWrite.has_value(), options.ownGridFingerprint};
        // What the write would refuse (a kept entry that cannot be read),
        // a preview refuses too, so it never offers a track the save
        // cannot write.
        if (keepBeforeWrite(id, options.keepKey, keep, now, apply, true) == Keep::Refused) {
            return cannotKeep();
        }
        if (options.previewOnly) {
            return result;
        }
        beforeWrite();
        result.keptEngineAnalysis = keepBeforeWrite(id, options.keepKey, keep, now, apply, false) == Keep::Kept;
        // The columns in one transaction, then the file: a failure between
        // the two leaves a mix of the row before and after, which the kept
        // entry recognises as Seabass's own (keepBeforeWrite()).
        EngineAnalysisColumns after = now.columns;
        apply(after);
        writeColumns(id, now.columns, after, knownShiftMs);
        if (fileWrite) {
            putOverviewFile(result.overviewFileRelativePath, *fileWrite);
        }
        return result;
    }

    // A row the player has not analysed, or one marked analysed with
    // nothing stored (RowFlags). Each part says what it lacks;
    // a part that lacks nothing is RowNotAnalysed when the other part
    // keeps the row from being completed.
    // The row's length in samples, as the player's own analysis would
    // have counted it. Where the file's container says it (fileSampleCount),
    // that is it: measured on a Prime 4, the player stores the container's
    // count to the sample for FLAC, AIFF (tests/fixtures/testshark_t73) and
    // M4A, and an MP3's frames times 1152. For an MP3 the file's frame
    // count wins over rekordbox's rule where both are there: on LAME files
    // rekordbox counts one frame more, and what a player stores for those
    // has not been measured (nor for 48 kHz or VBR MP3s). A FLAC, WAV,
    // AIFF or M4A whose count cannot be read is not completed from an
    // estimate. An MP3 whose file cannot be read: whole frames from
    // rekordbox's scrolling waveform (domain::engineSampleCount), the rule
    // that matched every Lavc-encoded file measured. Any other file (Ogg,
    // raw AAC): an ESTIMATE from the scrolling waveform, within about 3 ms,
    // since no rule for how the player counts those has been measured.
    const double rate = options.fileSampleRateHz.value_or(0.0);
    const bool rateUsable = rate > 0.0 && std::isfinite(rate);
    std::uint64_t sampleCount = 0;
    if (!rateUsable) {
        result.noSampleCountReason = "the file's sample rate could not be read";
    } else if (options.fileSampleCount && *options.fileSampleCount > 0) {
        sampleCount = *options.fileSampleCount;
    } else if (options.fileCountRequired) {
        result.noSampleCountReason = "the file's own sample count could not be read, and a FLAC, WAV, AIFF or M4A "
                                     "file is not completed from an estimate";
    } else if (!analysis.scrollEntries || *analysis.scrollEntries == 0) {
        result.noSampleCountReason = "rekordbox has no scrolling waveform to count the track's length by";
    } else {
        sampleCount = domain::engineSampleCount(*analysis.scrollEntries, rate, options.fileIsMp3);
    }
    if (options.grids) {
        result.grid.outcome = hasGrid ? Outcome::RowNotAnalysed : Outcome::NoSourceGrid;
    }
    if (options.waveforms) {
        result.waveform = !hasPreview ? Waveform::NoSourceWaveform
                          : sampleCount == 0 ? Waveform::NoSampleCount
                                             : Waveform::RowNotAnalysed;
    }
    // A row whose flag is down may still hold the player's waveform (an
    // analysis it broke off: seen on a stick a Prime 4 loaded). Completing
    // it would replace that waveform, so unless asked to the row is not
    // completed, and the grid, which goes only with a completion, waits
    // for the player with it.
    // Under differingIsConflict that waveform and rekordbox's differ only
    // where an answer can take rekordbox's: by completing the row, so with
    // both parts asked, a grid in rekordbox and no lock. Without the row's
    // length yet, it stays NoSampleCount, asked again with the file's.
    if (options.waveforms && hasPreview && !options.overwritePlayerWaveforms
        && holdsPlayersWaveform(id, options.keepKey, now, options.ownGridFingerprint)) {
        const bool completable = options.grids && hasGrid && !(locked && !overwriteEdits);
        if (!options.differingIsConflict || !completable) {
            result.waveform = Waveform::PlayersWaveform;
        } else if (sampleCount > 0) {
            result.waveform = Waveform::DiffersFromEngine;
        }
    }
    if (result.waveform != Waveform::NoSampleCount) {
        result.noSampleCountReason.clear();
    }
    if (!options.grids || !options.waveforms || !hasGrid || !hasPreview || sampleCount == 0
        || result.waveform == Waveform::PlayersWaveform || result.waveform == Waveform::DiffersFromEngine) {
        return result;
    }
    // A lock is the user's word whether or not the player has analysed
    // the row.
    if (locked && !overwriteEdits) {
        result.grid.outcome = Outcome::RowLocked;
        return result;
    }
    const auto samples = static_cast<double>(sampleCount);
    // The preview spans rekordbox's own length; with no scrolling waveform
    // to say what that is, the file's.
    const domain::EngineOverview overview = overviewOver(scrollSeconds().value_or(samples / rate), samples / rate);
    if (overview.empty()) {
        result.waveform = Waveform::NoSourceWaveform;
        return result;
    }
    const auto [grid, barBreaks] = convertedGrid(beats, rate, samples);
    // rekordbox's tempo; with none, a row Engine marked analysed takes the
    // grid's own, so its old tempo never sits beside rekordbox's grid. A
    // row the player never analysed keeps its own, as before.
    const std::optional<double> tempo = analysis.bpm && *analysis.bpm > 0.0 && std::isfinite(*analysis.bpm)
        ? analysis.bpm
        : flags.analysedNothingStored ? gridTempo(grid, rate)
                                      : std::nullopt;

    // The analysis record as the player writes it: rate, length, key (0
    // for none, as libdjinterop writes no key) and the three loudness
    // values; on 3.x 24 zero bytes follow, as on every row a Prime 4
    // analysed (68 bytes in all). 2.x records end after the loudness.
    TrackDataBlob record{};
    record.sample_rate = rate;
    record.samples = static_cast<std::int64_t>(sampleCount);
    // With no key from rekordbox a row Engine marked analysed keeps its own,
    // and the record says the same; otherwise 0, as libdjinterop writes.
    record.key = analysis.key ? *analysis.key
        : flags.analysedNothingStored && now.columns.key.type == EngineSqlValue::Type::Integer
        ? static_cast<int>(now.columns.key.integer)
        : 0;
    const std::array<double, 3> loudness =
        options.zeroLoudness ? std::array<double, 3>{0.0, 0.0, 0.0} : domain::loudnessFromOverview(overview);
    record.average_loudness_low = loudness[0];
    record.average_loudness_mid = loudness[1];
    record.average_loudness_high = loudness[2];
    if (m_tables->v3) {
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
        result.overviewFileRelativePath = overviewFileRelativePath(now.libraryUuid, id);
        file = fileBytesOf(overviewBlob(domain::secondWaveformFromOverview(overview), samples));
    }

    result.grid = {Outcome::Written, static_cast<int>(grid.size()), barBreaks, gridFingerprint(beatData)};
    result.waveform = Waveform::Written;
    result.completedRow = true;
    const auto apply = [&](EngineAnalysisColumns &after) {
        after.trackData = EngineSqlValue::ofBlob(textOf(record.to_blob()));
        after.overviewWaveFormData = EngineSqlValue::ofBlob(textOf(overviewData.to_blob()));
        after.beatData = EngineSqlValue::ofBlob(textOf(beatData.to_blob()));
        if (!options.skipOverviewFile) {
            after.overviewFile = file;
        }
        // The tempo worked out above; none leaves the row's.
        if (tempo) {
            after.bpm = EngineSqlValue::ofInteger(std::llround(*tempo));
            after.bpmAnalyzed = EngineSqlValue::ofReal(*tempo);
        }
        if (analysis.key) {
            after.key = EngineSqlValue::ofInteger(*analysis.key);
        }
        after.isAnalyzed = EngineSqlValue::ofInteger(1);
    };
    // A completion over the waveform the player drew (asked for: an answer,
    // or "Overwrite Engine's Analysis") keeps the player's row first, every
    // column and the overview file, so Restore Engine's Analysis brings it
    // back. What cannot be kept is not replaced, in a preview as in the
    // write.
    const bool overPlayers = holdsPlayersWaveform(id, options.keepKey, now, options.ownGridFingerprint);
    const KeepFacts keep{overPlayers, overPlayers, options.ownGridFingerprint, overPlayers};
    if (overPlayers
        && ((m_keptStore && options.keepKey.empty())
            || keepBeforeWrite(id, options.keepKey, keep, now, apply, true) == Keep::Refused)) {
        result.grid = {Outcome::CannotKeepEngines};
        result.waveform = Waveform::CannotKeepEngines;
        result.completedRow = false;
        result.overviewFileRelativePath.clear();
        return result;
    }
    if (options.previewOnly) {
        return result;
    }
    beforeWrite();
    result.keptEngineAnalysis = keepBeforeWrite(id, options.keepKey, keep, now, apply, false) == Keep::Kept;
    // The file first, then every column and the analysed flag in one
    // transaction: until the flag is up the player takes the row for one
    // it is still to analyse, so a failure anywhere leaves a row that
    // plays, never one flagged analysed with a piece missing.
    // Taken off again on a failure below only when this call put it there,
    // on the stick itself: a sink (the GUI's save) undoes its own.
    const bool fileWasNew = !options.skipOverviewFile && !m_overviewFileSink && !overviewFileThere(now, id);
    if (!options.skipOverviewFile) {
        putOverviewFile(result.overviewFileRelativePath, file);
    }
    EngineAnalysisColumns after = now.columns;
    apply(after);
    try {
        writeColumns(id, now.columns, after, knownShiftMs);
    } catch (...) {
        // The columns did not land: the file goes again, or the row would
        // hold an overview file and nothing else, unreadable from then on.
        // A failure to remove it does not hide the write's.
        if (fileWasNew) {
            try {
                putOverviewFile(result.overviewFileRelativePath, std::nullopt);
            } catch (const std::exception &) {
            }
        }
        throw;
    }
    return result;
}

// --- the player's own analysis, kept and put back -----------------------

namespace
{

// A value for a fingerprint: a number as a number whatever its storage
// class (libdjinterop and the player may bind one column differently), a
// blob through its decoder and libdjinterop's encoder, so the same
// analysis is the same text however it was compressed. A blob that does
// not decode stands as its bytes.
enum class BlobKind { TrackData, Overview, BeatData };

std::string canonicalBlob(const std::string &bytes, BlobKind kind)
{
    const auto encoded = [&](const auto &blob) { return blob ? "d" + textOf(blob->to_blob()) : "r" + bytes; };
    try {
        switch (kind) {
        case BlobKind::TrackData:
            return encoded(fromEngineBlob<TrackDataBlob>(bytes));
        case BlobKind::Overview:
            return encoded(fromEngineBlob<OverviewBlob>(bytes));
        case BlobKind::BeatData:
            return encoded(fromEngineBlob<BeatDataBlob>(bytes));
        }
    } catch (const std::exception &) {
    }
    return "r" + bytes;
}

std::string canonical(const EngineSqlValue &v, std::optional<BlobKind> kind = std::nullopt)
{
    switch (v.type) {
    case EngineSqlValue::Type::Null:
        return "n";
    case EngineSqlValue::Type::Integer:
    case EngineSqlValue::Type::Real: {
        char text[64];
        std::snprintf(text, sizeof text, "%.17g",
                      v.type == EngineSqlValue::Type::Integer ? static_cast<double>(v.integer) : v.real);
        return std::string("#") + text;
    }
    case EngineSqlValue::Type::Text:
        return "t" + v.bytes;
    case EngineSqlValue::Type::Blob:
        return kind ? "b" + canonicalBlob(v.bytes, *kind) : "b" + v.bytes;
    }
    return "n";
}

std::string canonicalFile(const std::optional<std::string> &file)
{
    return file ? "f" + canonicalBlob(*file, BlobKind::Overview) : std::string("none");
}

std::string fingerprintOf(const std::vector<std::string> &parts)
{
    hashing::Sha256 hash;
    for (const std::string &part : parts) {
        const std::string length = std::to_string(part.size()) + ":";
        hash.update(std::string_view(length));
        hash.update(std::string_view(part));
    }
    return hashing::toHex(hash.finish());
}

// Every column, as the parts of a fingerprint.
std::vector<std::string> stateColumnParts(const EngineAnalysisColumns &c)
{
    return {canonical(c.isAnalyzed),
            canonical(c.isBeatGridLocked),
            canonical(c.bpm),
            canonical(c.bpmAnalyzed),
            canonical(c.key),
            canonical(c.trackData, BlobKind::TrackData),
            canonical(c.overviewWaveFormData, BlobKind::Overview),
            canonical(c.beatData, BlobKind::BeatData)};
}

// What only an analysis writes of the columns: the record, the overview,
// Engine's own grid with the rate and length it is in, and the flag. Not
// the adjusted grid, the lock, the key or the tempo, which the DJ sets on
// the player.
std::vector<std::string> analysisColumnParts(const EngineAnalysisColumns &c)
{
    std::string beats = canonical(c.beatData);
    if (const auto blob = decoded<BeatDataBlob>(c.beatData)) {
        BeatDataBlob own = *blob;
        own.adjusted_beat_grid.clear();
        own.is_beatgrid_set = 0;
        beats = "d" + textOf(own.to_blob());
    }
    return {flagOf(c.isAnalyzed).value_or(false) ? "1" : "0", canonical(c.trackData, BlobKind::TrackData),
            canonical(c.overviewWaveFormData, BlobKind::Overview), beats};
}

// The whole row: everything kept.
std::string stateFingerprint(const EngineAnalysisColumns &c)
{
    std::vector<std::string> parts{"state 1"};
    const auto columns = stateColumnParts(c);
    parts.insert(parts.end(), columns.begin(), columns.end());
    parts.push_back(canonicalFile(c.overviewFile));
    return fingerprintOf(parts);
}

// What only an analysis writes, the overview file with it.
std::string analysisFingerprint(const EngineAnalysisColumns &c)
{
    std::vector<std::string> parts{"analysis 1"};
    const auto columns = analysisColumnParts(c);
    parts.insert(parts.end(), columns.begin(), columns.end());
    parts.push_back(canonicalFile(c.overviewFile));
    return fingerprintOf(parts);
}

// The same, in the row's two parts, which a write cut short can leave out
// of step: the columns (one transaction), and the overview file.
std::string columnStateFingerprint(const EngineAnalysisColumns &c)
{
    std::vector<std::string> parts{"columns 1"};
    const auto columns = stateColumnParts(c);
    parts.insert(parts.end(), columns.begin(), columns.end());
    return fingerprintOf(parts);
}

std::string columnAnalysisFingerprint(const EngineAnalysisColumns &c)
{
    std::vector<std::string> parts{"analysis columns 1"};
    const auto columns = analysisColumnParts(c);
    parts.insert(parts.end(), columns.begin(), columns.end());
    return fingerprintOf(parts);
}

std::string fileFingerprint(const EngineAnalysisColumns &c)
{
    return fingerprintOf({"file 1", canonicalFile(c.overviewFile)});
}

bool contains(const std::vector<std::string> &list, const std::string &fingerprint)
{
    return std::find(list.begin(), list.end(), fingerprint) != list.end();
}

// The analysis in the row's columns is one Seabass knows: the kept
// original's, or the row's before or after Seabass's last write. Anything
// else is the player's new analysis, the only thing that takes the
// original's place. The overview file plays no part: Seabass's write of it
// can be cut short, and a player's analysis writes the columns either way.
// An entry of format 1 records no parts, and is judged as it was then, by
// the whole analysis.
bool analysisIsKnown(const application::KeptEngineAnalysis &kept, const EngineAnalysisColumns &now)
{
    const std::string columns = columnAnalysisFingerprint(now);
    if (columns == columnAnalysisFingerprint(kept.original) || contains(kept.seabassColumnAnalyses, columns)) {
        return true;
    }
    const std::string whole = analysisFingerprint(now);
    return whole == analysisFingerprint(kept.original) || (!kept.seabassAnalysis.empty() && whole == kept.seabassAnalysis);
}

// The row is as Seabass left it, its last write finished or cut short, or
// as the original with one part of Seabass's (a restore cut short): each
// part, the columns and the file, one Seabass wrote or kept.
bool rowIsSeabassOrKept(const application::KeptEngineAnalysis &kept, const EngineAnalysisColumns &now)
{
    if (!kept.seabassState.empty() && stateFingerprint(now) == kept.seabassState) {
        return true;
    }
    if (kept.seabassColumnStates.empty()) {
        return false;
    }
    const std::string columns = columnStateFingerprint(now);
    const std::string file = fileFingerprint(now);
    return (columns == columnStateFingerprint(kept.original) || contains(kept.seabassColumnStates, columns))
        && (file == fileFingerprint(kept.original) || contains(kept.seabassFiles, file));
}

// The after first, then the before, once each.
std::vector<std::string> beforeAndAfter(std::string after, std::string before)
{
    std::vector<std::string> out{std::move(after)};
    if (before != out.front()) {
        out.push_back(std::move(before));
    }
    return out;
}

void addOnce(std::vector<std::string> &list, const std::string &fingerprint)
{
    if (!contains(list, fingerprint)) {
        list.push_back(fingerprint);
    }
}

// A row waiting for the player's analysis (markForDeviceAnalysis's state).
bool waitingForAnalysis(const EngineAnalysisColumns &c)
{
    return !flagOf(c.isAnalyzed).value_or(false) && c.trackData.isNull() && c.overviewWaveFormData.isNull()
        && c.beatData.isNull();
}

}  // namespace

LibdjinteropEngineCueWriter::Keep LibdjinteropEngineCueWriter::keepBeforeWrite(
    std::int64_t id, const std::string &keepKey, const KeepFacts &facts, const RowNow &row,
    const std::function<void(EngineAnalysisColumns &)> &apply, bool previewOnly)
{
    if (!m_keptStore) {
        return Keep::Nothing;
    }
    using Status = application::KeptEngineAnalysisLookup::Status;
    try {
        if (keepKey.empty()) {
            if (!facts.playerAnalysed) {
                return Keep::Nothing;
            }
            // The callers refuse such a row first (CannotKeepEngines).
            throw std::runtime_error("its file is not under the stick root, so there is no key to keep it by");
        }
        const application::KeptEngineAnalysisLookup found = m_keptStore->find(keepKey);
        if (found.status == Status::Damaged) {
            // A row the player never analysed has nothing to keep; any
            // other is not replaced while what was kept of it before
            // cannot be read (Let the Player Analyse Again clears it).
            return facts.playerAnalysed ? Keep::Refused : Keep::Nothing;
        }
        const bool valid = found.status == Status::Found && found.entry.trackFile == row.trackFile
            && found.entry.trackId == id && found.entry.libraryUuid == row.libraryUuid;
        if (!valid && !facts.playerAnalysed) {
            return Keep::Nothing;
        }
        // Before any other return, so that a library whose uuid cannot name
        // the file fails a preview as it fails the write.
        const std::string relative = overviewFileRelativePath(row.libraryUuid, id);
        const std::optional<BeatDataBlob> existing = decoded<BeatDataBlob>(row.columns.beatData);
        const std::optional<std::string> gridNow =
            existing ? std::optional<std::string>(gridFingerprint(*existing)) : std::nullopt;
        const bool ledgersGrid = gridNow && facts.ledgerGrid && *facts.ledgerGrid == *gridNow;
        if (!valid) {
            // Nothing kept, or a stale entry of another row. Nothing is
            // kept only when every part the write replaces is provably
            // Seabass's: a row the player never analysed; one Seabass
            // completed (both grids the ledger's, so the waveform is
            // Seabass's too: the player's analysis writes its own grid);
            // or a write of only the grid, when the grid is the ledger's.
            // Never when the waveform replaced is the player's
            // (facts.playersWaveform): a row whose flag is down over it
            // holds the player's waveform whatever the ledger says of its
            // grid (a fill, then an analysis the player broke off).
            const bool completedBySeabass = !facts.playersWaveform && ledgersGrid
                && sameGrid(existing->default_beat_grid, existing->adjusted_beat_grid);
            const bool onlySeabassGrid = ledgersGrid && !facts.replacesWaveform;
            if (!facts.playerAnalysed || completedBySeabass || onlySeabassGrid || previewOnly) {
                return Keep::Nothing;
            }
        } else if (previewOnly && !facts.playerAnalysed) {
            return Keep::Nothing;
        }
        RowNow now = row;
        now.columns.overviewFileRelativePath = relative;
        now.columns.overviewFile = readFileBytes(pathFromUtf8(overviewFileRoot()) / pathFromUtf8(relative));
        EngineAnalysisColumns after = now.columns;
        apply(after);
        application::KeptEngineAnalysis next;
        bool kept = false;
        if (valid) {
            next = found.entry;
            // The kept original is replaced only on positive evidence that
            // the player analysed the row anew since Seabass wrote it: an
            // analysis in its columns that is neither the original's nor
            // one Seabass's last write left or found there. A write of
            // Seabass's cut short, the columns written and the overview file
            // not (or a restore cut short the other way round), is known by
            // its parts and costs the original nothing.
            if (facts.playerAnalysed && !next.seabassAnalysis.empty() && !analysisIsKnown(next, now.columns)) {
                next.original = now.columns;
                kept = true;
            } else if (facts.playerAnalysed && existing && after.beatData != now.columns.beatData && !ledgersGrid
                       && !contains(next.seabassGrids, *gridNow) && !rowIsSeabassOrKept(next, now.columns)) {
                // The grid about to be replaced is none Seabass wrote, on a
                // row changed since Seabass left it: the DJ moved it on the
                // player. It is the newest grid the player made, and the
                // kept original takes it, its own analysis staying as kept.
                const std::optional<BeatDataBlob> original = decoded<BeatDataBlob>(next.original.beatData);
                if (!original || original->sample_rate != existing->sample_rate
                    || original->samples != existing->samples) {
                    // Nothing holds both the player's analysis and the
                    // DJ's grid: unsure, so not replaced.
                    return Keep::Refused;
                }
                if (original->is_beatgrid_set != existing->is_beatgrid_set
                    || !sameGrid(original->adjusted_beat_grid, existing->adjusted_beat_grid)) {
                    BeatDataBlob merged = *original;
                    merged.is_beatgrid_set = existing->is_beatgrid_set;
                    merged.adjusted_beat_grid = existing->adjusted_beat_grid;
                    next.original.beatData = EngineSqlValue::ofBlob(textOf(merged.to_blob()));
                    kept = true;
                }
            }
        } else {
            next.pathKey = keepKey;
            next.trackFile = now.trackFile;
            next.trackId = id;
            next.libraryUuid = now.libraryUuid;
            next.original = now.columns;
            kept = true;
        }
        if (previewOnly) {
            return Keep::Nothing;
        }
        // Before the row's first byte changes, the row as it is and as this
        // write will leave it: any mix of the two is Seabass's.
        next.seabassState = stateFingerprint(after);
        next.seabassAnalysis = analysisFingerprint(after);
        next.seabassColumnStates = beforeAndAfter(columnStateFingerprint(after), columnStateFingerprint(now.columns));
        next.seabassColumnAnalyses =
            beforeAndAfter(columnAnalysisFingerprint(after), columnAnalysisFingerprint(now.columns));
        next.seabassFiles = beforeAndAfter(fileFingerprint(after), fileFingerprint(now.columns));
        if (after.beatData != now.columns.beatData) {
            if (const auto written = decoded<BeatDataBlob>(after.beatData)) {
                addOnce(next.seabassGrids, gridFingerprint(*written));
            }
        }
        if (valid && next == found.entry) {
            return Keep::Nothing;
        }
        m_keptStore->keep(next);
        return kept ? Keep::Kept : Keep::Nothing;
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Engine's own analysis of this track could not be kept on the stick, so "
                                             "it was not replaced: ")
                                 + e.what());
    }
}

bool LibdjinteropEngineCueWriter::holdsPlayersWaveform(std::int64_t id, const std::string &keepKey, const RowNow &row,
                                                       const std::optional<std::string> &ledgerGrid)
{
    const EngineSqlValue &overview = row.columns.overviewWaveFormData;
    if (overview.isNull()) {
        return false;
    }
    if (const auto blob = decoded<OverviewBlob>(overview); blob && blob->waveform_points.empty()) {
        return false;
    }
    // Seabass never leaves an overview of its own on a row whose flag is
    // down: a completion raises the flag in the transaction that writes the
    // overview, and markForDeviceAnalysis clears the overview with the
    // flag. An overview there is the player's (an analysis it broke off),
    // whatever the ledger or the kept store say: a grid fill (the CLI's
    // --unanalysed fill) leaves the ledger's grid as both of its grids.
    if (!flagOf(row.columns.isAnalyzed).value_or(false)) {
        return true;
    }
    using Status = application::KeptEngineAnalysisLookup::Status;
    if (m_keptStore && !keepKey.empty()) {
        application::KeptEngineAnalysisLookup found;
        try {
            found = m_keptStore->find(keepKey);
        } catch (const std::exception &) {
            // Not knowing whose it is: the player's.
            return true;
        }
        if (found.status == Status::Damaged) {
            return true;
        }
        if (found.status == Status::Found && found.entry.trackFile == row.trackFile && found.entry.trackId == id
            && found.entry.libraryUuid == row.libraryUuid) {
            // The player's original is kept: an overview that is not the
            // original's, in an analysis Seabass knows, is Seabass's.
            const bool original = canonical(overview, BlobKind::Overview)
                == canonical(found.entry.original.overviewWaveFormData, BlobKind::Overview);
            return original || !analysisIsKnown(found.entry, row.columns);
        }
    }
    // Nothing kept of the row: Seabass's only on a row it completed, both
    // grids the ledger's (the player's analysis writes its own grid).
    // Blind spot: a player that analyses a row Seabass FILLED (--unanalysed
    // fill, a CLI-only hardware experiment) and keeps the filled grid as
    // both grids leaves flag 1, the ledger's grid and its own waveform,
    // which this calls Seabass's.
    const std::optional<BeatDataBlob> existing = decoded<BeatDataBlob>(row.columns.beatData);
    const bool completedBySeabass = existing && ledgerGrid && *ledgerGrid == gridFingerprint(*existing)
        && sameGrid(existing->default_beat_grid, existing->adjusted_beat_grid);
    return !completedBySeabass;
}

EngineAnalysisRestoreResult LibdjinteropEngineCueWriter::restoreEngineAnalysis(const std::string &trackSourceId,
                                                                              const std::string &keepKey,
                                                                              const EngineAnalysisRestoreOptions &options)
{
    using Outcome = EngineAnalysisRestoreResult::Outcome;
    using Status = application::KeptEngineAnalysisLookup::Status;
    EngineAnalysisRestoreResult result;
    auto track = database().track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    if (!m_tables) {
        result.outcome = Outcome::UnsupportedSchema;
        return result;
    }
    if (!m_keptStore || keepKey.empty()) {
        result.outcome = Outcome::NothingKept;
        return result;
    }
    const std::int64_t id = track->id();
    const application::KeptEngineAnalysisLookup found = m_keptStore->find(keepKey);
    if (found.status == Status::None) {
        result.outcome = Outcome::NothingKept;
        return result;
    }
    if (found.status == Status::Damaged) {
        result.outcome = Outcome::KeptDamaged;
        result.reason = found.error;
        return result;
    }
    const application::KeptEngineAnalysis &kept = found.entry;
    const RowNow now = readRowNow(id, true);
    if (kept.trackId != id || kept.trackFile != now.trackFile || kept.libraryUuid != now.libraryUuid) {
        result.outcome = Outcome::Stale;
        result.reason = kept.libraryUuid != now.libraryUuid ? "it was kept from another Engine library"
            : kept.trackFile != now.trackFile             ? "it was kept for a row of another file"
                                                           : "it was kept for another row of this file";
        return result;
    }
    const auto dropKept = [&]() {
        if (!options.previewOnly) {
            m_keptStore->drop(keepKey);
        }
    };
    if (stateFingerprint(now.columns) == stateFingerprint(kept.original)) {
        // The kept analysis is what the row holds: nothing to put back,
        // and the entry has done its work.
        result.outcome = Outcome::AlreadyEngines;
        if (!options.previewOnly) {
            beforeWrite();
            dropKept();
        }
        return result;
    }
    // As Seabass left it, its own write or an earlier restore cut short
    // included; or waiting for the player's analysis, which nothing was
    // done to since.
    const bool asSeabassLeftIt = rowIsSeabassOrKept(kept, now.columns);
    if (!asSeabassLeftIt && !waitingForAnalysis(now.columns)) {
        // The player's new analysis is newer than what was kept and would
        // be lost unkept: left alone, forced or not. Force reaches only a
        // grid, key or tempo changed on the player.
        const bool newerAnalysis = !analysisIsKnown(kept, now.columns);
        if (newerAnalysis || !options.force) {
            result.outcome = Outcome::ChangedSince;
            result.reason = newerAnalysis ? "the player analysed it again since Seabass wrote it"
                                          : "its beat grid, key or tempo was changed on the player since Seabass wrote it";
            return result;
        }
    }
    const std::string &relative = kept.original.overviewFileRelativePath.empty() ? now.columns.overviewFileRelativePath
                                                                                 : kept.original.overviewFileRelativePath;
    const bool fileChanges = kept.original.overviewFile != now.columns.overviewFile;
    result.outcome = Outcome::Restored;
    if (fileChanges) {
        result.overviewFileRelativePath = relative;
    }
    if (options.previewOnly) {
        return result;
    }
    beforeWrite();
    if (!asSeabassLeftIt) {
        // The row as it is now (waiting, or forced over) becomes one of
        // the states the entry knows first, so that this restore cut short
        // between the columns and the file is still put back by the next.
        application::KeptEngineAnalysis next = kept;
        addOnce(next.seabassColumnStates, columnStateFingerprint(now.columns));
        addOnce(next.seabassColumnAnalyses, columnAnalysisFingerprint(now.columns));
        addOnce(next.seabassFiles, fileFingerprint(now.columns));
        if (!(next == kept)) {
            m_keptStore->keep(next);
        }
    }
    std::string error;
    if (!rowAccess().write(id, kept.original, &error)) {
        throw std::runtime_error("could not put Engine's analysis back: " + error);
    }
    if (fileChanges) {
        putOverviewFile(relative, kept.original.overviewFile);
    }
    dropKept();
    return result;
}

LetPlayerAnalyseResult LibdjinteropEngineCueWriter::letPlayerAnalyseAgain(const std::string &trackSourceId,
                                                                         const std::string &keepKey, bool previewOnly)
{
    using Outcome = LetPlayerAnalyseResult::Outcome;
    LetPlayerAnalyseResult result;
    auto track = database().track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }
    if (!m_tables) {
        result.outcome = Outcome::UnsupportedSchema;
        return result;
    }
    const std::int64_t id = track->id();
    const RowNow now = readRowNow(id, false);
    // A kept entry that cannot be read can never be put back; it would
    // only stand in the way of keeping the player's analysis. Cleared here
    // whether or not the row is analysed: on a row whose flag is down over
    // the player's waveform it is what keeps a completion from keeping it.
    const auto dropDamaged = [&]() {
        if (m_keptStore && !keepKey.empty()
            && m_keptStore->find(keepKey).status == application::KeptEngineAnalysisLookup::Status::Damaged) {
            result.clearedDamagedEntry = true;
            if (!previewOnly) {
                m_keptStore->drop(keepKey);
            }
        }
    };
    if (!flagOf(now.columns.isAnalyzed).value_or(false)) {
        result.outcome = Outcome::NotAnalysed;
        dropDamaged();
        return result;
    }
    result.outcome = Outcome::Marked;
    if (previewOnly) {
        return result;
    }
    beforeWrite();
    std::string error;
    if (markForDeviceAnalysis(databaseFile(), {id}, &error) != 1) {
        throw std::runtime_error("could not mark the track for the player's analysis: " + error);
    }
    dropDamaged();
    return result;
}

EngineAnalysisFacts LibdjinteropEngineCueWriter::engineAnalysisFacts(const std::string &trackSourceId,
                                                                     const std::string &keepKey)
{
    using Status = application::KeptEngineAnalysisLookup::Status;
    EngineAnalysisFacts facts;
    auto track = database().track_by_id(std::stoll(trackSourceId));
    if (!track || !m_tables) {
        return facts;
    }
    const std::int64_t id = track->id();
    std::string error;
    const auto row = rowAccess().read(id, &error);
    facts.rowAnalysed = row && flagOf(row->columns.isAnalyzed).value_or(false);
    if (!m_keptStore || keepKey.empty()) {
        return facts;
    }
    const application::KeptEngineAnalysisLookup found = m_keptStore->find(keepKey);
    if (found.status == Status::Damaged) {
        facts.kept = EngineAnalysisFacts::Kept::Damaged;
    } else if (found.status == Status::Found) {
        facts.kept = row && found.entry.trackId == id && found.entry.trackFile == row->trackFile
                && found.entry.libraryUuid == row->libraryUuid
            ? EngineAnalysisFacts::Kept::Kept
            : EngineAnalysisFacts::Kept::Stale;
    }
    return facts;
}

}  // namespace seabass::infrastructure::engine
