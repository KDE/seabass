// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace seabass::infrastructure
{

// `tp` in UTC, to the second, as strftime's `format` lays it out.
//
// Not std::format("{:%Y...}", tp): libc++ instantiates its floating-point
// formatter for any chrono value, and that calls std::to_chars(double),
// which Apple's libc++ has only since macOS 13.3. One timestamp was
// enough to make every Seabass binary refuse to start on macOS 12, which
// the Intel Macs that stop at Monterey cannot get past.
//
// Times from 1970-01-01 00:00:00 to 3000-12-31 23:59:59 UTC, on every
// platform: MSVC's gmtime_s converts nothing later, and nothing earlier
// than 1970. Outside them, or when the system's conversion fails, the
// text is empty.
std::string utcTimestamp(std::chrono::system_clock::time_point tp, const char *format);

// `unixSeconds` (seconds since 1970, UTC) as utcTimestamp() lays it out;
// nullopt for a time before 1970 or after 3000-12-31 23:59:59, or one the
// system cannot convert, so a number read from a file that is not a time
// cannot overflow the conversion.
std::optional<std::string> utcTimestampOfUnixSeconds(std::int64_t unixSeconds, const char *format);

}  // namespace seabass::infrastructure
