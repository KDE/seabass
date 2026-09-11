#include <cassert>
#include <cmath>
#include <iostream>

#include "domain/stick_performance.hpp"

using namespace seabass::domain;

namespace
{

StickPerformanceMeasurement referenceLike()
{
    const auto ref = ReferenceStick{}.drive;
    StickPerformanceMeasurement m;
    m.streamingBytesPerSecond = ref.streamingBytesPerSecond;
    m.randomReadMedianMs = ref.randomReadMs;
    m.randomReadP95Ms = ref.randomReadMs * 1.5;
    m.randomReads = 300;
    m.smallFileMedianMs = ref.smallFileMs;
    m.smallFileOpensPerSecond = 1000.0 / ref.smallFileMs;
    m.smallFilesRead = 150;
    m.catalogBytes = 1437696;
    return m;
}

// WHALESHARK2 on 2026-09-11, measured read-only: a healthy 2012 stick on
// a USB 2.0 port.
StickPerformanceMeasurement healthyOldStick()
{
    StickPerformanceMeasurement m;
    m.streamingBytesPerSecond = 40.0e6;
    m.randomReadMedianMs = 1.08;
    m.randomReadP95Ms = 1.43;
    m.randomReads = 300;
    m.smallFileMedianMs = 1.11;
    m.smallFileOpensPerSecond = 858;
    m.smallFilesRead = 150;
    m.catalogBytes = 1437696 + 3571712;
    return m;
}

StickPerformanceMeasurement wornStick()
{
    StickPerformanceMeasurement m;
    m.streamingBytesPerSecond = 9.4e6;
    m.randomReadMedianMs = 14.0;
    m.randomReadP95Ms = 30.0;
    m.randomReads = 300;
    m.smallFileMedianMs = 18.0;
    m.smallFileOpensPerSecond = 55;
    m.smallFilesRead = 150;
    m.catalogBytes = 9800000;
    return m;
}

}  // namespace

