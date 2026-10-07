// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
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
std::string utcTimestamp(std::chrono::system_clock::time_point tp, const char *format);

}  // namespace seabass::infrastructure
