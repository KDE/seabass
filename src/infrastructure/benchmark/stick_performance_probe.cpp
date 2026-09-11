#include "infrastructure/benchmark/stick_performance_probe.hpp"

namespace seabass::infrastructure::benchmark
{

domain::StickPerformanceMeasurement StickPerformanceProbe::run(const std::vector<std::string> &audioFiles,
                                                               const std::vector<std::string> &smallFiles,
                                                               const std::vector<std::string> &databaseFiles,
                                                               const application::CancellationToken &cancel,
                                                               const ProbeOptions &options)
{
    try {
        return storageprobe::ReadProbe::run(audioFiles, smallFiles, databaseFiles, [cancel] { return cancel.cancelled(); },
                                            options);
    } catch (const storageprobe::Cancelled &) {
        throw application::OperationCancelled();
    }
}

}  // namespace seabass::infrastructure::benchmark
