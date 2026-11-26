// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/utc_timestamp.hpp"

#include <ctime>

namespace seabass::infrastructure
{

namespace
{

// 3000-12-31 23:59:59 UTC: the last second MSVC's gmtime_s converts (past
// it gmtime_s fails, and strftime given what it left aborts the program).
// The same limit on every platform, so a time is shown, or not, alike.
constexpr std::int64_t LastSecond =
    std::chrono::duration_cast<std::chrono::seconds>(
        (std::chrono::sys_days{std::chrono::year{3001} / 1 / 1} - std::chrono::seconds{1}).time_since_epoch())
        .count();
static_assert(LastSecond == 32535215999);

std::optional<std::string> formatted(std::int64_t unixSeconds, const char *format)
{
    if (unixSeconds < 0 || unixSeconds > LastSecond) {
        return std::nullopt;
    }
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &t) != 0) {
        return std::nullopt;
    }
#else
    if (gmtime_r(&t, &tm) == nullptr) {
        return std::nullopt;
    }
#endif
    char buffer[64];
    const std::size_t length = std::strftime(buffer, sizeof(buffer), format, &tm);
    return std::string(buffer, length);
}

}  // namespace

std::string utcTimestamp(std::chrono::system_clock::time_point tp, const char *format)
{
    const std::int64_t seconds =
        std::chrono::floor<std::chrono::seconds>(tp).time_since_epoch().count();
    return formatted(seconds, format).value_or(std::string());
}

std::optional<std::string> utcTimestampOfUnixSeconds(std::int64_t unixSeconds, const char *format)
{
    // Converted from the number itself, never through the system clock,
    // whose nanoseconds end in 2262 on Linux: no number can overflow it.
    return formatted(unixSeconds, format);
}

}  // namespace seabass::infrastructure
