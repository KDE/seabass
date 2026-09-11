// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/system/process_liveness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace seabass::infrastructure::system
{

#if defined(__linux__)

namespace
{

// /proc/<pid>/stat is "pid (comm) state ppid ...". comm may contain
// spaces and parentheses, so split after the *last* ')' -- from there on
// every field is a plain token, field 3 (state) first.
struct ProcStat
{
    char state = '\0';
    std::uint64_t startTime = 0;  // field 22
    bool ok = false;
};

ProcStat readProcStat(std::int64_t pid)
{
    ProcStat result;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in) {
        return result;
    }
    std::string line;
    std::getline(in, line);
    auto close = line.rfind(')');
    if (close == std::string::npos) {
        return result;
    }
    std::istringstream rest(line.substr(close + 1));
    std::vector<std::string> fields;
    std::string token;
    while (rest >> token) {
        fields.push_back(token);
    }
    // fields[0] is field 3 (state); field N is fields[N - 3].
    if (fields.size() < 20 || fields[0].empty()) {
        return result;
    }
    result.state = fields[0][0];
    try {
        result.startTime = std::stoull(fields[19]);
    } catch (const std::exception &) {
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace

std::int64_t currentPid()
{
    return static_cast<std::int64_t>(::getpid());
}

std::uint64_t currentProcessStartId()
{
    ProcStat self = readProcStat(currentPid());
    return self.ok ? self.startTime : 0;
}

bool isProcessAlive(std::int64_t pid, std::uint64_t startId)
{
    if (pid <= 0) {
        return false;
    }
    ProcStat stat = readProcStat(pid);
    if (!stat.ok) {
        return false;
    }
    if (stat.state == 'Z' || stat.state == 'X') {
        return false;  // a zombie holds nothing
    }
    if (startId != 0 && stat.startTime != 0 && stat.startTime != startId) {
        return false;  // the pid was recycled
    }
    return true;
}

#elif defined(_WIN32)

namespace
{

std::uint64_t creationTimeOf(HANDLE process)
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!::GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

}  // namespace

std::int64_t currentPid()
{
    return static_cast<std::int64_t>(::GetCurrentProcessId());
}

std::uint64_t currentProcessStartId()
{
    return creationTimeOf(::GetCurrentProcess());
}

bool isProcessAlive(std::int64_t pid, std::uint64_t startId)
{
    if (pid <= 0) {
        return false;
    }
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        // ERROR_ACCESS_DENIED means the process exists but belongs to
        // someone else -- alive as far as a lock is concerned.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    bool alive = ::GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    if (alive && startId != 0) {
        std::uint64_t creation = creationTimeOf(process);
        if (creation != 0 && creation != startId) {
            alive = false;
        }
    }
    ::CloseHandle(process);
    return alive;
}

#else

std::int64_t currentPid()
{
    return static_cast<std::int64_t>(::getpid());
}

std::uint64_t currentProcessStartId()
{
    return 0;
}

bool isProcessAlive(std::int64_t pid, std::uint64_t /*startId*/)
{
    if (pid <= 0) {
        return false;
    }
    // kill(pid, 0) sends nothing; it only reports whether the pid exists
    // (EPERM: exists, owned by someone else).
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

#endif

std::string hostName()
{
#if defined(_WIN32)
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(name);
    if (::GetComputerNameA(name, &size)) {
        return name;
    }
    return "windows";
#else
    char name[256] = {};
    if (::gethostname(name, sizeof(name) - 1) == 0 && name[0] != '\0') {
        return name;
    }
    return "localhost";
#endif
}

}  // namespace seabass::infrastructure::system
