// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain/beat_grid_comparison on grids whose answer is known by
// construction: the same grid, the same grid moved by a known amount
// (constant and moving), at half, double, two thirds and three halves the
// tempo, with the bar turned, rounded to whole milliseconds the way
// rekordbox stores it, ramps and steps, and grids with nothing in them.

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "domain/beat_grid_comparison.hpp"

using namespace seabass::domain;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

void near(double actual, double expected, double tolerance, const std::string &what)
{
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << "FAILED: " << what << ": " << actual << ", expected " << expected << " +- " << tolerance << "\n";
        ++failures;
    }
}

// Beats from a tempo given per beat interval: `bpmOf(k)` is the tempo
// from beat k to beat k + 1. Bars count from `firstBar`.
std::vector<Beat> gridFrom(double firstMs, int count, const std::function<double(int)> &bpmOf, int firstBar = 1)
{
    std::vector<Beat> beats;
    double time = firstMs;
    for (int k = 0; k < count; ++k) {
        beats.push_back({time, (firstBar - 1 + k) % 4 + 1});
        time += 60000.0 / bpmOf(k);
    }
    return beats;
}

std::vector<Beat> constantGrid(double firstMs, double bpm, int count, int firstBar = 1)
{
    return gridFrom(firstMs, count, [bpm](int) { return bpm; }, firstBar);
}

std::vector<Beat> shifted(std::vector<Beat> beats, double ms)
{
    for (Beat &beat : beats) {
        beat.timeMs += ms;
    }
    return beats;
}

std::vector<Beat> roundedToMs(std::vector<Beat> beats)
{
    for (Beat &beat : beats) {
        beat.timeMs = std::round(beat.timeMs);
    }
    return beats;
}

// 100 BPM for 16 beats, a ramp to 125 over 128 beats, 125 for 32.
std::vector<Beat> rampGrid()
{
    return gridFrom(250.0, 176, [](int k) {
        if (k < 16) {
            return 100.0;
        }
        if (k < 144) {
            return 100.0 + 25.0 * (k - 16) / 128.0;
        }
        return 125.0;
    });
}

// 120 BPM for 64 beats, then 126.
std::vector<Beat> stepGrid()
{
    return gridFrom(80.0, 160, [](int k) { return k < 64 ? 120.0 : 126.0; });
}

void identical()
{
    const auto grid = constantGrid(312.0, 124.0, 400);
    const auto result = compareBeatGrids(grid, grid);
    check(result.referenceBeats == 400 && result.candidateBeats == 400, "identical: beat counts");
    check(result.offset.count == 400, "identical: every beat measured");
    near(result.offset.medianMs, 0.0, 1e-9, "identical: median");
    near(result.offset.madMs, 0.0, 1e-9, "identical: scatter");
    near(result.offset.maxAbsMs, 0.0, 1e-9, "identical: max");
    near(result.hits.within1Ms, 1.0, 0.0, "identical: hits at 1 ms");
    check(result.tempo == TempoRelation::Same, "identical: same tempo");
    near(result.tempoRatio, 1.0, 1e-12, "identical: tempo ratio");
    check(result.bar.known && result.bar.beats == 0, "identical: bar not turned");
    near(result.bar.share, 1.0, 0.0, "identical: bar share");
    near(result.cover, 1.0, 0.0, "identical: cover");
    check(result.matchedBeats == 400, "identical: all matched");
    check(result.candidateShape.kind == GridShapeKind::Constant, "identical: constant");
    near(result.candidateShape.bpm, 124.0, 1e-9, "identical: bpm");
}

void shiftedComesBackExactly(const std::string &name, const std::vector<Beat> &reference, double shiftMs)
{
    const auto candidate = shifted(reference, shiftMs);
    const auto result = compareBeatGrids(candidate, reference);
    near(result.offset.medianMs, shiftMs, 1e-9, name + ": median is the shift");
    near(result.offset.madMs, 0.0, 1e-9, name + ": no scatter");
    near(result.offset.p95AbsMs, std::abs(shiftMs), 1e-9, name + ": p95 is the shift");
    near(result.offset.maxAbsMs, std::abs(shiftMs), 1e-9, name + ": max is the shift");
    check(result.tempo == TempoRelation::Same, name + ": same tempo");
    check(result.bar.known && result.bar.beats == 0 && result.bar.share == 1.0, name + ": bar not turned");
    // Taken back off by the caller's shift, nothing is left.
    const auto undone = compareBeatGrids(candidate, reference, -shiftMs);
    near(undone.offset.medianMs, 0.0, 1e-9, name + ": shift taken back off");
    near(undone.offset.maxAbsMs, 0.0, 1e-9, name + ": nothing left after taking it off");
    near(undone.hits.within1Ms, 1.0, 0.0, name + ": all hit after taking it off");
}

