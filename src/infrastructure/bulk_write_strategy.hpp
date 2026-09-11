#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace seabass::infrastructure
{

struct BulkWriteStrategyInputs
{
    int itemCount = 0;
    std::uintmax_t existingFileBytes = 0;
    // Real measured sequential throughput for this exact stick, when
    // available (see infrastructure::local::StickPerformanceHistory). Left unset by every caller
    // in this codebase today -- wiring that lookup in needs a mount
    // point/stick label these callers don't currently carry -- so a
    // conservative fallback below is used instead.
    std::optional<double> knownSequentialBytesPerSec;
};

// True if copying the whole existing file to fast local scratch, applying
// all itemCount writes there, and copying the result back in one pass is
// CLEARLY faster than itemCount direct writes against the real (possibly
// slow, removable) target -- not just marginally faster, so a handful of
// edits always takes the simpler, already-proven direct path instead of
// the extra moving parts (scratch copy, atomic swap) the whole-file
// route needs. See the .cpp for the constants and reasoning behind them.
bool shouldUseWholeFileReplace(const BulkWriteStrategyInputs &inputs);

// Soft preflight only for the whole-file-replace route above: false just
// means "use the direct per-item writes instead", never an error.
// Running out of space mid-swap (old file + new temp file briefly
// coexisting on the target's filesystem, plus the scratch copy on temp
// storage) would be a strictly worse failure mode than the simpler path
// this falls back to. Shared by every call site that stages a whole-file
// replace (sync, cleanup, library-consistency repair) rather than
// duplicating the same two fs::space() checks in each.
bool hasRoomForWholeFileReplace(const std::filesystem::path &targetDir, std::uintmax_t existingFileBytes);

}  // namespace seabass::infrastructure
