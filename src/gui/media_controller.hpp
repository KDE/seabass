#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QSet>
#include <QVector>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "application/ports/removable_media_locator.hpp"
#include "application/ports/removable_media_monitor.hpp"
#include "application/ports/removable_media_mounter.hpp"

namespace seabass::gui
{

// Read-only Qt list model over the sticks MediaController last detected.
// One row per DetectedStick.
class DetectedStickListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by MediaController; not constructible from QML")

public:
    enum Roles {
        LabelRole = Qt::UserRole + 1,
        MountPointRole,
        DevicePathRole,
        MountedRole,
        HasRekordboxRole,
        HasEngineRole,
        RekordboxPathRole,
        EnginePathRole,
        IsSdCardRole,
        // True for a library opened from an ordinary directory rather
        // than found on removable media: the list still shows it, but
        // there is nothing to mount, eject, format or benchmark.
        IsFolderRole,
        // See DetectedStick::isBrowsedBackup: a folder row whose catalogs
        // were extracted from a stick backup. Read-only; the write cards
        // are withheld for it.
        IsBrowsedBackupRole,
        // See application::StickIdentity. libraryId is the key every
        // edit-mode/lock feature uses for "this library"; identityStrength
        // ("hardware"/"filesystem"/"weak"/"none") says how trustworthy
        // "the same stick" answers are.
        LibraryIdRole,
        HardwareSerialRole,
        IdentityStrengthRole,
        // The whole disk's capacity, already carried by DetectedStick for
        // the Format feature, so showing it costs no extra I/O in a model
        // refresh that runs on every replug.
        CapacityBytesRole,
    };

    explicit DetectedStickListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The whole row as a plain object, for QML call sites that need more
    // than one field for a specific row at once (e.g. "the currently
    // selected stick's rekordboxPath and enginePath together") rather
    // than binding a delegate to individual roles. Empty map for an
    // out-of-range row.
    Q_INVOKABLE QVariantMap get(int row) const;

    void setSticks(std::vector<application::DetectedStick> sticks);
    const std::vector<application::DetectedStick> &sticks() const { return m_sticks; }

private:
    std::vector<application::DetectedStick> m_sticks;
};

// Wraps RemovableMediaLocator for QML: detects USB sticks (mounted or not),
// exposed as `sticks`, and auto-refreshes on udev hotplug events via
// RemovableMediaMonitor. Source-selection entry point for every other
// controller.
//
// A stick that shows up with a filesystem but no mount point is mounted
// automatically (at startup and on insertion), so what is on it can be
// read and shown right away instead of every page saying "not mounted".
// Every mount Seabass performed itself -- automatic or via the row's
// mount button -- is remembered and unmounted again when the app quits;
// a stick the desktop had already mounted is left alone. A stick the
// user ejected from the list is not re-mounted until it is re-inserted,
// and one whose mount fails is not retried until then either.
// Result of a background mount/unmount task -- see MediaController::
// mountStick()/unmountStick(). Built entirely on a worker thread, no
// access to the controller itself.
struct MediaTaskResult
{
    bool success = false;
    QString errorMessage;
};

class MediaController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(seabass::gui::DetectedStickListModel *sticks READ sticksModel CONSTANT)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyDevicePath READ busyDevicePath NOTIFY busyChanged)

