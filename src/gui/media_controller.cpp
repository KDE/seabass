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

    loadOpenedFolders();
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
    // Re-scanned on every refresh, not cached from openFolder(): a folder's
    // catalogs can be written, added or deleted underneath us exactly like
    // a stick's, and the folder itself can be gone by now -- in which case
    // it stays listed with no library so it can be seen and closed, rather
    // than vanishing without explanation.
    // A folder whose path is a mounted stick's own root is that stick,
    // already listed with the identity it really has; a second row would
    // carry a second edit-lock id for one export.pdb. openFolder() refuses
    // this at open time, but a persisted folder can find a stick on its
    // path after a restart or a replug (drive-letter reuse on Windows is
    // routine), so the rule is enforced here, on every refresh.
    // Only paths that actually canonicalised take part: on failure
    // weakly_canonical returns an empty path, and two failures would
    // compare equal (empty == empty) and erase an unrelated folder row.
    std::vector<std::filesystem::path> mountedRoots;
    for (const application::DetectedStick &stick : sticks) {
        if (stick.mounted && !stick.mountPoint.empty()) {
            std::error_code ec;
            const auto root = std::filesystem::weakly_canonical(std::filesystem::path(stick.mountPoint), ec);
            if (!ec && !root.empty()) {
                mountedRoots.push_back(root);
            }
        }
    }
    // Dropped, not merely hidden: a hidden entry stays persisted with no
    // row to close it from, and comes back as a library-less ghost the
    // moment the stick is unplugged. openFolder() already refuses this
    // case by name at open time; this is the same rule for a folder the
    // stick arrived under later.
    const auto coincides = [&](const application::DetectedStick &folder) {
        std::error_code ec;
        const auto folderRoot = std::filesystem::weakly_canonical(std::filesystem::path(folder.mountPoint), ec);
        return !ec && !folderRoot.empty()
               && std::find(mountedRoots.begin(), mountedRoots.end(), folderRoot) != mountedRoots.end();
    };
    // One pass: the coinciding rows move to the tail and out, so each
    // path is canonicalised once per refresh, not twice.
    const auto keepEnd = std::stable_partition(m_openedFolders.begin(), m_openedFolders.end(),
                                               [&](const application::DetectedStick &f) { return !coincides(f); });
    std::vector<application::DetectedStick> dropped(keepEnd, m_openedFolders.end());
    if (!dropped.empty()) {
        m_openedFolders.erase(keepEnd, m_openedFolders.end());
        saveOpenedFolders();
        // The row is going without anyone clicking close, so its edit
        // session -- if one is dirty or holds the lock -- must hear about
        // it the way it would for a pulled stick, or the edits sit
        // unseen until quit and then land on whatever is mounted there.
        for (const application::DetectedStick &folder : dropped) {
            emit stickRemoved(QString::fromStdString(folder.identity.libraryId()),
                              QString::fromStdString(folder.label));
        }
    }
    std::vector<std::string> unreachable;
    for (application::DetectedStick &folder : m_openedFolders) {
        folder.rekordboxPath.reset();
        folder.enginePath.reset();
        infrastructure::media::scanMountedRoot(folder.mountPoint, folder);
        // A folder whose directory is not there right now is not shown.
        //
        // loadOpenedFolders() restores these without checking, on the
        // stated grounds that "detect() does that for every opened folder
        // anyway" -- and this is the line that has to exist for that to
        // be true. Without it a remembered path that has since been
        // deleted came back on the first page for good, as a card with no
        // library in it and nothing to do: one developer's config had 232
        // such rows, all pointing into /tmp directories that had not
        // existed for weeks.
        //
        // Hidden, not forgotten, and deliberately narrower than "has no
        // library": a folder that IS there and has simply had its library
        // deleted keeps its row, because that row is how it gets seen and
        // closed (see case 7 in open_folder_test.cpp -- an older decision
        // this does not overturn). What is dropped from view is only the
        // folder nobody can look at, which is also the case
        // loadOpenedFolders() asks be kept in the store: a share that is
        // off right now and back in a minute must not lose the user's
        // shortcut, so the row stays listed and returns by itself.
        std::error_code dirEc;
        if (!std::filesystem::is_directory(std::filesystem::path(folder.mountPoint), dirEc)
            || dirEc) {
            unreachable.push_back(folder.mountPoint);
            continue;
        }
        // Decided from disk every time, not remembered from openBackup():
        // the marker is what makes the cache self-describing, and it is
        // what survives a restart. Only honoured under the browse cache --
        // see isBrowsedBackupRoot for why a stray marker elsewhere is not.
        folder.isBrowsedBackup = infrastructure::local::isBrowsedBackupRoot(std::filesystem::path(folder.mountPoint));
        sticks.push_back(folder);
    }
    // Same treatment the coinciding rows above get, and for the same
    // reason: a row leaving without anyone clicking close has to reach an
    // edit session holding it, or the edits sit unseen until quit. Only
    // on the transition, though -- a share that stays off would otherwise
    // announce itself removed on every single refresh.
    //
    // And the way BACK matters as much as the way out. stickRemoved puts
    // StickRemovedDialog on screen, which is NoAutoClose and whose
    // "Understood" is enabled only while session.stickPresent -- set true
    // by nothing but stickReturned. The presence bookkeeping below skips
    // folder rows entirely (they never enter m_presentIdentities, so they
    // never turn up in diff.appeared), so a folder that announced itself
    // removed and never announced itself back would leave that dialog
    // with exactly one live button: Discard Changes. Plugging the disk
    // back in -- the recovery the dialog exists to offer -- would not
    // work, and the user's staged edits would be reachable only by
    // throwing them away. So the pair is emitted here, both halves.
    for (const application::DetectedStick &folder : m_openedFolders) {
        const bool nowGone = std::find(unreachable.begin(), unreachable.end(), folder.mountPoint)
                             != unreachable.end();
        const bool wasGone = m_unreachableFolders.count(folder.mountPoint) > 0;
        if (nowGone && !wasGone) {
            emit stickRemoved(QString::fromStdString(folder.identity.libraryId()),
                              QString::fromStdString(folder.label));
        } else if (!nowGone && wasGone) {
            // A folder's identity is its path, so a folder that is back is
            // necessarily the same one: Strength::Folder, not a re-match.
            emit stickReturned(QString::fromStdString(folder.identity.libraryId()),
                               QString::fromUtf8(application::StickIdentity::strengthName(
                                   application::StickIdentity::Strength::Folder)));
        }
    }
    m_unreachableFolders.clear();
    m_unreachableFolders.insert(unreachable.begin(), unreachable.end());
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

    auto existing = std::find_if(m_openedFolders.begin(), m_openedFolders.end(),
                                 [&](const application::DetectedStick &f) { return f.mountPoint == canonical; });
    if (existing != m_openedFolders.end()) {
        *existing = folder;  // re-opening refreshes what is there
    } else {
        m_openedFolders.push_back(folder);
    }
    saveOpenedFolders();
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
    const std::string canonical = path.toStdString();
    auto it = std::find_if(m_openedFolders.begin(), m_openedFolders.end(),
                           [&](const application::DetectedStick &f) { return f.mountPoint == canonical; });
    if (it == m_openedFolders.end()) {
        return;
    }

    // Unsaved edits are the page's business, not this controller's: the
    // stick list already holds the edit registry (or a fake in tests)
    // and refuses the close there while a session on this row is dirty.
    // Depending on EditSessionRegistry from here would invert the one
    // direction that already exists (the registry watches this controller).
    // The shared open archive, if this was a browsed backup: an open
    // handle otherwise stays held until quit, and on Windows blocks
    // replacing that archive with a newer generation.
    if (auto archive = infrastructure::local::browsedBackupArchive(std::filesystem::path(canonical))) {
        infrastructure::rekordbox::forgetArchiveSource(archive->string());
    }

    m_openedFolders.erase(it);
    saveOpenedFolders();
    detect();
}

