// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/removable_media_mounter.hpp"

namespace seabass::infrastructure::media
{

// Mounts, unmounts and ejects removable devices via macOS's "diskutil",
// which needs no administrator rights for removable media belonging to
// the logged-in user's session.
// What a refused diskutil eject or unmount says about who refused it.
// diskutil names the process: "Unmount was dissented by PID 20848
// (/Applications/rekordbox 7/rekordbox.app/Contents/MacOS/rekordbox)".
struct DiskutilRefusal
{
    std::string holder;     // the program's name ("rekordbox"); empty if none was named
    long pid = 0;
    bool lasting = false;   // a DJ program: it keeps the stick as long as it runs
    std::string message;    // what to tell the user
};

// Reads diskutil's output; `rawOutput` itself becomes the message when it
// names no process.
DiskutilRefusal describeDiskutilRefusal(const std::string &rawOutput);

class DiskutilMediaMounter : public application::RemovableMediaMounter
{
public:
    std::optional<std::string> mount(const std::string &devicePath, std::string &errorMessage) override;
    bool unmount(const std::string &devicePath, std::string &errorMessage) override;
    bool release(const std::string &devicePath, std::string &errorMessage) override;
    bool lastRefusalIsLasting() const override { return m_lastRefusalLasting; }
    std::string lastRefusalHolder() const override { return m_lastRefusalHolder; }

private:
    bool m_lastRefusalLasting = false;
    std::string m_lastRefusalHolder;
};

}  // namespace seabass::infrastructure::media
