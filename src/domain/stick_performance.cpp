#include "domain/stick_performance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>

namespace seabass::domain
{

namespace
{

// Denon's published minimum for Engine OS media is 20 MB/s read; below
// 8 MB/s the on-player migration and every export drags.
constexpr double kStreamingFineBytesPerSecond = 20.0 * 1000 * 1000;
constexpr double kStreamingSlowerBytesPerSecond = 8.0 * 1000 * 1000;

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

DjWorkloadScore scoreDjWorkload(const StickPerformanceMeasurement &m, const DjWorkloadModel &model,
                                const ReferenceStick &reference)
{
    auto session = storageprobe::estimate(m, model.session(), reference.drive);
    const auto &browse = session.actions[0];
    const auto &load = session.actions[1];
    const auto &mount = session.actions[2];

    DjWorkloadScore s;
    s.score = session.score;
    s.speedClass = session.speedClass;
    s.browseScore = browse.score;
    s.trackLoadScore = load.score;
    s.mountScore = mount.score;
    s.browseActionSeconds = browse.seconds;
    s.trackLoadSeconds = load.seconds;
    s.mountSeconds = mount.seconds;
    s.setWaitSeconds = session.seconds;
    s.referenceSetWaitSeconds = session.referenceSeconds;
    s.browseVerdict = browse.verdict;
    s.trackLoadVerdict = load.verdict;
    s.mountVerdict = mount.verdict;
    if (m.streamingBytesPerSecond > 0.0) {
        s.streamingVerdict = storageprobe::verdictFor(-m.streamingBytesPerSecond, -kStreamingFineBytesPerSecond,
                                                      -kStreamingSlowerBytesPerSecond);
    }
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
    // A save that takes longer than a third of a second is a pause a
    // person notices on the button; past a second it is a wait.
    storageprobe::WriteAction cueSave{"cue save", 2, 4, 0, 0.3, 1.0};
    // Denon's published floor for Engine OS media is 6 MB/s write; under
    // 10 MB/s an evening's export is a coffee break. Judged on the
    // streaming rate alone, so the thresholds are what an 8 MB track
    // costs at those two rates.
    constexpr std::uint64_t kTrackBytes = 8u * 1000u * 1000u;
    storageprobe::WriteAction exportTrack{"export one track", 3, 2, kTrackBytes, 0.0, 0.0};

    WriteWorkloadEstimate e;
    auto save = storageprobe::estimateWrite(m, cueSave);
    if (save.measured) {
        e.cueSaveSeconds = save.seconds;
        e.cueSaveVerdict = save.verdict;
    }
    if (m.streamingWriteBytesPerSecond > 0.0) {
        auto track = storageprobe::estimateWrite(m, exportTrack);
        e.exportTrackSeconds = track.seconds;
        e.exportHundredTracksSeconds = 100 * track.seconds;
        e.exportVerdict = storageprobe::verdictFor(-m.streamingWriteBytesPerSecond, -10.0 * 1000 * 1000, -6.0 * 1000 * 1000);
    }
    return e;
}

}  // namespace seabass::domain
