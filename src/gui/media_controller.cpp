// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "media_controller.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

#include "application/path_key.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <set>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <utility>

#include "application/mount_unless_mounted.hpp"
#include "application/stick_presence_diff.hpp"
#include "application/use_cases/open_stick_backup.hpp"
#include "gui/seabass_settings.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "gui/local_file_url.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/media/filesystem_health.hpp"
#include "infrastructure/media/media_factory.hpp"
#include "infrastructure/media/stick_root_scan.hpp"
#include "gui/future_result.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"

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
        // A stick already mounted is not ours: see mountUnlessMounted.
        auto locator = infrastructure::media::createRemovableMediaLocator();
        const auto outcome = application::mountUnlessMounted(*locator, *mounter, devicePath.toStdString());
        result.success = outcome.success;
        result.mountedHere = outcome.mountedHere;
        error = outcome.errorMessage;
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
        return qtPathFromUtf8(stick.mountPoint);
    case DevicePathRole:
        return QString::fromStdString(stick.devicePath);
    case MountedRole:
        return stick.mounted;
    case HasRekordboxRole:
        return stick.rekordboxPath.has_value();
    case HasEngineRole:
        return stick.enginePath.has_value();
    case RekordboxPathRole:
        return stick.rekordboxPath ? qtPathFromUtf8(*stick.rekordboxPath) : QString();
    case EnginePathRole:
        return stick.enginePath ? qtPathFromUtf8(*stick.enginePath) : QString();
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
    case HasOneLibraryRole:
        // Looked up when asked rather than kept by the locator: one file
        // test, and only the stick card asks.
        return stick.rekordboxPath.has_value()
               && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(*stick.rekordboxPath);
    case ReadOnlyRole:
        return static_cast<size_t>(index.row()) < m_readOnly.size()
               && m_readOnly[static_cast<size_t>(index.row())];
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
        {HasOneLibraryRole, "hasOneLibrary"},
        {ReadOnlyRole, "readOnly"},
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

namespace
{

// What makes a row the same row across refreshes. A partition node is
// stable while a stick stays plugged in, which is the span that matters
// here; a folder library has no device, so it is its own path.
std::string rowKey(const application::DetectedStick &stick)
{
    return !stick.devicePath.empty() ? stick.devicePath : stick.mountPoint;
}

}  // namespace

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

    // That order decides where a row FIRST appears, and nothing after
    // that moves it.
    //
    // This used to reset the whole model on every refresh, which had two
    // costs. A reset re-sorted every row, so unmounting a stick -- which
    // makes its library unknowable, and hasKnownLibrary false -- sent it
    // to the bottom of the list while the user was looking at it; the
    // card they had just acted on jumped somewhere else. And a reset
    // tells a view only that everything changed, so a view can neither
    // animate an arrival nor keep anything that was on screen: every
    // delegate is destroyed and rebuilt, four times a second's worth of
    // udev chatter included.
    //
    // So: rows that are gone are removed, rows that remain keep their
    // place and are updated where their data moved, and a row that is new
    // is inserted where the sort above says it belongs. The view is told
    // which is which, so it can show the difference.
    auto readOnlyFor = [](const application::DetectedStick &stick) {
        return stick.mounted && !stick.mountPoint.empty()
            && infrastructure::media::isMountedReadOnly(stick.mountPoint);
    };

    std::set<std::string> incoming;
    for (const application::DetectedStick &stick : sticks) {
        incoming.insert(rowKey(stick));
    }
    for (int row = static_cast<int>(m_sticks.size()) - 1; row >= 0; --row) {
        if (incoming.count(rowKey(m_sticks[static_cast<size_t>(row)])) == 0) {
            beginRemoveRows(QModelIndex(), row, row);
            m_sticks.erase(m_sticks.begin() + row);
            m_readOnly.erase(m_readOnly.begin() + row);
            endRemoveRows();
        }
    }

    int after = -1;  // the row the next newcomer follows
    for (const application::DetectedStick &stick : sticks) {
        const std::string key = rowKey(stick);
        auto existing = std::find_if(m_sticks.begin(), m_sticks.end(),
                                      [&key](const application::DetectedStick &have) { return rowKey(have) == key; });
        if (existing != m_sticks.end()) {
            const int row = static_cast<int>(std::distance(m_sticks.begin(), existing));
            const bool wasReadOnly = m_readOnly[static_cast<size_t>(row)];
            const bool nowReadOnly = readOnlyFor(stick);
            // Mounting, unmounting, gaining a library: the same row,
            // saying something different. Compared field by field rather
            // than by an operator== on DetectedStick, because that type
            // belongs to the application layer and would be carrying a
            // comparison for one view's benefit.
            const application::DetectedStick &have = *existing;
            const bool sameRow = have.mountPoint == stick.mountPoint && have.label == stick.label
                && have.mounted == stick.mounted && have.isSdCard == stick.isSdCard
                && have.isFolder == stick.isFolder && have.isBrowsedBackup == stick.isBrowsedBackup
                && have.rekordboxPath == stick.rekordboxPath && have.enginePath == stick.enginePath
                && have.capacityBytes == stick.capacityBytes
                && have.hasNoFilesystem == stick.hasNoFilesystem && have.rootEntries == stick.rootEntries
                && have.identity.libraryId() == stick.identity.libraryId();
            if (!sameRow || wasReadOnly != nowReadOnly) {
                *existing = stick;
                m_readOnly[static_cast<size_t>(row)] = nowReadOnly;
                emit dataChanged(index(row), index(row));
            }
            after = row;
            continue;
        }
        const int row = after + 1;
        beginInsertRows(QModelIndex(), row, row);
        m_sticks.insert(m_sticks.begin() + row, stick);
        m_readOnly.insert(m_readOnly.begin() + row, readOnlyFor(stick));
        endInsertRows();
        after = row;
    }

    // Insertions and removals tell a view its rows changed; they do not
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

