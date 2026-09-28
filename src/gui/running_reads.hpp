// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace seabass::gui
{

// Every read running in the process, counted for the one moment that has
// to know: the end of the process (see endProcess()). AsyncWorkers counts
// the reads it runs here, and the catalog cache its prefetch passes, which
// run on a thread of the cache's own. Free of Qt, because the cache is
// built into tools that have none. Never destroyed, since a read let go of
// may outlive every static there is.
class RunningReads
{
public:
    static RunningReads &instance()
    {
        static RunningReads *reads = new RunningReads();
        return *reads;
    }

    // One more read, unless `ceiling` are running already.
    bool enterBelow(int ceiling)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_live >= ceiling) {
            return false;
        }
        ++m_live;
        return true;
    }

    // One more read, unless the process is ending: then none starts.
    bool enterUnlessEnding()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shuttingDown.load()) {
            return false;
        }
        ++m_live;
        return true;
    }

    void leave()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            --m_live;
        }
        m_cv.notify_all();
    }

    int live()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_live;
    }

    // True once no read is running, false if `timeout` passed first.
    bool waitForAll(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this] { return m_live == 0; });
    }

    void beginShutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shuttingDown.store(true);
    }
    bool shuttingDown() const { return m_shuttingDown.load(); }

private:
    RunningReads() = default;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    int m_live = 0;
    std::atomic<bool> m_shuttingDown{false};
};

}  // namespace seabass::gui
