// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>

#include <atomic>
#include <functional>

namespace seabass::gui
{

// Writes handed on by a page that went away (see detached_write.hpp),
// counted until what follows them -- giving the lock back, telling the
// cache -- has run. That runs on the GUI thread, from the event loop, and
// the end of the process has no event loop any more: settleDetachedWrites()
// runs it by hand, so a write that finished never leaves its lock behind.
class DetachedWrites
{
public:
    static std::atomic<int> &pending()
    {
        static std::atomic<int> count{0};
        return count;
    }
};

// Runs `atEnd` on the GUI thread once `write` has finished, whatever became
// of the page that started it.
template <typename T>
void whenWriteEnds(const QFuture<T> &write, std::function<void()> atEnd)
{
    ++DetachedWrites::pending();
    auto *watcher = new QFutureWatcher<T>(QCoreApplication::instance());
    QObject::connect(watcher, &QFutureWatcherBase::finished, watcher, [watcher, atEnd]() {
        if (atEnd) {
            atEnd();
        }
        watcher->deleteLater();
        --DetachedWrites::pending();
    });
    watcher->setFuture(write);
}

// After the writes themselves are done (the global pool waited for): the
// follow-ups their watchers have queued, run now. Loops until none is
// pending, since each is a finished signal already posted.
inline void settleDetachedWrites()
{
    while (DetachedWrites::pending().load() > 0) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

}  // namespace seabass::gui
