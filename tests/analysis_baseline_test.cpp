// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The phase 2 baseline (tools/analysis_baseline, built with
// -DSEABASS_ANALYSIS_BASELINE=ON): Mixxx's ironing on beat lists made by
// hand, whose answer follows from Mixxx's rules; the three beat
// analysers on generated tracks, with bounds the method's own resolution
// sets (its step is 512 frames at 44.1 kHz, 11.6 ms); and the key
// detector on the generated tracks whose key it names correctly. It
// names the relative key of most of the others, which is reported here
// and not asserted.

#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "analysis_baseline/baseline_analysers.hpp"
#include "analysis_baseline/mixxx_beatutils.hpp"
#include "analysis_baseline/mixxx_qm_plugins.hpp"
#include "domain/beat_grid_comparison.hpp"
#include "synth_track/synth_track.hpp"

using namespace seabass;
namespace mixxx = seabass::baseline::mixxx;

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

constexpr double Rate = 44100.0;
// The tracker's step at 44.1 kHz and where in it Mixxx puts a beat.
constexpr double Step = 512.0;

// Beats of a constant tempo from `firstFrame`, each moved to where the
// tracker could report it: a whole step plus half a step.
std::vector<double> trackerBeats(double firstFrame, double bpm, int count)
{
    std::vector<double> beats;
    const double length = 60.0 * Rate / bpm;
    for (int k = 0; k < count; ++k) {
        const double frame = firstFrame + k * length;
        beats.push_back(std::floor(frame / Step) * Step + Step / 2);
    }
    return beats;
}

// --- The ironing ---

void rounding()
{
    // A whole BPM inside the band wins.
    near(mixxx::roundBpmWithinRange(123.9, 124.02, 124.1), 124.0, 1e-12, "a whole BPM in the band");
    // Under 85 BPM a half BPM is tried before a third.
    near(mixxx::roundBpmWithinRange(84.4, 84.52, 84.6), 84.5, 1e-12, "a half BPM under 85");
    // Over 127 BPM a value on two thirds comes before a third: 142.5 is
    // 95 times three halves, while 142.333 would be the third.
    near(mixxx::roundBpmWithinRange(142.3, 142.4, 142.6), 142.5, 1e-12, "two thirds over 127");
    near(mixxx::roundBpmWithinRange(127.30, 127.34, 127.38), 382.0 / 3.0, 1e-12, "a third of a BPM");
    near(mixxx::roundBpmWithinRange(130.55, 130.6, 130.65), 1567.0 / 12.0, 1e-12, "a twelfth of a BPM");
    // Nothing fits a narrow band around 137.98: the measured tempo stays.
    near(mixxx::roundBpmWithinRange(137.97, 137.98, 137.99), 137.98, 1e-12, "no snap fits: the centre stays");
    // The band is open: a snap on its edge does not count.
    check(!mixxx::trySnap(124.0, 124.05, 124.1, 1.0), "a snap on the band's lower edge is refused");
}

void constantRegions()
{
    // 120 BPM from frame 1000, evenly spaced: one region and the closing
    // marker.
    std::vector<double> even;
    for (int k = 0; k < 96; ++k) {
        even.push_back(1000.0 + k * 22050.0);
    }
    const std::vector<mixxx::ConstRegion> regions = mixxx::retrieveConstRegions(even, Rate);
    check(regions.size() == 2, "an even list is one region and the end marker, got " + std::to_string(regions.size()));
    if (regions.size() == 2) {
        near(regions[0].firstBeat, 1000.0, 1e-9, "the region starts at the first beat");
        near(regions[0].beatLength, 22050.0, 1e-9, "the region's beat length");
        near(regions[1].beatLength, 0.0, 0.0, "the end marker has no length");
        near(regions[1].firstBeat, even.back(), 1e-9, "the end marker sits on the last beat");
    }

    // One beat 30 ms out is an outlier Mixxx allows; two split the list.
    std::vector<double> oneOutlier = even;
    oneOutlier[40] += 0.030 * Rate;
    check(mixxx::retrieveConstRegions(oneOutlier, Rate).size() == 2, "one outlier keeps one region");
    std::vector<double> twoOutliers = oneOutlier;
    twoOutliers[60] += 0.030 * Rate;
    check(mixxx::retrieveConstRegions(twoOutliers, Rate).size() > 2, "two outliers split the region");
    // Beats that drift 1 ms a beat stay within 25 ms of a straight line
    // through them for a while, but their errors add up past 100 ms: the
    // list is cut.
    std::vector<double> drifting;
    for (int k = 0; k < 96; ++k) {
        drifting.push_back(1000.0 + k * 22050.0 + 0.001 * Rate * k * k / 95.0);
    }
    check(mixxx::retrieveConstRegions(drifting, Rate).size() > 2, "errors that add up cut the region");

    // The same tempo as the tracker reports it, every beat on a step: the
    // quantising cuts the list into several regions (Mixxx's errors add
    // up over a long region), yet the grid is 120 BPM exactly, its anchor
    // folded to within one beat of the start and moved by the mean phase
    // error to within half a step of the true 1000.
    const std::vector<double> beats = trackerBeats(1000.0, 120.0, 96);
    double bpm = 0.0;
    const std::vector<domain::Beat> grid = baseline::mixxxConstantGrid(beats, static_cast<int>(Rate), 96 * 22050.0 + 5000.0, true, &bpm);
    near(bpm, 120.0, 0.0, "the grid's tempo");
    check(!grid.empty(), "the grid has beats");
    if (!grid.empty()) {
        near(grid.front().timeMs, 1000.0 / Rate * 1000.0, Step / 2 / Rate * 1000.0, "the grid's first beat");
        near(grid[1].timeMs - grid[0].timeMs, 500.0, 1e-9, "the grid's beat length");
        check(grid.back().timeMs < (96 * 22050.0 + 5000.0) / Rate * 1000.0, "the grid ends inside the track");
        check(grid.front().beatInBar == 0, "Mixxx finds no downbeat");
    }
}

