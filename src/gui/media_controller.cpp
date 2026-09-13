// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "media_controller.hpp"

#include "application/stick_path_match.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <utility>

#include "application/stick_presence_diff.hpp"
#include "application/use_cases/open_stick_backup.hpp"
#include "gui/seabass_settings.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "gui/local_file_url.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/media/media_factory.hpp"
#include "infrastructure/media/stick_root_scan.hpp"
#include "gui/future_result.hpp"

namespace seabass::gui
{

namespace
{

// Runs entirely on a background thread (see MediaController::startTask())
// -- no access to the controller itself.
MediaTaskResult runMediaTask(bool mount, QString devicePath)
{
    MediaTaskResult result;
    auto mounter = infrastructure::media::createRemovableMediaMounter();
    std::string error;
    if (mount) {
        result.success = mounter->mount(devicePath.toStdString(), error).has_value();
    } else {
        result.success = mounter->unmount(devicePath.toStdString(), error);
    }
    if (!result.success) {
        result.errorMessage = QString::fromStdString(error);
    }
    return result;
}

}  // namespace

DetectedStickListModel::DetectedStickListModel(QObject *parent) : QAbstractListModel(parent) {}

int DetectedStickListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_sticks.size());
}

QVariant DetectedStickListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_sticks.size()) {
        return {};
    }
    const auto &stick = m_sticks[static_cast<size_t>(index.row())];
    switch (role) {
    case LabelRole:
        return QString::fromStdString(stick.label);
    case MountPointRole:
        return QString::fromStdString(stick.mountPoint);
    case DevicePathRole:
        return QString::fromStdString(stick.devicePath);
    case MountedRole:
        return stick.mounted;
    case HasRekordboxRole:
        return stick.rekordboxPath.has_value();
    case HasEngineRole:
        return stick.enginePath.has_value();
    case RekordboxPathRole:
        return stick.rekordboxPath ? QString::fromStdString(*stick.rekordboxPath) : QString();
    case EnginePathRole:
        return stick.enginePath ? QString::fromStdString(*stick.enginePath) : QString();
    case IsSdCardRole:
        return stick.isSdCard;
    case IsFolderRole:
        return stick.isFolder;
    case IsBrowsedBackupRole:
        return stick.isBrowsedBackup;
    case LibraryIdRole:
        return QString::fromStdString(stick.identity.libraryId());
    case HardwareSerialRole:
        return QString::fromStdString(stick.identity.hardwareSerial);
    case IdentityStrengthRole:
        return QString::fromLatin1(application::StickIdentity::strengthName(stick.identity.strength()));
    case CapacityBytesRole:
        return QVariant::fromValue(static_cast<qulonglong>(stick.capacityBytes));
    case SafeToUnplugRole: {
        if (stick.mounted) {
            return false;
        }
        // A folder library is not a device and nothing can be pulled.
        if (stick.isFolder) {
            return false;
        }
        // Without a device to compare against there is no way to prove no
        // sibling is mounted, and an unproven safety claim is not one
        // worth making: the row falls back to describing its own state.
        if (stick.wholeDiskPath.empty()) {
            return false;
        }
        for (const application::DetectedStick &other : m_sticks) {
            if (other.mounted && other.wholeDiskPath == stick.wholeDiskPath) {
                return false;
            }
        }
        return true;
    }
    default:
        return {};
    }
}

QHash<int, QByteArray> DetectedStickListModel::roleNames() const
{
    return {
        {LabelRole, "label"},
        {CapacityBytesRole, "capacityBytes"},
        {MountPointRole, "mountPoint"},
        {DevicePathRole, "devicePath"},
        {MountedRole, "mounted"},
        {HasRekordboxRole, "hasRekordbox"},
        {HasEngineRole, "hasEngine"},
        {RekordboxPathRole, "rekordboxPath"},
        {EnginePathRole, "enginePath"},
        {IsSdCardRole, "isSdCard"},
        {IsFolderRole, "isFolder"},
        {IsBrowsedBackupRole, "isBrowsedBackup"},
        {LibraryIdRole, "libraryId"},
        {HardwareSerialRole, "hardwareSerial"},
        {IdentityStrengthRole, "identityStrength"},
        {SafeToUnplugRole, "safeToUnplug"},
    };
}

