// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>

#include "infrastructure/utc_timestamp.hpp"

using seabass::infrastructure::utcTimestamp;

// The text the std::format calls it replaced wrote, so backup directory
// names and log lines read the same as before.
int main()
{
    using namespace std::chrono;
    // 2026-10-07 06:50:59.750 UTC: the fraction is dropped, not rounded up.
    const system_clock::time_point tp = sys_days{year{2026} / 10 / 7} + hours{6} + minutes{50} + seconds{59} +
                                        milliseconds{750};
    assert(utcTimestamp(tp, "%Y-%m-%d") == "2026-10-07");
    assert(utcTimestamp(tp, "%Y%m%dT%H%M%S") == "20261007T065059");
    assert(utcTimestamp(tp, "%Y-%m-%dT%H:%M:%S") == "2026-10-07T06:50:59");
    // UTC whatever the zone: the epoch is midnight.
    assert(utcTimestamp(system_clock::time_point{}, "%H:%M:%S") == "00:00:00");
    std::cout << "utcTimestamp writes what std::format wrote, in UTC\n";

    // From seconds since 1970, and refused for a number that is no time,
    // never an overflow (9e18 seconds is past what a nanosecond clock
    // holds).
    using seabass::infrastructure::utcTimestampOfUnixSeconds;
    assert(utcTimestampOfUnixSeconds(0, "%Y-%m-%d %H:%M:%S") == "1970-01-01 00:00:00");
    assert(utcTimestampOfUnixSeconds(1791200000, "%Y-%m-%d") == "2026-10-05");
    assert(!utcTimestampOfUnixSeconds(9000000000000000000LL, "%Y"));
    assert(!utcTimestampOfUnixSeconds(-1, "%Y"));
    assert(!utcTimestampOfUnixSeconds(std::numeric_limits<std::int64_t>::max(), "%Y"));
    assert(!utcTimestampOfUnixSeconds(std::numeric_limits<std::int64_t>::min(), "%Y"));
    std::cout << "utcTimestampOfUnixSeconds refuses a number that is no time\n";

    // The last second MSVC's gmtime_s converts is the last on every
    // platform: 3000-12-31 23:59:59 UTC is shown, a second later is not,
    // and neither is the year 9999.
    const std::int64_t lastSecond = 32535215999;
    assert(utcTimestampOfUnixSeconds(lastSecond, "%Y-%m-%d %H:%M:%S") == "3000-12-31 23:59:59");
    assert(!utcTimestampOfUnixSeconds(lastSecond + 1, "%Y"));
    assert(!utcTimestampOfUnixSeconds(253402300799, "%Y"));
    // Not through the system clock: on Linux its nanoseconds end in 2262.
    assert(utcTimestampOfUnixSeconds(9223372037LL, "%Y") == "2262");
    // Before 1970 neither function gives a text (gmtime_s converts none).
    assert(utcTimestamp(system_clock::time_point{} - seconds{1}, "%Y").empty());
    std::cout << "times past 3000-12-31 23:59:59, and before 1970, have no text on any platform\n";
    return 0;
}
