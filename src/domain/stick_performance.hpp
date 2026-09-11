#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "storageprobe/measurement.hpp"
#include "storageprobe/surface_check.hpp"
#include "storageprobe/workload.hpp"

namespace seabass::domain
{

// Seabass's DJ workload on top of storageprobe (src/storageprobe/README.md):
// the library measures a drive and models sessions; this file says what a
// DJ set looks like as a session, what a stick has to be for nobody to
// notice, and what each player generation does with the result. The
// measurement types are the library's own, under this project's names.
using StickPerformanceMeasurement = storageprobe::ReadMeasurement;
using StickWriteMeasurement = storageprobe::WriteMeasurement;
using Verdict = storageprobe::Verdict;
using SpeedClass = storageprobe::SpeedClass;
using storageprobe::speedClassFor;
using storageprobe::speedClassLabel;
using storageprobe::verdictLabel;

// A two-hour set, as a stick sees it. Playback itself is not modelled:
// streaming a track needs well under 1 MB/s, which even a bad stick
// manages without the DJ noticing. What the DJ waits on is the mount at
// insertion, every browse/search action (a burst of database page reads;
// rekordbox's export.pdb is a paged file read on demand, OneLibrary and
// Engine OS are SQLite queried in place), and every track load (a few
// small analysis files plus the first stretch of audio pre-buffered
// before play). Thresholds are per occurrence and perceptual: "fine" is
// below what a person notices as a pause on a button press, with the
// browse budget tighter because a scroll fires many actions in a row.
// Provisional: calibrated against two healthy sticks, not yet a known-
// slow one.
struct DjWorkloadModel
{
    storageprobe::Action browse{"browsing", 200, 20, 0, 0, false, 0.040, 0.120};
    storageprobe::Action trackLoad{"track load", 40, 0, 3, 2u * 1024u * 1024u, false, 0.100, 0.300};
    storageprobe::Action mount{"plugging in", 1, 500, 0, 0, true, 2.0, 10.0};

    storageprobe::Session session() const { return {{browse, trackLoad, mount}}; }
};

// The stick every score is relative to: a current USB 3 flash stick on a
// USB 2.0 port, which is what every DJ player offers. Streaming is capped
// by the port, the other two by a modern controller. Score 100 means
// "waits exactly as long as that stick would".
struct ReferenceStick
{
    storageprobe::ReferenceDrive drive{40.0 * 1000 * 1000, 0.3, 0.4};
};

struct DjWorkloadScore
{
    // 100 = the reference stick; uncapped above, 0 when nothing was measured.
    int score = 0;
    SpeedClass speedClass = SpeedClass::Unknown;
    int browseScore = 0;
    int trackLoadScore = 0;
    int mountScore = 0;

    // Modelled waits, per action and over the whole set, in seconds.
    double browseActionSeconds = 0.0;
    double trackLoadSeconds = 0.0;
    double mountSeconds = 0.0;
    double setWaitSeconds = 0.0;
    double referenceSetWaitSeconds = 0.0;

    // Perceptual verdicts on each action, independent of the reference:
    // a stick can score 50 against a modern one and still be fine to play
    // on, which is exactly the case of a good 2012 stick.
    Verdict browseVerdict = Verdict::Unknown;
    Verdict trackLoadVerdict = Verdict::Unknown;
    Verdict mountVerdict = Verdict::Unknown;
    Verdict streamingVerdict = Verdict::Unknown;  // against Engine OS's documented 20 MB/s floor
};

DjWorkloadScore scoreDjWorkload(const StickPerformanceMeasurement &measurement, const DjWorkloadModel &model = {},
                                const ReferenceStick &reference = {});

// One row of the "on a player" advisory. Player groups are by the
// database they read, because that decides which measurement matters;
// the sources are listed in the design note under docs/design.
struct PlayerAdvisory
{
    std::string group;    // "Device Library players"
    std::string players;  // "CDJ-2000 · NXS · NXS2 · XDJ-1000MK2 · XDJ-XZ · XDJ-RX3 · CDJ-3000"
    Verdict verdict = Verdict::Unknown;
    std::string summary;  // one or two sentences, complete, ending in a full stop
};

std::vector<PlayerAdvisory> advisePlayers(const StickPerformanceMeasurement &measurement, const DjWorkloadScore &score);

// Text for the score itself: "About 7 s of waiting across a two-hour set"
// and what the reference stick would have waited.
std::string describeSetWait(const DjWorkloadScore &score);

// The two write workloads a stick sees from the computer: saving cue
// points (two analysis files rewritten, a few database pages updated in
// place) and a library export from rekordbox or Engine DJ (the audio
// file plus three analysis files plus its database rows, per track).
struct WriteWorkloadEstimate
{
    double cueSaveSeconds = 0.0;
    Verdict cueSaveVerdict = Verdict::Unknown;
    double exportTrackSeconds = 0.0;
    double exportHundredTracksSeconds = 0.0;
    Verdict exportVerdict = Verdict::Unknown;  // against Engine OS's documented 6 MB/s write floor
};

WriteWorkloadEstimate estimateWriteWorkloads(const StickWriteMeasurement &measurement);

// What the wear check concluded. A stick cannot be asked how worn it is
// (no SMART over USB mass storage), so this is read from symptoms: reads
// that fail, and reads that take far longer than the rest because the
// controller is retrying error correction on weak cells.
using StickSurfaceCheck = storageprobe::SurfaceCheckResult;

enum class WearState
{
    Unknown,  // not checked
    Healthy,  // every file read at a normal rate
    Watch,    // some files read abnormally slowly: weak blocks, back this stick up and watch it
    Failing,  // files could not be read
};

struct WearAssessment
{
    WearState state = WearState::Unknown;
    std::string label;    // "No sign of wear"
    std::string summary;  // one or two sentences, ending in a full stop
};

WearAssessment assessWear(const StickSurfaceCheck &check, const StickPerformanceMeasurement &probe);

// A stick over time. Each earlier measurement is one point; the trend
// compares today's with the best of the earlier ones, because a stick
// only ever gets slower and a port change can only make it look faster
// than it is, never slower.
struct TrendPoint
{
    std::string measuredAtUtc;
    int score = 0;
    double randomReadMedianMs = 0.0;
    int outliers = 0;
    std::string wearState;
    double usbSpeedMbps = 0.0;  // the link the measurement ran on; 0 when unknown
};

enum class TrendState
{
    Unknown,   // first measurement, nothing to compare with
    Steady,    // within noise of the earlier measurements
    Slowing,   // clearly slower than it used to be: wear, or a slower port
    Worsened,  // the wear check has gone from healthy to watch/failing
};

struct TrendAssessment
{
    TrendState state = TrendState::Unknown;
    std::string summary;  // "Measured 4 times since June: 85, 84, 83, 83. Steady."
    int bestEarlierScore = 0;
    int earlierCount = 0;
};

// currentWearState: the wear check's result for this measurement when it
// has run ("healthy", "watch", "failing"), else empty. Worsened means an
// earlier check said healthy and this one does not.
// currentUsbSpeedMbps: the link this measurement ran on. Scores are only
// compared with earlier points on the same class of link (USB 2.0 versus
// USB 3): the streaming term dominates the score, and the same stick is
// far faster on a USB 3 port, so a port change would otherwise read as
// wear in one direction and hide it in the other.
TrendAssessment assessTrend(int currentScore, double currentRandomReadMs, int currentOutliers,
                            const std::string &currentWearState, double currentUsbSpeedMbps,
                            const std::vector<TrendPoint> &earlier);

}  // namespace seabass::domain
