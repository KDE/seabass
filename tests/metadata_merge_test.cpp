// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "domain/metadata_merge.hpp"

using seabass::domain::CuePoint;
using seabass::domain::takeIncomingComment;
using seabass::domain::takeIncomingCues;
using seabass::domain::takeIncomingField;
using seabass::domain::takeIncomingPlayCount;
using seabass::domain::takeIncomingRating;

namespace
{

// Two times far enough apart that no tolerance anywhere could confuse
// them, named for what they mean rather than for their values.
constexpr std::int64_t Older = 1'700'000'000;
constexpr std::int64_t Newer = 1'800'000'000;
constexpr std::int64_t Unknown = 0;

CuePoint hotCue(int number, double positionMs)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = number;
    cue.positionMs = positionMs;
    cue.color = "#FF0000";
    return cue;
}

std::vector<CuePoint> cueSet(int count, double firstPosition = 1000.0)
{
    std::vector<CuePoint> cues;
    for (int i = 0; i < count; ++i) {
        cues.push_back(hotCue(i + 1, firstPosition + i * 1000.0));
    }
    return cues;
}

// ---- step 1: a blank is always filled, and never made ---------------

void fillsABlankWhicheverSideIsNewer()
{
    // The incoming side is older and still wins, because the other side
    // has nothing: this is not a disagreement, it is a gap.
    assert(takeIncomingComment("Peak time", "", Older, Newer));
    assert(takeIncomingRating(std::optional<int>(4), std::nullopt, Older, Newer));
    assert(takeIncomingCues(cueSet(1), {}, Older, Newer));
}

void neverReplacesAValueWithNothing()
{
    // The guarantee the whole rule rests on. A catalog that failed to
    // read a comment reports an empty one, and a newer empty reading
    // must not be allowed to erase the comment it failed to read.
    assert(!takeIncomingComment("", "Peak time", Newer, Older));
    assert(!takeIncomingRating(std::nullopt, std::optional<int>(4), Newer, Older));
    assert(!takeIncomingCues({}, cueSet(3), Newer, Older));
}

void agreementIsNotAWrite()
{
    // Nothing to decide, so nothing is rewritten. Without this a second
    // run of an unchanged library would rewrite every track it saw.
    assert(!takeIncomingComment("Peak time", "Peak time", Newer, Older));
    assert(!takeIncomingRating(std::optional<int>(4), std::optional<int>(4), Newer, Older));
    assert(!takeIncomingCues(cueSet(3), cueSet(3), Newer, Older));
}

void ratedZeroStarsIsRated()
{
    // 0 stars and unrated are different facts, and the store keeps them
    // apart. A zero must therefore read as content, not as a blank: the
    // later side wins rather than the rated one.
    assert(takeIncomingRating(std::optional<int>(0), std::optional<int>(3), Newer, Older));
    assert(!takeIncomingRating(std::optional<int>(0), std::optional<int>(3), Older, Newer));
    // And a zero fills a genuine blank like any other value.
    assert(takeIncomingRating(std::optional<int>(0), std::nullopt, Older, Newer));
}

// ---- step 2: more cues wins -----------------------------------------

void moreCuesBeatsFewerHoweverRecent()
{
    // The headline of the rule. Four cues stored months ago beat the one
    // cue a re-export left behind this morning.
    assert(!takeIncomingCues(cueSet(1), cueSet(4), Newer, Older));
    assert(takeIncomingCues(cueSet(4), cueSet(1), Older, Newer));
}

void cueCountOutranksTimeOnly()
{
    // Count decides only when the sets genuinely differ; equal sets were
    // already settled above, and equal counts fall through to time.
    assert(takeIncomingCues(cueSet(3, 5000.0), cueSet(3, 1000.0), Newer, Older));
    assert(!takeIncomingCues(cueSet(3, 5000.0), cueSet(3, 1000.0), Older, Newer));
}

// ---- step 3: the later edit wins ------------------------------------

void laterEditWinsWhenNothingElseSeparatesThem()
{
    assert(takeIncomingComment("New note", "Old note", Newer, Older));
    assert(!takeIncomingComment("Old note", "New note", Older, Newer));
    assert(takeIncomingPlayCount(std::optional<int>(9), std::optional<int>(2), Newer, Older));
    assert(!takeIncomingPlayCount(std::optional<int>(9), std::optional<int>(2), Older, Newer));
}

