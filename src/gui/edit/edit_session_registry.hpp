// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QJSEngine>
#include <QObject>
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
        enum class Kind { Locked, ReadOnly };
        Kind kind = Kind::Locked;
        QVariantMap holder;  // Locked only, and may still be empty -- see below

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
    void onStickReturned(const QString &libraryId, const QString &identityStrength);

    std::unique_ptr<application::LibraryEditLockStore> m_store;
    std::string m_instanceId;
    std::string m_hostName;
    std::map<QString, LibraryEditSession *> m_sessions;
    std::map<QString, int> m_directWrites;  // libraryId -> nesting count
    QStringList m_lockedByOther;
    QPointer<MediaController> m_mediaController;
    QPointer<LibraryEditSession> m_stickRemovedSession;
    QTimer m_heartbeat;
    bool m_quitAfterSave = false;
};

}  // namespace seabass::gui
