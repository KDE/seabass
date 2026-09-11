#include "infrastructure/bulk_write_strategy.hpp"

namespace seabass::infrastructure
{

namespace
{

// Below this many items, the difference between the two approaches is
// small in absolute terms regardless of what the formula below says, and
// the direct path's simplicity (no scratch copy, no atomic swap) is worth
// more than a saving of a couple of seconds.
constexpr int MinItemCountForWholeFileReplace = 15;

// The whole-file route has to look CLEARLY better, not just marginally,
// before it's worth the extra moving parts -- avoids flip-flopping
// between strategies on borderline estimates that are themselves rough.
constexpr double SafetyMargin = 1.5;

// A conservative order-of-magnitude estimate of one direct reopen+write
// +fsync round trip against removable media, confirmed this session to
// be roughly this shape (a full database reopen plus a commit) for both
// LibdjinteropEngineCueWriter and OneLibraryCueWriter. Unlike sequential
// throughput (see knownSequentialBytesPerSec), this isn't measured per-
// stick anywhere yet -- a natural follow-up would be recording real
// observed per-item latency the same way the USB Stick Performance page already
// does for sequential speed, rather than guessing indefinitely.
constexpr double ConservativePerItemWriteSeconds = 0.2;

// Used only when the caller has no real measured throughput for this
// stick. Deliberately pessimistic: guessing a stick is slower than it
// really is just costs a few direct writes' worth of time by not taking
// the whole-file route; guessing it's faster than it really is could
// make the whole-file route -- which moves 2x the file's bytes -- the
// slower choice without our knowing it.
constexpr double FallbackSequentialBytesPerSec = 5.0 * 1024 * 1024;

}  // namespace

bool shouldUseWholeFileReplace(const BulkWriteStrategyInputs &inputs)
{
    if (inputs.itemCount < MinItemCountForWholeFileReplace) {
        return false;
    }

    double throughput = inputs.knownSequentialBytesPerSec.value_or(FallbackSequentialBytesPerSec);
    if (throughput <= 0.0) {
        return false;
    }

    double directSeconds = inputs.itemCount * ConservativePerItemWriteSeconds;
    // The whole-file route crosses the slow medium twice: once reading
    // the existing file into scratch, once writing the patched copy back.
    double wholeFileSeconds = 2.0 * static_cast<double>(inputs.existingFileBytes) / throughput;

    return wholeFileSeconds * SafetyMargin < directSeconds;
}

bool hasRoomForWholeFileReplace(const std::filesystem::path &targetDir, std::uintmax_t existingFileBytes)
{
    std::error_code ec;
    auto targetSpace = std::filesystem::space(targetDir, ec);
    if (ec) {
        return false;
    }
    auto tempSpace = std::filesystem::space(std::filesystem::temp_directory_path(), ec);
    if (ec) {
        return false;
    }
    return targetSpace.available >= 2 * existingFileBytes && tempSpace.available >= existingFileBytes;
}

}  // namespace seabass::infrastructure