public:
    explicit MediaController(QObject *parent = nullptr);
    ~MediaController() override;

    DetectedStickListModel *sticksModel() { return &m_model; }
    QString errorMessage() const { return m_errorMessage; }
    bool busy() const { return m_busy; }
    // Which stick's devicePath mount/unmount is in flight -- lets a row's
    // own delegate show a spinner instead of the eject icon for just the
    // stick actually being acted on, not every row.
    QString busyDevicePath() const { return m_busyTask.devicePath; }

    Q_INVOKABLE void detect();

    // Opens an ordinary directory as a library, listed alongside any
    // detected sticks. `path` is checked for the same PIONEER/Engine
    // Library signatures a mounted stick root is (scanMountedRoot, the
    // one definition of what makes a root a library), so a restored stick
    // backup, a copy of a library on an internal disk, or a test fixture
    // all open the same way. Returns a message for the page to show, or
    // an empty string on success; opening a folder that is already open
    // is a no-op, not an error.
    //
    // Everything downstream of here already takes plain paths -- this
    // entry point was the only thing missing.
    //
    // `path` may be a file:// URL, which is what a QML FolderDialog hands
    // over; it is converted with QUrl::toLocalFile, never by stripping the
    // scheme, so a Windows path ("file:///C:/...") and a name with '#' or
    // '%' in it both survive.
    Q_INVOKABLE QString openFolder(const QString &path);

    // openFolder() with the row's name given rather than taken from the
    // directory: a browsed backup should say which stick it came from,
    // not the hash its cache directory is named after.
    QString openFolder(const QString &path, const QString &label);

    // Forgets a folder opened with openFolder(). Nothing on disk is
    // touched; the folder is only dropped from the list, and the shared
    // open archive (a browsed backup) is released. The unsaved-changes
    // check lives in StickListPage.qml, which holds the edit registry.
    Q_INVOKABLE void closeFolder(const QString &path);

    // Opens a full stick backup for browsing, without unpacking it: the
    // catalogs are extracted into a cache directory (about 1% of the
    // archive's metadata; see OpenStickBackup), the analysis files stay
    // in the ZIP and are read per track, and the cache is then opened as
    // a folder library like any other. Returns a message for the page to
    // show, or an empty string on success.
    //
    // Read-only, and enforced: the row is flagged isBrowsedBackup, which
    // withholds every card that writes, and LibraryEditSession refuses to
    // stage a change against a directory carrying the backup marker. The
    // row points at the cache, never at the archive, so even a write that
    // got through could not reach the backup itself. `archivePath` may be
    // a file:// URL, as for openFolder().
    Q_INVOKABLE QString openBackup(const QString &archivePath);

    // Both run the actual mount/unmount (a real syscall/subprocess that
    // can visibly take a moment -- confirmed by this exact freeze once
    // looking like the app had hung or lost the stick) on a background
    // thread rather than blocking the UI thread the way these used to.
    // Queued rather than dropped when another mount/unmount is already
    // running: a click while the app is busy with a *different* stick
    // used to be silently ignored (the row's button was disabled system-
    // wide while anything was in flight, which reads as "does nothing"
    // -- felt often once auto-mount started running in the background).
    // A second click on the same stick before its own queued request
    // has started replaces the first (last click wins); one already
    // running can't be cancelled, so the new request runs right after it.
    Q_INVOKABLE void mountStick(const QString &devicePath);
    Q_INVOKABLE void unmountStick(const QString &devicePath);

    // The library id (StickIdentity::libraryId()) of the stick mounted at
    // mountPoint -- or, if no stick is mounted there right now, of the
    // stick that was last seen there. Pages hold library paths, not
    // identities, and must still resolve their session after the stick
    // has been pulled, which is what the last-seen fallback is for. Empty
    // when the mount point was never seen.
    Q_INVOKABLE QString libraryIdForMountPoint(const QString &mountPoint) const;

    // The library id openFolder() gives a directory: derived from the
    // canonical path, because a folder has no serial or filesystem UUID
    // of its own and the path is the only thing that stays the same
    // across restarts. Exposed for tests.
    static std::string folderLibraryId(const std::string &canonicalPath);
    std::optional<application::StickIdentity> lastKnownIdentity(const std::string &mountPoint) const;
    // The listed stick (removable or folder) whose identity has this
    // library id. The one by-id lookup, so the edit registry's call sites
    // cannot drift in what they filter on.
    //
    // A mounted row wins over an unmounted one with the same id: two rows
    // can share a weak identity (libraryId() falls back to label plus
    // capacity when there is no filesystem UUID), and only a mounted row
    // has a mount point to record in a lock cookie.
    std::optional<application::DetectedStick> stickForLibraryId(const QString &libraryId) const;
    std::string mountPointFor(const application::StickIdentity &identity) const;

