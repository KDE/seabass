#include "domain/stick_performance.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace seabass::domain
{

namespace
{

// Perceptual thresholds per action, seconds. "Fine" is below what a
// person notices as a pause on a button press (about 100 ms), with the
// browse budget tighter because a scroll fires many actions in a row.
// Provisional: calibrated against one healthy 2012 stick and the
// numbers in the design note, not yet against a known-bad stick.
constexpr double kBrowseFineSeconds = 0.040;
constexpr double kBrowseSlowerSeconds = 0.120;
constexpr double kTrackLoadFineSeconds = 0.100;
constexpr double kTrackLoadSlowerSeconds = 0.300;
constexpr double kMountFineSeconds = 2.0;
constexpr double kMountSlowerSeconds = 10.0;
// Denon's published minimum for Engine OS media is 20 MB/s read; below
// 8 MB/s the on-player migration and every export drags.
constexpr double kStreamingFineBytesPerSecond = 20.0 * 1000 * 1000;
constexpr double kStreamingSlowerBytesPerSecond = 8.0 * 1000 * 1000;

Verdict verdictFor(double value, double fine, double slower)
{
    if (value <= fine) {
        return Verdict::Fine;
    }
    if (value <= slower) {
        return Verdict::Slower;
    }
    return Verdict::Sluggish;
}

Verdict worst(Verdict a, Verdict b)
{
    if (a == Verdict::Unknown) {
        return b;
    }
    if (b == Verdict::Unknown) {
        return a;
    }
    return std::max(a, b);
}

int relativeScore(double referenceSeconds, double measuredSeconds)
{
    if (measuredSeconds <= 0.0 || referenceSeconds <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::lround(100.0 * referenceSeconds / measuredSeconds));
}

double streamSeconds(std::uint64_t bytes, double bytesPerSecond)
{
    return bytesPerSecond > 0.0 ? static_cast<double>(bytes) / bytesPerSecond : 0.0;
}

std::string roundedSeconds(double seconds)
{
    std::ostringstream out;
    if (seconds < 10.0) {
        out.precision(1);
        out << std::fixed << seconds << " s";
    } else if (seconds < 120.0) {
        out << static_cast<int>(std::lround(seconds)) << " s";
    } else {
        out << static_cast<int>(std::lround(seconds / 60.0)) << " min";
    }
    return out.str();
}

}  // namespace

std::string verdictLabel(Verdict verdict)
{
    switch (verdict) {
    case Verdict::Fine:
        return "FINE";
    case Verdict::Slower:
        return "SLOWER";
    case Verdict::Sluggish:
        return "SLUGGISH";
    case Verdict::Unknown:
        break;
    }
    return "NOT MEASURED";
}

SpeedClass speedClassFor(int score)
{
    if (score <= 0) {
        return SpeedClass::Unknown;
    }
    if (score >= 120) {
        return SpeedClass::VeryFast;
    }
    if (score >= 80) {
        return SpeedClass::Fast;
    }
    if (score >= 45) {
        return SpeedClass::Average;
    }
    if (score >= 20) {
        return SpeedClass::Slow;
    }
    return SpeedClass::VerySlow;
}

std::string speedClassLabel(SpeedClass speedClass)
{
    switch (speedClass) {
    case SpeedClass::VeryFast:
        return "Very fast";
    case SpeedClass::Fast:
        return "Fast";
    case SpeedClass::Average:
        return "Average";
    case SpeedClass::Slow:
        return "Slow";
    case SpeedClass::VerySlow:
        return "Very slow";
    case SpeedClass::Unknown:
        break;
    }
    return "Not measured";
}

DjWorkloadScore scoreDjWorkload(const StickPerformanceMeasurement &m, const DjWorkloadModel &model,
                                const ReferenceStick &reference)
{
    DjWorkloadScore s;
    const bool haveRandom = m.randomReads > 0 && m.randomReadMedianMs > 0.0;
    const bool haveSmall = m.smallFilesRead > 0 && m.smallFileMedianMs > 0.0;
    const bool haveStreaming = m.streamingBytesPerSecond > 0.0;

    // Per-action waits on this stick and on the reference stick. A
    // dimension that was not measured contributes nothing on either side,
    // so the score compares like with like instead of punishing a probe
    // that could not run one part.
    double browse = 0.0, refBrowse = 0.0;
    if (haveRandom) {
        browse = model.pageReadsPerBrowseAction * m.randomReadMedianMs / 1000.0;
        refBrowse = model.pageReadsPerBrowseAction * reference.randomReadMs / 1000.0;
        s.browseVerdict = verdictFor(browse, kBrowseFineSeconds, kBrowseSlowerSeconds);
    }

    double load = 0.0, refLoad = 0.0;
    if (haveSmall) {
        load += model.smallFilesPerTrackLoad * m.smallFileMedianMs / 1000.0;
        refLoad += model.smallFilesPerTrackLoad * reference.smallFileMs / 1000.0;
    }
    if (haveStreaming) {
        load += streamSeconds(model.streamedBytesPerTrackLoad, m.streamingBytesPerSecond);
        refLoad += streamSeconds(model.streamedBytesPerTrackLoad, reference.streamingBytesPerSecond);
        s.streamingVerdict = verdictFor(-m.streamingBytesPerSecond, -kStreamingFineBytesPerSecond,
                                        -kStreamingSlowerBytesPerSecond);
    }
    if (haveSmall || haveStreaming) {
        s.trackLoadVerdict = verdictFor(load, kTrackLoadFineSeconds, kTrackLoadSlowerSeconds);
    }

    double mount = 0.0, refMount = 0.0;
    if (haveRandom) {
        mount += model.pageReadsAtMount * m.randomReadMedianMs / 1000.0;
        refMount += model.pageReadsAtMount * reference.randomReadMs / 1000.0;
    }
    if (haveStreaming && m.databaseBytes > 0) {
        mount += streamSeconds(m.databaseBytes, m.streamingBytesPerSecond);
        refMount += streamSeconds(m.databaseBytes, reference.streamingBytesPerSecond);
    }
    if (haveRandom || (haveStreaming && m.databaseBytes > 0)) {
        s.mountVerdict = verdictFor(mount, kMountFineSeconds, kMountSlowerSeconds);
    }

    s.browseActionSeconds = browse;
    s.trackLoadSeconds = load;
    s.mountSeconds = mount;
    s.setWaitSeconds = model.browseActions * browse + model.trackLoads * load + mount;
    s.referenceSetWaitSeconds = model.browseActions * refBrowse + model.trackLoads * refLoad + refMount;

    s.browseScore = relativeScore(refBrowse, browse);
    s.trackLoadScore = relativeScore(refLoad, load);
    s.mountScore = relativeScore(refMount, mount);
    s.score = relativeScore(s.referenceSetWaitSeconds, s.setWaitSeconds);
    s.speedClass = speedClassFor(s.score);
    return s;
}

std::string describeSetWait(const DjWorkloadScore &score)
{
    if (score.setWaitSeconds <= 0.0) {
        return "Nothing measured yet.";
    }
    std::ostringstream out;
    out << "About " << roundedSeconds(score.setWaitSeconds) << " of waiting across a two-hour set";
    if (score.referenceSetWaitSeconds > 0.0) {
        out << "; a current stick on the same port would wait " << roundedSeconds(score.referenceSetWaitSeconds);
    }
    out << ".";
    return out.str();
}

std::vector<PlayerAdvisory> advisePlayers(const StickPerformanceMeasurement &m, const DjWorkloadScore &s)
{
    std::vector<PlayerAdvisory> rows;
    const std::string pageMs = [&] {
        std::ostringstream o;
        o.precision(1);
        o << std::fixed << m.randomReadMedianMs << " ms";
        return o.str();
    }();
    const std::string opensPerSecond = std::to_string(static_cast<int>(std::lround(m.smallFileOpensPerSecond)));
    const std::string streamingMBps = std::to_string(static_cast<int>(std::lround(m.streamingBytesPerSecond / 1e6)));

    // Device Library: export.pdb's 4 KiB pages read as you browse, one
    // analysis file per track load.
    {
        PlayerAdvisory row;
        row.group = "Device Library players";
        row.players = "CDJ-2000 · NXS · NXS2 · XDJ-1000MK2 · XDJ-XZ · XDJ-RX3 · CDJ-3000";
        row.verdict = worst(s.browseVerdict, s.trackLoadVerdict);
        switch (row.verdict) {
        case Verdict::Fine:
            row.summary = "Reads the database in 4 KiB pages straight off the stick as you browse, the same size as the "
                          "small random read, and one analysis file per track load. " + pageMs + " a page keeps up with the "
                          "jog wheel.";
            break;
        case Verdict::Slower:
            row.summary = "Every database page is a " + pageMs + " wait and a folder of thousands of tracks is hundreds "
                          "of pages. Expect a longer \"Loading\" at insertion and a short pause before each waveform appears.";
            break;
        case Verdict::Sluggish:
            row.summary = "Every database page is a " + pageMs + " wait. Expect a long \"Loading\" at insertion, browsing "
                          "that lags the jog wheel, and a visible pause before each waveform appears.";
            break;
        case Verdict::Unknown:
            row.summary = "Not measured.";
            break;
        }
        rows.push_back(row);
    }

    // OneLibrary: SQLite on the stick for every list and search.
    {
        PlayerAdvisory row;
        row.group = "OneLibrary players";
        row.players = "CDJ-3000X · OPUS-QUAD · OMNIS-DUO · XDJ-AZ · rekordbox 7";
        row.verdict = worst(s.browseVerdict, s.trackLoadVerdict);
        switch (row.verdict) {
        case Verdict::Fine:
            row.summary = "Runs every list, sort and search as a SQLite query on the stick. Small random reads decide how "
                          "snappy that feels; this stick answers in " + pageMs + ".";
            break;
        case Verdict::Slower:
            row.summary = "Search and sort run as SQLite queries on the stick as you type. At " + pageMs + " per read, "
                          "typing a search and scrolling a long playlist stutter.";
            break;
        case Verdict::Sluggish:
            row.summary = "Search and sort run as SQLite queries on the stick as you type. At " + pageMs + " per read "
                          "every keystroke and every scroll waits on the stick; this is the generation that suffers most.";
            break;
        case Verdict::Unknown:
            row.summary = "Not measured.";
            break;
        }
        rows.push_back(row);
    }

    // Engine OS: SQLite in place, plus Denon's 20 MB/s floor, which also
    // paces the on-player migration of an older library.
    {
        PlayerAdvisory row;
        row.group = "Engine OS players";
        row.players = "SC5000 · SC6000 · Prime 4 / 4+ · Prime 2 · Prime GO · SC Live";
        row.verdict = worst(s.browseVerdict, s.streamingVerdict);
        switch (row.verdict) {
        case Verdict::Fine:
            row.summary = "Reads its SQLite database in place, like OneLibrary. Denon asks for at least 20 MB/s and this "
                          "stick streams at " + streamingMBps + " MB/s. A library from an older Engine DJ is migrated on "
                          "the player at the stick's own speed.";
            break;
        case Verdict::Slower:
            row.summary = "Same SQLite-on-the-stick model. " + (s.streamingVerdict != Verdict::Fine
                              ? "Streaming at " + streamingMBps + " MB/s is under Denon's 20 MB/s minimum for Engine OS "
                                "media, so the one-time migration of an older library and every export take longer."
                              : "Browsing at " + pageMs + " per read is noticeably behind a current stick.");
            break;
        case Verdict::Sluggish:
            row.summary = "Same SQLite-on-the-stick model, and " + (s.streamingVerdict == Verdict::Sluggish
                              ? "streaming at " + streamingMBps + " MB/s is far under Denon's 20 MB/s minimum. "
                              : "reads of " + pageMs + " make every list wait. ") +
                          "Expect lag scrolling long playlists and the \"preparing library\" pass to run into minutes.";
            break;
        case Verdict::Unknown:
            row.summary = "Not measured.";
            break;
        }
        rows.push_back(row);
    }

    (void)opensPerSecond;
    return rows;
}

WriteWorkloadEstimate estimateWriteWorkloads(const StickWriteMeasurement &m)
{
    WriteWorkloadEstimate e;
    const bool haveSmall = m.smallFilesWritten > 0 && m.smallFileWriteMedianMs > 0.0;
    const bool haveInPlace = m.inPlaceUpdates > 0 && m.inPlaceUpdateMedianMs > 0.0;
    const bool haveStreaming = m.streamingWriteBytesPerSecond > 0.0;

    if (haveSmall || haveInPlace) {
        e.cueSaveSeconds = 2 * m.smallFileWriteMedianMs / 1000.0 + 4 * m.inPlaceUpdateMedianMs / 1000.0;
        // A save that takes longer than a third of a second is a pause a
        // person notices on the button; past a second it is a wait.
        e.cueSaveVerdict = verdictFor(e.cueSaveSeconds, 0.3, 1.0);
    }
    if (haveStreaming) {
        constexpr std::uint64_t kTrackBytes = 8u * 1000u * 1000u;
        e.exportTrackSeconds = streamSeconds(kTrackBytes, m.streamingWriteBytesPerSecond) +
                               3 * m.smallFileWriteMedianMs / 1000.0 + 2 * m.inPlaceUpdateMedianMs / 1000.0;
        e.exportHundredTracksSeconds = 100 * e.exportTrackSeconds;
        // Denon's published floor for Engine OS media is 6 MB/s write;
        // under 10 MB/s an evening's export is a coffee break.
        e.exportVerdict = verdictFor(-m.streamingWriteBytesPerSecond, -10.0 * 1000 * 1000, -6.0 * 1000 * 1000);
    }
    return e;
}

}  // namespace seabass::domain
