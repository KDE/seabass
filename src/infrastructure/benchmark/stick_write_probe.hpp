#pragma once

#include <string>

#include "application/ports/cancellation_token.hpp"
#include "domain/stick_performance.hpp"
#include "storageprobe/write_probe.hpp"

namespace seabass::infrastructure::benchmark
{

using WriteProbeOptions = storageprobe::WriteProbeOptions;
using ScratchFiles = storageprobe::ScratchFiles;

// Seabass's face of storageprobe::WriteProbe: throwaway files in a
// hidden folder of this app's own naming under the stick root, removed
// afterwards, with this project's CancellationToken and
// OperationCancelled. About 22 MiB with the defaults; refuses below
// 64 MiB free. With `keep`, the files stay for a read measurement on a
// blank stick and the caller removes them with removeScratch().
class StickWriteProbe
{
public:
    static constexpr const char *kScratchFolderName = ".seabass-write-test";

    static domain::StickWriteMeasurement run(const std::string &stickRoot,
                                             const application::CancellationToken &cancel = application::CancellationToken::none(),
                                             WriteProbeOptions options = {}, ScratchFiles *keep = nullptr);

    static void removeScratch(const std::string &stickRoot);
};

}  // namespace seabass::infrastructure::benchmark
