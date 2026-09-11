// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/zip_format.hpp"

#include <ctime>

namespace seabass::infrastructure::stick_backup::zip
{

DosDateTime dosDateTimeFromUnix(std::int64_t unixSeconds)
{
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tmVal{};
#if defined(_WIN32)
    gmtime_s(&tmVal, &t);
#else
    gmtime_r(&t, &tmVal);
#endif
    int year = tmVal.tm_year + 1900;
    DosDateTime result;
    if (year < 1980) {
        result.date = static_cast<std::uint16_t>((0 << 9) | (1 << 5) | 1);
        result.time = 0;
        return result;
    }
    if (year > 2107) {
        year = 2107;
    }
    result.date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((tmVal.tm_mon + 1) << 5) | tmVal.tm_mday);
    result.time = static_cast<std::uint16_t>((tmVal.tm_hour << 11) | (tmVal.tm_min << 5) | (tmVal.tm_sec / 2));
    return result;
}

std::int64_t unixFromDosDateTime(DosDateTime value)
{
    std::tm tmVal{};
    tmVal.tm_year = ((value.date >> 9) & 0x7f) + 1980 - 1900;
    tmVal.tm_mon = ((value.date >> 5) & 0x0f) - 1;
    tmVal.tm_mday = value.date & 0x1f;
    tmVal.tm_hour = (value.time >> 11) & 0x1f;
    tmVal.tm_min = (value.time >> 5) & 0x3f;
    tmVal.tm_sec = (value.time & 0x1f) * 2;
#if defined(_WIN32)
    return static_cast<std::int64_t>(_mkgmtime(&tmVal));
#else
    return static_cast<std::int64_t>(timegm(&tmVal));
#endif
}

}  // namespace seabass::infrastructure::stick_backup::zip
