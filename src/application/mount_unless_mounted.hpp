// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "application/ports/removable_media_locator.hpp"
#include "application/ports/removable_media_mounter.hpp"

namespace seabass::application
{

struct MountOutcome
{
    bool success = false;
    // Whether this call is what mounted it. Only a stick Seabass mounted
    // itself is Seabass's to unmount when it quits; one the system (or the
    // user) mounted stays mounted.
    bool mountedHere = false;
    std::string errorMessage;
};

// Mounts `devicePath` unless something else already has. A stick that is
// already mounted is a success and not ours: only a stick Seabass mounted
// itself is Seabass's to unmount when it quits.
//
// On macOS "diskutil mount" says yes whether it mounted the volume or found
// it mounted, so its success alone cannot tell the two apart; that is how a
// stick the system had mounted was counted as Seabass's and ejected when
// Seabass quit. Asking first settles it for every mount that is not racing
// another one, and on macOS nothing races: Seabass leaves the automatic
// mount to the system there (see MediaController::queueAutoMounts).
inline MountOutcome mountUnlessMounted(RemovableMediaLocator &locator, RemovableMediaMounter &mounter,
                                       const std::string &devicePath)
{
    bool present = false;
    for (const DetectedStick &stick : locator.detect()) {
        if (stick.devicePath == devicePath) {
            if (stick.mounted) {
                return {true, false, {}};
            }
            present = true;
            break;
        }
    }
    if (!present) {
        return {false, false, "The stick is no longer there."};
    }
    MountOutcome outcome;
    outcome.success = mounter.mount(devicePath, outcome.errorMessage).has_value();
    outcome.mountedHere = outcome.success;
    return outcome;
}

// Unmounts, and keeps trying for a few seconds before saying it cannot.
// A stick is often busy for a moment after the last thing that touched it
// (a file manager's thumbnailer, the desktop's indexer, this app's own
// reader letting go of a file): the first attempt is refused with "target
// is busy" and the same request two seconds later goes through. Reporting
// that first refusal gave an error for a stick that then unmounted fine
// when asked again. So: `attempts` tries, `pause` apart, and only the
// last refusal is reported. `sleep` is the test seam.
inline bool unmountPersistently(
    RemovableMediaMounter &mounter, const std::string &devicePath, std::string &errorMessage, int attempts = 8,
    std::chrono::milliseconds pause = std::chrono::milliseconds(750),
    const std::function<void(std::chrono::milliseconds)> &sleep = [](std::chrono::milliseconds d) {
        std::this_thread::sleep_for(d);
    })
{
    for (int attempt = 1;; ++attempt) {
        std::string error;
        if (mounter.unmount(devicePath, error)) {
            errorMessage.clear();
            return true;
        }
        errorMessage = error;
        if (attempt >= attempts) {
            return false;
        }
        sleep(pause);
    }
}

}  // namespace seabass::application