void stepTempo()
{
    // 48 beats at 120 BPM, then 80 at 126 starting on the next beat: the
    // longer stretch decides the constant tempo, and it is a whole BPM.
    std::vector<double> beats = trackerBeats(2000.0, 120.0, 48);
    const double stepStart = 2000.0 + 48 * 22050.0;
    for (const double frame : trackerBeats(stepStart, 126.0, 80)) {
        beats.push_back(frame);
    }
    const auto regions = mixxx::retrieveConstRegions(beats, Rate);
    check(regions.size() >= 3, "a step in tempo makes at least two regions, got " + std::to_string(regions.size()));
    const auto grid = mixxx::makeConstGrid(beats, Rate, true);
    check(grid.has_value(), "a grid for the stepped list");
    if (grid) {
        near(grid->bpm, 126.0, 0.0, "the longer stretch's tempo");
    }
    // Unrounded: the same stretch's tempo as measured, near 126 but not
    // forced onto it.
    const auto unrounded = mixxx::makeConstGrid(beats, Rate, false);
    check(unrounded.has_value(), "an unrounded grid for the stepped list");
    if (unrounded) {
        near(unrounded->bpm, 126.0, 0.05, "the unrounded tempo");
    }
}

void noTempo()
{
    check(!mixxx::makeConstGrid({}, Rate, true), "no beats, no grid");
    check(!mixxx::makeConstGrid({1000.0}, Rate, true), "one beat, no grid");
    check(baseline::mixxxConstantGrid({}, 44100, 1e6, true).empty(), "no beats, no grid beats");
}

void globalKey()
{
    check(mixxx::calculateGlobalKey({{7, 0.0}}, 1000.0) == 7, "one key is the key");
    check(mixxx::calculateGlobalKey({{10, 0.0}, {22, 1000.0}, {10, 5000.0}}, 6000.0) == 22,
          "the key that holds longest, counted over every stretch");
    check(mixxx::calculateGlobalKey({{5, 0.0}, {3, 100.0}}, 200.0) == 3, "a tie goes to the lower key number");
    check(mixxx::calculateGlobalKey({}, 200.0) == 0, "no changes, no key");
    check(baseline::chromaticKeyName(1) == "C" && baseline::chromaticKeyName(10) == "A"
              && baseline::chromaticKeyName(22) == "Am" && baseline::chromaticKeyName(14) == "Dbm"
              && baseline::chromaticKeyName(0).empty() && baseline::chromaticKeyName(25).empty(),
          "Mixxx's key numbers as names");
}

// --- The analysers on generated tracks ---

struct Expect
{
    const char *preset;
    const char *analyser;
    domain::GridShapeKind shape;
    // The tempo the constant grid must have exactly, 0 for no such check.
    double exactBpm;
    double minHit20;
};

