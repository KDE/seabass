// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "format_usb_controller.hpp"

#include "gui/future_result.hpp"
#include "gui/sleep_inhibitor.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/format_usb_stick.hpp"
#include "domain/usb_filesystem.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/write_guard.hpp"
#include "infrastructure/media/media_factory.hpp"

namespace seabass::gui
{

using application::DetectedStick;
using application::NullProgressReporter;
using application::RemovableMediaLocator;
using application::RemovableMediaMounter;
using application::UsbFormatter;
using domain::UsbFilesystem;

namespace
{

QString filesystemToString(UsbFilesystem fs)
{
    return fs == UsbFilesystem::Fat32 ? QStringLiteral("fat32") : QStringLiteral("exfat");
}

UsbFilesystem filesystemFromString(const QString &s)
{
    return s == QStringLiteral("fat32") ? UsbFilesystem::Fat32 : UsbFilesystem::ExFat;
}

QVariantMap diskToVariant(const DetectedStick &disk)
{
    QVariantMap map;
    map["label"] = QString::fromStdString(disk.label);
    map["wholeDiskPath"] = QString::fromStdString(disk.wholeDiskPath);
    map["devicePath"] = QString::fromStdString(disk.devicePath);
    map["capacityBytes"] = static_cast<qlonglong>(disk.capacityBytes);
    map["mounted"] = disk.mounted;
    map["hasNoFilesystem"] = disk.hasNoFilesystem;
    map["hasDjLibrary"] = disk.rekordboxPath.has_value() || disk.enginePath.has_value();
    // The library another instance may be editing right now (see
    // format()); empty for an unlabelled or blank drive.
    map["libraryId"] = QString::fromStdString(disk.identity.libraryId());
    QVariantList rootEntries;
    for (const auto &entry : disk.rootEntries) {
        rootEntries.push_back(QString::fromStdString(entry));
    }
    map["rootEntries"] = rootEntries;
    return map;
}

// Runs entirely on a background thread (see FormatUsbController::format())
// -- no access to the controller itself. Owns its own locator/mounter/
// formatter instances rather than sharing the app-wide MediaController's,
// same as every other write controller in this codebase constructs its
// own use case dependencies per task.
FormatUsbTaskResult runFormatTask(QString wholeDiskPath, application::StickIdentity chosen, QString filesystem,
                                    QString volumeLabel, std::shared_ptr<QtProgressReporter> reporter)
{
    FormatUsbTaskResult result;
    QString refusal = refuseIfDjSoftwareRunning();
    if (!refusal.isEmpty()) {
        result.errorMessage = refusal;
        return result;
    }
    try {
        auto locator = infrastructure::media::createRemovableMediaLocator();
        auto mounter = infrastructure::media::createRemovableMediaMounter();
        auto formatter = infrastructure::media::createUsbFormatter();
        application::FormatUsbStick useCase(*locator, *mounter, *formatter);

        std::string errorMessage;
        bool ok = useCase.execute(wholeDiskPath.toStdString(), chosen, filesystemFromString(filesystem),
                                   volumeLabel.toStdString(), errorMessage, *reporter);
        if (!ok) {
            result.errorMessage = QString::fromStdString(errorMessage);
        }
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

FormatUsbController::FormatUsbController(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<FormatUsbTaskResult>::finished, this,
            &FormatUsbController::onFormatFinished);

    auto formatter = infrastructure::media::createUsbFormatter();
    if (auto cap = formatter->maxSizeFor(UsbFilesystem::Fat32)) {
        m_fat32MaxBytes = static_cast<qlonglong>(*cap);
    }

    refresh();

    // Without this, a stick pulled (or inserted) while this page is open
    // never disappears (or appears) from "1. Choose a drive" until the
    // page is closed and reopened -- confirmed as a real report, not
    // hypothetical: unplugging an unrelated backup stick mid-session left
    // it sitting in the list as if still present. Same
    // debounce-timer-then-refresh pattern as MediaController's own
    // hotplug handling (a burst of udev events for one physical
    // plug/unplug should still trigger exactly one re-scan, not one per
    // event).
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(500);
    connect(&m_debounceTimer, &QTimer::timeout, this, &FormatUsbController::refresh);

    m_monitor = infrastructure::media::createRemovableMediaMonitor();
    m_monitor->start([this]() {
        QMetaObject::invokeMethod(this, [this]() { m_debounceTimer.start(); }, Qt::QueuedConnection);
    });
}

FormatUsbController::~FormatUsbController()
{
    if (m_monitor) {
        m_monitor->stop();
    }
    // Waits for a format still running, because m_writeHold is a member
    // and is released the moment this body returns. Leaving the page
    // mid-format used to drop the edit lock while the partition was
    // still being rewritten, so something else in the app could start
    // writing to the drive being formatted.
    //
    // Only awaited, not cancelled. Every sibling controller
    // (StickBackup, CloneStick) cancels first and then awaits, and this
    // one has no cancellation token on purpose: a partition rewrite
    // interrupted halfway is the thing a format exists to avoid
    // producing. So the wait is the whole of it, and it is the reason
    // closing this page during a format does not return instantly.
    //
    // awaitQuietly() rather than waitForFinished(): the latter rethrows
    // a stored exception, and a destructor is noexcept.
    awaitQuietly(m_watcher);
}

void FormatUsbController::refresh()
{
    // A refresh landing mid-format (the monitor's own callback can fire
    // for changes the format operation itself causes, e.g. the target
    // unmounting) would otherwise race runFormatTask()'s own use of the
    // locator/mounter on a background thread -- safe to just skip it,
    // onFormatFinished() already calls refresh() again once the format
    // itself completes.
    if (m_busy) {
        return;
    }
    // Whole-disk enumeration (this project's own udev/Get-Disk-backed
    // adapters) is lightweight -- no library scan, just a handful of
    // devices -- so this runs synchronously on the UI thread, same as
    // MediaController's own sticks refresh.
    auto locator = infrastructure::media::createRemovableMediaLocator();
    QVariantList disks;
    // Kept beside the list the page shows, not derived from it: format()
    // hands the use case who this drive was when it was listed, and the
    // use case refuses if someone else is in that port by then. A
    // QVariantMap of identity fields would be the same thing spelled so
    // that QML could edit it.
    m_identities.clear();
    bool chosenStillThere = false;
    for (const auto &disk : locator->detect()) {
        const QString path = QString::fromStdString(disk.wholeDiskPath);
        // First entry wins: one disk is listed once per partition, and
        // they share a wholeDiskPath. Which of them the identity comes
        // from does not matter to the use case (it compares against every
        // entry for the disk), but overwriting per partition would make
        // this map say something different on each refresh.
        if (!m_identities.contains(path)) {
            m_identities[path] = disk.identity;
        }
        if (!m_chosenPath.isEmpty() && path == m_chosenPath
            && application::FormatUsbStick::sameDrive(m_chosenIdentity, disk.identity, disk.capacityBytes)) {
            chosenStillThere = true;
        }
        disks.push_back(diskToVariant(disk));
    }
    setDisks(std::move(disks));

    // A replug lands the new stick on the same device node and this
    // refresh runs half a second later, so without this the selection
    // quietly follows the port to whatever is in it now -- which is the
    // whole scenario the format's identity check exists for.
    if (!m_chosenPath.isEmpty() && !chosenStillThere) {
        m_chosenPath.clear();
        m_chosenIdentity = {};
        // Through the error channel on purpose: the page paints
        // statusMessage in the success colour, and "something else is in
        // that port now" is the opposite of a success.
        setErrorMessage(QStringLiteral("The drive you picked is no longer there, so nothing is selected."));
        emit chosenDriveWentAway();
    }
}

void FormatUsbController::chooseDrive(const QString &wholeDiskPath)
{
    m_chosenPath = wholeDiskPath;
    m_chosenIdentity = m_identities.value(wholeDiskPath);
}

QString FormatUsbController::recommendedFilesystem(qlonglong capacityBytes) const
{
    return filesystemToString(domain::recommendedUsbFilesystem(static_cast<std::uint64_t>(capacityBytes)));
}

void FormatUsbController::format(const QString &wholeDiskPath, const QString &filesystem, const QString &volumeLabel)
{
    if (m_busy) {
        return;
    }
    // Before the lock and before busy, because a refusal here starts no
    // task and nothing would clear either: the page would keep its
    // spinner, refuse to refresh, hold the library's edit lock and not
    // even let the person leave. Reachable by way of the locked-library
    // dialog, whose retry re-enters this with the path it captured while
    // a hotplug refresh had already dropped the selection.
    //
    // Who the drive was when it was PICKED, not when the list was last
    // rebuilt: see chooseDrive(). A format for a path nobody picked is
    // refused rather than checked against a fresh reading of that port,
    // which would be no check at all.
    if (wholeDiskPath != m_chosenPath) {
        setErrorMessage(QStringLiteral("Pick the drive again before formatting it."));
        return;
    }
    const application::StickIdentity chosen = m_chosenIdentity;

    // Formatting is one udisks/Format-Volume call and cannot be cancelled;
    // what the edit lock adds is the refusal while another instance is
    // editing the library on this very drive.
    QString libraryId;
    for (const QVariant &entry : m_disks) {
        const QVariantMap disk = entry.toMap();
        if (disk.value("wholeDiskPath").toString() == wholeDiskPath) {
            libraryId = disk.value("libraryId").toString();
            break;
        }
    }
    if (auto refusal = m_writeHold.acquire({libraryId}, volumeLabel, [this, wholeDiskPath, filesystem, volumeLabel] {
            format(wholeDiskPath, filesystem, volumeLabel);
        })) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder, m_writeHold.refusedLibraryId());

        }
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    setBusy(true);

