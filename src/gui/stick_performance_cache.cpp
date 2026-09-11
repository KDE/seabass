#include "gui/stick_performance_cache.hpp"

namespace seabass::gui
{

StickPerformanceCache &StickPerformanceCache::instance()
{
    static StickPerformanceCache cache;
    return cache;
}

void StickPerformanceCache::store(const std::string &stickIdentifier,
                                  const domain::StickPerformanceMeasurement &measurement)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_measurements[stickIdentifier] = measurement;
}

std::optional<domain::StickPerformanceMeasurement> StickPerformanceCache::lookup(const std::string &stickIdentifier) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_measurements.find(stickIdentifier);
    if (it == m_measurements.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace seabass::gui
