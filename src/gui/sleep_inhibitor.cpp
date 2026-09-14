// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/sleep_inhibitor.hpp"

#include <QtGlobal>

#include <cassert>
#include <mutex>

#if defined(Q_OS_WIN)
#include <windows.h>

#include <string>
#elif defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#endif

namespace seabass::gui
{

namespace
{

#if defined(Q_OS_LINUX)
// logind holds the inhibitor while this process holds the descriptor, and
// drops it the moment the descriptor closes -- including when the process
// dies, so a crash mid-write does not leave the machine unable to sleep.
class LogindBackend : public SleepInhibitor::Backend
{
public:
    bool acquire(const QString &why) override
    {
        QDBusConnection bus = QDBusConnection::systemBus();
        if (!bus.isConnected()) {
            qWarning("seabass: cannot keep the system awake: no D-Bus system bus");
            return false;
        }
        QDBusMessage call = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Inhibit"));
        // "sleep" only: logind refuses the suspend the desktop asks for at
        // its idle timeout while this is held. Not "idle" as well, which
        // desktops also read as "do not lock the screen" -- a long backup
        // left unattended should still lock.
        call << QStringLiteral("sleep") << QStringLiteral("Seabass") << why << QStringLiteral("block");
        // Bounded: this runs where an operation starts, and an unreachable
        // logind must cost a moment, not the operation.
        QDBusReply<QDBusUnixFileDescriptor> reply = bus.call(call, QDBus::Block, 3000);
        if (!reply.isValid()) {
            qWarning("seabass: cannot keep the system awake: %s", qPrintable(reply.error().message()));
            return false;
        }
        m_descriptor = reply.value();
        return m_descriptor.isValid();
    }

    void release() override { m_descriptor = QDBusUnixFileDescriptor(); }

private:
    QDBusUnixFileDescriptor m_descriptor;
};
#elif defined(Q_OS_WIN)
// A power request rather than SetThreadExecutionState: that one belongs to
// the thread that set it, and a hold is taken on one thread and let go on
// another.
class PowerRequestBackend : public SleepInhibitor::Backend
{
public:
    bool acquire(const QString &why) override
    {
        m_reason = why.toStdWString();
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = m_reason.data();
        HANDLE request = PowerCreateRequest(&context);
        if (request == INVALID_HANDLE_VALUE) {
            return false;
        }
        if (!PowerSetRequest(request, PowerRequestSystemRequired)) {
            CloseHandle(request);
            return false;
        }
        m_request = request;
        return true;
    }

    void release() override
    {
        if (m_request != nullptr) {
            PowerClearRequest(m_request, PowerRequestSystemRequired);
            CloseHandle(m_request);
            m_request = nullptr;
        }
    }

private:
    std::wstring m_reason;
    HANDLE m_request = nullptr;
};
#endif

class NoBackend : public SleepInhibitor::Backend
{
public:
    bool acquire(const QString &) override { return false; }
    void release() override {}
};

std::unique_ptr<SleepInhibitor::Backend> makeSystemBackend()
{
#if defined(Q_OS_LINUX)
    return std::make_unique<LogindBackend>();
#elif defined(Q_OS_WIN)
    return std::make_unique<PowerRequestBackend>();
#else
    return std::make_unique<NoBackend>();
#endif
}

struct State
{
    std::mutex mutex;
    std::unique_ptr<SleepInhibitor::Backend> backend = makeSystemBackend();
    int holds = 0;
    bool acquired = false;
};

State &state()
{
    static State s;
    return s;
}

}  // namespace

SleepInhibitor::Token SleepInhibitor::hold(const QString &why)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.holds == 0) {
        // One system inhibitor for every overlapping operation, named after
        // the first. A refusal is not retried until everything has let go.
        s.acquired = s.backend->acquire(why);
    }
    ++s.holds;
    return Token(new Hold());
}

SleepInhibitor::Hold::~Hold()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (--s.holds == 0 && s.acquired) {
        s.backend->release();
        s.acquired = false;
    }
}

int SleepInhibitor::activeHolds()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.holds;
}

bool SleepInhibitor::granted()
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.holds > 0 && s.acquired;
}

void SleepInhibitor::setBackendForTesting(std::unique_ptr<Backend> backend)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    assert(s.holds == 0 && "the backend can only be swapped while nothing is held");
    s.backend = backend ? std::move(backend) : std::make_unique<NoBackend>();
    s.acquired = false;
}

}  // namespace seabass::gui
