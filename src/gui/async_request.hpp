// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QPromise>
#include <QString>
#include <QStringList>

#include <chrono>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "application/path_key.hpp"
#include "application/ports/cancellation_token.hpp"
#include "gui/future_result.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_events.hpp"
#include "gui/stick_path.hpp"

namespace seabass::gui
{

// Where every AsyncRequest's worker runs: a thread of its own, not a pool.
// Rules 5 and 7 let go of a worker that cannot see its token, and such a
// worker keeps its thread until its I/O returns. In any pool with a cap --
// the global one the writes use, or one of its own -- enough of them on a
// hung stick would take every thread, and every later read (or save)
// would queue behind them and never start. A thread per request has no
// cap to reach; a page asks for a read when it opens or when the user
// picks something, so there are never many.
//
// It also counts the workers still running, for the one moment that has
// to know: the end of the process (see endProcess()). Never
// destroyed, since a worker let go may outlive every static there is.
class AsyncWorkers
{
public:
    static AsyncWorkers &instance()
    {
        static AsyncWorkers *workers = new AsyncWorkers();
        return *workers;
    }

    // `work` on a new thread, its answer (or what it threw) in the future.
    // Never throws: a thread that cannot be had -- the system refused one,
    // or too many are stuck already -- is a future that has already failed,
    // which the request then ends in error (rule 2).
    template <typename Result>
    QFuture<Result> run(std::function<Result()> work)
    {
        auto promise = std::make_shared<QPromise<Result>>();
        QFuture<Result> future = promise->future();
        promise->start();
        const auto refuse = [&promise](const std::exception_ptr &why) {
            promise->setException(why);
            promise->finish();
        };
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_live >= m_ceiling) {
                refuse(std::make_exception_ptr(std::runtime_error(
                    "Too many reads are stuck on a stick that stopped answering. Unplug it, or restart Seabass.")));
                return future;
            }
            ++m_live;
        }
        try {
            // The thread owns the only other reference to the work and the
            // promise, and lets go of both before it says it is done, so
            // waitForAll() never answers while a destructor of theirs runs.
            auto state = std::make_shared<std::pair<std::shared_ptr<QPromise<Result>>, std::function<Result()>>>(
                promise, std::move(work));
            std::thread([this, state]() mutable {
                {
                    auto owned = std::move(*state);
                    state.reset();
                    try {
                        owned.first->addResult(owned.second());
                    } catch (...) {
                        owned.first->setException(std::current_exception());
                    }
                    owned.first->finish();
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    --m_live;
                }
                m_cv.notify_all();
            }).detach();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_live;
            }
            m_cv.notify_all();
            refuse(std::current_exception());
        }
        return future;
    }

    // How many workers may be running at once before a new one is refused.
    // A page asks for a handful; hundreds means reads are stuck on a device
    // that stopped answering, and one more thread would not help.
    static constexpr int DefaultCeiling = 256;
    void setCeilingForTesting(int ceiling)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ceiling = ceiling;
    }

    int live()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_live;
    }

    // True once no worker is running, false if `timeout` passed first.
    bool waitForAll(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this] { return m_live == 0; });
    }

    // From here on a request going away does not wait for its worker at
    // all: the process is ending, every page is being torn down one after
    // the other, and endProcess() waits once for all of them.
    void beginShutdown() { m_shuttingDown.store(true); }
    bool shuttingDown() const { return m_shuttingDown.load(); }

private:
    AsyncWorkers() = default;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    int m_live = 0;
    int m_ceiling = DefaultCeiling;
    std::atomic<bool> m_shuttingDown{false};
};

// A read that is not a page's request (a folder listing, a mount, a
// process list) run the same way: a thread of its own, counted with the
// rest, never on the global pool. The global pool is for writes, and the
// end of the process waits for those without limit; a read stuck on a
// pulled stick must not be among them.
template <typename F>
auto runRead(F work) -> QFuture<std::invoke_result_t<F>>
{
    using Result = std::invoke_result_t<F>;
    return AsyncWorkers::instance().run<Result>(std::function<Result()>(std::move(work)));
}


// One read a page is waiting for, run on the thread pool, under the rule
// docs/async-requests.md writes down. In short:
//
//  1. busy() is derived: true exactly while a request is outstanding.
//     There is no flag beside it to forget, and no isRunning().
//  2. Every request ends exactly once, on the owner's thread, in one of
//     result / error / cancelled. A worker that throws ends in error.
//  3. A request for the key already outstanding is served by that one.
//  4. A request for another key supersedes it: the old token is
//     cancelled and whatever the old worker returns is swallowed.
//  5. cancel() ends the request at once; the worker is told and its late
//     answer swallowed.
//  6. A stick going away ends a request whose key is on it, in error.
//  7. Destruction cancels everything and waits a bounded moment for the
//     workers to stop; one that has not by then is let go. A worker never
//     touches its controller, so it finishes alone and its answer goes
//     nowhere.
//
// Reads only. A write's cancel has to be acknowledged by the writer --
// a half-done write cannot be declared over -- so writes keep their own
// flow (see docs/write-path-rules.md).
//
// Every request gets a watcher of its own rather than reusing one with
// setFuture(): replacing a watcher's future drops a finished signal that
// is already on its way, which is exactly the kind of lost "done" this
// class exists to rule out.
template <typename Result>
class AsyncRequest
{
public:
    using Work = std::function<Result(application::CancellationToken cancel)>;