void analysers()
{
    // One step of the tracker: a constant grid fitted through its beats
    // has no business being further than that from the truth.
    constexpr double StepMs = 512.0 / 44100.0 * 1000.0;
    const std::vector<Expect> expectations = {
        {"house-124", "qm-raw", domain::GridShapeKind::Moving, 0.0, 0.80},
        {"house-124", "mixxx-constant", domain::GridShapeKind::Constant, 124.0, 0.95},
        {"house-124", "mixxx-constant-unrounded", domain::GridShapeKind::Constant, 0.0, 0.95},
        {"house-124-48k", "qm-raw", domain::GridShapeKind::Moving, 0.0, 0.80},
        {"house-124-48k", "mixxx-constant", domain::GridShapeKind::Constant, 124.0, 0.95},
        {"house-124-48k", "mixxx-constant-unrounded", domain::GridShapeKind::Constant, 0.0, 0.95},
        {"breakbeat-swing-132", "mixxx-constant", domain::GridShapeKind::Constant, 132.0, 0.95},
        {"long-breakdown-126", "mixxx-constant", domain::GridShapeKind::Constant, 126.0, 0.95},
    };
    std::string renderedName;
    synth::SynthTrack track;
    for (const Expect &e : expectations) {
        if (renderedName != e.preset) {
            track = synth::renderSynthTrack(*synth::synthPreset(e.preset));
            renderedName = e.preset;
        }
        const auto analyser = baseline::makeBaselineGridAnalyser(e.analyser);
        check(analyser && analyser->name() == e.analyser, std::string("the analyser ") + e.analyser + " exists");
        if (!analyser) {
            continue;
        }
        domain::InMemoryPcmSource source(track.samples, track.sampleRate);
        const domain::GridAnalysis analysis = analyser->analyse(source);
        const std::string what = std::string(e.preset) + ", " + e.analyser;
        check(analysis.error.empty(), what + ": ran (" + analysis.error + ")");
        const domain::BeatGridComparison c = domain::compareBeatGrids(analysis.beats, track.beats);
        check(c.tempo == domain::TempoRelation::Same,
              what + ": the same tempo, got " + domain::tempoRelationName(c.tempo));
        near(c.offset.medianMs, 0.0, StepMs, what + ": median offset within one step");
        check(c.hits.within20Ms >= e.minHit20, what + ": beats within 20 ms, got " + std::to_string(c.hits.within20Ms));
        check(c.candidateShape.kind == e.shape,
              what + ": shape " + domain::gridShapeKindName(c.candidateShape.kind));
        if (e.exactBpm > 0.0) {
            near(c.candidateShape.bpm, e.exactBpm, 1e-6, what + ": the rounded tempo");
        }
        bool noBar = true;
        for (const domain::Beat &beat : analysis.beats) {
            noBar = noBar && beat.beatInBar == 0;
        }
        check(noBar, what + ": no place in the bar");
    }
}

void failingSource()
{
    // A source that fails is reported, not passed off as a track without
    // beats.
    domain::InMemoryPcmSource broken({0.0f, 0.0f}, 0);
    for (const std::string &name : baseline::baselineGridAnalyserNames()) {
        domain::InMemoryPcmSource source({0.0f, 0.0f}, 0);
        check(!baseline::makeBaselineGridAnalyser(name)->analyse(source).error.empty(), name + ": a failed source is an error");
    }
    check(!baseline::makeBaselineKeyAnalyser("qm-key")->analyse(broken).error.empty(), "qm-key: a failed source is an error");
}

void keys()
{
    // The presets whose key the detector names correctly. On the others
    // (every minor preset, and the one in E major) it names the relative
    // key; that is printed, not asserted.
    const std::vector<std::pair<const char *, const char *>> correct = {{"ramp-100-125", "D"}, {"drumless-intro-128", "G"}};
    for (const auto &[preset, expected] : correct) {
        const synth::SynthTrack track = synth::renderSynthTrack(*synth::synthPreset(preset));
        domain::InMemoryPcmSource source(track.samples, track.sampleRate);
        const baseline::KeyAnalysis key = baseline::makeBaselineKeyAnalyser("qm-key")->analyse(source);
        check(key.error.empty(), std::string(preset) + ": the key detector ran");
        check(key.key == expected, std::string(preset) + ": key " + key.key + ", expected " + expected);
    }
    const synth::SynthTrack house = synth::renderSynthTrack(*synth::synthPreset("house-124"));
    domain::InMemoryPcmSource source(house.samples, house.sampleRate);
    std::cout << "house-124 (A minor): qm-key says " << baseline::makeBaselineKeyAnalyser("qm-key")->analyse(source).key
              << "\n";
}

}  // namespace

int main()
{
    rounding();
    constantRegions();
    stepTempo();
    noTempo();
    globalKey();
    analysers();
    failingSource();
    keys();
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "analysis_baseline_test: all checks passed\n";
    return 0;
}
