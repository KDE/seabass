// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/logging/file_operation_log.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <system_error>

namespace seabass::infrastructure::logging
{

namespace
{

std::string timestampNow()
{
    return std::format("{:%Y-%m-%dT%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(
                                                     std::chrono::system_clock::now()));
}

}  // namespace

FileOperationLog::FileOperationLog(std::string logFilePath) : m_logFilePath(std::move(logFilePath)) {}

void FileOperationLog::record(const std::string &message)
{
    if (!m_out.is_open()) {
        // The log lives under <stick>/Seabass now: ofstream will not
        // create the directory, and an unopened log drops every line
        // without saying so.
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(m_logFilePath).parent_path(), ec);
        m_out.open(m_logFilePath, std::ios::app);
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
