// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A process named "rekordbox" that does nothing but wait, so the rig can
// prove the DJ-software guard refuses a write while one is running.
//
// Built rather than copied from /bin/sleep, which is how the rig used to
// make one: on macOS the system binaries are signed and arm64e, and a
// copy of one is killed the moment it starts (SIGKILL, exit 137), so the
// guard checks quietly proved nothing there. CMake names the binary
// "rekordbox", because the detector matches a process name.
#include <charconv>
#include <chrono>
#include <string_view>
#include <thread>

int main(int argc, char **argv)
{
    int seconds = 60;
    if (argc > 1) {
        const std::string_view given(argv[1]);
        std::from_chars(given.data(), given.data() + given.size(), seconds);
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    return 0;
}
