// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QException>
#include <QString>

#include <exception>

namespace seabass::gui
{

// Reads a finished QFutureWatcher's result without letting an exception
// from the worker thread reach the event loop.
//
// QtConcurrent stores an exception thrown inside a run() body and
// rethrows it from QFutureWatcher::result(). That call happens in a slot
// on the GUI thread, where nothing catches it: Qt wraps it as
// QUnhandledException and the process calls std::terminate. A stick
// pulled mid-walk, an unreadable archive -- any std::filesystem_error on
// the background side -- therefore killed the whole app instead of
// showing an error (confirmed from a real crash dump: BackupStick::
// preview() threw, StickBackupController::onPreviewFinished() rethrew,
// terminate).
//
// On a throw this hands back a default-constructed result -- every
// caller already tolerates the "no result" case -- and puts the message
// in `error` for the controller to surface however it normally does.
//
// The message is the exception's own, not its wrapper's. QtConcurrent
// hands anything that is not a QException back as QUnhandledException,
// which does not override what(), so reading that said "std::exception"
// -- for every std::runtime_error and std::filesystem_error thrown on a
// worker thread in the app. A failed filesystem repair, for one,
// reported "std::exception" as its reason. The original is still inside
// it, and that is what is described.
inline QString describeException(const std::exception_ptr &thrown)
{
    if (!thrown) {
        return QStringLiteral("Unknown error");
    }
    try {
        std::rethrow_exception(thrown);
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    } catch (...) {
        return QStringLiteral("Unknown error");
    }
}

template <typename Watcher>
auto takeResult(Watcher &watcher, QString *error = nullptr) -> decltype(watcher.result())
{
    try {
        return watcher.result();
    } catch (const QUnhandledException &e) {
        // Before std::exception, which it derives from: see above.
        if (error != nullptr) {
            *error = describeException(e.exception());
        }
    } catch (const std::exception &e) {
        if (error != nullptr) {
            *error = QString::fromUtf8(e.what());
        }
    } catch (...) {
        if (error != nullptr) {
            *error = QStringLiteral("Unknown error");
        }
    }
    return {};
}

// Waits for a watcher's task to finish, swallowing an exception it
// stored. Destructors call this: QFutureWatcher::waitForFinished()
// rethrows a stored exception exactly like result() does, and a
// destructor is noexcept, so leaving a page while its background task
// had thrown called std::terminate straight from the unwinder (a real
// crash: closing the Full Stick Backup page after its preview threw).
// There is nowhere left to report an error to at this point -- the
// controller is going away and its page with it -- so the exception is
// deliberately dropped rather than surfaced.
template <typename Watcher>
void awaitQuietly(Watcher &watcher) noexcept
{
    try {
        watcher.waitForFinished();
    } catch (...) {
    }
}

}  // namespace seabass::gui
