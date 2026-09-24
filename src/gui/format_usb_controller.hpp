// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

#include <functional>
#include <memory>

#include "application/ports/removable_media_monitor.hpp"
#include "application/stick_identity.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/edit/direct_write_hold.hpp"

namespace seabass::gui
{

// Result of the background format task, see FormatUsbController::format().
// Built entirely on a worker thread, no access to the controller.
struct FormatUsbTaskResult
{
    QString errorMessage;  // empty on success
};

// Wraps application::FormatUsbStick for QML. `disks` is a plain
// QVariantList (array of QVariantMap), not a QAbstractListModel like
// DetectedStickListModel -- deliberately, so a QML test can stand in a
// plain JS array/object for the whole controller (see
// tests/qml/tst_FormatUsbPage.qml and its own comment) without needing a
// registered C++ model type just to fake a list.
class FormatUsbController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList disks READ disks NOTIFY disksChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    // The largest FAT32 volume this platform can actually create, in
    // bytes; -1 means no known limit. Backed by
    // application::UsbFormatter::maxSizeFor() so the "over 32GB on
    // Windows" ceiling lives in exactly one place (the formatter) rather
    // than being duplicated as a magic number in QML -- and so a QML test
    // can fake this as a plain number without needing to run on Windows
    // itself to exercise that branch.
    Q_PROPERTY(qlonglong fat32MaxBytes READ fat32MaxBytes CONSTANT)

public:
    explicit FormatUsbController(QObject *parent = nullptr);
    ~FormatUsbController() override;

    QVariantList disks() const { return m_disks; }
    bool busy() const { return m_busy; }
    QString errorMessage() const { return m_errorMessage; }
    QString statusMessage() const { return m_statusMessage; }
    qlonglong fat32MaxBytes() const { return m_fat32MaxBytes; }

    // Re-scans for removable disks (blank and already-recognized alike)
    // and repopulates `disks`. Called on construction, after a successful
    // format, and automatically on every hotplug event (see m_monitor
    // below) -- a stick pulled while this page is open (to back up to,
    // say, unrelated to whatever's about to be formatted) needs to
    // disappear from "1. Choose a drive" immediately, the same way
    // MediaController already keeps the Home page's own stick list live.
    Q_INVOKABLE void refresh();
    // Called when a person picks a drive in the list, so the format that
    // follows is checked against the drive they were looking at rather
    // than against whatever is in that port by then. Without it the
    // monitor's own refresh re-points the selection at a stick plugged in
    // afterwards, and the staleness check has nothing stale to catch.
    Q_INVOKABLE void chooseDrive(const QString &wholeDiskPath);

    // Pure heuristic, no I/O -- domain::recommendedUsbFilesystem() exposed
    // for QML. Returns "fat32" or "exfat".
    Q_INVOKABLE QString recommendedFilesystem(qlonglong capacityBytes) const;

    // filesystem is "fat32" or "exfat", matching recommendedFilesystem()'s
    // own return values.
    Q_INVOKABLE void format(const QString &wholeDiskPath, const QString &filesystem, const QString &volumeLabel);
    Q_INVOKABLE void retryLockedAction() { m_writeHold.retryLockedAction(); }

    // Replaces the background format task for tests, so that nothing is
    // ever formatted; it runs on the worker thread in the real task's
    // place. An empty function puts the real task back.
    static void setFormatTaskForTesting(std::function<FormatUsbTaskResult()> task);

signals:
    void disksChanged();
    void busyChanged();
    void errorMessageChanged();
    void statusMessageChanged();
    void actionFeedback(const QString &message, bool isError);
    // Another instance is editing the library on that drive; nothing
    // was started.
    void lockRefused(const QVariantMap &holder, const QString &libraryId);
    // The drive that was picked is gone, or something else is in that
    // port now: the page drops the selection rather than leave it
    // pointing at a drive nobody chose.
    void chosenDriveWentAway();

private:
    void onFormatFinished();
    void setBusy(bool busy);
    void setErrorMessage(const QString &message);
    void setStatusMessage(const QString &message);
    void setDisks(QVariantList disks);

    QFutureWatcher<FormatUsbTaskResult> m_watcher;
    std::unique_ptr<application::RemovableMediaMonitor> m_monitor;
    QTimer m_debounceTimer;
    QVariantList m_disks;
    // wholeDiskPath -> who that drive was when it was listed; see
    // refresh() and format().
    QHash<QString, application::StickIdentity> m_identities;
    // The drive a person chose, and who it was at that moment.
    QString m_chosenPath;
    application::StickIdentity m_chosenIdentity;
    DirectWriteHold m_writeHold;
    bool m_busy = false;
    QString m_errorMessage;
    QString m_statusMessage;
    qlonglong m_fat32MaxBytes = -1;
};

}  // namespace seabass::gui
