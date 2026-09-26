// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QCoreApplication>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>

#include <functional>
#include <memory>

#include "gui/edit/direct_write_hold.hpp"

namespace seabass::gui
{

// A write whose page is going away while it runs: the write is neither
// abandoned with its lock still held (the page's own finished handler
// will never run) nor waited for on the GUI thread (the page goes when
// the stick-gone dialog pops it, which is exactly when a write can be
// stuck on a device that is not there). It is watched from the
// application instead, and its locks are given back when it returns,
// whenever that is. See docs/async-requests.md, "Writes are different".
//
// `afterwards` is what the page's own finished handler would have done
// that does not need the page: telling the catalog cache the stick was
// rewritten, say. It runs before the locks are given back, so nothing can
// read the stick in between.
template <typename T>
void finishWriteDetached(const QFuture<T> &write, std::unique_ptr<DirectWriteHold> hold,
                         std::function<void()> afterwards = {})
{
    auto *watcher = new QFutureWatcher<T>(QCoreApplication::instance());
    std::shared_ptr<DirectWriteHold> held(std::move(hold));
    QObject::connect(watcher, &QFutureWatcherBase::finished, watcher, [watcher, held, afterwards]() {
        if (afterwards) {
            afterwards();
        }
        held->release();
        watcher->deleteLater();
    });
    watcher->setFuture(write);
}

}  // namespace seabass::gui
