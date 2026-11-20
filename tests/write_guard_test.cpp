// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The write guard's refusal, from a process scan this test sets up itself:
// what the non-live suite cannot see any more since its binaries scan for
// nothing (tests/nothing_else_running.hpp). Each case names a scan, so the
// result says nothing about what else runs on the machine.

#include "gui/write_guard.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace
{

using seabass::infrastructure::system::setProcessScanForTesting;

bool nothing(const std::string &)
{
    return false;
}

bool rekordbox(const std::string &name)
{
    return name == "rekordbox";
}

bool engineDj(const std::string &name)
{
    return name == "Engine DJ";
}

bool everything(const std::string &)
{
    return true;
}

}  // namespace

int main()
{
    setProcessScanForTesting(&nothing);
    assert(seabass::gui::refuseIfDjSoftwareRunning().isEmpty());
    std::cout << "case 1 (nothing running: no refusal) OK\n";

    setProcessScanForTesting(&rekordbox);
    const QString rekordboxRefusal = seabass::gui::refuseIfDjSoftwareRunning();
    assert(rekordboxRefusal.startsWith(QStringLiteral("Refused: rekordbox appears to be running on this machine.")));
    std::cout << "case 2 (rekordbox running: refused, and named) OK\n";

    setProcessScanForTesting(&engineDj);
    const QString engineRefusal = seabass::gui::refuseIfDjSoftwareRunning();
    assert(engineRefusal.startsWith(QStringLiteral("Refused: Engine DJ appears to be running on this machine.")));
    std::cout << "case 3 (Engine DJ running: refused, and named) OK\n";

    // nullptr puts the real scan back: after a scan that saw everything,
    // a name no process has is not seen again.
    setProcessScanForTesting(&everything);
    assert(seabass::infrastructure::system::isProcessRunning("definitely-not-a-real-process-xyz123"));
    setProcessScanForTesting(nullptr);
    assert(!seabass::infrastructure::system::isProcessRunning("definitely-not-a-real-process-xyz123"));
    std::cout << "case 4 (nullptr restores the real scan) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
