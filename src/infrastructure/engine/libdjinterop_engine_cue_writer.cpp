#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

#include <optional>
#include <stdexcept>

#include <djinterop/djinterop.hpp>

#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/work_counters.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

constexpr int HotCueSlotCount = 8;

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

}  // namespace

LibdjinteropEngineCueWriter::LibdjinteropEngineCueWriter(std::string engineLibraryPath)
    : m_engineLibraryPath(std::move(engineLibraryPath))
{
}

djinterop::database &LibdjinteropEngineCueWriter::database()
{
    if (!m_database) {
        WorkCounters::instance().noteEngineDatabaseOpen();
        m_database = djinterop::engine::load_database(m_engineLibraryPath);
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
    auto &db = database();

    auto track = db.track_by_id(std::stoll(trackSourceId));
    if (!track) {
        throw std::runtime_error("no Engine track with id=" + trackSourceId);
    }

    // Some tracks' sample_rate() throws even though set_hot_cues() below
    // works fine (same libdjinterop decoder quirk documented in the Engine
    // reader: it lives in a different blob than the one hot cues use).
    // Fall back to 44.1kHz -- see libdjinterop_engine_reader.cpp for the
    // same reasoning.
    double sampleRate = 44100.0;
    try {
        if (auto rate = track->sample_rate()) {
            sampleRate = *rate;
        }
    } catch (const std::exception &) {
        // fall back to the default above
    }

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
    std::optional<double> earliestMemoryCueMs;
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
        double startOffset = cue.positionMs / 1000.0 * sampleRate;
        if (cue.isLoop) {
            double endOffset = cue.loopEndMs / 1000.0 * sampleRate;
            loopSlots[slot] = djinterop::loop{cue.comment, startOffset, endOffset, parseColor(cue.color)};
        } else {
            slots[slot] = djinterop::hot_cue{cue.comment, startOffset, parseColor(cue.color)};
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
                            ? std::optional<double>(*earliestMemoryCueMs / 1000.0 * sampleRate)
                            : std::nullopt);
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

}  // namespace seabass::infrastructure::engine
