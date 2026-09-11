// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "rekordbox_process_detector.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace seabass::infrastructure::system
{

#if defined(__linux__)

namespace
{

namespace fs = std::filesystem;

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isAllDigits(const std::string &s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace

// Reads /proc/<pid>/comm for every numeric entry under /proc -- the
// kernel's own record of each process's short name, truncated to 15
// bytes, which is why isRekordboxRunning() never needs to worry about
// matching a longer command line. Wrapped in a single try/catch rather
// than threading error_code through every filesystem call: any failure
// here (a process exiting mid-scan, a permission-denied /proc entry) just
// means "couldn't confirm," which this function already treats the same
// as "not found" -- see the header's doc comment.
bool isProcessRunning(const std::string &name)
{
    std::string wanted = toLower(name);
    try {
        for (const auto &entry : fs::directory_iterator("/proc")) {
            std::error_code ec;
            if (!entry.is_directory(ec) || ec) {
                continue;
            }
            std::string pid = entry.path().filename().string();
            if (!isAllDigits(pid)) {
                continue;
            }
            std::ifstream comm(entry.path() / "comm");
            if (!comm) {
                continue;  // process likely exited between listing and reading
            }
            std::string line;
            std::getline(comm, line);
            if (toLower(line) == wanted) {
                return true;
            }
        }
    } catch (const std::exception &) {
        return false;
    }
    return false;
}

#elif defined(_WIN32)

namespace
{

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

// Enumerates every running process via Toolhelp32Snapshot and compares
// each one's executable name (szExeFile, e.g. "rekordbox.exe") against
// name + ".exe", case-insensitively -- Windows process names always
// include the extension, unlike Linux's /proc/<pid>/comm.
bool isProcessRunning(const std::string &name)
{
    std::string wanted = toLower(name) + ".exe";

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    bool found = false;
    if (::Process32First(snapshot, &entry)) {
        do {
            if (toLower(entry.szExeFile) == wanted) {
                found = true;
                break;
            }
        } while (::Process32Next(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return found;
}

#else

// No detector implemented for this platform yet. Always reports "not
// running": never treat that as a guarantee either way, per the header's
// doc comment.
bool isProcessRunning(const std::string &)
{
    return false;
}

#endif

bool isRekordboxRunning()
{
    return isProcessRunning("rekordbox");
}

bool isEngineDjRunning()
{
    // "Engine DJ" is the current name (and the Windows executable, "Engine
    // DJ.exe"); "Engine Prime" was the previous one. Linux's 15-byte comm
    // truncation is not a concern for any of these.
    for (const char *name : {"Engine DJ", "EngineDJ", "Engine Prime", "EnginePrime", "Engine OS"}) {
        if (isProcessRunning(name)) {
            return true;
        }
    }
    return false;
}

bool isConflictingDjSoftwareRunning()
{
    return isRekordboxRunning() || isEngineDjRunning();
}

std::string conflictingDjSoftwareName()
{
    if (isRekordboxRunning()) {
        return "rekordbox";
    }
    if (isEngineDjRunning()) {
        return "Engine DJ";
    }
    return {};
}

}  // namespace seabass::infrastructure::system
