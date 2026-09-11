#include "read_probe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace storageprobe
{

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{

constexpr std::uint64_t kAlignment = 4096;

// Best-effort: asks the kernel to drop any cached pages for this file so
// the read that follows hits the device. Linux only; without it a repeat
// run measures the page cache and looks inconsistently, misleadingly
// faster.
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

double elapsedMs(Clock::time_point since)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

int outlierCount(std::vector<double> values, double factor)
{
    if (values.size() < 4) {
        return 0;
    }
    double m = median(values);
    if (m <= 0.0) {
        return 0;
    }
    int count = 0;
    for (double v : values) {
        if (v > factor * m) {
            ++count;
        }
    }
    return count;
}

namespace
{

double percentile95(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(values.size() * 95 / 100))];
}

// A file opened for direct (unbuffered) 4 KiB reads at aligned offsets.
// Where direct I/O is refused (a filesystem that does not support it, or
// no platform support at all) it falls back to a buffered read after
// dropping the cache, which still measures the device on Linux and is
// simply best-effort elsewhere.
class DirectReader
{
public:
    explicit DirectReader(const std::string &path)
    {
#if defined(__linux__)
        m_fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
        if (m_fd < 0) {
            m_fd = ::open(path.c_str(), O_RDONLY);
            m_direct = false;
        }
#elif defined(_WIN32)
        std::wstring wide(path.begin(), path.end());
        m_handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            m_handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            m_direct = false;
        }
#else
        m_stream.open(path, std::ios::binary);
        m_direct = false;
#endif
    }

    ~DirectReader()
    {
#if defined(__linux__)
        if (m_fd >= 0) {
            ::close(m_fd);
        }
#elif defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
#endif
    }

    DirectReader(const DirectReader &) = delete;
    DirectReader &operator=(const DirectReader &) = delete;

    bool ok() const
    {
#if defined(__linux__)
        return m_fd >= 0;
#elif defined(_WIN32)
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_stream.good();
#endif
    }

    bool direct() const { return m_direct; }

    // Reads kAlignment bytes at an aligned offset into an aligned buffer.
    bool readAt(std::uint64_t offset, void *buffer)
    {
#if defined(__linux__)
        if (!m_direct) {
            posix_fadvise(m_fd, static_cast<off_t>(offset), static_cast<off_t>(kAlignment), POSIX_FADV_DONTNEED);
        }
        return ::pread(m_fd, buffer, kAlignment, static_cast<off_t>(offset)) > 0;
#elif defined(_WIN32)
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        return ReadFile(m_handle, buffer, static_cast<DWORD>(kAlignment), &got, &ov) && got > 0;
#else
        m_stream.clear();
        m_stream.seekg(static_cast<std::streamoff>(offset));
        m_stream.read(static_cast<char *>(buffer), static_cast<std::streamsize>(kAlignment));
        return m_stream.gcount() > 0;
#endif
    }

private:
    bool m_direct = true;
#if defined(__linux__)
    int m_fd = -1;
#elif defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    std::ifstream m_stream;
#endif
};

struct AlignedBuffer
{
    void *data = nullptr;
    AlignedBuffer()
    {
#if defined(_WIN32)
        data = _aligned_malloc(kAlignment, kAlignment);
#else
        data = std::aligned_alloc(kAlignment, kAlignment);
#endif
    }
    ~AlignedBuffer()
    {
#if defined(_WIN32)
        _aligned_free(data);
#else
        std::free(data);
#endif
    }
    AlignedBuffer(const AlignedBuffer &) = delete;
    AlignedBuffer &operator=(const AlignedBuffer &) = delete;
};

double measureStreaming(const std::vector<std::string> &files, std::uint64_t bytesPerFile,
                        const CancelCheck &cancelled)
{
    for (const auto &path : files) {
        dropCacheFor(path);
    }
    std::vector<char> buffer(bytesPerFile);
    std::uint64_t total = 0;
    auto start = Clock::now();
    for (const auto &path : files) {
        if (cancelled()) {
            throw Cancelled();
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            continue;
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) {
            total += static_cast<std::uint64_t>(in.gcount());
        }
    }
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return (seconds > 0.0 && total > 0) ? static_cast<double>(total) / seconds : 0.0;
}

