#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>

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

    // Total size of the "catalog" files the caller named (a database, an
    // index) so a workload can model reading them whole at start-up.
    std::uint64_t catalogBytes = 0;
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