void shifts()
{
    shiftedComesBackExactly("constant +7.25", constantGrid(100.0, 128.0, 500), 7.25);
    shiftedComesBackExactly("constant -24", constantGrid(100.0, 128.0, 500), -24.0);
    shiftedComesBackExactly("ramp +4.5", rampGrid(), 4.5);
    shiftedComesBackExactly("step -13", stepGrid(), -13.0);
    shiftedComesBackExactly("rounded fractional +3", roundedToMs(constantGrid(40.0, 137.98, 600)), 3.0);

    const auto reference = rampGrid();
    const auto result = compareBeatGrids(shifted(reference, 4.5), reference);
    near(result.hits.within3Ms, 0.0, 0.0, "ramp +4.5: none within 3 ms");
    near(result.hits.within8Ms, 1.0, 0.0, "ramp +4.5: all within 8 ms");
    check(result.candidateShape.kind == GridShapeKind::Moving, "ramp +4.5: candidate moving");
}

void scatter()
{
    // Every other beat 2 ms early, the rest 2 ms late: no shift, 2 ms of
    // scatter.
    const auto reference = constantGrid(0.0, 120.0, 200);
    auto candidate = reference;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        candidate[i].timeMs += i % 2 == 0 ? -2.0 : 2.0;
    }
    const auto result = compareBeatGrids(candidate, reference);
    near(result.offset.medianMs, 0.0, 1e-9, "scatter: median");
    near(result.offset.madMs, 2.0, 1e-9, "scatter: mad");
    near(result.offset.p95AbsMs, 2.0, 1e-9, "scatter: p95");
    near(result.hits.within1Ms, 0.0, 0.0, "scatter: none within 1 ms");
    near(result.hits.within3Ms, 1.0, 0.0, "scatter: all within 3 ms");
    check(result.candidateShape.kind == GridShapeKind::Moving, "scatter: a 2 ms zigzag is not a line");

    // One beat far out: the max sees it, the median and p95 do not.
    auto oneOut = reference;
    oneOut[100].timeMs += 30.0;
    const auto outlier = compareBeatGrids(oneOut, reference);
    near(outlier.offset.maxAbsMs, 30.0, 1e-9, "outlier: max");
    near(outlier.offset.p95AbsMs, 0.0, 1e-9, "outlier: p95");
    near(outlier.hits.within20Ms, 199.0 / 200.0, 1e-12, "outlier: one miss at 20 ms");
}

void tempoRelations()
{
    const auto reference = constantGrid(500.0, 128.0, 256);
    std::vector<Beat> half;
    for (std::size_t i = 0; i < reference.size(); i += 2) {
        half.push_back(reference[i]);
    }
    const auto halfResult = compareBeatGrids(half, reference);
    check(halfResult.tempo == TempoRelation::Half,
          std::string("half: got ") + tempoRelationName(halfResult.tempo));
    near(halfResult.tempoRatio, 0.5, 1e-9, "half: ratio");
    near(halfResult.hits.within1Ms, 0.5, 0.01, "half: every other beat hit");

    const auto doubled = constantGrid(500.0, 256.0, 511);
    const auto doubleResult = compareBeatGrids(doubled, reference);
    check(doubleResult.tempo == TempoRelation::Double,
          std::string("double: got ") + tempoRelationName(doubleResult.tempo));
    near(doubleResult.hits.within1Ms, 1.0, 0.0, "double: every reference beat has a candidate");

    const auto twoThirds = constantGrid(500.0, 128.0 * 2.0 / 3.0, 171);
    const auto twoThirdsResult = compareBeatGrids(twoThirds, reference);
    check(twoThirdsResult.tempo == TempoRelation::TwoThirds,
          std::string("two thirds: got ") + tempoRelationName(twoThirdsResult.tempo));

    const auto threeHalves = constantGrid(500.0, 192.0, 383);
    const auto threeHalvesResult = compareBeatGrids(threeHalves, reference);
    check(threeHalvesResult.tempo == TempoRelation::ThreeHalves,
          std::string("three halves: got ") + tempoRelationName(threeHalvesResult.tempo));

    const auto other = constantGrid(500.0, 140.0, 280);
    check(compareBeatGrids(other, reference).tempo == TempoRelation::Other, "140 against 128 is other");

    // On a moving grid too: the reference ramps, the candidate is the
    // same ramp at every other beat.
    const auto ramp = rampGrid();
    std::vector<Beat> halfRamp;
    for (std::size_t i = 0; i < ramp.size(); i += 2) {
        halfRamp.push_back(ramp[i]);
    }
    const auto movingHalf = compareBeatGrids(halfRamp, ramp);
    check(movingHalf.tempo == TempoRelation::Half,
          std::string("half on a ramp: got ") + tempoRelationName(movingHalf.tempo));
    near(movingHalf.tempoRatio, 0.5, 0.01, "half on a ramp: ratio");
}

