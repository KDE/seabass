// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <filesystem>

namespace seabass::infrastructure
{

// Converts between std::filesystem's file clock and the system clock,
// returning each clock's own time_point type on every platform.
// std::chrono::clock_cast is the C++20 way, and what libstdc++ and MSVC
// use here; Apple's libc++ does not implement it (it reports
// __cpp_lib_chrono as 201611), but it does give file_clock the
// to_sys()/from_sys() pair, which is exact for these two clocks. MSVC's
// file_clock has only to_utc()/from_utc(), so neither spelling alone
// builds everywhere. The time_point_cast matters on libc++, whose
// system_clock counts microseconds while its file_clock counts
// nanoseconds in a 128-bit integer.
inline std::chrono::system_clock::time_point toSystemClock(std::filesystem::file_time_type time)
{
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    auto converted = std::chrono::clock_cast<std::chrono::system_clock>(time);
#else
    auto converted = std::filesystem::file_time_type::clock::to_sys(time);
#endif
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(converted);
}

inline std::filesystem::file_time_type toFileClock(std::chrono::system_clock::time_point time)
{
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    auto converted = std::chrono::clock_cast<std::filesystem::file_time_type::clock>(time);
#else
    auto converted = std::filesystem::file_time_type::clock::from_sys(time);
#endif
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(converted);
}

}  // namespace seabass::infrastructure
