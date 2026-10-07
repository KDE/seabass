// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <chrono>
#include <iostream>

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
    return 0;
}