void barTurns()
{
    const auto reference = constantGrid(0.0, 126.0, 300, 1);
    for (int turn = 0; turn < 4; ++turn) {
        // The candidate calls the reference's first beat beat 1 + turn.
        const auto candidate = constantGrid(0.0, 126.0, 300, 1 + turn);
        const auto result = compareBeatGrids(candidate, reference);
        check(result.bar.known, "bar turned " + std::to_string(turn) + ": known");
        check(result.bar.beats == turn,
              "bar turned " + std::to_string(turn) + ": got " + std::to_string(result.bar.beats));
        near(result.bar.share, 1.0, 0.0, "bar turned " + std::to_string(turn) + ": share");
        check(result.bar.counted == 300, "bar turned " + std::to_string(turn) + ": counted");
    }
    // A grid that does not say where the bar is gives no turn.
    auto unknownBars = reference;
    for (Beat &beat : unknownBars) {
        beat.beatInBar = 0;
    }
    check(!compareBeatGrids(unknownBars, reference).bar.known, "no bar places: turn unknown");
}

void shapes()
{
    struct Rounded
    {
        double bpm;
        int beats;
    };
    for (const Rounded r : {Rounded{137.98, 828}, Rounded{128.0, 768}, Rounded{174.0, 1000}, Rounded{93.37, 500}}) {
        const auto shape = gridShape(roundedToMs(constantGrid(37.0, r.bpm, r.beats)));
        const std::string name = "rounded " + std::to_string(r.bpm);
        check(shape.kind == GridShapeKind::Constant, name + ": constant");
        near(shape.bpm, r.bpm, 0.005, name + ": bpm");
        check(shape.straightRuns == 1, name + ": one run");
        // A least-squares line is not the one that keeps the furthest beat
        // nearest, so it can leave a rounded beat a little over half a
        // millisecond away; well within the tolerance all the same.
        check(shape.maxLineResidualMs < 1.0, name + ": residual " + std::to_string(shape.maxLineResidualMs));
    }

    const auto ramp = gridShape(rampGrid());
    check(ramp.kind == GridShapeKind::Moving, "ramp: moving");
    near(ramp.lowestBpm, 100.0, 0.5, "ramp: lowest");
    near(ramp.highestBpm, 125.0, 0.5, "ramp: highest");
    check(ramp.straightRuns > 3, "ramp: many runs, got " + std::to_string(ramp.straightRuns));

    for (const bool rounded : {false, true}) {
        const auto step = gridShape(rounded ? roundedToMs(stepGrid()) : stepGrid());
        const std::string name = rounded ? "rounded step" : "step";
        check(step.kind == GridShapeKind::Moving, name + ": moving");
        check(step.straightRuns == 2, name + ": two runs, got " + std::to_string(step.straightRuns));
        near(step.lowestBpm, 120.0, 0.01, name + ": lowest");
        near(step.highestBpm, 126.0, 0.01, name + ": highest");
    }

    // Six runs of 32 beats, 127.5 and 128 by turns, rounded as rekordbox
    // would store them.
    const auto alternating =
        gridFrom(10.0, 6 * 32 + 1, [](int k) { return (k / 32) % 2 == 0 ? 127.5 : 128.0; });
    const auto altShape = gridShape(roundedToMs(alternating));
    check(altShape.kind == GridShapeKind::Moving, "alternating: moving");
    check(altShape.straightRuns == 6, "alternating: six runs, got " + std::to_string(altShape.straightRuns));
    near(altShape.lowestBpm, 127.5, 0.02, "alternating: lowest");
    near(altShape.highestBpm, 128.0, 0.02, "alternating: highest");
}

void emptyAndOne()
{
    const std::vector<Beat> none;
    const std::vector<Beat> one{{1000.0, 1}};
    const auto grid = constantGrid(0.0, 120.0, 64);

    check(gridShape(none).kind == GridShapeKind::None, "empty: no shape");
    check(gridShape(one).kind == GridShapeKind::None && gridShape(one).beats == 1, "one beat: no shape");

    const auto emptyCandidate = compareBeatGrids(none, grid);
    check(emptyCandidate.offset.count == 0 && emptyCandidate.hits.within20Ms == 0.0, "empty candidate: nothing hit");
    check(emptyCandidate.tempo == TempoRelation::Unknown && !emptyCandidate.bar.known, "empty candidate: unknown");
    near(emptyCandidate.cover, 0.0, 0.0, "empty candidate: no cover");

    const auto emptyReference = compareBeatGrids(grid, none);
    check(emptyReference.offset.count == 0 && emptyReference.referenceBeats == 0, "empty reference: nothing");

    const auto oneOnOne = compareBeatGrids(one, one);
    check(oneOnOne.offset.count == 1, "one on one: measured");
    near(oneOnOne.offset.medianMs, 0.0, 0.0, "one on one: offset");
    near(oneOnOne.hits.within1Ms, 1.0, 0.0, "one on one: hit");
    near(oneOnOne.cover, 1.0, 0.0, "one on one: cover");
    check(oneOnOne.tempo == TempoRelation::Unknown, "one on one: no tempo from one beat");

    // A one-beat candidate in the middle of a long reference: it covers
    // none of its span and is near one beat.
    const auto oneInMany = compareBeatGrids(one, grid);
    check(oneInMany.offset.count == 1, "one in many: only the beat at it measured");
    near(oneInMany.hits.within1Ms, 1.0 / 64.0, 1e-12, "one in many: one hit of 64");
    near(oneInMany.cover, 0.0, 0.0, "one in many: no cover");
}