void MediaController::loadOpenedFolders()
{
    // ("seabass", "seabass") explicitly, never the default constructor:
    // this app sets no organizationName/applicationName, so a default-
    // constructed QSettings resolves to a different (empty-organization)
    // store than every other setting here, and opened folders would
    // silently fail to persist. Same construction as main.cpp and
    // AppSettingsController.
    QSettings settings("seabass", "seabass");
    // One array, one entry per row, path and label together -- so a row is
    // either whole or absent. (Two parallel lists would let an index
    // drift give row N row N+1's label, and on a browsed backup the label
    // is what the backup archive's own name is derived from.) The label
    // matters because a browsed backup's directory is named after a hash;
    // its label is the stick the backup came from.
    const int count = settings.beginReadArray(QStringLiteral("openedFolders"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString path = settings.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            continue;
        }
        application::DetectedStick folder;
        folder.mountPoint = path.toStdString();
        folder.mounted = true;
        folder.isFolder = true;
        const std::filesystem::path dir(folder.mountPoint);
        folder.label = folderLabelFor(dir, settings.value(QStringLiteral("label")).toString());
        folder.identity.label = folder.label;
        folder.identity.explicitLibraryId = folderLibraryId(folder.mountPoint);
        // Deliberately not re-scanned or existence-checked here: detect()
        // does that for every opened folder anyway, and a folder on a
        // network share that is slow or absent at startup must not hold
        // up construction (or disappear from the list for good).
        m_openedFolders.push_back(std::move(folder));
    }
    settings.endArray();
}

void MediaController::saveOpenedFolders()
{
    QSettings settings("seabass", "seabass");  // see loadOpenedFolders()
    settings.beginWriteArray(QStringLiteral("openedFolders"), static_cast<int>(m_openedFolders.size()));
    for (std::size_t i = 0; i < m_openedFolders.size(); ++i) {
        settings.setArrayIndex(static_cast<int>(i));
        settings.setValue(QStringLiteral("path"), QString::fromStdString(m_openedFolders[i].mountPoint));
        settings.setValue(QStringLiteral("label"), QString::fromStdString(m_openedFolders[i].label));
    }
    settings.endArray();
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
