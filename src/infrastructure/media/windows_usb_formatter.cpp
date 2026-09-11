// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/windows_usb_formatter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace seabass::infrastructure::media
{

namespace
{

// wholeDiskPath is always "\\.\PhysicalDriveN" -- constructed by
// WindowsRemovableMediaLocator itself, never a path this project accepts
// from anywhere else (see FormatUsbStick's own staleness check, which
// re-validates the path is still one of that locator's current results
// before this ever runs).
std::optional<int> parseDiskNumber(const std::string &wholeDiskPath)
{
    auto pos = wholeDiskPath.rfind("PhysicalDrive");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::string digits = wholeDiskPath.substr(pos + std::string("PhysicalDrive").size());
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }
    return std::stoi(digits);
}

// Escapes a string for embedding inside a single-quoted PowerShell string
// literal -- PowerShell's own escaping rule for that quoting style is
// simply doubling any embedded single quote.
std::string escapePowerShellSingleQuoted(const std::string &s)
{
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == '\'') {
            escaped.push_back('\'');
        }
        escaped.push_back(c);
    }
    return escaped;
}

std::string tempFilePath(const std::string &suffix)
{
    char tempDir[MAX_PATH + 1] = {};
    ::GetTempPathA(sizeof(tempDir), tempDir);
    auto uniquePart = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream oss;
    oss << tempDir << "seabass-format-" << ::GetCurrentProcessId() << "-" << uniquePart << suffix;
    return oss.str();
}

}  // namespace

std::optional<std::uint64_t> WindowsUsbFormatter::maxSizeFor(domain::UsbFilesystem fs) const
{
    if (fs == domain::UsbFilesystem::Fat32) {
        // Windows' own Format-Volume (like the format.com/GUI path it
        // wraps, and diskpart before it) refuses to create a FAT32 volume
        // over 32GB -- a Microsoft-imposed limit, not a filesystem one.
        // Verified as a known, documented Windows limitation; not yet
        // independently reproduced against a real >32GB stick on real
        // hardware (see the plan's "Real-hardware verification" section)
        // -- this ceiling exists specifically so FormatUsbStick refuses
        // that combination before Clear-Disk ever runs, rather than
        // discovering the failure after the disk is already wiped.
        constexpr std::uint64_t ThirtyTwoGb = 32ULL * 1024 * 1024 * 1024;
        return ThirtyTwoGb;
    }
    return std::nullopt;
}

bool WindowsUsbFormatter::format(const std::string &wholeDiskPath, domain::UsbFilesystem fsType,
                                  const std::string &volumeLabel, std::string &errorMessage,
                                  application::ProgressReporter &progress)
{
    auto diskNumber = parseDiskNumber(wholeDiskPath);
    if (!diskNumber) {
        errorMessage = "Internal error: not a recognized Windows physical drive path (" + wholeDiskPath + ")";
        return false;
    }

    progress.start("Formatting disk " + std::to_string(*diskNumber), 0);

    const std::string scriptPath = tempFilePath(".ps1");
    const std::string resultPath = tempFilePath(".result.txt");
    const std::string fsName = fsType == domain::UsbFilesystem::Fat32 ? "FAT32" : "exFAT";

    {
        std::ofstream script(scriptPath);
        script << "$ErrorActionPreference = 'Stop'\n";
        script << "try {\n";
        script << "    Clear-Disk -Number " << *diskNumber << " -RemoveData -RemoveOEM -Confirm:$false\n";
        // Set-Disk, not Initialize-Disk: Clear-Disk removes the
        // partitions but leaves the disk INITIALIZED (still GPT if it
        // was GPT before), so Initialize-Disk always fails afterwards
        // with "the disk has already been initialized" -- discovered by
        // running this against real hardware, after Clear-Disk had
        // already destroyed the previous layout. Set-Disk changes the
        // partition style of an already-initialized disk directly and is
        // what actually enforces this feature's "always MBR" promise.
        script << "    Set-Disk -Number " << *diskNumber << " -PartitionStyle MBR\n";
        // -AssignDriveLetter matters as much as the format itself here.
        // Without it Windows creates the partition but assigns no drive
        // letter, so a *successful* format hands the user a stick they
        // cannot open: it doesn't appear in Explorer, and this app's own
        // WindowsRemovableMediaLocator can't see it either -- detect()
        // reports devicePath empty, mounted false, and falls back to the
        // hardware model ("ASolid USB") instead of the volume label that
        // was just written. Confirmed on real hardware for both
        // filesystems; the drive letter had to be assigned by hand
        // afterwards to make the stick usable again.
        //
        // Unlike Linux, there's no separate mount step afterwards that
        // could recover this (see WindowsRemovableMediaMounter::mount(),
        // which only confirms a letter Windows already assigned) -- so if
        // it isn't requested here, nothing else will do it.
        script << "    $partition = New-Partition -DiskNumber " << *diskNumber
               << " -UseMaximumSize -AssignDriveLetter\n";
        script << "    Format-Volume -Partition $partition -FileSystem " << fsName << " -NewFileSystemLabel '"
               << escapePowerShellSingleQuoted(volumeLabel) << "' -Confirm:$false | Out-Null\n";
        script << "    Set-Content -Path '" << resultPath << "' -Value 'OK'\n";
        script << "} catch {\n";
        script << "    Set-Content -Path '" << resultPath << "' -Value ('ERROR: ' + $_.Exception.Message)\n";
        script << "}\n";
    }

    std::string parameters = "-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\"";

    SHELLEXECUTEINFOA execInfo = {};
    execInfo.cbSize = sizeof(SHELLEXECUTEINFOA);
    execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    execInfo.lpVerb = "runas";  // triggers the real UAC consent prompt -- see this class's own comment
    execInfo.lpFile = "powershell.exe";
    execInfo.lpParameters = parameters.c_str();
    execInfo.nShow = SW_HIDE;

    if (!::ShellExecuteExA(&execInfo) || execInfo.hProcess == nullptr) {
        DWORD err = ::GetLastError();
        progress.finish();
        std::remove(scriptPath.c_str());
        if (err == ERROR_CANCELLED) {
            errorMessage = "Formatting was cancelled at the Windows permission prompt.";
        } else {
            errorMessage = "Couldn't start the elevated formatting step (Windows error " + std::to_string(err) + ").";
        }
        return false;
    }

    ::WaitForSingleObject(execInfo.hProcess, INFINITE);
    ::CloseHandle(execInfo.hProcess);
    progress.finish();
    std::remove(scriptPath.c_str());

    std::ifstream resultFile(resultPath);
    std::string result;
    std::getline(resultFile, result);
    resultFile.close();
    std::remove(resultPath.c_str());

    if (result == "OK") {
        return true;
    }
    errorMessage = result.empty() ? "Formatting failed for an unknown reason (no result reported)." : result;
    return false;
}

}  // namespace seabass::infrastructure::media