int DetectedStickListModel::removableCount() const
{
    int count = 0;
    for (const auto &stick : m_sticks) {
        if (!stick.isFolder) {
            count++;
        }
    }
    return count;
}

QVariantMap DetectedStickListModel::get(int row) const
{
    QVariantMap result;
    if (row < 0 || static_cast<size_t>(row) >= m_sticks.size()) {
        return result;
    }
    const auto roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        result.insert(QString::fromUtf8(it.value()), data(index(row), it.key()));
    }
    return result;
}

void DetectedStickListModel::setSticks(std::vector<application::DetectedStick> sticks)
{
    // Sticks with a DJ library Seabass can actually do something with are
    // what the user opened this page for -- put those first (per
    // BRAINSTORM.md's "List sticks / SD cards with DJ libraries first,
    // those without second") rather than making them scroll past plain
    // storage devices. stable_sort so unmounted sticks (whose library
    // status is unknown until mounted) keep udev's own enumeration order
    // relative to each other, not a second, arbitrary reshuffle.
    auto hasKnownLibrary = [](const application::DetectedStick &s) {
        return s.mounted && (s.rekordboxPath.has_value() || s.enginePath.has_value());
    };
    std::stable_sort(sticks.begin(), sticks.end(), [&](const auto &a, const auto &b) {
        return hasKnownLibrary(a) && !hasKnownLibrary(b);
    });

    beginResetModel();
    m_sticks = std::move(sticks);
    endResetModel();
    // endResetModel() tells a view its rows changed; it does not
    // re-evaluate a binding on a property of this object, so the counts
    // have to say so themselves.
    emit countsChanged();
}

MediaController::MediaController(QObject *parent) : QObject(parent)
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(500);
    connect(&m_debounceTimer, &QTimer::timeout, this, &MediaController::detect);

    loadOpenedFolder();
    detect();

    m_monitor = infrastructure::media::createRemovableMediaMonitor();
    m_monitor->start([this]() {
        QMetaObject::invokeMethod(this, [this]() { m_debounceTimer.start(); }, Qt::QueuedConnection);
    });

    connect(&m_watcher, &QFutureWatcher<MediaTaskResult>::finished, this, &MediaController::onTaskFinished);
    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, &MediaController::unmountOwnMounts);
    }
}

MediaController::~MediaController()
{
    if (m_monitor) {
        m_monitor->stop();
    }
    unmountOwnMounts();
}

// Where a just-detected stick is mounted right now.
std::string MediaController::mountPointFor(const application::StickIdentity &identity) const
{
    for (const application::DetectedStick &stick : m_model.sticks()) {
        if (stick.mounted && stick.identity.libraryId() == identity.libraryId()) {
            return stick.mountPoint;
        }
    }
    return {};
}