namespace
{

// Every catalog cached from a stick that is no longer mounted where it
// was, dropped along with any background read queued for it. A stick
// pulled and plugged back in can come back with its cues changed on a
// player and export.pdb's mtime untouched; the catalog cache compares
// only that mtime, so without this the re-inserted stick was served
// from RAM as it was before. Keyed on the mount point and the library
// id together, so a different stick mounted at the same place in
// between two refreshes counts as the first one gone. Every row counts:
// a stick too anonymous to be announced by stickRemoved has its catalogs
// cached all the same, and an opened folder that went away (an
// unmounted share, a disk pulled) or was closed or replaced is the same
// case as a pulled stick.
void forgetCatalogsOfSticksGone(const std::vector<application::DetectedStick> &before,
                                const std::vector<application::DetectedStick> &after)
{
    for (const application::DetectedStick &was : before) {
        if (!was.mounted || was.mountPoint.empty()) {
            continue;
        }
        const bool stillThere = std::any_of(after.begin(), after.end(), [&](const application::DetectedStick &is) {
            return is.mounted && is.mountPoint == was.mountPoint
                && is.identity.libraryId() == was.identity.libraryId();
        });
        if (!stillThere) {
            LibraryCatalogCache::instance().invalidateEveryCatalogOn(was.mountPoint);
        }
    }
}

}  // namespace

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
        const std::filesystem::path dir = pathFromUtf8(folder.mountPoint);
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
            const auto stickRoot = std::filesystem::weakly_canonical(pathFromUtf8(stick.mountPoint), stickEc);
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
    forgetCatalogsOfSticksGone(m_model.sticks(), sticks);
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
                           qtPathFromUtf8(mountPointFor(identity)));
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
        std::filesystem::canonical(pathFromQString(localPathFromUrl(path)), ec);
    if (ec) {
        return tr("That folder could not be opened: %1").arg(QString::fromStdString(ec.message()));
    }
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return tr("That is not a folder.");
    }
    const std::string canonical = pathToUtf8(dir);

    // A mounted stick's own root is already listed, with the identity the
    // stick actually has. Listing it a second time as a folder would put
    // two rows on one library and let the folder's synthetic id shadow
    // the real one in the pulled-stick lookup.
    for (const application::DetectedStick &stick : m_model.sticks()) {
        std::error_code cmpEc;
        if (!stick.isFolder && stick.mounted
            && std::filesystem::weakly_canonical(pathFromUtf8(stick.mountPoint), cmpEc) == dir) {
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
    const std::filesystem::path archive = pathFromQString(localPathFromUrl(archivePath));
    // One cache directory per archive, named after its path rather than
    // its label: two backups of differently-named sticks must not land on
    // top of each other, and re-opening the same archive should reuse (and
    // refresh) the same directory instead of accumulating copies. Keyed on
    // the canonical path, so the same ZIP reached through a symlinked
    // directory or a ".." spelling is one backup, one cache, one row.
    const std::filesystem::path archiveKey = infrastructure::local::canonicalOrAbsolute(archive);
    const std::filesystem::path cacheRoot =
        infrastructure::paths::localBrowsedBackupsDir() / folderLibraryId(pathToUtf8(archiveKey));

    const application::OpenedStickBackup opened = application::OpenStickBackup::execute(archiveKey, cacheRoot);
    if (!opened.error.empty()) {
        return QString::fromStdString(opened.error);
    }
    // The label is the stick the backup was taken from, when the manifest
    // says; the cache directory's own name is a hash and would tell the
    // user nothing.
    return openFolder(pathToQString(opened.libraryRoot),
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
    return dir.filename().empty() ? pathToUtf8(dir) : pathToUtf8(dir.filename());
}

void MediaController::closeFolder(const QString &path)
{
    // Compared as paths: the folder is kept by its native canonical path
    // and a page hands back the forward-slash one, so on Windows the
    // strings never matched and Close Folder did nothing.
    if (!m_openedFolder || !application::samePath(m_openedFolder->mountPoint, localPathFromUrl(path).toStdString())) {
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
    if (auto archive = infrastructure::local::browsedBackupArchive(pathFromUtf8(folderPath))) {
        infrastructure::rekordbox::forgetArchiveSource(pathToUtf8(*archive));
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
    folder.label = folderLabelFor(pathFromUtf8(folder.mountPoint),
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
    // Paths, not strings: a page's path is forward-slash and a mount point
    // is native, which on Windows are different strings for one drive.
    const std::string wanted = path.toStdString();
    for (const auto &stick : m_model.sticks()) {
        if (application::pathIsAtOrUnder(wanted, stick.mountPoint)) {
            return true;
        }
    }
    return false;
}

QString MediaController::stickLabelForPath(const QString &path) const
{
    const std::string wanted = path.toStdString();  // compared as in pathIsPresent()
    for (const auto &stick : m_model.sticks()) {
        if (application::pathIsAtOrUnder(wanted, stick.mountPoint)) {
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
    // The mount point asked about is usually a page's forward-slash one
    // and the list keeps the native spelling, so they are compared as
    // paths. The exact spelling still wins first: samePath also folds
    // case, and two sticks labelled "usb" and "USB" can both be mounted
    // on Linux.
    for (const bool exact : {true, false}) {
        for (const application::DetectedStick &stick : m_model.sticks()) {
            if (stick.mounted
                && (exact ? stick.mountPoint == mountPoint : application::samePath(stick.mountPoint, mountPoint))) {
                return stick.identity;
            }
        }
    }
    for (const bool exact : {true, false}) {
        for (const auto &[known, identity] : m_lastKnownByMountPoint) {
            if (exact ? known == mountPoint : application::samePath(known, mountPoint)) {
                return identity;
            }
        }
    }
    return std::nullopt;
}

// Every stick with a filesystem and no mount point becomes a candidate
// (except on macOS, which mounts sticks itself; see below), except one the
// user ejected here or one that already failed; both
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
        const QString label = QString::fromStdString(stick.label);
        const auto failedWith = m_autoMountFailed.constFind(devicePath);
        if (failedWith != m_autoMountFailed.constEnd()) {
            if (failedWith.value() == label) {
                continue;  // the same filesystem that would not mount
            }
            // A different label on the same path is a different
            // filesystem: a format, or a swap. Worth one more try.
            m_autoMountFailed.remove(devicePath);
        }
        if (stick.mounted || stick.hasNoFilesystem || queuedOrBusy.contains(devicePath)
            || m_userUnmounted.contains(devicePath)) {
            continue;
        }
        // macOS mounts a removable stick by itself, a second or so after it
        // appears -- after a filesystem check, when the stick was pulled out of
        // a player without ejecting, which can take much longer. An automatic
        // mount of our own could only race that one, and "diskutil mount" says
        // yes to a volume the system mounted meanwhile, so the stick was
        // counted as Seabass's and ejected when Seabass quit. A stick that
        // stays unmounted (one ejected here, or unmounted in Disk Utility) is
        // mounted from its row, as ever.
        // Only the mount is skipped: forgetting sticks that are gone, below,
        // still runs.
#ifndef Q_OS_MACOS
        enqueue(devicePath, true, true, false);
#endif
    }
    for (QSet<QString> *set : {&m_userUnmounted, &m_mountedByUs}) {
        for (auto it = set->begin(); it != set->end();) {
            it = present.contains(*it) ? std::next(it) : set->erase(it);
        }
    }
    for (auto it = m_autoMountFailed.begin(); it != m_autoMountFailed.end();) {
        it = present.contains(it.key()) ? std::next(it) : m_autoMountFailed.erase(it);
    }
    m_taskQueue.erase(std::remove_if(m_taskQueue.begin(), m_taskQueue.end(),
                                     [&](const PendingTask &t) { return !present.contains(t.devicePath); }),
                      m_taskQueue.end());
}

QString MediaController::labelOf(const QString &devicePath) const
{
    for (const application::DetectedStick &stick : m_model.sticks()) {
        if (QString::fromStdString(stick.devicePath) == devicePath) {
            return QString::fromStdString(stick.label);
        }
    }
    return {};
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
        if (result.success && result.mountedHere) {
            m_mountedByUs.insert(task.devicePath);
        } else if (!result.success && task.automatic) {
            m_autoMountFailed.insert(task.devicePath, labelOf(task.devicePath));
        }
    } else if (result.success) {
        m_mountedByUs.remove(task.devicePath);
    }
    // An automatic mount is Seabass being helpful, not something the user
    // asked for, so its failure is not the user's problem to read. It is
    // also the one that loses a race with a format: while mkfs is running
    // the partition is briefly not a mountable filesystem, and udisks says
    // so in words that land on screen as though something were wrong with
    // the stick. The failure is still remembered, and the row still shows
    // the stick as unmounted with a Mount button that reports properly.
    if (!task.automatic || result.success) {
        setErrorMessage(result.errorMessage);
    }
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
