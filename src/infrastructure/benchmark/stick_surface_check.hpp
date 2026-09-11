#pragma once

#include <functional>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "domain/stick_performance.hpp"
#include "storageprobe/surface_check.hpp"

namespace seabass::infrastructure::benchmark
{

using SurfaceProgress = storageprobe::SurfaceProgress;

// Seabass's face of storageprobe::SurfaceCheck: reads every file on the
// stick once and reports unreadable and abnormally slow ones, with this
// project's CancellationToken and OperationCancelled.
class StickSurfaceCheck
{
public:
    static domain::StickSurfaceCheck run(const std::string &stickRoot, const SurfaceProgress &progress = {},
                                         const application::CancellationToken &cancel = application::CancellationToken::none());
};

}  // namespace seabass::infrastructure::benchmark
