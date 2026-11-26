// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the analysis ledger says about a track today
// (src/infrastructure/local/analysis_ledger_status.hpp): every state, in
// the order the rules are asked, and the rules a wrong answer would be
// costly for: "-" on either side is never a grid changed by hand, a
// different modification time beside the same size is never a changed
// file, and a grid changed since wins over everything that would offer
// the track for analysis again.

#include <iostream>
#include <string>

#include "infrastructure/local/analysis_ledger_status.hpp"

using namespace seabass::infrastructure::local;

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

const std::string GridA(64, 'a');
const std::string GridB(64, 'b');
const std::string GridE(64, 'e');
const std::string GridF(64, 'f');

AnalysisLedgerEntry recorded()
{
    AnalysisLedgerEntry e;
    e.pathKey = "contents/a.wav";
    e.fileSize = 1000;
    e.fileMtimeUnix = 5000;
    e.inputRevision = 1;
    e.gridRevision = 2;
    e.keyRevision = 1;
    e.waveformRevision = 1;
    e.tempoMin = "70.00";
    e.tempoMax = "180.00";
    e.rekordboxGrid = GridA;
    e.engineGrid = GridE;
    return e;
}

CurrentAnalyser today()
{
    CurrentAnalyser now;
    now.inputRevision = 1;
    now.gridRevision = 2;
    now.keyRevision = 1;
    now.waveformRevision = 1;
    now.tempoMin = "70.00";
    now.tempoMax = "180.00";
    return now;
}

TrackNow asLeft()
{
    TrackNow track;
    track.fileSize = 1000;
    track.fileMtimeUnix = 5000;
    track.rekordboxGrid = GridA;
    track.engineGrid = GridE;
    return track;
}

const char *nameOf(AnalysisState state)
{
    switch (state) {
    case AnalysisState::Current: return "Current";
    case AnalysisState::OlderAnalysis: return "OlderAnalysis";
    case AnalysisState::FileChanged: return "FileChanged";
    case AnalysisState::GridChangedSince: return "GridChangedSince";
    case AnalysisState::NotOurs: return "NotOurs";
    }
    return "?";
}

void expect(const AnalysisStatus &status, AnalysisState state, const std::string &what)
{
    check(status.state == state, what + ": " + nameOf(state) + " expected, got " + nameOf(status.state));
}

}  // namespace

