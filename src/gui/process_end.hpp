// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QThreadPool>
#include <QtGlobal>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "gui/async_request.hpp"
#include "gui/detached_completion.hpp"

namespace seabass::gui
{

// What main() does once the pages are gone (docs/async-requests.md, "The
// end of the process"):
//
//  1. The writes. Every write runs on the global pool, and nothing else
//     does (reads go to AsyncWorkers, see runRead()), so this waits for
//     the pool without limit: a write is never cut off. A line goes to
//     the log every 15 seconds while one still runs.
//  2. What finished writes left for the GUI thread: a handed-on write's
//     lock and cache follow-up (settleDetachedWrites()), so no lock is
//     left behind by the step below.
//  3. The reads, for `readBound` at most. One still running (stuck on a
//     pulled stick) may be inside the catalog cache or the readers under
//     it, so the process ends without running static destructors: flushed
//     output, then std::_Exit (std::quick_exit is missing from Apple's C
//     library). Nothing is lost -- a read writes nothing, and every write
//     is over by now.
inline int endProcess(int result, std::chrono::milliseconds readBound = std::chrono::milliseconds(2000))
{
    AsyncWorkers::instance().beginShutdown();
    while (!QThreadPool::globalInstance()->waitForDone(15000)) {
        qWarning("Seabass is still finishing a write to a stick; it will quit when the write is done.");
    }
    settleDetachedWrites();
    if (!AsyncWorkers::instance().waitForAll(readBound)) {
        std::fflush(nullptr);
        std::_Exit(result);
    }
    return result;
}

}  // namespace seabass::gui
