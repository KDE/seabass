// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <string>

#include "infrastructure/media/diskutil_media_mounter.hpp"

using seabass::infrastructure::media::describeDiskutilRefusal;

int main()
{
    // diskutil's own words, from an eject rekordbox refused on a real stick.
    const std::string rekordbox =
        "Unmount of disk5 failed: at least one volume could not be unmounted\n"
        "Unmount was dissented by PID 20848 (/Applications/rekordbox 7/rekordbox.app/Contents/MacOS/rekordbox)\n"
        "Dissenter parent PPID 1 (/sbin/launchd)\n";
    auto r = describeDiskutilRefusal(rekordbox);
    assert(r.holder == "rekordbox" && r.pid == 20848 && r.lasting);
    assert(r.message == "rekordbox has files on this stick open. Quit rekordbox, then eject again.");
    std::cout << "case (rekordbox: lasting, said plainly) OK\n";

    auto e = describeDiskutilRefusal(
        "Unmount of disk5 failed: at least one volume could not be unmounted\n"
        "Unmount was dissented by PID 31337 (/Applications/Engine DJ.app/Contents/MacOS/Engine DJ)\n");
    assert(e.holder == "Engine DJ" && e.lasting);
    assert(e.message == "Engine DJ has files on this stick open. Quit Engine DJ, then eject again.");
    std::cout << "case (Engine DJ, a name with a space) OK\n";

    // Anything else may let go on its own: not lasting, so the eject keeps
    // trying, but the message names it instead of quoting diskutil.
    auto other = describeDiskutilRefusal(
        "Unmount of disk5 failed: at least one volume could not be unmounted\n"
        "Unmount was dissented by PID 66383 (/Users/sebas/Seabass/builds/font-scale/src/gui/seabass.app/Contents/MacOS/seabass)\n"
        "Dissenter parent PPID 1 (/sbin/launchd)\n");
    assert(other.holder == "seabass" && other.pid == 66383 && !other.lasting);
    assert(other.message == "seabass (PID 66383) still has files on this stick open. Close it, then eject again.");
    std::cout << "case (another program: named, retried) OK\n";

    // No process named: diskutil's text as it is, and retried.
    const std::string plain = "Unmount of disk5 failed: at least one volume could not be unmounted\n";
    auto none = describeDiskutilRefusal(plain);
    assert(none.holder.empty() && !none.lasting && none.message == plain);
    std::cout << "case (no process named: unchanged) OK\n";
    return 0;
}