void measureRandomReads(const std::vector<std::string> &files, int reads, const CancelCheck &cancelled,
                        ReadMeasurement &out)
{
    struct Candidate
    {
        std::string path;
        std::uint64_t size;
    };
    std::vector<Candidate> candidates;
    for (const auto &path : files) {
        std::error_code ec;
        auto size = fs::file_size(path, ec);
        if (!ec && size >= 4 * kAlignment) {
            candidates.push_back({path, size});
        }
    }
    if (candidates.empty() || reads <= 0) {
        return;
    }

    // Fixed seed: two runs on the same stick read the same offsets, so a
    // difference between them is the stick, not the dice.
    std::mt19937_64 rng(0x5EABA55u);
    AlignedBuffer buffer;
    if (buffer.data == nullptr) {
        return;
    }
    std::vector<double> latencies;
    latencies.reserve(static_cast<std::size_t>(reads));

    // Round-robin over the files, one open per read: a player opens a
    // database once and reads many pages from it, but here one open per
    // read is what keeps the fallback path's cache drop honest, and the
    // open itself is a directory-cache hit after the first pass.
    for (int i = 0; i < reads; ++i) {
        if ((i & 0x1f) == 0) {
            if (cancelled()) {
                throw Cancelled();
            }
        }
        const Candidate &c = candidates[static_cast<std::size_t>(i) % candidates.size()];
        std::uint64_t slots = c.size / kAlignment - 1;
        std::uint64_t offset = std::uniform_int_distribution<std::uint64_t>(0, slots - 1)(rng) * kAlignment;
        DirectReader reader(c.path);
        if (!reader.ok()) {
            continue;
        }
        auto start = Clock::now();
        if (reader.readAt(offset, buffer.data)) {
            latencies.push_back(elapsedMs(start));
        }
    }
    out.randomReads = static_cast<int>(latencies.size());
    out.randomReadMedianMs = median(latencies);
    out.randomReadP95Ms = percentile95(latencies);
    out.randomReadOutliers = outlierCount(latencies);
}

void measureSmallFiles(const std::vector<std::string> &files, std::uint64_t readBytes,
                       const CancelCheck &cancelled, ReadMeasurement &out)
{
    for (const auto &path : files) {
        dropCacheFor(path);
    }
    std::vector<char> buffer(readBytes);
    std::vector<double> latencies;
    latencies.reserve(files.size());
    auto groupStart = Clock::now();
    for (const auto &path : files) {
        if (cancelled()) {
            throw Cancelled();
        }
        auto start = Clock::now();
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            continue;
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() <= 0) {
            continue;
        }
        in.close();
        latencies.push_back(elapsedMs(start));
    }
    double seconds = std::chrono::duration<double>(Clock::now() - groupStart).count();
    out.smallFilesRead = static_cast<int>(latencies.size());
    out.smallFileMedianMs = median(latencies);
    out.smallFileOutliers = outlierCount(latencies);
    out.smallFileOpensPerSecond = (seconds > 0.0 && !latencies.empty()) ? latencies.size() / seconds : 0.0;
}

}  // namespace

ReadMeasurement ReadProbe::run(const std::vector<std::string> &largeFiles, const std::vector<std::string> &smallFiles,
                               const std::vector<std::string> &catalogFiles, const CancelCheck &cancelled,
                               const ReadProbeOptions &options)
{
    ReadMeasurement m;
    for (const auto &path : catalogFiles) {
        std::error_code ec;
        auto size = fs::file_size(path, ec);
        if (!ec) {
            m.catalogBytes += size;
        }
    }
    m.streamingBytesPerSecond = measureStreaming(largeFiles, options.streamingBytesPerFile, cancelled);
    measureRandomReads(largeFiles, options.randomReads, cancelled, m);
    measureSmallFiles(smallFiles, options.smallFileReadBytes, cancelled, m);
    return m;
}

}  // namespace storageprobe
