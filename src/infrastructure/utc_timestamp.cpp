// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/utc_timestamp.hpp"

#include <ctime>

namespace seabass::infrastructure
{

std::string utcTimestamp(std::chrono::system_clock::time_point tp, const char *format)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::floor<std::chrono::seconds>(tp));
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[64];
    const std::size_t length = std::strftime(buffer, sizeof(buffer), format, &tm);
    return std::string(buffer, length);
}

}  // namespace seabass::infrastructure
