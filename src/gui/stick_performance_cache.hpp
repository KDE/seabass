#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "domain/stick_performance.hpp"

namespace seabass::gui
{

// The last measurement taken on each stick this session, keyed by the
// stick identifier from StickHardwareInfo. In memory only, on purpose:
// nothing about a stick's speed is worth persisting (the port it is on
// changes it, and the old benchmark history was a table nobody read
// back). The Full Stick Backup page reads the streaming rate from here
// for its time estimate, when the stick has been measured since launch.
class StickPerformanceCache
{
public:
    static StickPerformanceCache &instance();

    void store(const std::string &stickIdentifier, const domain::StickPerformanceMeasurement &measurement);
    std::optional<domain::StickPerformanceMeasurement> lookup(const std::string &stickIdentifier) const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, domain::StickPerformanceMeasurement> m_measurements;
};

}  // namespace seabass::gui
