#include "infrastructure/benchmark/stick_surface_check.hpp"

namespace seabass::infrastructure::benchmark
{

domain::StickSurfaceCheck StickSurfaceCheck::run(const std::string &stickRoot, const SurfaceProgress &progress,
                                                 const application::CancellationToken &cancel)
{
    try {
        return storageprobe::SurfaceCheck::run(stickRoot, progress, [cancel] { return cancel.cancelled(); });
    } catch (const storageprobe::Cancelled &) {
        throw application::OperationCancelled();
    }
}

}  // namespace seabass::infrastructure::benchmark
