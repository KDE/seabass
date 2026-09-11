// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTimer>

namespace seabass::gui
{

// The one process guard: polls for rekordbox / Engine DJ on a single
// timer whose interval follows what is at stake --
//
//   stopped  nothing is in edit or write mode and no page asked to watch
//   5 s      a library is in edit or write mode (EditSessionRegistry), or
//            a page that shows the live status (full stick backup, clone)
//            is open
//   1 s      the "close it first" dialog is up
//
// `blocking` is what the modal DjSoftwareRunningDialog binds to: one of
// the two apps is running *and* a library is in edit or write mode.
// Every write task still refuses on its own (write_guard.hpp) -- this is
// the advisory layer, that is the safety net.
class DjSoftwareGuardController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // "" | "rekordbox" | "Engine DJ" -- whichever is running, rekordbox
    // first. Cleared when the timer is stopped, so a stale value is never
    // shown.
    Q_PROPERTY(QString conflictingSoftware READ conflictingSoftware NOTIFY conflictingSoftwareChanged)
    Q_PROPERTY(bool blocking READ blocking NOTIFY blockingChanged)
    Q_PROPERTY(bool dialogOpen READ dialogOpen WRITE setDialogOpen NOTIFY dialogOpenChanged)
    Q_PROPERTY(int pollIntervalMs READ pollIntervalMs NOTIFY pollIntervalChanged)

public:
    explicit DjSoftwareGuardController(QObject *parent = nullptr);

    QString conflictingSoftware() const { return m_conflictingSoftware; }
    bool blocking() const;
    bool dialogOpen() const { return m_dialogOpen; }
    void setDialogOpen(bool open);
    int pollIntervalMs() const { return m_timer.isActive() ? m_timer.interval() : 0; }

    Q_INVOKABLE void pollNow();
    // Pages that display the live status keep the 5 s poll running while
    // they are open, edit mode or not.
    Q_INVOKABLE void addWatcher();
    Q_INVOKABLE void removeWatcher();

signals:
    void conflictingSoftwareChanged();
    void blockingChanged();
    void dialogOpenChanged();
    void pollIntervalChanged();

private:
    void updateInterval();
    void poll();
    void onPollFinished();
    void setConflictingSoftware(const QString &name);

    QTimer m_timer;
    QFutureWatcher<QString> m_watcher;
    QString m_conflictingSoftware;
    bool m_dialogOpen = false;
    int m_watchers = 0;
    bool m_wasBlocking = false;
};

}  // namespace seabass::gui