    auto reporter = std::make_shared<QtProgressReporter>();
    // Awake for the whole format: see SleepInhibitor.
    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Formatting a USB stick"));
    m_watcher.setFuture(QtConcurrent::run([keepAwake, wholeDiskPath, chosen, filesystem, volumeLabel, reporter]() {
        return runFormatTask(wholeDiskPath, chosen, filesystem, volumeLabel, reporter);
    }));
}

void FormatUsbController::onFormatFinished()
{
    QString thrown;
    FormatUsbTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }
    m_writeHold.release();
    setBusy(false);
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        emit actionFeedback(result.errorMessage, true);
        return;
    }
    setStatusMessage(QStringLiteral("Drive formatted successfully."));
    emit actionFeedback(QStringLiteral("Your USB stick has been formatted. All the data that once was on it is now gone, gone, gone with the wind..."), false);
    // The drive legitimately is not who it was a moment ago: a new label
    // and a new filesystem UUID are what a format makes. Dropped here so
    // the refresh below does not report it as the drive having gone away,
    // and so the page's next format has to be picked again on purpose.
    m_chosenPath.clear();
    m_chosenIdentity = {};
    emit chosenDriveWentAway();
    refresh();
}

void FormatUsbController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged();
}

void FormatUsbController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void FormatUsbController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    emit statusMessageChanged();
}

void FormatUsbController::setDisks(QVariantList disks)
{
    m_disks = std::move(disks);
    emit disksChanged();
}

}  // namespace seabass::gui
