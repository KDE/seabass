// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QJSEngine>
#include <QObject>

#include <functional>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "application/ports/library_edit_lock_store.hpp"
#include "gui/edit/library_edit_session.hpp"

namespace seabass::gui
{

class MediaController;

// Process-wide owner of every LibraryEditSession, keyed by library id,
// and the one place that talks to the edit-lock store. QML reaches it as
// the EditSessionRegistry singleton; controllers via instance().
//
// Also the source of truth for the process guard (anyEditing/anyWriting)
// and for the "Read Only" state of other instances' libraries
// (lockedByOther, refreshed by refreshLocks()).
class EditSessionRegistry : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool anyEditing READ anyEditing NOTIFY stateChanged)
    Q_PROPERTY(bool anyWriting READ anyWriting NOTIFY stateChanged)
    Q_PROPERTY(bool anyDirty READ anyDirty NOTIFY stateChanged)
    Q_PROPERTY(QStringList lockedByOther READ lockedByOther NOTIFY locksChanged)
    Q_PROPERTY(QObject *mediaController READ mediaController WRITE setMediaController NOTIFY mediaControllerChanged)
    Q_PROPERTY(seabass::gui::LibraryEditSession *stickRemovedSession READ stickRemovedSession NOTIFY
                   stickRemovedSessionChanged)
    // Set by the window while it is saving in order to quit: the page's
    // own summary dialog stays quiet and the window shows one instead.
    Q_PROPERTY(bool quitAfterSave READ quitAfterSave WRITE setQuitAfterSave NOTIFY quitAfterSaveChanged)

