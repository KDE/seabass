// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "dj_software_guard_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/future_result.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"

namespace seabass::gui
{

namespace
{
constexpr int EditModeIntervalMs = 5000;
constexpr int DialogOpenIntervalMs = 1000;

QString detectConflictingSoftware()
{
    return QString::fromStdString(infrastructure::system::conflictingDjSoftwareName());
}
}  // namespace

DjSoftwareGuardController::DjSoftwareGuardController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<QString>::finished, this, &DjSoftwareGuardController::onPollFinished);
    connect(&m_timer, &QTimer::timeout, this, &DjSoftwareGuardController::poll);
    connect(EditSessionRegistry::instance(), &EditSessionRegistry::stateChanged, this, [this]() {
        bool active = m_timer.isActive();
        updateInterval();
        // Entering edit or write mode: do not wait a whole interval for
        // the first answer.
        if (!active && m_timer.isActive()) {
            pollNow();
        }
        bool blockingNow = blocking();
        if (blockingNow != m_wasBlocking) {
            m_wasBlocking = blockingNow;
            emit blockingChanged();
        }
    });
    updateInterval();
}

bool DjSoftwareGuardController::blocking() const
{
    if (m_conflictingSoftware.isEmpty()) {
        return false;
    }
    auto *registry = EditSessionRegistry::instance();
    return registry->anyEditing() || registry->anyWriting();
}

void DjSoftwareGuardController::setDialogOpen(bool open)
{
    if (m_dialogOpen == open) {
        return;
    }
    m_dialogOpen = open;
    emit dialogOpenChanged();
    updateInterval();
}

void DjSoftwareGuardController::pollNow()
{
    poll();
}

void DjSoftwareGuardController::addWatcher()
{
    ++m_watchers;
    bool active = m_timer.isActive();
    updateInterval();
    if (!active) {
        pollNow();
    }
}

void DjSoftwareGuardController::removeWatcher()
{
    if (m_watchers > 0) {
        --m_watchers;
    }
    updateInterval();
}

void DjSoftwareGuardController::updateInterval()
{
    auto *registry = EditSessionRegistry::instance();
    int interval = 0;
    if (m_dialogOpen) {
        interval = DialogOpenIntervalMs;
    } else if (registry->anyEditing() || registry->anyWriting() || m_watchers > 0) {
        interval = EditModeIntervalMs;
    }
    int before = pollIntervalMs();
    if (interval == 0) {
        m_timer.stop();
        setConflictingSoftware({});
    } else if (!m_timer.isActive() || m_timer.interval() != interval) {
        m_timer.start(interval);
    }
    if (pollIntervalMs() != before) {
        emit pollIntervalChanged();
    }
}

void DjSoftwareGuardController::poll()
{
    if (m_watcher.isRunning()) {
        return;  // the previous check has not returned yet; skip this tick
    }
    m_watcher.setFuture(QtConcurrent::run(detectConflictingSoftware));
}

void DjSoftwareGuardController::onPollFinished()
{
    if (!m_timer.isActive()) {
        return;  // stopped meanwhile; a late answer must not resurrect the state
    }
    // A failed probe reads as "nothing running" and the next tick tries
    // again -- this polls forever in the background, so it must never be
    // the thing that takes the app down.
    setConflictingSoftware(takeResult(m_watcher));
}

void DjSoftwareGuardController::setConflictingSoftware(const QString &name)
{
    if (m_conflictingSoftware == name) {
        return;
    }
    m_conflictingSoftware = name;
    emit conflictingSoftwareChanged();
    bool blockingNow = blocking();
    if (blockingNow != m_wasBlocking) {
        m_wasBlocking = blockingNow;
        emit blockingChanged();
    }
}

}  // namespace seabass::gui
