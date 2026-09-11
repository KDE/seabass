#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "measurement.hpp"

namespace storageprobe
{

// A perceptual judgement of one action, independent of any reference
// drive: does a person notice the wait?
enum class Verdict
{
    Unknown,   // the measurement this verdict needs was not taken
    Fine,      // no wait anyone would notice
    Slower,    // noticeable pauses
    Sluggish,  // waiting is part of using this drive
};

std::string verdictLabel(Verdict verdict);

// value <= fine -> Fine, <= slower -> Slower, else Sluggish. For a
// "higher is better" number (a throughput floor), negate all three.
Verdict verdictFor(double value, double fineThreshold, double slowerThreshold);

// The drive rated by today's standards, from its score against the
// reference drive.
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

// One thing the application does to the drive, and how often per
// session. The costs are per occurrence; count multiplies them.
struct Action
{
    std::string name;
    int count = 1;
    int randomReads = 0;
    int smallFileOpens = 0;
    std::uint64_t streamedBytes = 0;
    bool streamsCatalog = false;  // adds ReadMeasurement::catalogBytes of streaming per occurrence
    // Perceptual thresholds for one occurrence, in seconds; 0 = no verdict.
    double fineSeconds = 0.0;
    double slowerSeconds = 0.0;
};

struct Session
{
    std::vector<Action> actions;
};

// What "100" means: a reference drive's speeds.
struct ReferenceDrive
{
    double streamingBytesPerSecond = 0.0;
    double randomReadMs = 0.0;
    double smallFileMs = 0.0;
};

struct ActionEstimate
{
    std::string name;
    bool measured = false;  // false when none of the action's costs could be modelled
    double seconds = 0.0;   // per occurrence, on the measured drive
    double referenceSeconds = 0.0;
    int score = 0;  // 100 * reference / measured; 0 when not measured
    Verdict verdict = Verdict::Unknown;
};

struct SessionEstimate
{
    std::vector<ActionEstimate> actions;
    double seconds = 0.0;  // whole session, counts applied
    double referenceSeconds = 0.0;
    int score = 0;  // 100 = the reference drive; uncapped above
    SpeedClass speedClass = SpeedClass::Unknown;
};

// Models the session on the measured drive and on the reference drive.
// A cost whose measurement is missing (zero) is left out on both sides,
// so the comparison stays like for like.
SessionEstimate estimate(const ReadMeasurement &measurement, const Session &session, const ReferenceDrive &reference);

// The same idea for writes: one write-side action and its estimate.
struct WriteAction
{
    std::string name;
    int smallFileWrites = 0;
    int inPlaceUpdates = 0;
    std::uint64_t streamedBytes = 0;
    double fineSeconds = 0.0;
    double slowerSeconds = 0.0;
};

struct WriteActionEstimate
{
    std::string name;
    bool measured = false;
    double seconds = 0.0;
    Verdict verdict = Verdict::Unknown;
};

WriteActionEstimate estimateWrite(const WriteMeasurement &measurement, const WriteAction &action);

}  // namespace storageprobe
