// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "format_usb_controller.hpp"

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
FormatUsbTaskResult runFormatTask(QString wholeDiskPath, QString filesystem, QString volumeLabel,
                                    std::shared_ptr<QtProgressReporter> reporter)
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
        bool ok = useCase.execute(wholeDiskPath.toStdString(), filesystemFromString(filesystem),
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
    for (const auto &disk : locator->detect()) {
        disks.push_back(diskToVariant(disk));
    }
    setDisks(std::move(disks));
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
    m_watcher.setFuture(QtConcurrent::run(runFormatTask, wholeDiskPath, filesystem, volumeLabel, reporter));
}

void FormatUsbController::onFormatFinished()
{
    FormatUsbTaskResult result = m_watcher.result();
    m_writeHold.release();
    setBusy(false);
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        emit actionFeedback(result.errorMessage, true);
        return;
    }
    setStatusMessage(QStringLiteral("Drive formatted successfully."));
    emit actionFeedback(QStringLiteral("Your USB stick has been formatted. All the data that once was on it is now gone, gone, gone with the wind..."), false);
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
