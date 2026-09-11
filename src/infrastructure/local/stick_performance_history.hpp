#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "domain/stick_performance.hpp"

namespace seabass::infrastructure::local
{

// One past measurement of a stick, kept on this computer so a stick that
// is getting slower can be told from one that was always slow. Small on
// purpose: a line of numbers per measurement, at most twenty per stick.
struct StickPerformanceRecord
{
    std::string measuredAtUtc;  // ISO 8601, "2026-09-11T16:52:00Z"
    std::string stickIdentifier;
    std::string stickLabel;
    int score = 0;
    double streamingBytesPerSecond = 0.0;
    double randomReadMedianMs = 0.0;
    double smallFileMedianMs = 0.0;
    int outliers = 0;  // random-read plus small-file tail outliers
    std::string wearState;  // "healthy", "watch", "failing", or "" when the wear check did not run
    double usbSpeedMbps = 0.0;  // the negotiated link of that run; 0 when unknown
};

// A plain text file, one record per line, tab-separated, under the app
// data directory (see app_data_directory.hpp). Written whole on every
// change; it is a few kilobytes.
class StickPerformanceHistory
{
public:
    static constexpr int kKeepPerStick = 20;

    explicit StickPerformanceHistory(std::filesystem::path path = defaultPath());

    // Oldest first.
    std::vector<StickPerformanceRecord> forStick(const std::string &stickIdentifier) const;

    // Appends, then drops the oldest beyond kKeepPerStick for that stick.
    void append(const StickPerformanceRecord &record);

    // Sets the wear state on the newest record for the stick, if any.
    void setLatestWearState(const std::string &stickIdentifier, const std::string &wearState);

    static std::filesystem::path defaultPath();

private:
    std::vector<StickPerformanceRecord> readAll() const;
    void writeAll(const std::vector<StickPerformanceRecord> &records) const;

    std::filesystem::path m_path;
};

}  // namespace seabass::infrastructure::local