void MediaController::detect()
{
    auto locator = infrastructure::media::createRemovableMediaLocator();
    std::vector<application::DetectedStick> sticks = locator->detect();
    // The opened folder is listed only while it is there with a library in
    // it -- a card with nothing to open is clutter -- and re-scanned every
    // refresh rather than cached from openFolder(), because its catalogs
    // can change underneath us exactly like a stick's. Nor while a mounted
    // stick sits at its path: that is the stick, already listed with the
    // identity it really has, and a second row would put a second
    // edit-lock id on one export.pdb.
    bool folderListed = false;
    if (m_openedFolder) {
        application::DetectedStick folder = *m_openedFolder;
        folder.rekordboxPath.reset();
        folder.enginePath.reset();
        const std::filesystem::path dir(folder.mountPoint);
        std::error_code dirEc;
        if (std::filesystem::is_directory(dir, dirEc) && !dirEc) {
            infrastructure::media::scanMountedRoot(folder.mountPoint, folder);
        }
        // Only paths that actually canonicalised are compared: on failure
        // weakly_canonical returns an empty path, and two failures would
        // compare equal.
        bool isAMountedStick = false;
        std::error_code rootEc;
        const auto folderRoot = std::filesystem::weakly_canonical(dir, rootEc);
        for (const application::DetectedStick &stick : sticks) {
            if (rootEc || folderRoot.empty() || !stick.mounted || stick.mountPoint.empty()) {
                continue;
            }
            std::error_code stickEc;
            const auto stickRoot = std::filesystem::weakly_canonical(std::filesystem::path(stick.mountPoint), stickEc);
            isAMountedStick = isAMountedStick || (!stickEc && stickRoot == folderRoot);
        }
        folderListed = (folder.rekordboxPath.has_value() || folder.enginePath.has_value()) && !isAMountedStick;
        if (folderListed) {
            // Decided from disk every time: the marker is what makes the
            // browse cache self-describing, and it survives a restart. See
            // isBrowsedBackupRoot for why a stray marker elsewhere is not.
            folder.isBrowsedBackup = infrastructure::local::isBrowsedBackupRoot(dir);
            sticks.push_back(std::move(folder));
        }
        if (folderListed != m_openedFolderListed) {
            announceOpenedFolder(*m_openedFolder, folderListed);
        }
    }
    m_openedFolderListed = folderListed;
    m_model.setSticks(std::move(sticks));
    std::vector<application::StickIdentity> present;
    for (const application::DetectedStick &stick : m_model.sticks()) {
        // Folder rows are not physical: nothing pulls them, nothing
        // re-inserts them, and closing one is a deliberate act -- so they
        // take no part in the removed/returned bookkeeping below. Without
        // this, closing a folder raised the "USB stick removed" dialog
        // for a directory still on disk, and a folder opened at a mounted
        // stick's own root overwrote that stick's last-known identity.
        if (stick.isFolder) {
            continue;
        }
        if (stick.mounted && !stick.mountPoint.empty()) {
            m_lastKnownByMountPoint[stick.mountPoint] = stick.identity;
            if (stick.identity.strength() != application::StickIdentity::Strength::None) {
                present.push_back(stick.identity);
            }
        }
    }
    // Pulled and returned sticks, by identity. A pulled stick stays
    // awaited until the very same one is back, however long that takes;
    // a different stick on the same mount point in the meantime is just
    // a new stick.
    auto diff = application::diffStickPresence(m_presentIdentities, present);
    m_presentIdentities = std::move(present);
    for (const application::StickIdentity &identity : diff.gone) {
        if (!application::containsSameStick(m_awaitedIdentities, identity)) {
            m_awaitedIdentities.push_back(identity);
        }
        emit stickRemoved(QString::fromStdString(identity.libraryId()), QString::fromStdString(identity.label));
    }
    for (const application::StickIdentity &identity : diff.appeared) {
        emit stickAppeared(QString::fromStdString(identity.libraryId()),
                           QString::fromStdString(mountPointFor(identity)));
        auto awaited = application::findAwaited(m_awaitedIdentities, identity);
        if (!awaited) {
            continue;
        }
        m_awaitedIdentities.erase(std::remove_if(m_awaitedIdentities.begin(), m_awaitedIdentities.end(),
                                                 [&](const application::StickIdentity &a) { return a.isSameStick(identity); }),
                                  m_awaitedIdentities.end());
        emit stickReturned(QString::fromStdString(awaited->libraryId()),
                           QString::fromUtf8(application::StickIdentity::strengthName(
                               application::matchStrength(*awaited, identity))));
    }
    queueAutoMounts();
}

std::string MediaController::folderLibraryId(const std::string &canonicalPath)
{
    // Hashed rather than sanitized: the id becomes a lock-cookie filename
    // (see StickIdentity::sanitizeForFileName) and a full path is both too
    // long for that and lossy once the separators are stripped -- two
    // different folders could sanitize to the same name. 16 hex digits of
    // SHA-256 is far more than enough to keep a person's folders apart.
    const std::string hex = infrastructure::hashing::toHex(infrastructure::hashing::Sha256::of(canonicalPath));
    return "folder-" + hex.substr(0, 16);
}

QString MediaController::openFolder(const QString &path)
{
    return openFolder(path, QString());
}

