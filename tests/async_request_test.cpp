// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// gui::AsyncRequest against the seven rules of docs/async-requests.md,
// one case per rule, with workers held at a latch so each case acts
// while a request is still outstanding. The controllers' own cases (the
// QML suite's tst_*Requests.qml) show each page wired to it; these show
// the helper keeps its promises on its own.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gui/async_request.hpp"
#include "gui/stick_events.hpp"

using seabass::application::CancellationToken;
using seabass::gui::AsyncRequest;
using seabass::gui::StickEvents;

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

// A gate a worker waits at until the test opens it; `honourCancel` false
// is a worker that cannot look at its token.
struct Latch
{
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    std::atomic<int> arrived{0};
    std::atomic<int> left{0};

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }

    void wait(const CancellationToken &cancel, bool honourCancel)
    {
        ++arrived;
        std::unique_lock<std::mutex> lock(mutex);
        while (!open && !(honourCancel && cancel.cancelled())) {
            cv.wait_for(lock, std::chrono::milliseconds(2));
        }
        ++left;
    }
};

// Runs the event loop until `done` or two seconds pass.
template <typename Done>
bool pumpUntil(Done done)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return done();
}

void pumpFor(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

struct Endings
{
    int results = 0;
    int errors = 0;
    int cancels = 0;
    int lastValue = -1;
    QString lastError;

    AsyncRequest<int>::Ending make()
    {
        return {
            [this](int &&value) {
                ++results;
                lastValue = value;
            },
            [this](const QString &message) {
                ++errors;
                lastError = message;
            },
            [this]() { ++cancels; },
        };
    }
    int total() const { return results + errors + cancels; }
};

void ruleOneAndTwoAResultEndsItOnce()
{
    QObject owner;
    int busyChanges = 0;
    AsyncRequest<int> request(&owner, [&] { ++busyChanges; });
    Endings endings;
    auto latch = std::make_shared<Latch>();
    request.start("a", {}, [latch](CancellationToken cancel) { latch->wait(cancel, true); return 7; }, endings.make());
    check(request.busy(), "rule 1: busy while outstanding");
    check(busyChanges == 1, "rule 1: busy announced once");
    latch->release();
    check(pumpUntil([&] { return !request.busy(); }), "rule 2: the request ends");
    check(endings.results == 1 && endings.total() == 1 && endings.lastValue == 7, "rule 2: one result, the right one");
    check(busyChanges == 2, "rule 1: not busy announced once");
}

void ruleTwoAThrowEndsInError()
{
    QObject owner;
    AsyncRequest<int> request(&owner, nullptr);
    Endings endings;
    request.start("a", {}, [](CancellationToken) -> int { throw std::runtime_error("the stick said no"); }, endings.make());
    check(pumpUntil([&] { return !request.busy(); }), "rule 2: a throwing worker ends the request");
    check(endings.errors == 1 && endings.total() == 1, "rule 2: in an error, once");
    check(endings.lastError == QStringLiteral("the stick said no"), "rule 2: carrying the worker's own message");
}

void ruleThreeTheSameKeyIsServed()
{
    QObject owner;
    AsyncRequest<int> request(&owner, nullptr);
    Endings first;
    Endings second;
    auto latch = std::make_shared<Latch>();
    std::atomic<int> runs{0};
    auto work = [latch, &runs](CancellationToken cancel) {
        ++runs;
        latch->wait(cancel, true);
        return 1;
    };
    check(request.start("a", {}, work, first.make()), "rule 3: the first request starts");
    check(!request.start("a", {}, work, second.make()), "rule 3: the same key again does not");
    latch->release();
    check(pumpUntil([&] { return !request.busy(); }), "rule 3: the one request ends");
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
    check(runs == 1, "rule 3: one worker ran");
    check(first.results == 1 && second.total() == 0, "rule 3: the running request answered");
}

void ruleFourAnotherKeySupersedes()
{
    QObject owner;
    int busyChanges = 0;
    AsyncRequest<int> request(&owner, [&] { ++busyChanges; });
    Endings first;
    Endings second;
    auto oldLatch = std::make_shared<Latch>();
    auto newLatch = std::make_shared<Latch>();
    request.start("a", {}, [oldLatch](CancellationToken cancel) { oldLatch->wait(cancel, false); return 1; },
                  first.make());
    request.start("b", {}, [newLatch](CancellationToken cancel) { newLatch->wait(cancel, false); return 2; },
                  second.make());
    check(request.busy() && request.key() == QStringLiteral("b"), "rule 4: the new request is the one outstanding");
    check(busyChanges == 1, "rule 4: busy stays on across the handover");
    oldLatch->release();
    pumpUntil([&] { return oldLatch->left.load() == 1; });
    pumpFor(50);
    check(request.busy(), "rule 4: the old worker's return does not end the new request");
    check(first.total() == 0, "rule 4: the old answer is swallowed");
    newLatch->release();
    check(pumpUntil([&] { return !request.busy(); }), "rule 4: the new request ends");
    check(second.results == 1 && second.lastValue == 2, "rule 4: with its own answer");
    check(first.total() == 0, "rule 4: and the old one never had one");
}

void ruleFourRestartSupersedesTheSameKey()
{
    QObject owner;
    AsyncRequest<int> request(&owner, nullptr);
    Endings first;
    Endings second;
    auto latch = std::make_shared<Latch>();
    std::atomic<int> runs{0};
    request.start("a", {}, [latch, &runs](CancellationToken cancel) { ++runs; latch->wait(cancel, true); return 1; },
                  first.make());
    request.restart("a", {}, [&runs](CancellationToken) { ++runs; return 2; }, second.make());
    check(pumpUntil([&] { return !request.busy(); }), "restart: the new request ends");
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
    check(runs == 2, "restart: a second worker ran for the same key");
    check(second.results == 1 && second.lastValue == 2 && first.total() == 0,
          "restart: the fresh answer, never the one read before");
    latch->release();
}

void ruleFiveCancelEndsAtOnce()
{
    QObject owner;
    AsyncRequest<int> request(&owner, nullptr);
    Endings endings;
    auto latch = std::make_shared<Latch>();
    request.start("a", {}, [latch](CancellationToken cancel) { latch->wait(cancel, false); return 1; }, endings.make());
    pumpUntil([&] { return latch->arrived.load() == 1; });
    request.cancel();
    check(!request.busy(), "rule 5: cancel ends the request at once, with the worker still stuck");
    check(endings.cancels == 1 && endings.total() == 1, "rule 5: as cancelled, once");
    latch->release();
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
    QCoreApplication::processEvents();
    check(endings.total() == 1, "rule 5: the worker's late answer is swallowed");
}

void ruleSixAPulledStickEndsItsRequest()
{
    QObject owner;
    AsyncRequest<int> onStick(&owner, nullptr);
    AsyncRequest<int> onOther(&owner, nullptr);
    Endings stickEndings;
    Endings otherEndings;
    auto latch = std::make_shared<Latch>();
    onStick.start("a", QStringLiteral("/media/STICK"),
                  [latch](CancellationToken cancel) { latch->wait(cancel, false); return 1; }, stickEndings.make());
    onOther.start("b", QStringLiteral("/media/OTHER"),
                  [latch](CancellationToken cancel) { latch->wait(cancel, false); return 2; }, otherEndings.make());
    StickEvents::instance().announceStickGone(QStringLiteral("/media/STICK"));
    check(!onStick.busy() && stickEndings.errors == 1, "rule 6: the pulled stick's request ends in an error");
    check(onOther.busy() && otherEndings.total() == 0, "rule 6: another stick's request carries on");
    latch->release();
    check(pumpUntil([&] { return !onOther.busy(); }), "rule 6: and ends on its own");
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
    QCoreApplication::processEvents();
    check(stickEndings.total() == 1, "rule 6: the pulled stick's late answer is swallowed");
}

void anEndingMayStartTheNextRequest()
{
    QObject owner;
    int busyChanges = 0;
    AsyncRequest<int> request(&owner, [&] { ++busyChanges; });
    int legs = 0;
    std::function<void()> next;
    next = [&] {
        request.start(QString::number(legs), {}, [](CancellationToken) { return 0; },
                      {[&](int &&) {
                           if (++legs < 3) {
                               next();
                           }
                       },
                       nullptr, nullptr});
    };
    next();
    check(pumpUntil([&] { return legs == 3 && !request.busy(); }), "chain: three legs, then idle");
    check(busyChanges == 2, "chain: busy never dips between legs");
}

void ruleSevenDestructionDoesNotWait()
{
    auto latch = std::make_shared<Latch>();
    auto token = std::make_shared<CancellationToken>();
    QElapsedTimer timer;
    {
        QObject owner;
        AsyncRequest<int> request(&owner, nullptr);
        request.start("a", {}, [latch, token](CancellationToken cancel) {
            *token = cancel;
            latch->wait(cancel, false);
            return 1;
        }, {});
        pumpUntil([&] { return latch->arrived.load() == 1; });
        timer.start();
    }
    check(timer.elapsed() < 1500, "rule 7: going away waits no longer than its bound for a worker that cannot notice");
    check(token->cancelled(), "rule 7: but it does tell the worker");
    latch->release();
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
}

void ruleSevenAWorkerThatNoticesIsWaitedFor()
{
    auto latch = std::make_shared<Latch>();
    {
        QObject owner;
        AsyncRequest<int> request(&owner, nullptr);
        request.start("a", {}, [latch](CancellationToken cancel) {
            latch->wait(cancel, true);
            return 1;
        }, {});
        pumpUntil([&] { return latch->arrived.load() == 1; });
    }
    check(latch->left.load() == 1, "rule 7: a worker that notices the cancel has stopped by the time it is gone");
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
}

// Workers let go of while stuck keep their threads. They must not take
// the writes' pool with them, nor leave the next read nowhere to run.
void letGoWorkersStarveNothing()
{
    auto latch = std::make_shared<Latch>();
    QObject owner;
    // More than any pool this helper ever had (max(32, 4 x cores)), so a
    // cap anywhere would leave the fresh read below without a thread.
    const int stuck = std::max(32, 4 * QThread::idealThreadCount()) + 8;
    std::vector<std::unique_ptr<AsyncRequest<int>>> requests;
    for (int i = 0; i < stuck; ++i) {
        requests.push_back(std::make_unique<AsyncRequest<int>>(&owner, nullptr));
        requests.back()->start("a", {}, [latch](CancellationToken cancel) { latch->wait(cancel, false); return 1; }, {});
        requests.back()->cancel();
    }
    pumpUntil([&] { return latch->arrived.load() == stuck; });
    check(latch->arrived.load() == stuck, "starvation: every stuck worker started");

    std::atomic<bool> globalRan{false};
    auto global = QtConcurrent::run([&globalRan] { globalRan = true; });
    check(pumpUntil([&] { return globalRan.load(); }), "starvation: a task on the global pool (a write) still runs");
    global.waitForFinished();

    AsyncRequest<int> fresh(&owner, nullptr);
    Endings endings;
    fresh.start("b", {}, [](CancellationToken) { return 5; }, endings.make());
    check(pumpUntil([&] { return endings.results == 1; }), "starvation: a new read still gets a thread");

    latch->release();
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
    requests.clear();
}

void ruleSixAnyOfSeveralSticks()
{
    QObject owner;
    AsyncRequest<int> request(&owner, nullptr);
    Endings endings;
    auto latch = std::make_shared<Latch>();
    request.startOnSticks("a", QStringList{QStringLiteral("/media/SOURCE"), QStringLiteral("/media/TARGET")},
                  [latch](CancellationToken cancel) { latch->wait(cancel, false); return 1; }, endings.make());
    StickEvents::instance().announceStickGone(QStringLiteral("/media/TARGET"));
    check(!request.busy() && endings.errors == 1, "rule 6: a request on two sticks ends when either goes");
    latch->release();
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
}

void aReporterSpeaksOnlyForItsRequest()
{
    QObject owner;
    AsyncRequest<int> request(&owner, nullptr);
    auto latch = std::make_shared<Latch>();
    auto work = [latch](CancellationToken cancel) { latch->wait(cancel, true); return 1; };
    const auto first = request.speaksForNext();
    request.start("a", {}, work, {});
    check(first(), "reporter: the request's own reporter speaks while it is outstanding");
    const auto served = request.speaksForNext();
    request.start("a", {}, work, {});
    check(!served() && first(), "reporter: a served request has no voice; the running one keeps its");
    const auto second = request.speaksForNext();
    request.start("b", {}, work, {});
    check(second() && !first(), "reporter: a superseded request falls silent");
    check(!served(), "reporter: a served request stays silent when the next one starts");
    request.cancel();
    check(!second(), "reporter: and so does a cancelled one");
    latch->release();
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
}

// A request the process cannot give a thread to ends in error, at once,
// instead of throwing into the page or waiting for good. Too many stuck
// workers is the case a test can make happen; a thread the system refuses
// takes the same path.
void noThreadEndsTheRequestInError()
{
    auto latch = std::make_shared<Latch>();
    QObject owner;
    auto &workers = seabass::gui::AsyncWorkers::instance();
    workers.waitForAll(std::chrono::seconds(30));
    workers.setCeilingForTesting(2);
    AsyncRequest<int> one(&owner, nullptr);
    AsyncRequest<int> two(&owner, nullptr);
    one.start("a", {}, [latch](CancellationToken cancel) { latch->wait(cancel, false); return 1; }, {});
    two.start("b", {}, [latch](CancellationToken cancel) { latch->wait(cancel, false); return 2; }, {});
    AsyncRequest<int> three(&owner, nullptr);
    Endings endings;
    three.start("c", {}, [](CancellationToken) { return 3; }, endings.make());
    check(pumpUntil([&] { return !three.busy(); }), "no thread: the request still ends");
    check(endings.errors == 1 && endings.lastError.contains(QStringLiteral("stuck")),
          "no thread: in an error that says why: " + endings.lastError.toStdString());
    workers.setCeilingForTesting(seabass::gui::AsyncWorkers::DefaultCeiling);
    latch->release();
    workers.waitForAll(std::chrono::seconds(30));
}

// "All done" means the workers' captures are gone too: a capture's
// destructor may still be running (a reporter, a reader's handle) when
// the work returns, and the end of the process must wait for it.
void allDoneMeansCapturesAreGone()
{
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    struct SlowToGo
    {
        std::shared_ptr<std::atomic<bool>> flag;
        ~SlowToGo()
        {
            if (flag) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                flag->store(true);
            }
        }
    };
    {
        QObject owner;
        AsyncRequest<int> request(&owner, nullptr);
        auto capture = std::make_shared<SlowToGo>();
        capture->flag = destroyed;
        request.start("a", {}, [capture](CancellationToken) { return 1; }, {});
        capture.reset();
        pumpUntil([&] { return !request.busy(); });
    }
    check(!destroyed->load(), "all done: the capture is still alive until the worker lets go of it");
    const bool allDone = seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(10));
    check(allDone && destroyed->load(), "all done: the work's captures were destroyed before it said so");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ruleOneAndTwoAResultEndsItOnce();
    ruleTwoAThrowEndsInError();
    ruleThreeTheSameKeyIsServed();
    ruleFourAnotherKeySupersedes();
    ruleFourRestartSupersedesTheSameKey();
    ruleFiveCancelEndsAtOnce();
    ruleSixAPulledStickEndsItsRequest();
    anEndingMayStartTheNextRequest();
    ruleSevenDestructionDoesNotWait();
    ruleSevenAWorkerThatNoticesIsWaitedFor();
    letGoWorkersStarveNothing();
    ruleSixAnyOfSeveralSticks();
    aReporterSpeaksOnlyForItsRequest();
    noThreadEndsTheRequestInError();
    allDoneMeansCapturesAreGone();
    seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "async_request_test: all rules hold\n";
    return 0;
}
