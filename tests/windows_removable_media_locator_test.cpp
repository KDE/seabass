// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// WindowsRemovableMediaLocator against whatever drives this machine has.
// Since it also takes drives Windows calls fixed (external USB SSDs, and
// external drives holding a DJ library), what it must never do is list
// the drive Windows runs from or an internal disk, and it must stay quick:
// detect() runs on the UI thread at every drive change. Nothing here
// needs a stick plugged in; with one, it is listed and checked too.

#include <cassert>
#include <cctype>
#include <chrono>
#include <iostream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <string>

#include "infrastructure/media/windows_removable_media_locator.hpp"

int main()
{
    wchar_t windowsDirectory[MAX_PATH + 1] = {};
    const UINT length = ::GetWindowsDirectoryW(windowsDirectory, MAX_PATH + 1);
    assert(length > 0 && windowsDirectory[1] == L':');
    const char systemLetter = static_cast<char>(std::toupper(static_cast<unsigned char>(windowsDirectory[0])));

    // The disk Windows runs from: on a machine booted from a USB drive it
    // is a USB disk, and must still not be listed, as a bare disk either.
    std::string systemDiskPath;
    {
        const std::wstring volume = std::wstring(L"\\\\.\\") + windowsDirectory[0] + L":";
        HANDLE handle = ::CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                       nullptr);
        assert(handle != INVALID_HANDLE_VALUE);
        STORAGE_DEVICE_NUMBER number = {};
        DWORD bytesReturned = 0;
        if (::DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof(number),
                              &bytesReturned, nullptr)) {
            systemDiskPath = "\\\\.\\PhysicalDrive" + std::to_string(number.DeviceNumber);
        }
        ::CloseHandle(handle);
    }
    std::cout << "system disk: " << systemDiskPath << "\n";

    seabass::infrastructure::media::WindowsRemovableMediaLocator locator;
    const auto started = std::chrono::steady_clock::now();
    const auto sticks = locator.detect();
    const auto took =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "detect() listed " << sticks.size() << " drive(s) in " << took << " ms\n";

    for (const auto &stick : sticks) {
        std::cout << "  " << stick.label << "  mount=" << stick.mountPoint << "  disk=" << stick.wholeDiskPath
                  << "  reportedFixed=" << stick.reportedFixed
                  << "  library=" << (stick.rekordboxPath || stick.enginePath ? "yes" : "no") << "\n";
        if (!stick.mountPoint.empty()) {
            const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(stick.mountPoint[0])));
            assert(letter != systemLetter && "the drive Windows runs from is never a stick");
        }
        assert((systemDiskPath.empty() || stick.wholeDiskPath != systemDiskPath)
               && "nothing on the disk Windows runs from is a stick");
        // A fixed drive is listed for one of two reasons: it is a USB disk
        // (and then has its whole disk), or it holds a library.
        if (stick.reportedFixed) {
            assert(!stick.wholeDiskPath.empty() || stick.rekordboxPath || stick.enginePath
                   || stick.libraryAccessDenied);
        }
    }
    // Generous for a test machine under load; a normal run is a few ms.
    assert(took < 2000 && "detect() runs on the UI thread");
    std::cout << "all checks passed\n";
    return 0;
}