QString MediaController::openFolder(const QString &path, const QString &label)
{
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::canonical(std::filesystem::path(localPathFromUrl(path).toStdString()), ec);
    if (ec) {
        return tr("That folder could not be opened: %1").arg(QString::fromStdString(ec.message()));
    }
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return tr("That is not a folder.");
    }
    const std::string canonical = dir.string();

    // A mounted stick's own root is already listed, with the identity the
    // stick actually has. Listing it a second time as a folder would put
    // two rows on one library and let the folder's synthetic id shadow
    // the real one in the pulled-stick lookup.
    for (const application::DetectedStick &stick : m_model.sticks()) {
        std::error_code cmpEc;
        if (!stick.isFolder && stick.mounted
            && std::filesystem::weakly_canonical(std::filesystem::path(stick.mountPoint), cmpEc) == dir) {
            return tr("That is the USB stick \"%1\", which is already in the list.")
                .arg(QString::fromStdString(stick.label));
        }
    }

    application::DetectedStick folder;
    folder.mountPoint = canonical;
    folder.mounted = true;  // there is nothing to mount; the library is readable now
    folder.isFolder = true;
    folder.label = folderLabelFor(dir, label);
    infrastructure::media::scanMountedRoot(canonical, folder);
    if (!folder.rekordboxPath.has_value() && !folder.enginePath.has_value()) {
        return tr("No rekordbox or Engine DJ library in that folder. Open the folder that holds "
                  "\"PIONEER\" or \"Engine Library\", not one of those itself.");
    }
    folder.identity.label = folder.label;
    folder.identity.explicitLibraryId = folderLibraryId(canonical);

    // One folder at a time. The one it replaces leaves the list the way a
    // pulled stick does, so a session still holding it hears about it.
    if (m_openedFolder && m_openedFolder->mountPoint != canonical) {
        if (m_openedFolderListed) {
            announceOpenedFolder(*m_openedFolder, false);
        }
        releaseBrowsedBackup(m_openedFolder->mountPoint);
        m_openedFolderListed = false;
    }
    m_openedFolder = std::move(folder);
    saveOpenedFolder();
    detect();
    return {};
}

QString MediaController::openBackup(const QString &archivePath)
{
    const std::filesystem::path archive(localPathFromUrl(archivePath).toStdString());
    // One cache directory per archive, named after its path rather than
    // its label: two backups of differently-named sticks must not land on
    // top of each other, and re-opening the same archive should reuse (and
    // refresh) the same directory instead of accumulating copies. Keyed on
    // the canonical path, so the same ZIP reached through a symlinked
    // directory or a ".." spelling is one backup, one cache, one row.
    const std::filesystem::path archiveKey = infrastructure::local::canonicalOrAbsolute(archive);
    const std::filesystem::path cacheRoot =
        infrastructure::paths::localBrowsedBackupsDir() / folderLibraryId(archiveKey.string());

    const application::OpenedStickBackup opened = application::OpenStickBackup::execute(archiveKey, cacheRoot);
    if (!opened.error.empty()) {
        return QString::fromStdString(opened.error);
    }
    // The label is the stick the backup was taken from, when the manifest
    // says; the cache directory's own name is a hash and would tell the
    // user nothing.
    return openFolder(QString::fromStdString(opened.libraryRoot.string()),
                      QString::fromStdString(opened.stickLabel));
}

// The row's name: the one given (a browsed backup's stick label), else
// the directory's own name, else -- for a path ending in a separator, or
// a root like "/" -- the whole path, so the row is never blank.
std::string MediaController::folderLabelFor(const std::filesystem::path &dir, const QString &given)
{
    if (!given.isEmpty()) {
        return given.toStdString();
    }
    return dir.filename().empty() ? dir.string() : dir.filename().string();
}

void MediaController::closeFolder(const QString &path)
{
    if (!m_openedFolder || m_openedFolder->mountPoint != path.toStdString()) {
        return;
    }
    // Unsaved edits are the page's business, not this controller's: the
    // stick list already holds the edit registry (or a fake in tests)
    // and refuses the close there while a session on this row is dirty.
    // Depending on EditSessionRegistry from here would invert the one
    // direction that already exists (the registry watches this controller).
    releaseBrowsedBackup(m_openedFolder->mountPoint);
    m_openedFolder.reset();
    m_openedFolderListed = false;
    saveOpenedFolder();
    detect();
}

void MediaController::announceOpenedFolder(const application::DetectedStick &folder, bool listed)
{
    // Both halves, always. stickRemoved puts StickRemovedDialog up for a
    // session holding this library, and only stickReturned re-enables its
    // "Understood": a folder announced gone and never back would leave
    // Discard Changes as the one button that works.
    if (listed) {
        // A folder's identity is its path, so one that is back is
        // necessarily the same one: Strength::Folder, not a re-match.
        emit stickReturned(QString::fromStdString(folder.identity.libraryId()),
                           QString::fromUtf8(application::StickIdentity::strengthName(
                               application::StickIdentity::Strength::Folder)));
    } else {
        emit stickRemoved(QString::fromStdString(folder.identity.libraryId()),
                          QString::fromStdString(folder.label));
    }
}

