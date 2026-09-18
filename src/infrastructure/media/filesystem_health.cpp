// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/filesystem_health.hpp"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

#ifdef __linux__
#include <fstream>
#include <sstream>
#endif

#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/param.h>
#endif

#include "infrastructure/process/run_command.hpp"

namespace seabass::infrastructure::media
{

namespace fs = std::filesystem;
using process::runCommand;

bool isMountedReadOnly(const std::string &path)
{
    if (path.empty()) {
        return false;
    }
#ifdef _WIN32
    // The volume the path sits on, which for a stick is its drive root.
    char root[MAX_PATH] = {};
    if (::GetVolumePathNameA(path.c_str(), root, MAX_PATH) == 0) {
        return false;
    }
    DWORD flags = 0;
    if (::GetVolumeInformationA(root, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) == 0) {
        return false;
    }
    return (flags & FILE_READ_ONLY_VOLUME) != 0;
#else
    struct statvfs info = {};
    if (::statvfs(path.c_str(), &info) != 0) {
        // Unreadable is not the same as read-only, and guessing either way
        // would put a wrong sentence in front of the user.
        return false;
    }
    return (info.f_flag & ST_RDONLY) != 0;
#endif
}

std::string deviceForMountPoint(const std::string &mountPoint)
{
    if (mountPoint.empty()) {
        return {};
    }
#if defined(_WIN32)
    char root[MAX_PATH] = {};
    if (::GetVolumePathNameA(mountPoint.c_str(), root, MAX_PATH) == 0) {
        return {};
    }
    std::string drive(root);
    // "E:\" -> "E:", which is what Repair-Volume's -DriveLetter wants
    // without its colon, and what the caller trims.
    while (!drive.empty() && (drive.back() == '\\' || drive.back() == '/')) {
        drive.pop_back();
    }
    return drive;
#elif defined(__APPLE__)
    struct statfs info = {};
    if (::statfs(mountPoint.c_str(), &info) != 0) {
        return {};
    }
    return info.f_mntfromname;  // "/dev/disk4s1"
#else
    // /proc/self/mounts, not /etc/mtab: the kernel's own view, and the one
    // that is right after a udisks2 mount this process never saw.
    std::ifstream mounts("/proc/self/mounts");
    std::string line;
    std::string best;
    std::string bestMount;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string device;
        std::string where;
        if (!(fields >> device >> where)) {
            continue;
        }
        // Mount points carry octal escapes for spaces ("\040").
        std::string decoded;
        for (size_t i = 0; i < where.size(); ++i) {
            if (where[i] == '\\' && i + 3 < where.size()) {
                decoded += static_cast<char>(std::stoi(where.substr(i + 1, 3), nullptr, 8));
                i += 3;
            } else {
                decoded += where[i];
            }
        }
        // The longest mount point that is a prefix of the path: a stick
        // mounted under /media/<user>/<label> sits inside "/" too.
        if (mountPoint.rfind(decoded, 0) == 0 && decoded.size() > bestMount.size()) {
            bestMount = decoded;
            best = device;
        }
    }
    return best;
#endif
}

namespace
{

#ifdef __linux__
// udisks2 addresses a block device by object path, the same shape the
// formatter builds for Block.Format.
std::string blockObjectPath(const std::string &devicePath)
{
    return "/org/freedesktop/UDisks2/block_devices/" + fs::path(devicePath).filename().string();
}
#endif

}  // namespace

FilesystemRepairResult repairFilesystem(const std::string &mountPoint)
{
    FilesystemRepairResult result;
    const std::string device = deviceForMountPoint(mountPoint);
    if (device.empty()) {
        result.message = "Seabass could not tell which drive " + mountPoint + " is, so it did not try to repair it.";
        return result;
    }

#if defined(__linux__)
    // udisks2 does the work and polkit asks the user: nothing here runs as
    // root, and Seabass never sees a password. Filesystem.Repair needs the
    // filesystem unmounted, so it goes down and comes back around it.
    const auto unmounted = runCommand({"udisksctl", "unmount", "-b", device});
    if (unmounted.exitCode != 0) {
        const bool declined = unmounted.output.find("Not authorized") != std::string::npos;
        result.declined = declined;
        result.message = declined
            ? "The permission prompt was declined, so nothing was checked."
            : "Seabass could not unmount the stick to check it: " + unmounted.output;
        return result;
    }

    const auto repaired = runCommand({"gdbus", "call", "--system", "--dest", "org.freedesktop.UDisks2",
                                       "--object-path", blockObjectPath(device), "--method",
                                       "org.freedesktop.UDisks2.Filesystem.Repair", "{}"});
    // Mounted again either way: a stick left unmounted by a failed repair
    // is a worse place to be than where the user started.
    const auto remounted = runCommand({"udisksctl", "mount", "-b", device});
    if (repaired.exitCode != 0) {
        const bool declined = repaired.output.find("NotAuthorized") != std::string::npos
            || repaired.output.find("Not authorized") != std::string::npos;
        result.declined = declined;
        result.message = declined ? "The permission prompt was declined, so nothing was checked."
                                  : "The check could not run: " + repaired.output;
        return result;
    }
    // Repair() answers with whether the filesystem is consistent now.
    result.repaired = repaired.output.find("true") != std::string::npos;
    result.message = result.repaired
        ? "The filesystem was checked and repaired."
        : "The check ran, but the filesystem is still damaged. A copy of what is on it, then a fresh format, is "
          "the way back from here."
          + std::string(remounted.exitCode == 0 ? "" : " The stick could not be mounted again either.");
    return result;
#elif defined(__APPLE__)
    // diskutil asks for authorisation itself when the volume needs it, and
    // handles unmounting around its own repair.
    const auto repaired = runCommand({"diskutil", "repairVolume", device});
    result.repaired = repaired.exitCode == 0;
    result.message = result.repaired ? "The filesystem was checked and repaired." : repaired.output;
    return result;
#elif defined(_WIN32)
    // Repair-Volume -OfflineScanAndFix is chkdsk /f, and wants an
    // administrator: the same UAC consent prompt formatting already uses,
    // never a password Seabass handles.
    std::string drive = device;
    if (!drive.empty() && drive.back() == ':') {
        drive.pop_back();
    }
    const std::string parameters = "-NoProfile -ExecutionPolicy Bypass -Command \"Repair-Volume -DriveLetter "
        + drive + " -OfflineScanAndFix\"";
    SHELLEXECUTEINFOA execInfo = {};
    execInfo.cbSize = sizeof(SHELLEXECUTEINFOA);
    execInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    execInfo.lpVerb = "runas";
    execInfo.lpFile = "powershell.exe";
    execInfo.lpParameters = parameters.c_str();
    execInfo.nShow = SW_HIDE;
    if (!::ShellExecuteExA(&execInfo) || execInfo.hProcess == nullptr) {
        const DWORD error = ::GetLastError();
        result.declined = error == ERROR_CANCELLED;
        result.message = result.declined
            ? "The Windows permission prompt was declined, so nothing was checked."
            : "Windows would not start the check (error " + std::to_string(error) + ").";
        return result;
    }
    ::WaitForSingleObject(execInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(execInfo.hProcess, &exitCode);
    ::CloseHandle(execInfo.hProcess);
    result.repaired = exitCode == 0;
    result.message = result.repaired ? "The filesystem was checked and repaired."
                                     : "The check ran but reported a problem it could not fix.";
    return result;
#else
    result.message = "Seabass cannot check a filesystem on this platform.";
    return result;
#endif
}

}  // namespace seabass::infrastructure::media
