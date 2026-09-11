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
    const bool haveRandom = s.browseVerdict != Verdict::Unknown;
    const bool haveStreaming = s.streamingVerdict != Verdict::Unknown;
    const std::string pageMs = [&] {
        std::ostringstream o;
        o.precision(1);
        o << std::fixed << m.randomReadMedianMs << " ms";
        return o.str();
    }();
    const std::string streamingMBps = std::to_string(static_cast<int>(std::lround(m.streamingBytesPerSecond / 1e6)));

    // The rekordbox rows are judged on the small random read first; when
    // that could not be measured (no file on the stick large enough to
    // seek in), they fall back to the track-load verdict and say so
    // rather than quoting a 0.0 ms page.
    auto rekordboxRow = [&](const char *group, const char *players, const std::string &fine, const std::string &slower,
                            const std::string &sluggish) {
        PlayerAdvisory row;
        row.group = group;
        row.players = players;
        row.verdict = worst(s.browseVerdict, s.trackLoadVerdict);
        if (!haveRandom && row.verdict != Verdict::Unknown) {
            row.summary = "Small random reads could not be measured on this stick, so this is judged on track loads "
                          "alone" +
                          std::string(row.verdict == Verdict::Fine ? ", which are fine." : ", which are not quick.");
        } else {
            switch (row.verdict) {
            case Verdict::Fine:
                row.summary = fine;
                break;
            case Verdict::Slower:
                row.summary = slower;
                break;
            case Verdict::Sluggish:
                row.summary = sluggish;
                break;
            case Verdict::Unknown:
                row.summary = "Not measured.";
                break;
            }
        }
        rows.push_back(row);
    };

    rekordboxRow("Device Library players", "CDJ-2000 · NXS · NXS2 · XDJ-1000MK2 · XDJ-XZ · XDJ-RX3 · CDJ-3000",
                 "Reads the database in 4 KiB pages straight off the stick as you browse, the same size as the small "
                 "random read, and one analysis file per track load. " + pageMs + " a page keeps up with the jog wheel.",
                 "Every database page is a " + pageMs + " wait and a folder of thousands of tracks is hundreds of pages. "
                 "Expect a longer \"Loading\" at insertion and a short pause before each waveform appears.",
                 "Every database page is a " + pageMs + " wait. Expect a long \"Loading\" at insertion, browsing that "
                 "lags the jog wheel, and a visible pause before each waveform appears.");

    rekordboxRow("OneLibrary players", "CDJ-3000X · OPUS-QUAD · OMNIS-DUO · XDJ-AZ · rekordbox 7",
                 "Runs every list, sort and search as a SQLite query on the stick. Small random reads decide how snappy "
                 "that feels; this stick answers in " + pageMs + ".",
                 "Search and sort run as SQLite queries on the stick as you type. At " + pageMs + " per read, typing a "
                 "search and scrolling a long playlist stutter.",
                 "Search and sort run as SQLite queries on the stick as you type. At " + pageMs + " per read every "
                 "keystroke and every scroll waits on the stick; this is the generation that suffers most.");

    // Engine OS: SQLite in place, plus Denon's 20 MB/s floor, which also
    // paces the on-player migration of an older library. Each sentence
    // only quotes the number that was measured.
    {
        PlayerAdvisory row;
        row.group = "Engine OS players";
        row.players = "SC5000 · SC6000 · Prime 4 / 4+ · Prime 2 · Prime GO · SC Live";
        row.verdict = worst(s.browseVerdict, s.streamingVerdict);
        const std::string browsePart = !haveRandom ? std::string()
            : s.browseVerdict == Verdict::Fine ? "Browsing answers in " + pageMs + " per read, which keeps up."
            : s.browseVerdict == Verdict::Slower ? "Browsing at " + pageMs + " per read is noticeably behind a current stick."
            : "Reads of " + pageMs + " make every list wait.";
        const std::string streamPart = !haveStreaming ? std::string()
            : s.streamingVerdict == Verdict::Fine
                ? "Denon asks for at least 20 MB/s and this stick streams at " + streamingMBps + " MB/s."
            : s.streamingVerdict == Verdict::Slower
                ? "Streaming at " + streamingMBps + " MB/s is under Denon's 20 MB/s minimum for Engine OS media, so the "
                  "one-time migration of an older library and every export take longer."
                : "Streaming at " + streamingMBps + " MB/s is far under Denon's 20 MB/s minimum.";
        switch (row.verdict) {
        case Verdict::Fine:
            row.summary = "Reads its SQLite database in place, like OneLibrary. " + (streamPart.empty() ? browsePart : streamPart) +
                          " A library from an older Engine DJ is migrated on the player at the stick's own speed.";
            break;
        case Verdict::Slower:
        case Verdict::Sluggish: {
            row.summary = "Same SQLite-on-the-stick model. ";
            if (!streamPart.empty() && s.streamingVerdict != Verdict::Fine) {
                row.summary += streamPart;
            }
            if (!browsePart.empty() && s.browseVerdict != Verdict::Fine) {
                row.summary += (row.summary.back() == ' ' ? "" : " ") + browsePart;
            }
            if (row.verdict == Verdict::Sluggish) {
                row.summary += " Expect lag scrolling long playlists and the \"preparing library\" pass to run into minutes.";
            }
            break;
        }
        case Verdict::Unknown:
            row.summary = "Not measured.";
            break;
        }
        rows.push_back(row);
    }

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