int main()
{
    // Case 1: the reference stick scores exactly 100 and reads as Fast.
    {
        auto s = scoreDjWorkload(referenceLike());
        assert(s.score == 100);
        assert(s.browseScore == 100 && s.trackLoadScore == 100 && s.mountScore == 100);
        assert(s.speedClass == SpeedClass::Fast);
        assert(std::fabs(s.setWaitSeconds - s.referenceSetWaitSeconds) < 1e-9);
        assert(s.browseVerdict == Verdict::Fine && s.trackLoadVerdict == Verdict::Fine && s.mountVerdict == Verdict::Fine);
        assert(s.streamingVerdict == Verdict::Fine);
        std::cout << "case 1 (reference stick = 100, Fast, all fine) OK\n";
    }

    // Case 2: a healthy 2012 stick is Average by today's standards and
    // still fine to play on: every verdict Fine, every advisory Fine.
    {
        auto m = healthyOldStick();
        auto s = scoreDjWorkload(m);
        assert(s.score > 35 && s.score < 80);
        assert(s.speedClass == SpeedClass::Average || s.speedClass == SpeedClass::Slow);
        assert(s.browseVerdict == Verdict::Fine);
        assert(s.trackLoadVerdict == Verdict::Fine);
        assert(s.mountVerdict == Verdict::Fine);
        assert(s.streamingVerdict == Verdict::Fine);
        assert(s.setWaitSeconds > 3.0 && s.setWaitSeconds < 15.0);
        auto rows = advisePlayers(m, s);
        assert(rows.size() == 3);
        for (const auto &row : rows) {
            assert(row.verdict == Verdict::Fine);
            assert(!row.summary.empty() && row.summary.back() == '.');
        }
        assert(describeSetWait(s).rfind("About ", 0) == 0);
        std::cout << "case 2 (healthy old stick: mid score, all verdicts fine; score " << s.score << ") OK\n";
    }

    // Case 3: a worn stick is Very slow, sluggish everywhere, and the
    // advisories say so.
    {
        auto m = wornStick();
        auto s = scoreDjWorkload(m);
        assert(s.score < 20);
        assert(s.speedClass == SpeedClass::VerySlow);
        assert(s.browseVerdict == Verdict::Sluggish);
        // 3 x 18 ms of file opens plus 2 MiB at 9.4 MB/s is a quarter
        // second per load: a pause, not yet a wait.
        assert(s.trackLoadVerdict == Verdict::Slower);
        // 500 pages at 14 ms plus a 9.8 MB database at 9.4 MB/s is eight
        // seconds of "Loading": slower, and one more step from a wait.
        assert(s.mountVerdict == Verdict::Slower);
        assert(s.streamingVerdict == Verdict::Slower || s.streamingVerdict == Verdict::Sluggish);
        auto rows = advisePlayers(m, s);
        for (const auto &row : rows) {
            assert(row.verdict == Verdict::Sluggish);
        }
        std::cout << "case 3 (worn stick: very slow, sluggish; score " << s.score << ") OK\n";
    }

    // Case 4: nothing measured -> nothing claimed.
    {
        auto s = scoreDjWorkload(StickPerformanceMeasurement{});
        assert(s.score == 0);
        assert(s.speedClass == SpeedClass::Unknown);
        assert(s.browseVerdict == Verdict::Unknown && s.trackLoadVerdict == Verdict::Unknown &&
               s.mountVerdict == Verdict::Unknown && s.streamingVerdict == Verdict::Unknown);
        assert(describeSetWait(s) == "Nothing measured yet.");
        auto rows = advisePlayers(StickPerformanceMeasurement{}, s);
        for (const auto &row : rows) {
            assert(row.verdict == Verdict::Unknown);
        }
        assert(speedClassLabel(SpeedClass::Unknown) == "Not measured");
        std::cout << "case 4 (unmeasured stick claims nothing) OK\n";
    }

    // Case 5: a partial measurement compares like with like: only the
    // streaming number, and the score is that dimension alone.
    {
        StickPerformanceMeasurement m;
        m.streamingBytesPerSecond = 20.0e6;
        auto s = scoreDjWorkload(m);
        assert(s.browseVerdict == Verdict::Unknown);
        assert(s.trackLoadVerdict == Verdict::Fine || s.trackLoadVerdict == Verdict::Slower);
        assert(s.score == 50);  // half the reference streaming rate, nothing else measured
        std::cout << "case 5 (partial measurement scores only what was measured) OK\n";
    }

    // Case 6: slower is lower, monotonically, on each dimension.
    {
        auto base = healthyOldStick();
        auto slowerRandom = base;
        slowerRandom.randomReadMedianMs *= 2;
        auto slowerSmall = base;
        slowerSmall.smallFileMedianMs *= 2;
        auto slowerStream = base;
        slowerStream.streamingBytesPerSecond /= 2;
        int baseScore = scoreDjWorkload(base).score;
        assert(scoreDjWorkload(slowerRandom).score < baseScore);
        assert(scoreDjWorkload(slowerSmall).score < baseScore);
        assert(scoreDjWorkload(slowerStream).score < baseScore);
        std::cout << "case 6 (monotonic in every dimension) OK\n";
    }

    // Case 7: speed classes at their edges.
    {
        assert(speedClassFor(120) == SpeedClass::VeryFast);
        assert(speedClassFor(119) == SpeedClass::Fast);
        assert(speedClassFor(80) == SpeedClass::Fast);
        assert(speedClassFor(79) == SpeedClass::Average);
        assert(speedClassFor(45) == SpeedClass::Average);
        assert(speedClassFor(44) == SpeedClass::Slow);
        assert(speedClassFor(20) == SpeedClass::Slow);
        assert(speedClassFor(19) == SpeedClass::VerySlow);
        assert(speedClassFor(0) == SpeedClass::Unknown);
        assert(speedClassLabel(SpeedClass::VeryFast) == "Very fast");
        assert(speedClassLabel(SpeedClass::VerySlow) == "Very slow");
        std::cout << "case 7 (speed class edges) OK\n";
    }

    // Case 8: write workloads. A current stick saves cues without a
    // noticeable pause and exports fast; a worn one does neither.
    {
        StickWriteMeasurement fast;
        fast.streamingWriteBytesPerSecond = 25.0e6;
        fast.smallFileWriteMedianMs = 8.0;
        fast.smallFilesWritten = 100;
        fast.smallFileWritesPerSecond = 120;
        fast.inPlaceUpdateMedianMs = 6.0;
        fast.inPlaceUpdates = 100;
        auto e = estimateWriteWorkloads(fast);
        assert(e.cueSaveVerdict == Verdict::Fine);
        assert(e.exportVerdict == Verdict::Fine);
        assert(e.cueSaveSeconds > 0.0 && e.cueSaveSeconds < 0.3);
        assert(std::fabs(e.exportHundredTracksSeconds - 100 * e.exportTrackSeconds) < 1e-6);

        StickWriteMeasurement worn;
        worn.streamingWriteBytesPerSecond = 3.0e6;
        worn.smallFileWriteMedianMs = 250.0;
        worn.smallFilesWritten = 100;
        worn.smallFileWritesPerSecond = 4;
        worn.inPlaceUpdateMedianMs = 200.0;
        worn.inPlaceUpdates = 100;
        auto w = estimateWriteWorkloads(worn);
        assert(w.cueSaveVerdict == Verdict::Sluggish);
        assert(w.exportVerdict == Verdict::Sluggish);

        auto none = estimateWriteWorkloads(StickWriteMeasurement{});
        assert(none.cueSaveVerdict == Verdict::Unknown && none.exportVerdict == Verdict::Unknown);
        std::cout << "case 8 (write workload estimates) OK\n";
    }

    std::cout << "All stick_performance tests passed.\n";
    return 0;
}
