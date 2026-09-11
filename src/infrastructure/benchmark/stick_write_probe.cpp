#include "infrastructure/benchmark/stick_write_probe.hpp"

namespace seabass::infrastructure::benchmark
{

domain::StickWriteMeasurement StickWriteProbe::run(const std::string &stickRoot,
                                                   const application::CancellationToken &cancel,
                                                   WriteProbeOptions options, ScratchFiles *keep)
{
    options.scratchFolderName = kScratchFolderName;
    try {
        return storageprobe::WriteProbe::run(stickRoot, [cancel] { return cancel.cancelled(); }, options, keep);
    } catch (const storageprobe::Cancelled &) {
        throw application::OperationCancelled();
    }
}

void StickWriteProbe::removeScratch(const std::string &stickRoot)
{
    storageprobe::WriteProbe::removeScratch(stickRoot, kScratchFolderName);
}

}  // namespace seabass::infrastructure::benchmark