int main()
{
    const AnalysisLedgerEntry entry = recorded();

    // 1. No entry: not ours, whatever the track is like.
    expect(classifyAnalysis(nullptr, today(), asLeft()), AnalysisState::NotOurs, "no entry");
    std::cout << "case 1 (NotOurs) done\n";

    // 2. As Seabass left it, today's analyser: current.
    {
        const AnalysisStatus status = classifyAnalysis(&entry, today(), asLeft());
        expect(status, AnalysisState::Current, "as left");
        check(!status.inputStale && !status.gridStale && !status.keyStale && !status.waveformStale && !status.rangeStale
                  && !status.mtimeDiffers && !status.fileMissing,
              "as left: nothing stale, no hint");
    }
    std::cout << "case 2 (Current) done\n";

    // 3. Older analyses: each part on its own, and the range.
    {
        const auto older = [&](auto change, const char *what, auto flag) {
            CurrentAnalyser now = today();
            change(now);
            const AnalysisStatus status = classifyAnalysis(&entry, now, asLeft());
            expect(status, AnalysisState::OlderAnalysis, what);
            check(flag(status), std::string(what) + ": its flag set");
            check(behindRevisions(entry, now) == (std::string(what) != "range"),
                  std::string(what) + ": behindRevisions counts revisions, never the range");
        };
        older([](CurrentAnalyser &n) { n.inputRevision = 2; }, "input", [](const AnalysisStatus &s) { return s.inputStale && !s.gridStale; });
        older([](CurrentAnalyser &n) { n.gridRevision = 3; }, "grid", [](const AnalysisStatus &s) { return s.gridStale && !s.keyStale; });
        older([](CurrentAnalyser &n) { n.keyRevision = 2; }, "key", [](const AnalysisStatus &s) { return s.keyStale && !s.waveformStale; });
        older([](CurrentAnalyser &n) { n.waveformRevision = 2; }, "waveform", [](const AnalysisStatus &s) { return s.waveformStale && !s.inputStale; });
        older([](CurrentAnalyser &n) { n.tempoMin = "88.00"; n.tempoMax = "175.00"; }, "range",
              [](const AnalysisStatus &s) { return s.rangeStale && !s.gridStale; });
        // The range compared as text: "70.00" is not "70.0".
        CurrentAnalyser now = today();
        now.tempoMin = "70.0";
        expect(classifyAnalysis(&entry, now, asLeft()), AnalysisState::OlderAnalysis, "the range compared as its text");
        // A newer Seabass wrote the entry: nothing of today's is newer.
        CurrentAnalyser behind = today();
        behind.gridRevision = 1;
        expect(classifyAnalysis(&entry, behind, asLeft()), AnalysisState::Current, "an entry newer than today's analyser");
        check(!behindRevisions(entry, behind), "and not behind");
    }
    std::cout << "case 3 (OlderAnalysis, each part and the range) done\n";

    // 4. The file: size decides, the time is a hint.
    {
        TrackNow other = asLeft();
        other.fileSize = 1001;
        expect(classifyAnalysis(&entry, today(), other), AnalysisState::FileChanged, "another size");
        TrackNow gone = asLeft();
        gone.fileSize.reset();
        gone.fileMtimeUnix.reset();
        const AnalysisStatus missing = classifyAnalysis(&entry, today(), gone);
        expect(missing, AnalysisState::FileChanged, "no file");
        check(missing.fileMissing, "no file: fileMissing");
        // The same size, another time: two seconds of FAT, an hour of
        // summer time, a day of anything: never FileChanged on its own.
        for (const std::int64_t shift : {2, -2, 3600, -3600, 86400}) {
            TrackNow touched = asLeft();
            touched.fileMtimeUnix = 5000 + shift;
            const AnalysisStatus status = classifyAnalysis(&entry, today(), touched);
            expect(status, AnalysisState::Current, "the same size, the time moved by " + std::to_string(shift));
            check(status.mtimeDiffers, "and the hint is given");
        }
        TrackNow unknownTime = asLeft();
        unknownTime.fileMtimeUnix.reset();
        const AnalysisStatus noTime = classifyAnalysis(&entry, today(), unknownTime);
        expect(noTime, AnalysisState::Current, "the time unknown");
        check(!noTime.mtimeDiffers, "the time unknown: no hint");
        // Another size and an older analyser: the file is what is said.
        CurrentAnalyser newer = today();
        newer.gridRevision = 9;
        expect(classifyAnalysis(&entry, newer, other), AnalysisState::FileChanged, "another size beats an older analysis");
    }
    std::cout << "case 4 (FileChanged; the time is a hint only) done\n";

    // 5. A grid changed since, in either library, wins over everything.
    {
        TrackNow rekordbox = asLeft();
        rekordbox.rekordboxGrid = GridB;
        const AnalysisStatus r = classifyAnalysis(&entry, today(), rekordbox);
        expect(r, AnalysisState::GridChangedSince, "rekordbox's grid changed");
        check(r.rekordboxGridChanged && !r.engineGridChanged, "rekordbox's flag only");
        TrackNow engine = asLeft();
        engine.engineGrid = GridF;
        const AnalysisStatus e = classifyAnalysis(&entry, today(), engine);
        expect(e, AnalysisState::GridChangedSince, "Engine's grid changed");
        check(e.engineGridChanged && !e.rekordboxGridChanged, "Engine's flag only");
        // Over an older analyser and another file alike.
        CurrentAnalyser newer = today();
        newer.gridRevision = 9;
        TrackNow both = engine;
        both.fileSize = 1;
        expect(classifyAnalysis(&entry, newer, both), AnalysisState::GridChangedSince,
               "a changed grid beats another size and an older analysis");
    }
    std::cout << "case 5 (GridChangedSince) done\n";

    // 6. "-" is no evidence, on either side.
    {
        // Recorded "-": a grid there now is not a change.
        AnalysisLedgerEntry none = recorded();
        none.rekordboxGrid.clear();
        none.engineGrid.clear();
        TrackNow grids = asLeft();
        grids.rekordboxGrid = GridB;
        grids.engineGrid = GridF;
        expect(classifyAnalysis(&none, today(), grids), AnalysisState::Current, "recorded \"-\", a grid now");
        CurrentAnalyser newer = today();
        newer.gridRevision = 9;
        expect(classifyAnalysis(&none, newer, grids), AnalysisState::OlderAnalysis,
               "recorded \"-\", a grid now, an older analyser: older, not changed");
        // "-" now (a library gone, a row that cannot be read): not a change.
        TrackNow noGrids = asLeft();
        noGrids.rekordboxGrid.clear();
        noGrids.engineGrid.clear();
        expect(classifyAnalysis(&entry, today(), noGrids), AnalysisState::Current, "a grid recorded, \"-\" now");
        // One library "-", the other changed: changed.
        TrackNow half = asLeft();
        half.rekordboxGrid.clear();
        half.engineGrid = GridF;
        expect(classifyAnalysis(&entry, today(), half), AnalysisState::GridChangedSince,
               "one side \"-\", the other changed");
    }
    std::cout << "case 6 (\"-\" is never a change) done\n";

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "analysis_ledger_status_test: all passed\n";
    return 0;
}