public:
    static EditSessionRegistry *instance();
    static EditSessionRegistry *create(QQmlEngine *, QJSEngine *);

    // Test seam: a registry over its own lock directory.
    explicit EditSessionRegistry(std::unique_ptr<application::LibraryEditLockStore> store, QObject *parent = nullptr);
    ~EditSessionRegistry() override;

    bool anyEditing() const;
    bool anyWriting() const;
    bool anyDirty() const;
    QStringList lockedByOther() const { return m_lockedByOther; }
    QObject *mediaController() const;
    void setMediaController(QObject *controller);
    LibraryEditSession *stickRemovedSession() const { return m_stickRemovedSession; }
    bool quitAfterSave() const { return m_quitAfterSave; }
    void setQuitAfterSave(bool value);

    // A page opens its session for its lifetime (refcounted); no lock is
    // taken here -- that happens at the first staged change.
    Q_INVOKABLE seabass::gui::LibraryEditSession *openSession(const QString &libraryId, const QString &stickLabel,
                                                              const QString &rekordboxPath = QString(),
                                                              const QString &enginePath = QString());
    Q_INVOKABLE void closeSession(const QString &libraryId);
    // Lookup without a ref (controllers). Creates an idle session if none
    // exists, so a lazily-editing page (Browse's Add Cue) works too.
    Q_INVOKABLE seabass::gui::LibraryEditSession *sessionFor(const QString &libraryId,
                                                             const QString &stickLabel = QString());
    Q_INVOKABLE bool hasSession(const QString &libraryId) const;

    // The last save's undo, kept per library once its session is dropped
    // (#79): a page left and another opened is a new session, and Undo
    // Last Save, which the save's records on the stick still allow, must
    // not go with the old one. In memory only: after a restart nothing
    // says what changed on the stick since, so nothing is offered.
    struct KeptSave
    {
        std::vector<UndoableBackup> backups;
        bool interrupted = false;
        // The stick's backups folder of the session that saved, as
        // LibraryEditSession::stickBackupDir() names it.
        QString stickBackupDir;
    };
    // Handed to the library's next session, once, when that session is on
    // the same stick folder; nullopt otherwise.
    std::optional<KeptSave> takeKeptSave(const QString &libraryId, const QString &backupDir);
    // Every record Undo Last Save could still restore on this library, a
    // live session's or a kept one's: what Free Up Space must leave alone.
    QStringList lastSaveBackupIds(const QString &libraryId) const;

    // The library id for any catalog path (PIONEER / Engine Library
    // folder) or stick root, via MediaController's identities -- also for
    // a stick that has since been pulled.
    Q_INVOKABLE QString libraryIdForPath(const QString &anyLibraryPath);
    Q_INVOKABLE QString mountPointForPath(const QString &anyLibraryPath) const;

    Q_INVOKABLE bool isLockedByOther(const QString &libraryId);
    Q_INVOKABLE QVariantMap lockHolder(const QString &libraryId);
    // "Remove Lock": drops whoever's cookie, then refreshes.
    Q_INVOKABLE void removeLock(const QString &libraryId);

    // Direct write operations (full stick backup/restore/clone, format,
    // ...) take the same cookie for their duration so other instances see
    // the library as busy; false with the holder in lockHolder() when
    // another instance is editing it.
    // Also false, with directWriteRefused(), for a stick backup being
    // browsed: every direct writer -- backup, clone, restore, format,
    // Engine Library creation, pending-file deletion -- passes through
    // here, so this is the one place the read-only rule holds for all of
    // them, whichever page's button led here.
    //
    // Why a direct write was refused. Produced once, here, so no caller
    // has to re-derive it -- and so the "is there anything to show the
    // user" decision is made in one place rather than seven.
    struct Refusal
    {
        // NotTheStick: the stick at the write's place is not the one the
        // page read (writeRefusalFor); `message` says so.
        enum class Kind { Locked, ReadOnly, NotTheStick };
        Kind kind = Kind::Locked;
        QVariantMap holder;  // Locked only, and may still be empty -- see below
        QString message;     // NotTheStick only

        // True only when there is a holder to name. A lock attempt can
        // fail with nobody to report: an unparseable cookie, a lock
        // directory that cannot be written, or the holder releasing
        // between the attempt and the probe. A "locked by another
        // instance" dialog with a blank Held-by line and a Remove Lock
        // button for a lock nobody holds is worse than saying nothing,
        // which is what these paths did before.
        bool showsLockedDialog() const { return kind == Kind::Locked && !holder.isEmpty(); }
        // ReadOnly refusals have already shown their own dialog, from
        // reportReadOnlyRefusal() -- callers show nothing more.
        bool isReadOnly() const { return kind == Kind::ReadOnly; }
    };

    // Takes the direct-write hold, or says why not; nullopt means the
    // write may proceed.
    std::optional<Refusal> enterDirectWrite(const QString &libraryId, const QString &stickLabel);
    // What is known of the stick at a root: an id, and whether that id
    // tells one stick from every other. Strong: a filesystem UUID, a
    // hardware serial, or an opened folder's own id, as the stick list has
    // it. Weak: label and size, or anything from readStickHardwareInfo()'s
    // fallback, whose ids do not even agree with the stick list's for the
    // same stick.
    struct ProbedIdentity
    {
        QString id;
        bool strong = false;
    };
    ProbedIdentity probeStickIdentity(const QString &stickRoot);
    // The strong id of the stick at `stickRoot` now, or empty when there is
    // none to be had (weak, unlisted, nothing there). Every identity check
    // fails open on empty: it refuses only on two strong ids that differ,
    // so a stick that cannot be told apart is never refused for it (#114).
    QString strongStickIdentityAt(const QString &stickRoot);
    using StickIdentityProbe = std::function<ProbedIdentity(const QString &stickRoot)>;
    // Tests: stands in for the stick at a path, which in a test is a
    // folder on the developer's disk. Empty restores the real one.
    static void setStickIdentityProbeForTesting(StickIdentityProbe probe);
    // Why a direct write for the library locked as `lockId` under `path`
    // must not go ahead, or empty. With a session on that library, the
    // session decides (LibraryEditSession::notTheStickReason). Without one,
    // `expectedStrongId` (strongStickIdentityAt when the page read the
    // stick) and the strong id at `path` now have to agree when both are
    // known.
    QString writeRefusalFor(const QString &lockId, const QString &path, const QString &expectedStrongId);
    Q_INVOKABLE bool tryEnterDirectWrite(const QString &libraryId, const QString &stickLabel);
    // Whether the listed library with this id is a stick backup being
    // browsed -- read-only. Explicit, so a caller refused by
    // tryEnterDirectWrite() can tell this apart from a held lock
    // instead of inferring it from an empty holder.
    Q_INVOKABLE bool isReadOnlyLibrary(const QString &libraryId) const;
    // Emits directWriteRefused() for a read-only library. Used by
    // LibraryEditSession::stage() as well as tryEnterDirectWrite(), so
    // one dialog (Main.qml) shows every such refusal.
    void reportReadOnlyRefusal(const QString &libraryId, const QString &label);
    Q_INVOKABLE void leaveDirectWrite(const QString &libraryId);

    Q_INVOKABLE void refreshLocks();
    Q_INVOKABLE void saveAll();
    Q_INVOKABLE void discardAll();
    Q_INVOKABLE void acknowledgeStickReturned();

    // Session plumbing.
    application::LibraryEditLockStore &lockStore() { return *m_store; }
    const std::string &instanceId() const { return m_instanceId; }
    application::LibraryEditLock lockTemplate(const QString &libraryId, const QString &stickLabel,
                                              const QString &mountPoint) const;
    void sessionStateChanged();
    static QVariantMap holderToVariant(const std::optional<application::LibraryEditLock> &holder);

signals:
    void stateChanged();
    void locksChanged();
    void sessionsChanged();
    void mediaControllerChanged();
    void stickRemovedSessionChanged();
    void quitAfterSaveChanged();
    // Forwarded from every session, for the window's quit flow.
    void saveFinished(const QString &libraryId, const QVariantMap &summary);
    // tryEnterDirectWrite() refused a browsed backup. Main.qml shows it.
    void directWriteRefused(const QString &libraryId, const QString &reason);

private:
    EditSessionRegistry();
    LibraryEditSession *findSession(const QString &libraryId) const;
    LibraryEditSession *ensureSession(const QString &libraryId, const QString &stickLabel);
    void onStickRemoved(const QString &libraryId, const QString &label);
    void onStickReturned(const QString &libraryId, const QString &identityStrength, const QString &mountPoint);

    std::unique_ptr<application::LibraryEditLockStore> m_store;
    std::string m_instanceId;
    std::string m_hostName;
    std::map<QString, LibraryEditSession *> m_sessions;
    std::map<QString, KeptSave> m_keptSaves;
    std::map<QString, int> m_directWrites;  // libraryId -> nesting count
    QStringList m_lockedByOther;
    QPointer<MediaController> m_mediaController;
    QPointer<LibraryEditSession> m_stickRemovedSession;
    QTimer m_heartbeat;
    bool m_quitAfterSave = false;
};

}  // namespace seabass::gui
