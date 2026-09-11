#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "measurement.hpp"

namespace storageprobe
{

struct ReadProbeOptions
{
    std::uint64_t streamingBytesPerFile = 4 * 1024 * 1024;
    int randomReads = 300;
    std::uint64_t smallFileReadBytes = 16 * 1024;
};

// Reads real files already on the drive; never writes. See README.md
// for what the three measurements stand for. `largeFiles` are streamed
// and used for the random reads (files under 16 KiB are skipped there);
// `smallFiles` are opened and read one by one; `catalogFiles` are only
// sized. Every file that fails to open is skipped. The operating
// system's directory cache cannot be dropped without privileges, so the
// small-file number measures data latency, not name lookup.
class ReadProbe
{
public:
    static ReadMeasurement run(const std::vector<std::string> &largeFiles, const std::vector<std::string> &smallFiles,
                               const std::vector<std::string> &catalogFiles, const CancelCheck &cancelled = neverCancel(),
                               const ReadProbeOptions &options = {});
};

}  // namespace storageprobe
