#pragma once

#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/stick_performance.hpp"
#include "storageprobe/read_probe.hpp"

namespace seabass::infrastructure::benchmark
{

using ProbeOptions = storageprobe::ReadProbeOptions;

// Seabass's face of storageprobe::ReadProbe (src/storageprobe/README.md):
// the same three measurements, with this project's CancellationToken in
// place of the library's callback and OperationCancelled in place of its
// exception. audioFiles are streamed and read at random, smallFiles are
// the per-track analysis files, databaseFiles are only sized.
class StickPerformanceProbe
{
public:
    static domain::StickPerformanceMeasurement run(const std::vector<std::string> &audioFiles,
                                                   const std::vector<std::string> &smallFiles,
                                                   const std::vector<std::string> &databaseFiles,
                                                   const application::CancellationToken &cancel = application::CancellationToken::none(),
                                                   const ProbeOptions &options = {});
};

}  // namespace seabass::infrastructure::benchmark
