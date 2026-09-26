// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "application/path_key.hpp"
#include "application/ports/cancellation_token.hpp"
#include "gui/future_result.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_events.hpp"

namespace seabass::gui
{

// The pool every AsyncRequest's worker runs on, apart from the global one
// the writes use. Rules 5 and 7 let go of a worker that cannot see its
// token, and such a worker keeps its thread until its I/O returns: on the
// global pool a hung stick and a few cancels would use up every thread,
// and every later task in the process -- a save among them -- would queue
// behind them without starting. Here they can only crowd other reads, and
// there is room for many. Never destroyed: a pool's destructor waits for
// its threads, and one of them may be stuck on a device that is gone.
inline QThreadPool &asyncRequestPool()
{
    static QThreadPool *pool = [] {
        auto *created = new QThreadPool();
        created->setMaxThreadCount(std::max(32, 4 * QThread::idealThreadCount()));
        return created;
    }();
    return *pool;
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
        // controller, so it can finish alone, and main() waits for the
        // pool before the process ends.
        const auto deadline = std::chrono::steady_clock::now() + LetGoAfter;
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
        if (busy() && key == m_currentKey) {
            return false;
        }
        restart(key, stickRoot, std::move(work), std::move(ending));
        return true;
    }

    // start() without rule 3: supersedes even a request for the same key.
    // For a caller that knows what the outstanding read saw is out of
    // date -- the rescan after its own write, or after an undo -- and
    // would otherwise be answered with the stick as it was before.
    void restart(const QString &key, const QString &stickRoot, Work work, Ending ending)
    {
        supersede();
        const std::uint64_t ticket = ++m_lastTicket;
        application::CancellationToken cancel;
        auto *watcher = new QFutureWatcher<Result>();
        auto done = std::make_shared<Done>();
        m_flights.push_back({ticket, cancel, watcher, done});
        m_current = ticket;
        m_currentKey = key;
        m_currentStick = stickRoot;
        m_ending = std::move(ending);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, m_owner,
                         [this, ticket]() { onFinished(ticket); });
        watcher->setFuture(QtConcurrent::run(&asyncRequestPool(), [work = std::move(work), cancel, done]() {
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
        m_currentStick.clear();
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
        m_currentStick.clear();
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
        if (!busy() || m_currentStick.isEmpty() || mountPoint.isEmpty()) {
            return;
        }
        if (application::pathIsAtOrUnder(pathToUtf8(pathFromQString(m_currentStick)),
                                         pathToUtf8(pathFromQString(mountPoint)))) {
            fail(QStringLiteral("The stick was removed while it was being read."));
        }
    }

    QPointer<QObject> m_owner;
    std::function<void()> m_busyChanged;
    QMetaObject::Connection m_stickConnection;
    std::vector<Flight> m_flights;
    std::uint64_t m_lastTicket = 0;
    std::uint64_t m_current = 0;
    QString m_currentKey;
    QString m_currentStick;
    Ending m_ending;
    bool m_reportedBusy = false;
};

}  // namespace seabass::gui
