// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Copies of one song that are not sample-aligned: the shifts measured
// between them (domain/duplicate_cue_shift), and what Cues on Duplicate
// Copies and Clean Up do with them. The probe here is a table; the real
// decoder has alignment_probe_test.

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "domain/duplicate_cleanup.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/duplicate_cue_shift.hpp"
#include "domain/matching_policy.hpp"
#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/paths/utf8_path.hpp"

using namespace seabass::domain;

namespace
{

// Answers from a table, keyed by the pair, and counts what it was asked.
class TableProbe : public AudioAlignmentProbe
{
public:
    std::map<std::pair<std::string, std::string>, std::optional<double>> answers;
    std::set<std::pair<std::string, std::string>> same;
    int asked = 0;

    bool sameContent(const std::string &a, const std::string &b) override { return same.count({a, b}) > 0; }

    Answer align(const std::string &a, const std::string &b) override
    {
        ++asked;
        const auto it = answers.find({a, b});
        return it == answers.end() || !it->second ? Answer::notLinedUp() : Answer::linedUp(*it->second);
    }
};

Track copy(std::string id, std::string path, std::vector<CuePoint> cues, double seconds = 200.0, int bitrate = 320)
{
    Track t;
    t.sourceId = std::move(id);
    t.filePath = std::move(path);
    t.filename = t.filePath.substr(t.filePath.find_last_of('/') + 1);
    t.title = "Song";
    t.artist = "Artist";
    t.durationSeconds = seconds;
    t.bitrate = bitrate;
    t.format = "rekordbox";
    t.cues = std::move(cues);
    return t;
}

CuePoint hot(int pad, double ms)
{
    return CuePoint{CuePoint::Kind::Hot, pad, ms, "#FF0000", ""};
}

CuePoint memory(double ms)
{
    return CuePoint{CuePoint::Kind::Memory, 0, ms, "", ""};
}

CuePoint loop(double from, double to)
{
    CuePoint c{CuePoint::Kind::Memory, 0, from, "", ""};
    c.isLoop = true;
    c.loopEndMs = to;
    return c;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

const CueTimelineOffset noOffset = [](const std::string &) { return std::optional<double>(0.0); };

DuplicateGroup measured(std::vector<Track> tracks, std::vector<std::optional<double>> shifts)
{
    DuplicateGroup group;
    group.tracks = std::move(tracks);
    group.cueShiftMs = std::move(shifts);
    return group;
}

}  // namespace

int main(int argc, char **argv)
{
    MatchingPolicy::reset();

    // Every copy is measured against the first, pairs are counted before
    // any is measured, and a group without a cue is not measured at all.
    {
        std::vector<DuplicateGroup> groups(2);
        groups[0].tracks = {copy("1", "/s/a.mp3", {hot(1, 1000)}), copy("2", "/s/b.mp3", {}),
                            copy("3", "/s/c.mp3", {})};
        groups[1].tracks = {copy("4", "/s/d.mp3", {}), copy("5", "/s/e.mp3", {})};
        assert(cueShiftPairsToMeasure(groups) == 2 && "only the group with cues, one pair per copy after the first");
        TableProbe probe;
        probe.answers[{"/s/a.mp3", "/s/b.mp3"}] = 21.3;
        probe.answers[{"/s/a.mp3", "/s/c.mp3"}] = std::nullopt;
        int ticks = 0;
        measureCueShifts(groups, &probe, noOffset, [&]() { ++ticks; });
        assert(ticks == 2 && probe.asked == 2);
        assert(groups[0].cueShiftMs.size() == 3);
        assert(groups[0].cueShiftMs[0] && near(*groups[0].cueShiftMs[0], 0.0));
        assert(groups[0].cueShiftMs[1] && near(*groups[0].cueShiftMs[1], 21.3));
        assert(!groups[0].cueShiftMs[2] && "no answer is unknown, never 0");
        assert(groups[1].cueShiftMs.empty() && "a group without cues is left unmeasured");
    }

    // The decoder's shift is turned onto the cue timeline with each file's
    // own offset, and an offset that cannot be known makes the shift
    // unknown.
    {
        std::vector<DuplicateGroup> groups(1);
        groups[0].tracks = {copy("1", "/s/a.mp3", {hot(1, 1000)}), copy("2", "/s/b.mp3", {}),
                            copy("3", "/s/c.flac", {})};
        TableProbe probe;
        probe.answers[{"/s/a.mp3", "/s/b.mp3"}] = 10.0;
        probe.answers[{"/s/a.mp3", "/s/c.flac"}] = 10.0;
        const CueTimelineOffset offsets = [](const std::string &path) -> std::optional<double> {
            if (path == "/s/a.mp3") {
                return 51.18;
            }
            if (path == "/s/b.mp3") {
                return 25.0;
            }
            return std::nullopt;
        };
        measureCueShifts(groups, &probe, offsets);
        assert(groups[0].cueShiftMs[1] && near(*groups[0].cueShiftMs[1], 10.0 + 25.0 - 51.18));
        assert(!groups[0].cueShiftMs[2] && "an offset nobody knows is a shift nobody knows");
    }

    // Two files with the same bytes line up on every timeline, even when
    // neither's offset can be read (a ".mp3" whose lead-in is unreadable).
    {
        std::vector<DuplicateGroup> groups(1);
        groups[0].tracks = {copy("1", "/s/a.mp3", {hot(1, 1000)}), copy("2", "/s/b.mp3", {}),
                            copy("3", "/s/c.mp3", {})};
        TableProbe probe;
        probe.same.insert({"/s/a.mp3", "/s/b.mp3"});
        probe.answers[{"/s/a.mp3", "/s/c.mp3"}] = 0.0;
        const CueTimelineOffset unreadable = [](const std::string &) { return std::optional<double>(); };
        int ticks = 0;
        measureCueShifts(groups, &probe, unreadable, [&]() { ++ticks; });
        assert(groups[0].cueShiftMs[1] && near(*groups[0].cueShiftMs[1], 0.0) && "the same bytes need no offsets");
        assert(!groups[0].cueShiftMs[2] && "decoded alike, but the offsets are needed and unknown");
        assert(ticks == 2 && probe.asked == 1 && "the identical pair is not decoded");
    }

    // One file listed twice needs no probe; without a probe anything else
    // is unknown.
    {
        std::vector<DuplicateGroup> groups(1);
        groups[0].tracks = {copy("1", "/s/a.mp3", {hot(1, 1000)}), copy("2", "/s/a.mp3", {}),
                            copy("3", "/s/b.mp3", {}), copy("4", "", {})};
        measureCueShifts(groups, nullptr, noOffset);
        assert(groups[0].cueShiftMs[1] && near(*groups[0].cueShiftMs[1], 0.0) && "the same path is the same file");
        assert(!groups[0].cueShiftMs[2] && "no probe, no answer");
        assert(!groups[0].cueShiftMs[3] && "no file, no answer");
    }

    // Between two copies: the difference of their shifts against the
    // first; under 1 ms is none; an unknown either side is unknown; a group
    // never measured lines up as it is.
    {
        const auto group = measured({copy("1", "/a", {}), copy("2", "/b", {}), copy("3", "/c", {}), copy("4", "/d", {})},
                                    {0.0, 150.0, std::nullopt, 150.6});
        assert(near(*cueShiftBetween(group, 0, 1), 150.0));
        assert(near(*cueShiftBetween(group, 1, 0), -150.0));
        assert(near(*cueShiftBetween(group, 1, 3), 0.0) && "0.6 ms is no shift");
        assert(!cueShiftBetween(group, 0, 2) && !cueShiftBetween(group, 2, 1));
        DuplicateGroup unmeasured;
        unmeasured.tracks = group.tracks;
        assert(near(*cueShiftBetween(unmeasured, 0, 1), 0.0));
    }

    // Moving cues onto another copy: positions and both loop ends move, and
    // a cue that lands off either end is left out and counted.
    {
        const std::vector<CuePoint> cues = {hot(1, 100), memory(5000), loop(199000, 199950), hot(2, 199900)};
        auto moved = movedCues(cues, 500.0, 200.0);
        assert(moved.leftOut == 2 && moved.cues.size() == 2 && "the loop's end and the late hot cue fall off the end");
        assert(near(moved.cues[0].positionMs, 600.0) && near(moved.cues[1].positionMs, 5500.0));
        moved = movedCues(cues, -200.0, 200.0);
        assert(moved.leftOut == 1 && moved.cues.size() == 3 && "the hot cue at 0.1 s falls off the start");
        assert(near(moved.cues[1].positionMs, 198800.0) && near(moved.cues[1].loopEndMs, 199750.0));
        moved = movedCues(cues, 500.0, 0.0);
        assert(moved.leftOut == 0 && "a length nobody knows bounds only the start");
        moved = movedCues({hot(1, 100)}, -0.9, 200.0);
        assert(moved.leftOut == 0 && near(moved.cues[0].positionMs, 100.0) && "under 1 ms nothing moves");
        const auto shifted = shiftedCues(cues, -200.0);
        assert(shifted.size() == cues.size() && near(shifted[0].positionMs, -100.0) && "comparing leaves nothing out");
    }

    // Cues on Duplicate Copies: the copy goes out with each target's shift.
    {
        const auto group = measured({copy("1", "/a", {}), copy("2", "/b", {hot(1, 1000)}), copy("3", "/c", {})},
                                    {0.0, 21.3, -40.0});
        const auto plan = DuplicateCueConsolidator::plan(group);
        assert(plan.kind == ConsolidationPlan::Kind::Unambiguous);
        assert(plan.source->sourceId == "2" && plan.targets.size() == 2 && plan.targetShiftMs.size() == 2);
        assert(near(plan.targetShiftMs[0], -21.3) && "onto copy 1: its sound is 21.3 ms earlier");
        assert(near(plan.targetShiftMs[1], -61.3));
    }
    // Two copies whose cues are the same once lined up are consistent, and
    // the same two read as they are would have been a conflict.
    {
        std::vector<Track> tracks = {copy("1", "/a", {hot(1, 10000), memory(60000)}),
                                     copy("2", "/b", {hot(1, 12000), memory(62000)})};
        assert(DuplicateCueConsolidator::plan(measured(tracks, {0.0, 2000.0})).kind
               == ConsolidationPlan::Kind::AlreadyConsistent);
        DuplicateGroup asTheyAre;
        asTheyAre.tracks = tracks;
        assert(DuplicateCueConsolidator::plan(asTheyAre).kind == ConsolidationPlan::Kind::Conflict);
        assert(DuplicateCueConsolidator::plan(measured(tracks, {0.0, 4000.0})).kind
               == ConsolidationPlan::Kind::Conflict && "lined up and still different is a conflict");
    }
    // Unknown: never copied, never compared.
    {
        assert(DuplicateCueConsolidator::plan(measured({copy("1", "/a", {hot(1, 1000)}), copy("2", "/b", {})},
                                                       {0.0, std::nullopt}))
                   .kind
               == ConsolidationPlan::Kind::Unaligned && "nowhere known to copy to");
        assert(DuplicateCueConsolidator::plan(measured({copy("1", "/a", {hot(1, 1000)}), copy("2", "/b", {hot(1, 1000)})},
                                                       {0.0, std::nullopt}))
                   .kind
               == ConsolidationPlan::Kind::Unaligned && "nothing to compare on");
        // The cued copies agree and are lined up; the copy that is not has
        // no cues and is not touched by a consistent group.
        assert(DuplicateCueConsolidator::plan(measured({copy("1", "/a", {hot(1, 1000)}), copy("2", "/b", {hot(1, 1000)}),
                                                        copy("3", "/c", {})},
                                                       {0.0, 0.0, std::nullopt}))
                   .kind
               == ConsolidationPlan::Kind::AlreadyConsistent);
        // ... but a conflict there would offer to copy onto it.
        assert(DuplicateCueConsolidator::plan(measured({copy("1", "/a", {hot(1, 1000)}), copy("2", "/b", {hot(1, 9000)}),
                                                        copy("3", "/c", {})},
                                                       {0.0, 0.0, std::nullopt}))
                   .kind
               == ConsolidationPlan::Kind::Unaligned);
    }

    // Clean Up: each removed copy's cues onto the survivor's timeline. The
    // survivor (320 kbps) is listed second, and its sound is 150 ms later
    // than the removed copy's.
    {
        const auto group = measured({copy("d", "/s/d.mp3", {memory(1000), hot(1, 3000), hot(2, 100000)}, 200.0, 128),
                                     copy("s", "/s/s.mp3", {hot(1, 5000)}, 200.0, 320)},
                                    {0.0, 150.0});
        const auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "s");
        assert(plan.removedShiftMs.size() == 1 && plan.removedShiftMs[0] && near(*plan.removedShiftMs[0], 150.0));
        assert(plan.mergedCuesForSurvivor.size() == 3);
        assert(near(plan.mergedCuesForSurvivor[0].positionMs, 5000.0) && "the survivor's own pad wins");
        assert(near(plan.mergedCuesForSurvivor[1].positionMs, 1150.0) && "the memory cue moves 150 ms later");
        assert(near(plan.mergedCuesForSurvivor[2].positionMs, 100150.0) && "and pad 2 with it");
        assert(plan.cuesLeftOut == 0 && !plan.hasCuesOnUnalignedCopy);
        assert(cuesPreservedBy(plan) == 2);
        assert(catalogNeedsMergedCues(plan, "rekordbox"));
    }
    // A removed cue that lands before the survivor's start is left out and
    // counted.
    {
        const auto group = measured({copy("s", "/s/s.mp3", {}, 200.0, 320), copy("d", "/s/d.mp3", {memory(100), memory(9000)}, 200.0, 128)},
                                    {0.0, 150.0});
        const auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "s" && near(*plan.removedShiftMs[0], -150.0));
        assert(plan.cuesLeftOut == 1 && plan.mergedCuesForSurvivor.size() == 1);
        assert(near(plan.mergedCuesForSurvivor[0].positionMs, 8850.0));
    }
    // A removed copy that could not be lined up gives nothing, and the
    // group says so when that copy had cues the survivor lacks.
    {
        auto group = measured({copy("s", "/s/s.mp3", {hot(1, 5000)}, 200.0, 320),
                               copy("d", "/s/d.mp3", {hot(2, 7000)}, 200.0, 128)},
                              {0.0, std::nullopt});
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.removedShiftMs[0]);
        assert(plan.hasCuesOnUnalignedCopy && "pad 2 would be lost");
        assert(plan.mergedCuesForSurvivor.size() == 1 && "and is not merged anywhere");
        assert(cuesPreservedBy(plan) == 0 && !catalogNeedsMergedCues(plan, "rekordbox"));
        // Nothing the survivor lacks: nothing at risk.
        group.tracks[1].cues = {hot(1, 7000)};
        plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.hasCuesOnUnalignedCopy && "the survivor's pad 1 wins anyway");
        group.tracks[1].cues = {};
        assert(!DuplicateCleanupPlanner::plan(group).hasCuesOnUnalignedCopy);
    }
    // Collapsed rows: each catalog's write is moved too.
    {
        Track survivor = copy("s", "/s/s.mp3", {hot(1, 5000)}, 200.0, 320);
        survivor.catalogRows = {{"rekordbox", "10", {hot(1, 5000)}}, {"engine", "20", {}}};
        Track doomed = copy("d", "/s/d.mp3", {memory(1000)}, 200.0, 128);
        doomed.catalogRows = {{"rekordbox", "11", {memory(1000)}}, {"engine", "21", {memory(1000)}}};
        const auto plan = DuplicateCleanupPlanner::plan(measured({doomed, survivor}, {0.0, 150.0}));
        const auto rekordbox = mergedCuesFor(plan, "rekordbox");
        const auto engine = mergedCuesFor(plan, "engine");
        assert(rekordbox.size() == 2 && near(rekordbox[1].positionMs, 1150.0));
        assert(engine.size() == 1 && near(engine[0].positionMs, 1150.0));
        assert(cuesPreservedBy(plan) == 1 && "one cue, written to two catalogs");
        // Not lined up: neither catalog gets it.
        const auto unsure = DuplicateCleanupPlanner::plan(measured({doomed, survivor}, {0.0, std::nullopt}));
        assert(mergedCuesFor(unsure, "engine").empty() && !catalogNeedsMergedCues(unsure, "engine"));
        assert(unsure.hasCuesOnUnalignedCopy);
    }

    // Formats, by extension and in any case.
    {
        DuplicateGroup group;
        group.tracks = {copy("1", "/s/Song.mp3", {}), copy("2", "/s/Song.FLAC", {})};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.mixedFormats && plan.fileFormats == std::vector<std::string>({"FLAC", "MP3"}));
        group.tracks = {copy("1", "/s/Song.mp3", {}), copy("2", "/s/other/Song-1.MP3", {})};
        plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.mixedFormats && plan.fileFormats == std::vector<std::string>({"MP3"}));
        group.tracks = {copy("1", "/s/Song.wav", {}), copy("2", "/s/Song.aiff", {})};
        assert(DuplicateCleanupPlanner::plan(group).mixedFormats && "any two formats, not only lossless and lossy");
    }

    // The cue timeline offset, from the real lead-in reader: the MP3's own
    // offset, 0 for a file that is not an MP3 (assumed), nothing for an MP3
    // whose lead-in cannot be read, and 0 for every file with the
    // correction off.
    if (argc > 1) {
        namespace fs = std::filesystem;
        const std::string clicks = argv[1];
        const std::string lame = seabass::pathToUtf8(seabass::pathFromUtf8(clicks) / "click-D-lame-44100.mp3");
        const auto leadIn = seabass::infrastructure::audio::readMp3LeadIn(lame);
        assert(leadIn && "the fixture's lead-in reads");
        const auto offset = seabass::infrastructure::audio::cueTimelineOffsetMs(lame);
        assert(offset && near(*offset, engineToRekordboxOffsetMs(*leadIn)) && *offset > 50.0);
        assert(near(*seabass::infrastructure::audio::cueTimelineOffsetMs("/nowhere/Song.flac"), 0.0));
        const fs::path junk = fs::temp_directory_path() / "duplicate_cue_shift_test-not-audio.mp3";
        std::ofstream(junk, std::ios::binary) << std::string(4096, 'x');
        assert(!seabass::infrastructure::audio::cueTimelineOffsetMs(seabass::pathToUtf8(junk))
               && "an MP3 whose lead-in cannot be read has no known offset");
        MatchingPolicy::setCorrectMp3CueTiming(false);
        assert(near(*seabass::infrastructure::audio::cueTimelineOffsetMs(lame), 0.0));
        assert(near(*seabass::infrastructure::audio::cueTimelineOffsetMs(seabass::pathToUtf8(junk)), 0.0));
        MatchingPolicy::reset();
        fs::remove(junk);
    } else {
        std::cerr << "duplicate_cue_shift_test: pass tests/fixtures/mp3-click as the first argument\n";
        return 1;
    }

    std::cout << "duplicate_cue_shift_test: all assertions passed\n";
    return 0;
}
