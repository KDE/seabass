#include "infrastructure/benchmark/stick_write_probe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace seabass::infrastructure::benchmark
{

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{

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

// A file opened for writing with a real fsync, which std::ofstream cannot
// do; a write test that never reaches the flash measures the page cache.
class SyncedFile
{
public:
    SyncedFile(const fs::path &path, bool truncate)
    {
#if defined(_WIN32)
        m_handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               truncate ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | (truncate ? O_TRUNC : 0), 0644);
#endif
    }
    ~SyncedFile() { close(); }
    SyncedFile(const SyncedFile &) = delete;
    SyncedFile &operator=(const SyncedFile &) = delete;

    bool ok() const
    {
#if defined(_WIN32)
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_fd >= 0;
#endif
    }

    bool writeAt(std::uint64_t offset, const char *data, std::uint64_t bytes)
    {
#if defined(_WIN32)
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD written = 0;
        return WriteFile(m_handle, data, static_cast<DWORD>(bytes), &written, &ov) && written == bytes;
#else
        return ::pwrite(m_fd, data, bytes, static_cast<off_t>(offset)) == static_cast<ssize_t>(bytes);
#endif
    }

    bool sync()
    {
#if defined(_WIN32)
        return FlushFileBuffers(m_handle) != 0;
#else
        return ::fsync(m_fd) == 0;
#endif
    }

    void close()
    {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

private:
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

// Removes the scratch folder; best-effort, since it runs on every exit
// path including the one where something already went wrong.
struct ScratchFolder
{
    fs::path path;
    explicit ScratchFolder(fs::path p) : path(std::move(p)) {}
    ~ScratchFolder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

domain::StickWriteMeasurement StickWriteProbe::run(const std::string &stickRoot,
                                                   const application::CancellationToken &cancel,
                                                   const WriteProbeOptions &options)
{
    if (stickRoot.empty()) {
        throw std::runtime_error("no stick root to write to");
    }
    fs::path root(stickRoot);
    std::error_code ec;
    auto space = fs::space(root, ec);
    if (ec) {
        throw std::runtime_error("cannot read free space on " + stickRoot + ": " + ec.message());
    }
    if (space.available < options.minimumFreeBytes) {
        throw std::runtime_error("less than 64 MiB free on the stick; not writing a test onto it");
    }

    fs::path scratch = root / kScratchFolderName;
    fs::remove_all(scratch, ec);  // a crashed earlier run
    ec.clear();
    fs::create_directories(scratch, ec);
    if (ec) {
        throw std::runtime_error("cannot create " + scratch.string() + ": " + ec.message());
    }
    ScratchFolder cleanup(scratch);

    domain::StickWriteMeasurement m;
    std::mt19937_64 rng(0x5EABA55u);

    // Streaming: a library export copying audio.
    {
        std::vector<char> chunk(1024 * 1024);
        std::uniform_int_distribution<int> byte(0, 255);
        for (auto &c : chunk) {
            c = static_cast<char>(byte(rng));  // not all zeros: some controllers compress those
        }
        std::uint64_t total = 0;
        auto start = Clock::now();
        for (int i = 0; i < options.streamingFiles; ++i) {
            cancel.throwIfCancelled();
            SyncedFile file(scratch / ("stream-" + std::to_string(i) + ".bin"), true);
            if (!file.ok()) {
                throw std::runtime_error("cannot create a file in " + scratch.string());
            }
            for (std::uint64_t offset = 0; offset < options.streamingBytesPerFile; offset += chunk.size()) {
                std::uint64_t bytes = std::min<std::uint64_t>(chunk.size(), options.streamingBytesPerFile - offset);
                if (!file.writeAt(offset, chunk.data(), bytes)) {
                    throw std::runtime_error("write failed in " + scratch.string());
                }
                total += bytes;
            }
            file.sync();
        }
        double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        m.streamingWriteBytesPerSecond = seconds > 0.0 ? static_cast<double>(total) / seconds : 0.0;
        m.bytesWritten += total;
    }

    // Small files: analysis files rewritten on save, created on export.
    {
        std::vector<char> payload(options.smallFileBytes, 'a');
        std::vector<double> latencies;
        auto groupStart = Clock::now();
        for (int i = 0; i < options.smallFiles; ++i) {
            if ((i & 0xf) == 0) {
                cancel.throwIfCancelled();
            }
            auto start = Clock::now();
            SyncedFile file(scratch / ("small-" + std::to_string(i) + ".dat"), true);
            if (!file.ok() || !file.writeAt(0, payload.data(), payload.size())) {
                continue;
            }
            file.sync();
            file.close();
            latencies.push_back(elapsedMs(start));
            m.bytesWritten += payload.size();
        }
        double seconds = std::chrono::duration<double>(Clock::now() - groupStart).count();
        m.smallFilesWritten = static_cast<int>(latencies.size());
        m.smallFileWriteMedianMs = median(latencies);
        m.smallFileWritesPerSecond = (seconds > 0.0 && !latencies.empty()) ? latencies.size() / seconds : 0.0;
    }

    // In place: a database page updated on save.
    {
        fs::path target = scratch / "inplace.db";
        {
            std::vector<char> fill(1024 * 1024, 'b');
            SyncedFile file(target, true);
            if (!file.ok()) {
                throw std::runtime_error("cannot create a file in " + scratch.string());
            }
            for (std::uint64_t offset = 0; offset < options.inPlaceFileBytes; offset += fill.size()) {
                file.writeAt(offset, fill.data(), std::min<std::uint64_t>(fill.size(), options.inPlaceFileBytes - offset));
            }
            file.sync();
            m.bytesWritten += options.inPlaceFileBytes;
        }
        std::vector<char> page(4096, 'c');
        std::uniform_int_distribution<std::uint64_t> slot(0, options.inPlaceFileBytes / page.size() - 1);
        std::vector<double> latencies;
        SyncedFile file(target, false);
        if (file.ok()) {
            for (int i = 0; i < options.inPlaceUpdates; ++i) {
                if ((i & 0xf) == 0) {
                    cancel.throwIfCancelled();
                }
                std::uint64_t offset = slot(rng) * page.size();
                auto start = Clock::now();
                if (!file.writeAt(offset, page.data(), page.size()) || !file.sync()) {
                    continue;
                }
                latencies.push_back(elapsedMs(start));
                m.bytesWritten += page.size();
            }
        }
        m.inPlaceUpdates = static_cast<int>(latencies.size());
        m.inPlaceUpdateMedianMs = median(latencies);
    }

    return m;
}

}  // namespace seabass::infrastructure::benchmark
