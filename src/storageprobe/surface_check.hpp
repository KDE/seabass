#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "measurement.hpp"

namespace storageprobe
{

// Called between files: bytes and files done so far, and the totals
// counted before the first read.
using SurfaceProgress = std::function<void(std::uint64_t bytesDone, std::uint64_t bytesTotal, std::uint64_t filesDone,
                                           std::uint64_t filesTotal)>;

struct SurfaceCheckOptions
{
    // Files smaller than this are read but not rated: a few KiB say
    // nothing about throughput.
    std::uint64_t rateMinBytes = 1024 * 1024;
    // A file slower than the median by this factor is reported.
    double slowFactor = 10.0;
    std::uint64_t bufferBytes = 1024 * 1024;
};

// Reads every regular file under root once, front to back, the way a
// backup would, and reports what could not be read and what read
// abnormally slowly. Read-only. Hidden folders are included: wear does
// not care what a file is for. Page cache is dropped per file where the
// platform allows, so the drive itself is read. Cancellation is checked
// between files and throws Cancelled.
class SurfaceCheck
{
public:
    static SurfaceCheckResult run(const std::string &root, const SurfaceProgress &progress = {},
                                  const CancelCheck &cancelled = neverCancel(), const SurfaceCheckOptions &options = {});
};

}  // namespace storageprobe
