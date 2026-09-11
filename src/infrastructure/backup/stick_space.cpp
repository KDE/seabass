#include "infrastructure/backup/stick_space.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <system_error>

namespace seabass::infrastructure::backup
{

namespace fs = std::filesystem;

namespace
{

constexpr std::uint64_t OneGigabyte = 1024ull * 1024ull * 1024ull;

bool isAnalysisFile(const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::toupper(c); });
    // .DAT is the smaller, older analysis file; no write path in Seabass
    // touches it, so counting it would overstate the worst case.
    return ext == ".EXT";
}

// The catalogs a save can rewrite whole. Small next to the analysis
// files, but a save that only touches settings backs up exactly these.
std::uint64_t catalogBytes(const fs::path &stickRoot)
{
    static const char *const catalogs[] = {
        "PIONEER/rekordbox/export.pdb",
        "PIONEER/rekordbox/exportExt.pdb",
        "PIONEER/rekordbox/exportLibrary.db",
        "Engine Library/Database2/m.db",
    };
    std::uint64_t total = 0;
    std::error_code ec;
    for (const char *relative : catalogs) {
        const fs::path candidate = stickRoot / relative;
        if (fs::is_regular_file(candidate, ec)) {
            const auto size = fs::file_size(candidate, ec);
            if (!ec) {
                total += size;
            }
        }
    }
    return total;
}

}  // namespace

std::uint64_t StickSpace::headroomBytes() const
{
    return std::max<std::uint64_t>(OneGigabyte, capacityBytes / 50);  // 2%
}

bool StickSpace::backupGoesLocal() const
{
    if (capacityBytes == 0) {
        return false;  // nothing measured, so nothing to warn about
    }
    if (worstCaseBackupBytes >= freeBytes) {
        return true;  // it would not fit at all
    }
    return freeBytes - worstCaseBackupBytes < headroomBytes();
}

StickSpace measureStickSpace(const fs::path &stickRoot)
{
    StickSpace measured;
    std::error_code ec;
    if (stickRoot.empty() || !fs::is_directory(stickRoot, ec)) {
        return measured;
    }

    const auto space = fs::space(stickRoot, ec);
    if (ec) {
        return measured;  // an unreadable stick warns about nothing
    }
    measured.capacityBytes = space.capacity;
    // `available` rather than `free`: the latter counts space only root
    // may use, which nobody here can.
    measured.freeBytes = space.available;

    // Only the analysis tree is walked. It holds thousands of files on a
    // real stick and the whole point of measuring here is that it happens
    // while an edit page opens, so this stays a metadata-only walk of one
    // subtree rather than of the entire device.
    const fs::path analysisRoot = stickRoot / "PIONEER" / "USBANLZ";
    if (fs::is_directory(analysisRoot, ec)) {
        for (fs::recursive_directory_iterator it(analysisRoot, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !isAnalysisFile(it->path())) {
                continue;
            }
            const auto size = it->file_size(ec);
            if (!ec) {
                measured.worstCaseBackupBytes += size;
            }
        }
    }
    measured.worstCaseBackupBytes += catalogBytes(stickRoot);
    return measured;
}

namespace
{

struct AnalysisSizeCacheEntry
{
    std::uintmax_t worstCaseBackupBytes = 0;
    std::filesystem::file_time_type analysisMtime{};
    bool analysisPresent = false;
};

std::mutex g_analysisCacheMutex;
std::map<std::string, AnalysisSizeCacheEntry> g_analysisCache;

}  // namespace

StickSpace measureStickSpaceCached(const fs::path &stickRoot)
{
    StickSpace measured;
    std::error_code ec;
    if (stickRoot.empty() || !fs::is_directory(stickRoot, ec)) {
        return measured;
    }
    // Never cached: free space is what the warning is about, and a
    // remembered figure would go stale the moment anything was written.
    const auto space = fs::space(stickRoot, ec);
    if (ec) {
        return measured;
    }
    measured.capacityBytes = space.capacity;
    measured.freeBytes = space.available;

    const fs::path analysisRoot = stickRoot / "PIONEER" / "USBANLZ";
    std::error_code stampEc;
    const bool present = fs::is_directory(analysisRoot, stampEc);
    fs::file_time_type stamp{};
    if (present) {
        stamp = fs::last_write_time(analysisRoot, stampEc);
        if (stampEc) {
            // No usable stamp means no way to tell a stale entry from a
            // fresh one, so this walks rather than trusts.
            measured.worstCaseBackupBytes = measureStickSpace(stickRoot).worstCaseBackupBytes;
            return measured;
        }
    }

    const std::string key = stickRoot.string();
    {
        const std::lock_guard<std::mutex> lock(g_analysisCacheMutex);
        const auto it = g_analysisCache.find(key);
        if (it != g_analysisCache.end() && it->second.analysisPresent == present
            && it->second.analysisMtime == stamp) {
            measured.worstCaseBackupBytes = it->second.worstCaseBackupBytes;
            return measured;
        }
    }

    const std::uintmax_t walked = measureStickSpace(stickRoot).worstCaseBackupBytes;
    {
        const std::lock_guard<std::mutex> lock(g_analysisCacheMutex);
        g_analysisCache[key] = AnalysisSizeCacheEntry{walked, stamp, present};
    }
    measured.worstCaseBackupBytes = walked;
    return measured;
}

}  // namespace seabass::infrastructure::backup