void anUnknownTimeNeverBeatsAKnownOne()
{
    // A reading we could not date must not overwrite work we can. It
    // loses to any real timestamp, in both directions of the comparison.
    assert(!takeIncomingComment("New note", "Old note", Unknown, Older));
    assert(takeIncomingComment("New note", "Old note", Older, Unknown));
    assert(!takeIncomingCues(cueSet(3, 5000.0), cueSet(3, 1000.0), Unknown, Older));
}

void aTieLeavesWhatIsThereAlone()
{
    // Two sides that disagree with nothing to separate them. A coin toss
    // that rewrites data on every run is worse than one that does not.
    assert(!takeIncomingComment("New note", "Old note", Older, Older));
    assert(!takeIncomingCues(cueSet(3, 5000.0), cueSet(3, 1000.0), Older, Older));
    assert(!takeIncomingComment("New note", "Old note", Unknown, Unknown));
}

// ---- the primitive itself -------------------------------------------

void takeIncomingFieldOrdersItsThreeSteps()
{
    // Blank-filling first: the incoming side is older and has content,
    // and wins anyway.
    assert(takeIncomingField(true, false, true, Older, Newer));
    // Emptiness never wins, even over a difference.
    assert(!takeIncomingField(false, true, true, Newer, Older));
    // No difference, no write.
    assert(!takeIncomingField(true, true, false, Newer, Older));
    // Only then does time decide.
    assert(takeIncomingField(true, true, true, Newer, Older));
    assert(!takeIncomingField(true, true, true, Older, Newer));
}

// The loop drop the review of 42eb4c0f flagged: cueSetsEqual took a loop
// for the cue at its start, so a newer copy whose pad 2 had become a loop
// read as "the same cues" and the loop never reached the store (backup) or
// the stick (restore). A loop whose out point moved was dropped the same
// way.
void aLoopIsNeverTheCueAtItsStart()
{
    CuePoint loop = hotCue(2, 2000.0);
    loop.isLoop = true;
    loop.loopEndMs = 6000.0;
    const std::vector<CuePoint> withLoop = {hotCue(1, 1000.0), loop};
    const std::vector<CuePoint> withCue = {hotCue(1, 1000.0), hotCue(2, 2000.0)};
    assert(takeIncomingCues(withLoop, withCue, Newer, Older) && "a newer loop replaces the cue on its pad");
    assert(takeIncomingCues(withCue, withLoop, Newer, Older) && "and a newer cue the loop");

    CuePoint longer = loop;
    longer.loopEndMs = 10000.0;
    const std::vector<CuePoint> withLongerLoop = {hotCue(1, 1000.0), longer};
    assert(takeIncomingCues(withLongerLoop, withLoop, Newer, Older) && "a loop made longer is a change");
    assert(!takeIncomingCues(withLongerLoop, withLoop, Older, Newer) && "and the merge rule still decides it");

    // Half a beat when the caller passes the tempo's: 100 ms of drift is
    // one cue at 128 BPM, two under the 60 ms fallback.
    const std::vector<CuePoint> drifted = {hotCue(1, 1100.0), loop};
    assert(!takeIncomingCues(drifted, withLoop, Newer, Older, seabass::domain::halfBeatMs(128.0)));
    assert(takeIncomingCues(drifted, withLoop, Newer, Older));
}

}  // namespace

int main()
{
    aLoopIsNeverTheCueAtItsStart();
    fillsABlankWhicheverSideIsNewer();
    neverReplacesAValueWithNothing();
    agreementIsNotAWrite();
    ratedZeroStarsIsRated();
    moreCuesBeatsFewerHoweverRecent();
    cueCountOutranksTimeOnly();
    laterEditWinsWhenNothingElseSeparatesThem();
    anUnknownTimeNeverBeatsAKnownOne();
    aTieLeavesWhatIsThereAlone();
    takeIncomingFieldOrdersItsThreeSteps();
    std::cout << "metadata_merge_test passed\n";
    return 0;
}
