// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "surface_check.hpp"

#include "walk_tree.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace storageprobe
{

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{

void dropCacheFor(const std::string &path)
{
#if defined(__linux__)
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
    }
#else
    (void)path;
#endif
}

struct Entry
{
    std::string path;
    std::uint64_t size;
};

}  // namespace

SurfaceCheckResult SurfaceCheck::run(const std::string &root, const SurfaceProgress &progress,
                                     const CancelCheck &cancelled, const SurfaceCheckOptions &options)
{
    SurfaceCheckResult result;

    // Walked by hand rather than with recursive_directory_iterator: its
    // skip_permission_denied only covers EACCES, and macOS answers EPERM
    // for .Spotlight-V100, which it creates on every USB volume it
    // indexes. The iterator sets the error code, the loop ends, and the
    // check reports "Nothing was read." for a stick full of music. See
    // walk_tree.hpp for the measurement.
    std::uint64_t totalBytes = 0;
    auto walk = walkTree(root, {}, [&cancelled](std::uint64_t) {
        if (cancelled()) {
            throw Cancelled();
        }
    });
    std::vector<Entry> entries;
    entries.reserve(walk.files.size());
    for (const auto &file : walk.files) {
        entries.push_back({file.path, file.size});
        totalBytes += file.size;
    }
    // A directory that could not be opened is not media that failed, but
    // it is a part of the stick this check did not see, and saying so is
    // the difference between "healthy" and "healthy as far as I looked".
    result.unopenable = walk.skipped;

    std::vector<char> buffer(options.bufferBytes);
    struct Rated
    {
        std::string path;
        double bytesPerSecond;
    };
    std::vector<Rated> rated;
    std::uint64_t bytesDone = 0;
    std::uint64_t filesDone = 0;
    auto start = Clock::now();
    for (const Entry &entry : entries) {
        if (cancelled()) {
            throw Cancelled();
        }
        dropCacheFor(entry.path);
        auto fileStart = Clock::now();
        std::ifstream in(entry.path, std::ios::binary);
        if (!in) {
            // Could not be opened: nothing read, nothing learned about
            // the media. Counted apart from files that failed mid-read.
            result.unopenable.push_back(entry.path);
            ++filesDone;
            result.filesRead = filesDone;
            if (progress) {
                progress(bytesDone, totalBytes, filesDone, entries.size());
            }
            continue;
        }
        std::uint64_t got = 0;
        bool failed = false;
        std::size_t chunks = 0;
        while (in) {
            // Inside the file too, not only between files: a single
            // gigabyte-sized file would otherwise hold a cancel (and the
            // page that waits on it) for as long as it takes to read.
            if ((++chunks & 0x7) == 0 && cancelled()) {
                throw Cancelled();
            }
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (in.bad()) {
                failed = true;
                break;
            }
            got += static_cast<std::uint64_t>(in.gcount());
            if (in.gcount() == 0) {
                break;
            }
        }
        // A file that ended early was truncated or unreadable past that
        // point; either way it did not deliver what the directory says.
        if (failed || got < entry.size) {
            result.unreadable.push_back(entry.path);
        }
        double seconds = std::chrono::duration<double>(Clock::now() - fileStart).count();
        if (!failed && got >= options.rateMinBytes && seconds > 0.0) {
            rated.push_back({entry.path, static_cast<double>(got) / seconds});
        }
        bytesDone += got;
        ++filesDone;
        result.filesRead = filesDone;
        result.bytesRead = bytesDone;
        if (progress) {
            progress(bytesDone, totalBytes, filesDone, entries.size());
        }
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();

    if (!rated.empty()) {
        std::vector<double> rates;
        rates.reserve(rated.size());
        for (const auto &r : rated) {
            rates.push_back(r.bytesPerSecond);
        }
        std::sort(rates.begin(), rates.end());
        result.medianBytesPerSecond = rates[rates.size() / 2];
        for (const auto &r : rated) {
            if (r.bytesPerSecond * options.slowFactor < result.medianBytesPerSecond) {
                result.slow.push_back({r.path, r.bytesPerSecond});
            }
        }
        std::sort(result.slow.begin(), result.slow.end(),
                  [](const auto &a, const auto &b) { return a.bytesPerSecond < b.bytesPerSecond; });
    }
    return result;
}

}  // namespace storageprobe
