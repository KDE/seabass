#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/backup/stick_space.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/undo_tracking.hpp"

namespace seabass::gui
{

class EditSessionRegistry;

// One library's edit state in this instance (see docs/edit-mode-and-
// cancel.md): the staged changes, the edit lock, and the save that writes
// them. Owned by EditSessionRegistry, keyed by library id; pages and
// controllers look it up, never create one.
//
//   state: "idle"    -- open, nothing staged
//          "editing" -- changes staged (the edit lock is held from the
//                       first one on; see lockHeld)
//          "writing" -- a save is running
//
// The lock is taken at the first staged change and kept until the session
// is clean *and* no page holds it open any more (a page keeps its session
// open for its lifetime), or until discard(). GUI thread only; the save
// loop runs on a worker with copies of the changes.
class LibraryEditSession : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Obtained from EditSessionRegistry")
    Q_PROPERTY(QString libraryId READ libraryId CONSTANT)
    Q_PROPERTY(QString stickLabel READ stickLabel NOTIFY stickLabelChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY pendingChanged)
    // The page that owns the staged batch, "" when nothing is staged. No
    // other page may stage into this library until it is clean again.
    Q_PROPERTY(QString editorOwner READ editorOwner NOTIFY pendingChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY stateChanged)
    Q_PROPERTY(bool lockHeld READ lockHeld NOTIFY stateChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingChanged)
    Q_PROPERTY(QStringList pendingDescriptions READ pendingDescriptions NOTIFY pendingChanged)
    Q_PROPERTY(QString writeLabel READ writeLabel NOTIFY writeProgressChanged)
    Q_PROPERTY(int writeCurrent READ writeCurrent NOTIFY writeProgressChanged)
    Q_PROPERTY(int writeTotal READ writeTotal NOTIFY writeProgressChanged)
    Q_PROPERTY(bool cancelRequested READ cancelRequested NOTIFY writeProgressChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(bool stickPresent READ stickPresent NOTIFY stickPresenceChanged)
    Q_PROPERTY(QString stickIdentityStrength READ stickIdentityStrength NOTIFY stickPresenceChanged)
    // {written, total, unit, cancelled, error} of the last save.
    Q_PROPERTY(QVariantMap lastSummary READ lastSummary NOTIFY saveFinished)
    // Where this session's backups will go, measured once when the
    // session opens. See infrastructure/backup/stick_space.hpp.
    // NOTIFY, not CONSTANT: the measurement behind these runs on a worker
    // thread now (it is thousands of stats on a real stick) and lands
    // after the page is already up, so a binding has to be told.
    Q_PROPERTY(bool backupGoesLocal READ backupGoesLocal NOTIFY stickSpaceChanged)
    Q_PROPERTY(double stickBytesFree READ stickBytesFree NOTIFY stickSpaceChanged)
    Q_PROPERTY(double stickBytesCapacity READ stickBytesCapacity NOTIFY stickSpaceChanged)
    Q_PROPERTY(double backupBytesWorstCase READ backupBytesWorstCase NOTIFY stickSpaceChanged)

public:
    LibraryEditSession(EditSessionRegistry *registry, QString libraryId, QString stickLabel, QString mountPoint,
                       QObject *parent = nullptr);
    ~LibraryEditSession() override;

    QString libraryId() const { return m_libraryId; }
    QString stickLabel() const { return m_stickLabel; }
    QString mountPoint() const { return m_mountPoint; }
    QString state() const;
    bool dirty() const { return !m_changes.empty(); }
    QString editorOwner() const { return m_editorOwner; }
    bool writing() const { return m_writing; }
    bool lockHeld() const { return m_lockHeld; }
    int pendingCount() const { return static_cast<int>(m_changes.size()); }
    QStringList pendingDescriptions() const;
    QString writeLabel() const { return m_writeLabel; }
    int writeCurrent() const { return m_writeCurrent; }
    int writeTotal() const { return m_writeTotal; }
    bool cancelRequested() const { return m_cancelRequested; }
    bool canUndo() const { return !m_lastBackups.empty(); }
    bool stickPresent() const { return m_stickPresent; }
    QString stickIdentityStrength() const { return m_stickIdentityStrength; }
    QVariantMap lastSummary() const { return m_lastSummary; }

    // The catalog paths a save writes through. Set by whichever
    // controller stages first (they all know them); a later call with a
    // non-empty path fills in what is still unknown.
    void setLibraryPaths(const QString &rekordboxPath, const QString &enginePath);
    void setStickLabel(const QString &label);
    bool backupGoesLocal() const { return m_stickSpace.backupGoesLocal(); }
    // double, not qint64: QML numbers are doubles anyway, and a stick's
    // byte counts are far inside the 2^53 a double holds exactly.
    double stickBytesFree() const { return static_cast<double>(m_stickSpace.freeBytes); }
    double stickBytesCapacity() const { return static_cast<double>(m_stickSpace.capacityBytes); }
    double backupBytesWorstCase() const { return static_cast<double>(m_stickSpace.worstCaseBackupBytes); }

    // True when this session's library is a stick backup being browsed --
    // read-only by construction. Asked of the paths on disk, so it is
    // still right when the stick list has no row for the library (or has
    // a stale one); the registry consults it for exactly that reason.
    bool editsBrowsedBackup() const;

    QString rekordboxPath() const { return m_rekordboxPath; }
    QString enginePath() const { return m_enginePath; }

    // Stages one change. The first one acquires the edit lock; a refusal
    // (another instance holds it) emits lockRefused(holder), stages
    // nothing and returns false. Re-staging an id replaces the old change.
    bool stage(std::unique_ptr<PendingChange> change);
    Q_INVOKABLE void unstage(const QString &changeId);
    bool hasChange(const QString &changeId) const;

    Q_INVOKABLE void save();
    Q_INVOKABLE void discard();
    Q_INVOKABLE void cancelWrite();
    // Stages one change that restores every backup the last save made,
    // then saves -- so undo gets the lock, the progress and the summary
    // for free.
    Q_INVOKABLE void undoLastSave();

    // Registry plumbing.
    void addRef() { ++m_refs; }
    void release();
    int refs() const { return m_refs; }
    void setStickPresent(bool present, const QString &identityStrength);
    // Drops the cookie if nothing needs it any more (clean, no page open,
    // not writing). Returns true when the cookie was released.
    bool releaseLockIfUnneeded();
    void heartbeat();

signals:
    void stickSpaceChanged();
    void stickLabelChanged();
    void stateChanged();
    void pendingChanged();
    void writeProgressChanged();
    void canUndoChanged();
    void stickPresenceChanged();
    // A change is fully on the stick; controllers update their models.
    void changeApplied(const QString &changeId);
    void saveFinished(const QVariantMap &summary);
    void changesDiscarded();
    // holder: {instanceId, hostname, pid, stickLabel, startedAtUtc}
    void lockRefused(const QVariantMap &holder);
    // A second page tried to stage into a library another page is already
    // editing. Nothing was staged.
    void editorConflict(const QString &owner, const QString &attempted);

private:
    bool acquireLock();
    void releaseLock();
    void clearOwnerIfClean();
    void onSaveFinished();
    void setWriting(bool writing);
    void setWriteProgress(const QString &label, int current, int total);

    EditSessionRegistry *m_registry;
    QString m_libraryId;
    QString m_stickLabel;
    QString m_mountPoint;
    QString m_rekordboxPath;
    QString m_enginePath;
    infrastructure::backup::StickSpace m_stickSpace;
    // Kept so the session can be torn down while a measurement is still
    // running; the task itself touches nothing but a path, so a watcher
    // going away with it is safe.
    QFutureWatcher<infrastructure::backup::StickSpace> m_stickSpaceWatcher;
    std::vector<std::shared_ptr<PendingChange>> m_changes;
    std::vector<UndoableBackup> m_lastBackups;
    QFutureWatcher<SaveLoopResult> m_watcher;
    application::CancellationToken m_writeCancel;
    QString m_writeLabel;
    int m_writeCurrent = 0;
    int m_writeTotal = 0;
    bool m_cancelRequested = false;
    bool m_writing = false;
    bool m_lockHeld = false;
    bool m_stickPresent = true;
    QString m_stickIdentityStrength;
    QVariantMap m_lastSummary;
    QString m_editorOwner;
    QString m_savingUnit;
    QString m_savingVerb;
    int m_refs = 0;
};

}  // namespace seabass::gui