    struct Ending
    {
        std::function<void(Result &&)> result;
        std::function<void(const QString &)> error;
        std::function<void()> cancelled;
    };

    // owner: the controller; endings run on its thread and never after it
    // is gone. busyChanged: called whenever busy() changes, and only then.
    AsyncRequest(QObject *owner, std::function<void()> busyChanged)
        : m_owner(owner), m_busyChanged(std::move(busyChanged))
    {
        m_stickConnection = QObject::connect(&StickEvents::instance(), &StickEvents::stickGone, owner,
                                             [this](const QString &mountPoint) { onStickGone(mountPoint); });
    }

    ~AsyncRequest()
    {
        QObject::disconnect(m_stickConnection);
        for (Flight &flight : m_flights) {
            flight.cancel.cancel();
        }
        // Waited for, but only so long. A worker that honours its token
        // stops within a row, so ordinarily nothing is left reading a
        // stick for a page that has gone. One that cannot notice (a
        // preview with no token, a read stuck on a device) is let go
        // after the bound rather than freezing the window: it holds its
        // token, its arguments and the process-wide cache, never its
        // controller, so it can finish alone; endProcess() sees to
        // the end of the process. While the process is ending it waits
        // for nothing: that function waits once for every worker.
        const auto deadline = std::chrono::steady_clock::now()
            + (AsyncWorkers::instance().shuttingDown() ? std::chrono::milliseconds(0) : LetGoAfter);
        for (Flight &flight : m_flights) {
            std::unique_lock<std::mutex> lock(flight.done->mutex);
            flight.done->cv.wait_until(lock, deadline, [&] { return flight.done->finished; });
        }
        for (Flight &flight : m_flights) {
            QObject::disconnect(flight.watcher, nullptr, nullptr, nullptr);
            delete flight.watcher;
        }
    }

    AsyncRequest(const AsyncRequest &) = delete;
    AsyncRequest &operator=(const AsyncRequest &) = delete;

    bool busy() const { return m_current != 0; }
    // The key of the outstanding request, or empty.
    QString key() const { return busy() ? m_currentKey : QString(); }

    // Starts `work` for `key`, or leaves the outstanding request for the
    // same key to answer (rule 3) and returns false. `stickRoot`, when
    // not empty, is the stick the work reads: a stick going away at or
    // above it ends the request (rule 6).
    bool start(const QString &key, const QString &stickRoot, Work work, Ending ending)
    {
        return startOnSticks(key, QStringList{stickRoot}, std::move(work), std::move(ending));
    }
    // A request reading more than one stick (a clone's source and target):
    // any of them going away ends it.
    bool startOnSticks(const QString &key, const QStringList &stickRoots, Work work, Ending ending)
    {
        if (busy() && key == m_currentKey) {
            // Served: a reporter taken for this request never speaks.
            m_nextVoice.reset();
            return false;
        }
        restartOnSticks(key, stickRoots, std::move(work), std::move(ending));
        return true;
    }

    // Whether the request about to be started is still the outstanding one,
    // for its progress reporter: a superseded or cancelled read goes on
    // reporting until its worker notices, and must not move the bar. Take
    // it right before start() or restart(); it is never true for a request
    // that was served by the running one instead.
    std::function<bool()> speaksForNext()
    {
        // One voice at a time, taken right before the start it belongs to:
        // a second one taken first would leave the earlier one mute for
        // good without anyone knowing.
        Q_ASSERT_X(!m_nextVoice, "AsyncRequest::speaksForNext", "a voice was taken for a request never started");
        m_nextVoice = std::make_shared<std::uint64_t>(0);
        return [this, voice = m_nextVoice]() { return *voice != 0 && m_current == *voice; };
    }