void MediaController::releaseBrowsedBackup(const std::string &folderPath)
{
    // An open handle otherwise stays held until quit, and on Windows
    // blocks replacing that archive with a newer generation.
    if (auto archive = infrastructure::local::browsedBackupArchive(std::filesystem::path(folderPath))) {
        infrastructure::rekordbox::forgetArchiveSource(archive->string());
    }
}

void MediaController::loadOpenedFolder()
{
    // ("seabass", "seabass") explicitly, never the default constructor:
    // this app sets no organizationName/applicationName, so a default-
    // constructed QSettings resolves to a different (empty-organization)
    // store than every other setting here, and the opened folder would
    // silently fail to persist. Same construction as main.cpp and
    // AppSettingsController.
    QSettings settings = openSeabassSettings();
    const QString path = settings.value(QStringLiteral("openedFolder/path")).toString();
    if (path.isEmpty()) {
        return;
    }
    application::DetectedStick folder;
    folder.mountPoint = path.toStdString();
    folder.mounted = true;
    folder.isFolder = true;
    // The label matters because a browsed backup's directory is named
    // after a hash; its label is the stick the backup came from.
    folder.label = folderLabelFor(std::filesystem::path(folder.mountPoint),
                                  settings.value(QStringLiteral("openedFolder/label")).toString());
    folder.identity.label = folder.label;
    folder.identity.explicitLibraryId = folderLibraryId(folder.mountPoint);
    // Not scanned or existence-checked here: detect() does that, and a
    // folder on a share that is slow or absent at startup must not hold
    // up construction.
    m_openedFolder = std::move(folder);
}

void MediaController::saveOpenedFolder()
{
    QSettings settings = openSeabassSettings();  // see loadOpenedFolder()
    settings.remove(QStringLiteral("openedFolder"));
    if (m_openedFolder) {
        settings.setValue(QStringLiteral("openedFolder/path"), QString::fromStdString(m_openedFolder->mountPoint));
        settings.setValue(QStringLiteral("openedFolder/label"), QString::fromStdString(m_openedFolder->label));
    }
}

QString MediaController::libraryIdForMountPoint(const QString &mountPoint) const
{
    auto identity = lastKnownIdentity(mountPoint.toStdString());
    return identity ? QString::fromStdString(identity->libraryId()) : QString();
}

bool MediaController::pathIsPresent(const QString &path) const
{
    if (path.isEmpty()) {
        return true;  // nothing referenced, so nothing missing
    }
    const std::string wanted = path.toStdString();
    for (const auto &stick : m_model.sticks()) {
        if (application::pathIsUnder(wanted, stick.mountPoint)) {
            return true;
        }
    }
    return false;
}

QString MediaController::stickLabelForPath(const QString &path) const
{
    const std::string wanted = path.toStdString();
    for (const auto &stick : m_model.sticks()) {
        if (application::pathIsUnder(wanted, stick.mountPoint)) {
            return QString::fromStdString(stick.label);
        }
    }
    return {};
}

std::optional<application::DetectedStick> MediaController::stickForLibraryId(const QString &libraryId) const
{
    const std::string id = libraryId.toStdString();
    const application::DetectedStick *unmounted = nullptr;
    for (const application::DetectedStick &stick : m_model.sticks()) {
        if (stick.identity.libraryId() != id) {
            continue;
        }
        if (stick.mounted) {
            return stick;
        }
        if (!unmounted) {
            unmounted = &stick;
        }
    }
    if (unmounted) {
        return *unmounted;
    }
    return std::nullopt;
}

std::optional<application::StickIdentity> MediaController::lastKnownIdentity(const std::string &mountPoint) const
{
    for (const application::DetectedStick &stick : m_model.sticks()) {
        if (stick.mounted && stick.mountPoint == mountPoint) {
            return stick.identity;
        }
    }
    auto it = m_lastKnownByMountPoint.find(mountPoint);
    if (it != m_lastKnownByMountPoint.end()) {
        return it->second;
    }
    return std::nullopt;
}

