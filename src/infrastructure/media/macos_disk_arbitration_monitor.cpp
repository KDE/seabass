// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/macos_disk_arbitration_monitor.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <dispatch/dispatch.h>

namespace seabass::infrastructure::media
{

namespace
{

void diskAppeared(DADiskRef, void *context)
{
    static_cast<MacDiskArbitrationMonitor *>(context)->notify();
}

void diskDisappeared(DADiskRef, void *context)
{
    static_cast<MacDiskArbitrationMonitor *>(context)->notify();
}

void diskDescriptionChanged(DADiskRef, CFArrayRef, void *context)
{
    static_cast<MacDiskArbitrationMonitor *>(context)->notify();
}

DASessionRef sessionOf(void *session)
{
    return static_cast<DASessionRef>(session);
}

dispatch_queue_t queueOf(void *queue)
{
    return static_cast<dispatch_queue_t>(queue);
}

}  // namespace

MacDiskArbitrationMonitor::~MacDiskArbitrationMonitor()
{
    stop();
}

void MacDiskArbitrationMonitor::start(std::function<void()> onChange)
{
    if (m_running.exchange(true)) {
        return;
    }
    m_onChange = std::move(onChange);

    DASessionRef session = DASessionCreate(kCFAllocatorDefault);
    if (!session) {
        m_running = false;
        return;
    }
    m_session = session;
    m_queue = dispatch_queue_create("org.kde.seabass.diskarbitration", DISPATCH_QUEUE_SERIAL);

    // A null match dictionary means every disk: which ones matter is the
    // locator's decision, made again on every refresh.
    DARegisterDiskAppearedCallback(session, nullptr, diskAppeared, this);
    DARegisterDiskDisappearedCallback(session, nullptr, diskDisappeared, this);
    DARegisterDiskDescriptionChangedCallback(session, nullptr, kDADiskDescriptionWatchVolumePath,
                                             diskDescriptionChanged, this);
    DASessionSetDispatchQueue(session, queueOf(m_queue));
}

void MacDiskArbitrationMonitor::notify()
{
    if (m_running.load() && m_onChange) {
        m_onChange();
    }
}

void MacDiskArbitrationMonitor::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    DASessionRef session = sessionOf(m_session);
    // Unscheduling stops new callbacks being queued; the empty synchronous
    // block then waits out any already running or queued, so none can
    // reach m_onChange once stop() has returned.
    DASessionSetDispatchQueue(session, nullptr);
    dispatch_sync_f(queueOf(m_queue), nullptr, [](void *) {});
    DAUnregisterCallback(session, reinterpret_cast<void *>(diskAppeared), this);
    DAUnregisterCallback(session, reinterpret_cast<void *>(diskDisappeared), this);
    DAUnregisterCallback(session, reinterpret_cast<void *>(diskDescriptionChanged), this);
    CFRelease(session);
    dispatch_release(queueOf(m_queue));
    m_session = nullptr;
    m_queue = nullptr;
}

}  // namespace seabass::infrastructure::media
