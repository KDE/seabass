// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QThread>

#include <memory>
#include <type_traits>
#include <utility>

namespace seabass::gui
{

// A QObject shared with work on another thread, which is still destroyed on
// the thread it lives on. A progress reporter (or ScanPhaseRelay) is made on
// the GUI thread and captured into the work a worker runs; the worker lets go
// of its captures when the work returns, and that is usually the last
// reference. With a plain make_shared the object is then destroyed on the
// worker, which Qt does not support: ~QObject runs beside the GUI thread
// delivering the signals it queued, and ThreadSanitizer reported it 37 times
// in one run of the QML suite.
//
// The last reference dropped on the object's own thread deletes it there and
// then. Dropped anywhere else, it is handed to deleteLater(), which posts the
// deletion to the object's thread.
//
// When no event loop runs on that thread again -- the process is ending, or a
// test that never spins one -- the posted deletion may never happen, and the
// object is left behind: a leak at exit, and harmless, since nothing is
// connected to it any more once its controller is gone and no worker holds
// it. That is better than the alternative, a destructor racing the GUI
// thread while the statics go.
template <typename T, typename... Args>
std::shared_ptr<T> makeMainThreadShared(Args &&...args)
{
    static_assert(std::is_base_of_v<QObject, T>, "only a QObject has a thread of its own to be destroyed on");
    return std::shared_ptr<T>(new T(std::forward<Args>(args)...), [](T *object) {
        if (object->thread() == QThread::currentThread()) {
            delete object;
        } else {
            object->deleteLater();
        }
    });
}

}  // namespace seabass::gui