signals:
    void errorMessageChanged();
    void busyChanged();
    // A mounted stick disappeared / the exact same stick (StickIdentity::
    // isSameStick) is back, by library id. identityStrength is
    // StickIdentity::strengthName() of the match, so the UI can say how
    // sure "the same stick" is.
    void stickRemoved(const QString &libraryId, const QString &label);
    void stickReturned(const QString &libraryId, const QString &identityStrength);
    // Every stick newly visible since the last detect(), including one
    // that was already plugged in at startup and one seen for the first
    // time -- neither of which stickReturned() covers, because that only
    // fires for a stick this session watched being removed. Anything that
    // needs to look at a stick when it shows up wants this one.
    void stickAppeared(const QString &libraryId, const QString &mountPoint);

public:
    // Unmounts every stick Seabass mounted itself that is still mounted
    // (synchronously, best effort). Runs on QCoreApplication::aboutToQuit
    // and again from the destructor; idempotent.
    void unmountOwnMounts();

    // One requested mount or unmount, waiting its turn -- see
    // enqueue()/processQueue(). `automatic` distinguishes a background
    // auto-mount from a row click only for m_autoMountFailed bookkeeping.
    struct PendingTask
    {
        QString devicePath;
        bool mount = false;
        bool automatic = false;
    };

    void setErrorMessage(const QString &message);
    // Supersedes any queued task for the same device (last request wins;
    // one already running is left to finish) and starts it once nothing
    // else is in flight. `priority`: goes to the front of the queue
    // (a direct row click) rather than the back (an auto-mount).
    void enqueue(const QString &devicePath, bool mount, bool automatic, bool priority);
    void processQueue();
    void startTask(const PendingTask &task);
    void onTaskFinished();
    void queueAutoMounts();

    // Opened folders survive a restart: they are the only libraries in
    // the list that nothing re-detects, so forgetting them on quit would
    // make the feature useless for the case it exists for (coming back to
    // a restored backup tomorrow).
    void loadOpenedFolders();
    void saveOpenedFolders();
    static std::string folderLabelFor(const std::filesystem::path &dir, const QString &given);


    DetectedStickListModel m_model;
    // Kept separately from the model because detect() rebuilds that from
    // the locator every refresh, and these are exactly the rows no
    // locator will ever produce.
    std::vector<application::DetectedStick> m_openedFolders;
    // Opened folders whose directory was missing at the last detect(), so
    // that becoming unreachable is announced once rather than on every
    // refresh for as long as it stays away.
    std::set<std::string> m_unreachableFolders;
    std::unique_ptr<application::RemovableMediaMonitor> m_monitor;
    QTimer m_debounceTimer;
    QString m_errorMessage;
    QFutureWatcher<MediaTaskResult> m_watcher;
    bool m_busy = false;
    PendingTask m_busyTask;
    QVector<PendingTask> m_taskQueue;
    QSet<QString> m_mountedByUs;      // devicePaths Seabass mounted, to unmount on quit
    QSet<QString> m_userUnmounted;    // ejected from the list: leave alone until re-inserted
    QSet<QString> m_autoMountFailed;  // do not retry until re-inserted
    bool m_ownMountsReleased = false;
    // Every mounted stick ever seen this session, by mount point, kept
    // after the stick is gone (see libraryIdForMountPoint()).
    std::map<std::string, application::StickIdentity> m_lastKnownByMountPoint;
    // Presence by identity (see application/stick_presence_diff.hpp):
    // what was mounted at the last detect(), and every stick pulled since
    // that has not come back, for stickRemoved()/stickReturned().
    std::vector<application::StickIdentity> m_presentIdentities;
    std::vector<application::StickIdentity> m_awaitedIdentities;
};

}  // namespace seabass::gui
