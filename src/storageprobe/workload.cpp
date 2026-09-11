#include "workload.hpp"

#include <cmath>

namespace storageprobe
{

namespace
{

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

Verdict verdictFor(double value, double fineThreshold, double slowerThreshold)
{
    if (value <= fineThreshold) {
        return Verdict::Fine;
    }
    if (value <= slowerThreshold) {
        return Verdict::Slower;
    }
    return Verdict::Sluggish;
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

SessionEstimate estimate(const ReadMeasurement &m, const Session &session, const ReferenceDrive &reference)
{
    const bool haveRandom = m.randomReads > 0 && m.randomReadMedianMs > 0.0;
    const bool haveSmall = m.smallFilesRead > 0 && m.smallFileMedianMs > 0.0;
    const bool haveStreaming = m.streamingBytesPerSecond > 0.0;

    SessionEstimate out;
    for (const Action &a : session.actions) {
        ActionEstimate e;
        e.name = a.name;
        double seconds = 0.0, referenceSeconds = 0.0;
        if (a.randomReads > 0 && haveRandom) {
            seconds += a.randomReads * m.randomReadMedianMs / 1000.0;
            referenceSeconds += a.randomReads * reference.randomReadMs / 1000.0;
            e.measured = true;
        }
        if (a.smallFileOpens > 0 && haveSmall) {
            seconds += a.smallFileOpens * m.smallFileMedianMs / 1000.0;
            referenceSeconds += a.smallFileOpens * reference.smallFileMs / 1000.0;
            e.measured = true;
        }
        std::uint64_t streamed = a.streamedBytes + (a.streamsCatalog ? m.catalogBytes : 0);
        if (streamed > 0 && haveStreaming) {
            seconds += streamSeconds(streamed, m.streamingBytesPerSecond);
            referenceSeconds += streamSeconds(streamed, reference.streamingBytesPerSecond);
            e.measured = true;
        }
        e.seconds = seconds;
        e.referenceSeconds = referenceSeconds;
        e.score = e.measured ? relativeScore(referenceSeconds, seconds) : 0;
        if (e.measured && a.slowerSeconds > 0.0) {
            e.verdict = verdictFor(seconds, a.fineSeconds, a.slowerSeconds);
        }
        out.seconds += a.count * seconds;
        out.referenceSeconds += a.count * referenceSeconds;
        out.actions.push_back(e);
    }
    out.score = relativeScore(out.referenceSeconds, out.seconds);
    out.speedClass = speedClassFor(out.score);
    return out;
}

WriteActionEstimate estimateWrite(const WriteMeasurement &m, const WriteAction &a)
{
    WriteActionEstimate e;
    e.name = a.name;
    if (a.smallFileWrites > 0 && m.smallFilesWritten > 0 && m.smallFileWriteMedianMs > 0.0) {
        e.seconds += a.smallFileWrites * m.smallFileWriteMedianMs / 1000.0;
        e.measured = true;
    }
    if (a.inPlaceUpdates > 0 && m.inPlaceUpdates > 0 && m.inPlaceUpdateMedianMs > 0.0) {
        e.seconds += a.inPlaceUpdates * m.inPlaceUpdateMedianMs / 1000.0;
        e.measured = true;
    }
    if (a.streamedBytes > 0 && m.streamingWriteBytesPerSecond > 0.0) {
        e.seconds += streamSeconds(a.streamedBytes, m.streamingWriteBytesPerSecond);
        e.measured = true;
    }
    if (e.measured && a.slowerSeconds > 0.0) {
        e.verdict = verdictFor(e.seconds, a.fineSeconds, a.slowerSeconds);
    }
    return e;
}

}  // namespace storageprobe
