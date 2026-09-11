#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace storageprobe
{

// Polled between files by every probe; return true to stop. The probe
// then throws Cancelled and, for the write probe, removes its folder.
using CancelCheck = std::function<bool()>;

inline CancelCheck neverCancel()
{
    return [] { return false; };
}

class Cancelled : public std::runtime_error
{
public:
    Cancelled() : std::runtime_error("storage probe cancelled") {}
};

// What ReadProbe measured. Zero means "not measured" for every field.
struct ReadMeasurement
{
    double streamingBytesPerSecond = 0.0;

    double randomReadMedianMs = 0.0;
    double randomReadP95Ms = 0.0;
    int randomReads = 0;

    double smallFileOpensPerSecond = 0.0;
    double smallFileMedianMs = 0.0;
    int smallFilesRead = 0;

    // Reads that took more than kOutlierFactor times the median. Healthy
    // flash has a flat tail; a controller retrying error correction on
    // weak cells shows up here long before anything fails to read.
    int randomReadOutliers = 0;
    int smallFileOutliers = 0;

    // Total size of the "catalog" files the caller named (a database, an
    // index) so a workload can model reading them whole at start-up.
    std::uint64_t catalogBytes = 0;
};

constexpr double kOutlierFactor = 5.0;

// How many values exceed factor times the median; 0 for fewer than
// four values, where a median means little.
int outlierCount(std::vector<double> values, double factor = kOutlierFactor);

// What SurfaceCheck found reading every file once.
struct SurfaceCheckResult
{
    std::uint64_t filesRead = 0;
    std::uint64_t bytesRead = 0;
    double medianBytesPerSecond = 0.0;  // over files of at least kSurfaceRateMinBytes
    double seconds = 0.0;
    // Files that returned a read error: the drive has started to fail.
    std::vector<std::string> unreadable;
    // Files that read at less than a tenth of the median rate: weak
    // blocks being retried. Each entry is "path" and its rate.
    struct SlowFile
    {
        std::string path;
        double bytesPerSecond = 0.0;
    };
    std::vector<SlowFile> slow;
};

// What WriteProbe measured. Zero means "not measured".
struct WriteMeasurement
{
    double streamingWriteBytesPerSecond = 0.0;
    double smallFileWritesPerSecond = 0.0;
    double smallFileWriteMedianMs = 0.0;
    int smallFilesWritten = 0;
    double inPlaceUpdateMedianMs = 0.0;
    int inPlaceUpdates = 0;
    std::uint64_t bytesWritten = 0;  // everything the probe wrote, so a UI can own up to it
};

}  // namespace storageprobe