void cover()
{
    const auto reference = constantGrid(0.0, 120.0, 201);  // 0 to 100 s
    std::vector<Beat> firstHalf(reference.begin(), reference.begin() + 101);
    const auto result = compareBeatGrids(firstHalf, reference);
    near(result.cover, 0.5, 1e-12, "half covered: cover");
    near(result.hits.within1Ms, 101.0 / 201.0, 1e-12, "half covered: the rest are misses");
    check(result.offset.count == 101, "half covered: only the covered beats measured");

    // Out of order and with a repeated beat: taken in order, the repeat
    // dropped.
    auto messy = reference;
    std::swap(messy[3], messy[7]);
    messy.push_back(messy[10]);
    const auto tidy = compareBeatGrids(messy, reference);
    check(tidy.candidateBeats == 201, "messy: repeat dropped");
    near(tidy.offset.maxAbsMs, 0.0, 0.0, "messy: in order again");
}

}  // namespace

// The drift over the track: a tempo a little off, an offset that grows
// past half a beat, a plain shift, stray beats, and where it is unknown.
void drift()
{
    // 128 against 128.01 BPM over 768 beats (six minutes): the candidate's
    // beat n is at n * (60000 / 128.01), so by the last beat it is
    // 767 * (60000 / 128.01 - 60000 / 128) ms ahead: about 28 ms early.
    const auto reference = constantGrid(100.0, 128.0, 768);
    const auto fast = constantGrid(100.0, 128.01, 768);
    const double expected = 767.0 * (60000.0 / 128.01 - 60000.0 / 128.0);
    const auto result = compareBeatGrids(fast, reference);
    check(result.drift.known, "drift: known on the same tempo");
    near(result.drift.ms, expected, 1e-6, "drift: 0.01 BPM over six minutes");
    check(result.drift.ms < -27.0 && result.drift.ms > -29.0, "drift: about 28 ms early");
    near(result.drift.startMs, 0.0, 1e-6, "drift: the start is exact");
    near(result.drift.endMs, expected, 1e-6, "drift: all of it at the end");

    // 128 against 129: the offset passes half a beat (234 ms) well before
    // the end, and is followed past it: 767 beats, 3.6 ms a beat.
    const auto faster = compareBeatGrids(constantGrid(100.0, 129.0, 780), reference);
    check(faster.tempo == TempoRelation::Same, "drift: 129 against 128 is the same tempo");
    near(faster.drift.ms, 767.0 * (60000.0 / 129.0 - 60000.0 / 128.0), 1e-6, "drift: followed past half a beat");

    // A shift is not a drift.
    const auto moved = compareBeatGrids(shifted(reference, -12.5), reference);
    check(moved.drift.known, "drift: known on a shifted grid");
    near(moved.drift.ms, 0.0, 1e-9, "drift: a shift alone is none");
    near(moved.drift.startMs, -12.5, 1e-9, "drift: a shift at the start");
    near(moved.drift.endMs, -12.5, 1e-9, "drift: the same shift at the end");

    // A few beats thrown far off do not move it.
    auto stray = fast;
    for (const std::size_t i : {5u, 60u, 300u, 700u, 760u}) {
        stray[i].timeMs += 40.0;
    }
    near(compareBeatGrids(stray, reference).drift.ms, expected, 0.2, "drift: stray beats do not move it");

    // Half the tempo, or too few beats: not said.
    check(!compareBeatGrids(constantGrid(100.0, 64.0, 384), reference).drift.known, "drift: unknown at half tempo");
    check(!compareBeatGrids(constantGrid(100.0, 128.0, 9), constantGrid(100.0, 128.0, 9)).drift.known,
          "drift: unknown on nine beats");
}

int main()
{
    drift();
    identical();
    shifts();
    scatter();
    tempoRelations();
    barTurns();
    shapes();
    emptyAndOne();
    cover();
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "beat_grid_comparison_test: all checks passed\n";
    return 0;
}
