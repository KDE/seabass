#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "measurement.hpp"

namespace storageprobe
{

struct WriteProbeOptions
{
    // Folder created directly under the drive root; hidden by convention.
    std::string scratchFolderName = ".storageprobe-write-test";
    int streamingFiles = 2;
    std::uint64_t streamingBytesPerFile = 8 * 1024 * 1024;
    int smallFiles = 100;
    std::uint64_t smallFileBytes = 16 * 1024;
    int inPlaceUpdates = 100;
    std::uint64_t inPlaceFileBytes = 4 * 1024 * 1024;
    // Refuse to start below this much free space.
    std::uint64_t minimumFreeBytes = 64 * 1024 * 1024;
};

// The files the probe left behind when asked to keep them: a blank drive
// has nothing for ReadProbe to read, so a caller can write these first,
// read them back, and then call removeScratch().
struct ScratchFiles
{
    std::string folder;
    std::vector<std::string> streamFiles;
    std::vector<std::string> smallFiles;
};

// Writes throwaway files into its own folder under `root` and removes
// them again; touches nothing else. About 22 MiB with the defaults.
// Throws std::runtime_error when the folder cannot be created or the
// drive is too full, Cancelled when `cancelled` says so; in both cases
// the folder is removed. With `keep`, the folder survives a successful
// run and the caller removes it with removeScratch().
class WriteProbe
{
public:
    static WriteMeasurement run(const std::string &root, const CancelCheck &cancelled = neverCancel(),
                                const WriteProbeOptions &options = {}, ScratchFiles *keep = nullptr);

    static void removeScratch(const std::string &root, const std::string &scratchFolderName = WriteProbeOptions{}.scratchFolderName);
};

}  // namespace storageprobe
