// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/logging/file_operation_log.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/utc_timestamp.hpp"
#include "seabass_version.hpp"

#include <chrono>
#include <filesystem>
#include <system_error>

namespace seabass::infrastructure::logging
{

namespace
{

std::string timestampNow()
{
    return utcTimestamp(std::chrono::system_clock::now(), "%Y-%m-%dT%H:%M:%S");
}

}  // namespace

std::string writerLine()
{
#if defined(_WIN32)
    const char *system = "Windows";
#elif defined(__APPLE__)
    const char *system = "macOS";
#elif defined(__linux__)
    const char *system = "Linux";
#else
    const char *system = "another system";
#endif
    std::string line = std::string("Seabass ") + version::Number + " (" + version::Channel;
    if (version::Commit[0] != '\0') {
        line += std::string(", ") + version::Commit;
    }
    return line + ") on " + system;
}

FileOperationLog::FileOperationLog(std::string logFilePath) : m_logFilePath(std::move(logFilePath)) {}

void FileOperationLog::record(const std::string &message)
{
    if (!m_out.is_open()) {
        // The log lives under <stick>/Seabass now: ofstream will not
        // create the directory, and an unopened log drops every line
        // without saying so.
        const std::filesystem::path file = pathFromUtf8(m_logFilePath);
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        m_out.open(file, std::ios::app);
        if (m_out) {
            m_out << timestampNow() << "  " << writerLine() << "\n";
        }
    }
    if (!m_out) {
        // The log is a record, not a gate: a stick that cannot be written
        // must not fail the save that was trying to describe itself.
        return;
    }
    m_out << timestampNow() << "  " << message << "\n";
    m_out.flush();
}

}  // namespace seabass::infrastructure::logging