    // start() without rule 3: supersedes even a request for the same key.
    // For a caller that knows what the outstanding read saw is out of
    // date -- the rescan after its own write, or after an undo -- and
    // would otherwise be answered with the stick as it was before.
    void restart(const QString &key, const QString &stickRoot, Work work, Ending ending)
    {
        restartOnSticks(key, QStringList{stickRoot}, std::move(work), std::move(ending));
    }
    void restartOnSticks(const QString &key, const QStringList &stickRoots, Work work, Ending ending)
    {
        supersede();
        const std::uint64_t ticket = ++m_lastTicket;
        if (m_nextVoice) {
            *m_nextVoice = ticket;
            m_nextVoice.reset();
        }
        application::CancellationToken cancel;
        auto *watcher = new QFutureWatcher<Result>();
        auto done = std::make_shared<Done>();
        m_flights.push_back({ticket, cancel, watcher, done});
        m_current = ticket;
        m_currentKey = key;
        m_currentSticks = stickRoots;
        m_currentSticks.removeAll(QString());
        m_ending = std::move(ending);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, m_owner,
                         [this, ticket]() { onFinished(ticket); });
        watcher->setFuture(AsyncWorkers::instance().run<Result>([work = std::move(work), cancel, done]() {
            struct SayDone
            {
                Done &done;
                ~SayDone()
                {
                    {
                        std::lock_guard<std::mutex> lock(done.mutex);
                        done.finished = true;
                    }
                    done.cv.notify_all();
                }
            } sayDone{*done};
            return work(cancel);
        }));
        if (watcher->future().isFinished()) {
            // Refused a thread (see AsyncWorkers::run): no worker will ever
            // say it is done, so nobody must wait for one.
            std::lock_guard<std::mutex> lock(done->mutex);
            done->finished = true;
        }
        report();
    }

    // Rule 5: ends the outstanding request now, as cancelled.
    void cancel() { endCurrent(Outcome::Cancelled, {}); }

    // Ends the outstanding request now, in error, as though its worker
    // had failed with `message`.
    void fail(const QString &message) { endCurrent(Outcome::Error, message); }

private:
    enum class Outcome { Cancelled, Error };

    // How long going away waits for a cancelled worker before letting it
    // go. Readers stop within a row, well inside this; OneLibrary's key
    // derivation, the one step no token can cut, takes a few hundred
    // milliseconds on a loaded machine.
    static constexpr std::chrono::milliseconds LetGoAfter{1000};

    // Set by the worker as it returns, whatever it returns with.
    struct Done
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
    };

    struct Flight
    {
        std::uint64_t ticket = 0;
        application::CancellationToken cancel;
        QFutureWatcher<Result> *watcher = nullptr;
        std::shared_ptr<Done> done;
    };

    void report()
    {
        if (m_reportedBusy != busy()) {
            m_reportedBusy = busy();
            if (m_busyChanged) {
                m_busyChanged();
            }
        }
    }

    Flight *flightFor(std::uint64_t ticket)
    {
        for (Flight &flight : m_flights) {
            if (flight.ticket == ticket) {
                return &flight;
            }
        }
        return nullptr;
    }

    // The outstanding request stops being anyone's: its worker is told,
    // and whatever it returns is swallowed when it lands.
    void supersede()
    {
        if (!busy()) {
            return;
        }
        if (Flight *flight = flightFor(m_current)) {
            flight->cancel.cancel();
        }
        m_current = 0;
        m_currentKey.clear();
        m_currentSticks.clear();
    }

    void endCurrent(Outcome outcome, const QString &message)
    {
        if (!busy()) {
            return;
        }
        Ending ending = std::move(m_ending);
        m_ending = {};
        supersede();
        // An ending may start the next request, so busy is reported
        // after it rather than flickering off and on around it.
        if (outcome == Outcome::Cancelled) {
            if (ending.cancelled) {
                ending.cancelled();
            }
        } else if (ending.error) {
            ending.error(message);
        }
        report();
    }

    void onFinished(std::uint64_t ticket)
    {
        QFutureWatcher<Result> *watcher = nullptr;
        for (auto it = m_flights.begin(); it != m_flights.end(); ++it) {
            if (it->ticket == ticket) {
                watcher = it->watcher;
                m_flights.erase(it);
                break;
            }
        }
        if (watcher == nullptr) {
            return;
        }
        QString thrown;
        Result result = takeResult(*watcher, &thrown);
        watcher->deleteLater();
        if (ticket != m_current) {
            return;  // superseded, cancelled or failed already: swallowed
        }
        Ending ending = std::move(m_ending);
        m_ending = {};
        m_current = 0;
        m_currentKey.clear();
        m_currentSticks.clear();
        if (!thrown.isEmpty()) {
            if (ending.error) {
                ending.error(thrown);
            }
        } else if (ending.result) {
            ending.result(std::move(result));
        }
        report();
    }

    void onStickGone(const QString &mountPoint)
    {
        if (!busy() || mountPoint.isEmpty()) {
            return;
        }
        for (const QString &stick : m_currentSticks) {
            if (application::pathIsAtOrUnder(pathToUtf8(pathFromQString(stick)), pathToUtf8(pathFromQString(mountPoint)))) {
                fail(QStringLiteral("The stick was removed while it was being read."));
                return;
            }
        }
    }

    QPointer<QObject> m_owner;
    std::function<void()> m_busyChanged;
    QMetaObject::Connection m_stickConnection;
    std::vector<Flight> m_flights;
    std::uint64_t m_lastTicket = 0;
    std::uint64_t m_current = 0;
    QString m_currentKey;
    QStringList m_currentSticks;
    Ending m_ending;
    bool m_reportedBusy = false;
    // The ticket slot handed out by speaksForNext(), filled by the start
    // that follows it (or left empty for good if that start was served).
    std::shared_ptr<std::uint64_t> m_nextVoice;
};

}  // namespace seabass::gui
