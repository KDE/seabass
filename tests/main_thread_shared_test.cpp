// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// gui::makeMainThreadShared(): a QObject made on the GUI thread and shared
// with a worker is destroyed on the GUI thread, even when the worker drops
// the last reference. With a plain make_shared the reporter a controller
// hands its read went down on the worker, and ThreadSanitizer saw ~QObject
// race the GUI thread delivering the reporter's own queued signals.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "gui/async_request.hpp"
#include "gui/main_thread_shared.hpp"
#include "gui/qt_progress_reporter.hpp"

using seabass::application::CancellationToken;
using seabass::gui::AsyncRequest;
using seabass::gui::makeMainThreadShared;
using seabass::gui::QtProgressReporter;

namespace
{

int failures = 0;

void check(bool condition, const std::string &what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

bool pumpUntil(const std::function<bool()> &done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    return true;
}

// Where the reporter was destroyed, and whether it has been.
struct Grave
{
    std::atomic<QThread *> thread{nullptr};
    std::atomic<bool> destroyed{false};
};

class WatchedReporter : public QtProgressReporter
{
public:
    explicit WatchedReporter(std::shared_ptr<Grave> grave) : m_grave(std::move(grave)) {}
    ~WatchedReporter() override
    {
        m_grave->thread.store(QThread::currentThread());
        m_grave->destroyed.store(true);
    }

private:
    std::shared_ptr<Grave> m_grave;
};

void theLastReferenceOnItsOwnThreadDeletesAtOnce()
{
    auto grave = std::make_shared<Grave>();
    auto reporter = makeMainThreadShared<WatchedReporter>(grave);
    reporter.reset();
    // No event loop was needed: the GUI thread deleted it there and then.
    check(grave->destroyed.load(), "own thread: destroyed at once");
    check(grave->thread.load() == QThread::currentThread(), "own thread: destroyed on it");
}

void theLastReferenceOnAWorkerStillDeletesOnItsOwnThread()
{
    auto grave = std::make_shared<Grave>();
    auto reporter = makeMainThreadShared<WatchedReporter>(grave);
    QThread *worker = nullptr;
    std::thread thread([held = reporter, &worker]() mutable {
        worker = QThread::currentThread();
        held->start("reading", 3);
        held.reset();  // the last reference, once the GUI thread let go
    });
    reporter.reset();
    thread.join();
    check(grave->thread.load() != worker || worker == nullptr, "worker: not destroyed on the worker");
    check(pumpUntil([&] { return grave->destroyed.load(); }), "worker: destroyed once the GUI thread runs");
    check(grave->thread.load() == QThread::currentThread(), "worker: destroyed on the GUI thread");
}

// The route the controllers take: the reporter captured into a request's
// work, the controller's own reference gone before the worker returns, and
// the worker dropping the last one as it lets go of its captures.
void aReporterCapturedIntoARequestDiesOnTheGuiThread()
{
    auto grave = std::make_shared<Grave>();
    std::atomic<QThread *> worker{nullptr};
    std::mutex mutex;
    std::condition_variable cv;
    bool go = false;
    int ticks = 0;
    int result = 0;
    {
        QObject owner;
        AsyncRequest<int> request(&owner, nullptr);
        auto reporter = makeMainThreadShared<WatchedReporter>(grave);
        QObject::connect(reporter.get(), &QtProgressReporter::progressed, &owner, [&](int) { ++ticks; });
        request.start("a", {},
                      [reporter, &worker, &mutex, &cv, &go](CancellationToken) {
                          worker.store(QThread::currentThread());
                          std::unique_lock<std::mutex> lock(mutex);
                          cv.wait(lock, [&] { return go; });
                          reporter->tick(1);
                          return 7;
                      },
                      {[&](int &&value) { result = value; }, nullptr, nullptr});
        reporter.reset();
        {
            std::lock_guard<std::mutex> lock(mutex);
            go = true;
        }
        cv.notify_all();
        check(pumpUntil([&] { return !request.busy(); }), "request: ends");
        check(pumpUntil([&] { return grave->destroyed.load(); }), "request: the reporter is destroyed");
    }
    check(result == 7, "request: its result arrived");
    check(ticks == 1, "request: the reporter's signal was delivered");
    check(worker.load() != nullptr && grave->thread.load() != worker.load(), "request: not destroyed on the worker");
    check(grave->thread.load() == QThread::currentThread(), "request: destroyed on the GUI thread");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    theLastReferenceOnItsOwnThreadDeletesAtOnce();
    theLastReferenceOnAWorkerStillDeletesOnItsOwnThread();
    aReporterCapturedIntoARequestDiesOnTheGuiThread();
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "main_thread_shared_test: every object died on its own thread\n";
    return 0;
}
