#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace seabass::domain
{

// What a probe measured on one stick, in the three ways a DJ player
// actually reads it (see docs/design/stick-performance-mockups): long
// sequential reads of audio, small random reads (rekordbox's export.pdb
// is a file of 4 KiB pages read on demand; OneLibrary and Engine OS are
// SQLite files queried in place), and opening one small analysis file
// per track load. Zero means "not measured" for every field; the score
// below treats an unmeasured dimension as absent rather than as infinitely
// slow.
struct StickPerformanceMeasurement
{
    double streamingBytesPerSecond = 0.0;

    double randomReadMedianMs = 0.0;
    double randomReadP95Ms = 0.0;
    int randomReads = 0;

    double smallFileOpensPerSecond = 0.0;
    double smallFileMedianMs = 0.0;
    int smallFilesRead = 0;

    // Size of the catalog database(s) a player reads at insertion,
    // export.pdb plus m.db; feeds the modelled mount time only.
    std::uint64_t databaseBytes = 0;
};

// A two-hour set, as a stick sees it. Playback itself is not modelled:
// streaming a track needs well under 1 MB/s, which even a bad stick
// manages without the DJ noticing. What the DJ waits on is the mount at
// insertion, every browse/search action (a burst of database page
// reads), and every track load (a few small analysis files plus the
// first stretch of audio pre-buffered before play).
struct DjWorkloadModel
{
    int browseActions = 200;
    int pageReadsPerBrowseAction = 20;
    int trackLoads = 40;
    int smallFilesPerTrackLoad = 3;
    std::uint64_t streamedBytesPerTrackLoad = 2u * 1024u * 1024u;
    int pageReadsAtMount = 500;
};

// The stick every score is relative to: a current USB 3 flash stick on a
// USB 2.0 port, which is what every DJ player offers. Streaming is capped
// by the port, the other two by a modern controller. Score 100 means
// "waits exactly as long as that stick would"; more is possible on the
// computer's USB 3 port, less is the normal case for older sticks.
struct ReferenceStick
{
    double streamingBytesPerSecond = 40.0 * 1000 * 1000;
    double randomReadMs = 0.3;
    double smallFileMs = 0.4;
};

enum class Verdict
{
    Unknown,   // the measurement this verdict needs was not taken
    Fine,      // no wait a DJ would notice
    Slower,    // noticeable pauses
    Sluggish,  // waiting is part of using this stick
};

std::string verdictLabel(Verdict verdict);

// The stick rated by today's standards, from its score against the
// reference stick: "Fast" is what a current USB 3 stick does, "Very fast"
// beats it (an SSD, or a USB 3 port on the computer), the rest is how far
// behind an older stick has fallen. Independent of the verdicts, which
// say whether anyone would notice.
enum class SpeedClass
{
    Unknown,
    VeryFast,  // score >= 120
    Fast,      // 80 .. 119
    Average,   // 45 .. 79
    Slow,      // 20 .. 44
    VerySlow,  // < 20
};

SpeedClass speedClassFor(int score);
std::string speedClassLabel(SpeedClass speedClass);

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

// What the optional write test measured. Writes happen on the computer,
// never in the booth: saving cue points rewrites one small analysis file
// per track and updates a few database pages in place, and a library
// export from rekordbox or Engine DJ streams one audio file per track
// plus its analysis files. Zero means "not measured".
struct StickWriteMeasurement
{
    double streamingWriteBytesPerSecond = 0.0;  // large sequential writes, fsync'd
    double smallFileWritesPerSecond = 0.0;      // create + write 16 KiB + fsync + close
    double smallFileWriteMedianMs = 0.0;
    int smallFilesWritten = 0;
    double inPlaceUpdateMedianMs = 0.0;  // 4 KiB overwrite at a random offset + fsync
    int inPlaceUpdates = 0;
    std::uint64_t bytesWritten = 0;  // everything the test wrote, for the page to own up to
};

struct WriteWorkloadEstimate
{
    // Saving cue points on one track: two analysis files rewritten and a
    // handful of database pages updated in place.
    double cueSaveSeconds = 0.0;
    Verdict cueSaveVerdict = Verdict::Unknown;
    // Exporting one track from rekordbox or Engine DJ: the audio file plus
    // three analysis files plus its database rows; and a hundred of them.
    double exportTrackSeconds = 0.0;
    double exportHundredTracksSeconds = 0.0;
    Verdict exportVerdict = Verdict::Unknown;  // against Engine OS's documented 6 MB/s write floor
};

WriteWorkloadEstimate estimateWriteWorkloads(const StickWriteMeasurement &measurement);

}  // namespace seabass::domain
