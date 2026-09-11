#pragma once

#include <cstdint>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "domain/stick_performance.hpp"

namespace seabass::infrastructure::benchmark
{

// The optional write test: writes throwaway files into a hidden folder
// of its own under the stick root and removes them again, never touching
// anything else. Three measurements, matching the two write workloads a
// DJ actually runs on a stick from the computer:
//
//   streaming     two 8 MiB files written in 1 MiB pieces and fsync'd,
//                 a library export copying audio;
//   small files   100 files of 16 KiB, each written, fsync'd and closed,
//                 an analysis file rewritten on a cue save or created on
//                 export;
//   in place      100 overwrites of 4 KiB at random offsets of a 4 MiB
//                 file, each fsync'd, a database page updated on save.
//
// About 22 MiB written in total. Refuses to start when the stick has
// less than 64 MiB free, and a leftover folder from a crashed earlier
// run is removed first. Cancellation is checked between files.
struct WriteProbeOptions
{
    int streamingFiles = 2;
    std::uint64_t streamingBytesPerFile = 8 * 1024 * 1024;
    int smallFiles = 100;
    std::uint64_t smallFileBytes = 16 * 1024;
    int inPlaceUpdates = 100;
    std::uint64_t inPlaceFileBytes = 4 * 1024 * 1024;
    std::uint64_t minimumFreeBytes = 64 * 1024 * 1024;
};

class StickWriteProbe
{
public:
    static constexpr const char *kScratchFolderName = ".seabass-write-test";

    // Throws std::runtime_error when the folder cannot be created or the
    // stick is too full; the folder is removed on every exit path.
    static domain::StickWriteMeasurement run(const std::string &stickRoot,
                                             const application::CancellationToken &cancel = application::CancellationToken::none(),
                                             const WriteProbeOptions &options = {});
};

}  // namespace seabass::infrastructure::benchmark