WearAssessment assessWear(const StickSurfaceCheck &check, const StickPerformanceMeasurement &probe)
{
    WearAssessment w;
    if (check.filesRead == 0) {
        w.state = WearState::Unknown;
        w.label = "Not checked";
        w.summary = "Nothing was read.";
        return w;
    }
    std::ostringstream out;
    const std::string unopenableNote = check.unopenable.empty()
        ? std::string()
        : " " + std::to_string(check.unopenable.size())
              + (check.unopenable.size() == 1 ? " file could not be opened" : " files could not be opened")
              + " and was skipped; that says nothing about the flash.";
    if (!check.unreadable.empty()) {
        w.state = WearState::Failing;
        w.label = "Failing";
        out << check.unreadable.size() << " of " << check.filesRead
            << " files could not be read in full. Copy what this stick still gives up and retire it; a stick that has "
               "started to lose blocks does not recover." << unopenableNote;
    } else if (!check.slow.empty()) {
        w.state = WearState::Watch;
        w.label = "Watch this stick";
        out << check.slow.size() << " of " << check.filesRead
            << " files read at under a tenth of this stick's own rate, which is what a controller retrying weak cells "
               "looks like. Everything is still readable. Keep a backup current and check again in a few months."
            << unopenableNote;
    } else {
        w.state = WearState::Healthy;
        w.label = "No sign of wear";
        out << "Every one of " << check.filesRead << " files read in full at a normal rate";
        const int tail = probe.randomReadOutliers + probe.smallFileOutliers;
        if (probe.randomReads > 0 || probe.smallFilesRead > 0) {
            if (tail == 0) {
                out << ", and the small-read tail is flat.";
            } else if (tail == 1) {
                out << ", though one small read was unusually slow.";
            } else {
                out << ", though " << tail << " small reads were unusually slow.";
            }
        } else {
            out << ".";
        }
        out << unopenableNote;
    }
    w.summary = out.str();
    return w;
}

namespace
{
// USB 2.0 and everything slower on one side, SuperSpeed on the other.
bool sameLinkClass(double a, double b)
{
    if (a <= 0.0 || b <= 0.0) {
        return true;  // unknown: compare rather than discard
    }
    return (a >= 5000.0) == (b >= 5000.0);
}
}  // namespace

TrendAssessment assessTrend(int currentScore, double currentRandomReadMs, int currentOutliers,
                            const std::string &currentWearState, double currentUsbSpeedMbps,
                            const std::vector<TrendPoint> &allEarlier)
{
    TrendAssessment t;
    std::vector<TrendPoint> earlier;
    for (const auto &p : allEarlier) {
        if (sameLinkClass(p.usbSpeedMbps, currentUsbSpeedMbps)) {
            earlier.push_back(p);
        }
    }
    t.earlierCount = static_cast<int>(earlier.size());
    if (earlier.empty() || currentScore <= 0) {
        t.state = TrendState::Unknown;
        t.summary = allEarlier.empty()
            ? "First measurement of this stick on this computer; the next one will show whether it is changing."
            : "First measurement of this stick on this kind of USB port; earlier ones were on a different port and "
              "are not comparable.";
        return t;
    }
    bool wasHealthy = false;
    int earlierOutliers = 0;
    for (const auto &p : earlier) {
        t.bestEarlierScore = std::max(t.bestEarlierScore, p.score);
        wasHealthy = wasHealthy || p.wearState == "healthy";
        earlierOutliers = std::max(earlierOutliers, p.outliers);
    }
    std::ostringstream out;
    out << "Measured " << (earlier.size() + 1) << " times since " << earlier.front().measuredAtUtc.substr(0, 10) << ": ";
    for (const auto &p : earlier) {
        out << p.score << ", ";
    }
    out << currentScore << ".";

    // A quarter slower than its own best is beyond port-to-port and
    // day-to-day noise on the same computer; so is the random-read
    // median doubling, which is the wear symptom, not the port one.
    double bestRandom = 0.0;
    for (const auto &p : earlier) {
        if (p.randomReadMedianMs > 0.0 && (bestRandom == 0.0 || p.randomReadMedianMs < bestRandom)) {
            bestRandom = p.randomReadMedianMs;
        }
    }
    const bool scoreDropped = t.bestEarlierScore > 0 && currentScore * 4 < t.bestEarlierScore * 3;
    const bool randomDoubled = bestRandom > 0.0 && currentRandomReadMs > 2.0 * bestRandom;
    // A tail that was flat every time before and now stalls repeatedly is
    // the wear symptom arriving, even while the medians hold.
    const bool tailAppeared = earlierOutliers == 0 && currentOutliers >= 5;
    const bool wearWorsened = wasHealthy && (currentWearState == "watch" || currentWearState == "failing");
    if (wearWorsened) {
        t.state = TrendState::Worsened;
        out << " The wear check found " << (currentWearState == "failing" ? "unreadable" : "abnormally slow")
            << " files where an earlier check found none: this stick is wearing out.";
    } else if (scoreDropped || randomDoubled || tailAppeared) {
        t.state = TrendState::Slowing;
        out << " Slower than it used to be";
        if (randomDoubled) {
            out << ": small reads take " << std::fixed;
            out.precision(1);
            out << currentRandomReadMs << " ms against " << bestRandom << " ms before, which is what wear looks like";
        } else if (tailAppeared) {
            out << ": " << currentOutliers << " small reads stalled where none did before; run the wear check";
        } else {
            out << "; if it is on the same kind of port as before, run the wear check";
        }
        out << ".";
    } else {
        t.state = TrendState::Steady;
        out << " Steady.";
    }
    t.summary = out.str();
    return t;
}

}  // namespace seabass::domain
