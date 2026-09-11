#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "domain/stick_performance.hpp"

namespace seabass::infrastructure::benchmark
{

// Measures a stick the three ways a DJ player reads it, against real
// files already on it, never writing anything:
//
//   streaming     a 4 MiB read from the front of each audio file, page
//                 cache dropped first (Linux: POSIX_FADV_DONTNEED), the
//                 same measurement the old read-speed benchmark took;
//   random reads  4 KiB reads at random 4 KiB-aligned offsets spread over
//                 the audio files, opened with O_DIRECT (Linux) or
//                 FILE_FLAG_NO_BUFFERING (Windows) so the page cache is
//                 bypassed on every read, falling back to a cache-dropped
//                 buffered read where direct I/O is refused;
//   small files   open + read 16 KiB + close on each small file (ANLZ .DAT
//                 files or Engine OverviewData), cache dropped first.
//
// The kernel's directory cache cannot be dropped without root, so after a
// scan the small-file number measures data latency, not directory
// search; the page says so. Every file that fails to open is skipped.
// Cancellation is checked between files, never mid-read.
struct ProbeOptions
{
    std::uint64_t streamingBytesPerFile = 4 * 1024 * 1024;
    int randomReads = 300;
    std::uint64_t smallFileReadBytes = 16 * 1024;
};

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