// Every stick with a filesystem and no mount point becomes a candidate,
// except one the user ejected here or one that already failed; both
// forget their exemption once the stick is gone, so re-inserting it
// mounts it again.
void MediaController::queueAutoMounts()
{
    QSet<QString> present;
    QSet<QString> queuedOrBusy;
    for (const PendingTask &task : m_taskQueue) {
        queuedOrBusy.insert(task.devicePath);
    }
    if (m_busy) {
        queuedOrBusy.insert(m_busyTask.devicePath);
    }
    for (const application::DetectedStick &stick : m_model.sticks()) {
        const QString devicePath = QString::fromStdString(stick.devicePath);
        if (devicePath.isEmpty()) {
            continue;
        }
        present.insert(devicePath);
        if (stick.mounted || stick.hasNoFilesystem || queuedOrBusy.contains(devicePath)
            || m_userUnmounted.contains(devicePath) || m_autoMountFailed.contains(devicePath)) {
            continue;
        }
        enqueue(devicePath, true, true, false);
    }
    for (QSet<QString> *set : {&m_userUnmounted, &m_autoMountFailed, &m_mountedByUs}) {
        for (auto it = set->begin(); it != set->end();) {
            it = present.contains(*it) ? std::next(it) : set->erase(it);
        }
    }
    m_taskQueue.erase(std::remove_if(m_taskQueue.begin(), m_taskQueue.end(),
                                     [&](const PendingTask &t) { return !present.contains(t.devicePath); }),
                      m_taskQueue.end());
}

void MediaController::mountStick(const QString &devicePath)
{
    m_userUnmounted.remove(devicePath);
    m_autoMountFailed.remove(devicePath);
    enqueue(devicePath, true, false, true);
}

void MediaController::unmountStick(const QString &devicePath)
{
    m_userUnmounted.insert(devicePath);
    enqueue(devicePath, false, false, true);
}

void MediaController::enqueue(const QString &devicePath, bool mount, bool automatic, bool priority)
{
    m_taskQueue.erase(std::remove_if(m_taskQueue.begin(), m_taskQueue.end(),
                                     [&](const PendingTask &t) { return t.devicePath == devicePath; }),
                      m_taskQueue.end());
    const PendingTask task{devicePath, mount, automatic};
    if (priority) {
        m_taskQueue.prepend(task);
    } else {
        m_taskQueue.append(task);
    }
    processQueue();
}

void MediaController::processQueue()
{
    if (m_busy || m_taskQueue.isEmpty()) {
        return;
    }
    startTask(m_taskQueue.takeFirst());
}

void MediaController::startTask(const PendingTask &task)
{
    setErrorMessage({});
    m_busy = true;
    m_busyTask = task;
    emit busyChanged();
    m_watcher.setFuture(QtConcurrent::run(runMediaTask, task.mount, task.devicePath));
}

void MediaController::onTaskFinished()
{
    QString thrown;
    MediaTaskResult result = takeResult(m_watcher, &thrown);
    if (!thrown.isEmpty()) {
        result.errorMessage = thrown;
    }
    const PendingTask task = m_busyTask;
    m_busy = false;
    m_busyTask = {};
    if (task.mount) {
        if (result.success) {
            m_mountedByUs.insert(task.devicePath);
        } else if (task.automatic) {
            m_autoMountFailed.insert(task.devicePath);
        }
    } else if (result.success) {
        m_mountedByUs.remove(task.devicePath);
    }
    setErrorMessage(result.errorMessage);
    emit busyChanged();
    detect();
    processQueue();
}

void MediaController::unmountOwnMounts()
{
    if (m_ownMountsReleased) {
        return;
    }
    m_ownMountsReleased = true;
    m_taskQueue.clear();
    awaitQuietly(m_watcher);
    if (m_mountedByUs.isEmpty()) {
        return;
    }
    // Only what is still mounted: the user may have ejected it meanwhile.
    QSet<QString> stillMounted;
    auto locator = infrastructure::media::createRemovableMediaLocator();
    for (const application::DetectedStick &stick : locator->detect()) {
        if (stick.mounted) {
            stillMounted.insert(QString::fromStdString(stick.devicePath));
        }
    }
    auto mounter = infrastructure::media::createRemovableMediaMounter();
    for (const QString &devicePath : std::as_const(m_mountedByUs)) {
        if (!stillMounted.contains(devicePath)) {
            continue;
        }
        std::string error;
        mounter->unmount(devicePath.toStdString(), error);  // best effort: nothing left to report to
    }
    m_mountedByUs.clear();
}

void MediaController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
