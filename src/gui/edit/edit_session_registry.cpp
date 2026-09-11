// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/browsed_backup_root.hpp"
#include "gui/edit/edit_session_registry.hpp"

#include <QUuid>

#include <filesystem>

#include "gui/media_controller.hpp"
#include "infrastructure/local/file_library_edit_lock_store.hpp"
#include "infrastructure/system/process_liveness.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

namespace
{
constexpr int HeartbeatIntervalMs = 15000;
}

EditSessionRegistry *EditSessionRegistry::instance()
{
    static EditSessionRegistry registry;
    return &registry;
}

EditSessionRegistry *EditSessionRegistry::create(QQmlEngine *, QJSEngine *)
{
    EditSessionRegistry *registry = instance();
    QJSEngine::setObjectOwnership(registry, QJSEngine::CppOwnership);
    return registry;
}

EditSessionRegistry::EditSessionRegistry()
    : EditSessionRegistry(std::make_unique<infrastructure::local::FileLibraryEditLockStore>(
          infrastructure::local::FileLibraryEditLockStore::defaultDirectory()))
{
}

EditSessionRegistry::EditSessionRegistry(std::unique_ptr<application::LibraryEditLockStore> store, QObject *parent)
    : QObject(parent),
      m_store(std::move(store)),
      m_instanceId(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()),
      m_hostName(infrastructure::system::hostName())
{
    m_heartbeat.setInterval(HeartbeatIntervalMs);
    connect(&m_heartbeat, &QTimer::timeout, this, [this]() {
        for (auto &[id, session] : m_sessions) {
            session->heartbeat();
        }
        for (auto &[id, count] : m_directWrites) {
            if (count > 0) {
                m_store->heartbeat(id.toStdString(), m_instanceId);
            }
        }
    });
    m_heartbeat.start();
}

EditSessionRegistry::~EditSessionRegistry()
{
    for (auto &[id, count] : m_directWrites) {
        if (count > 0) {
            m_store->release(id.toStdString(), m_instanceId);
        }
    }
    for (auto &[id, session] : m_sessions) {
        delete session;
    }
}

bool EditSessionRegistry::anyEditing() const
{
    for (const auto &[id, session] : m_sessions) {
        if (session->lockHeld() || session->dirty()) {
            return true;
        }
    }
    return anyWriting();
}

bool EditSessionRegistry::anyWriting() const
{
    for (const auto &[id, session] : m_sessions) {
        if (session->writing()) {
            return true;
        }
    }
    for (const auto &[id, count] : m_directWrites) {
        if (count > 0) {
            return true;
        }
    }
    return false;
}

bool EditSessionRegistry::anyDirty() const
{
    for (const auto &[id, session] : m_sessions) {
        if (session->dirty()) {
            return true;
        }
    }
    return false;
}

QObject *EditSessionRegistry::mediaController() const
{
    return m_mediaController;
}

void EditSessionRegistry::setMediaController(QObject *controller)
{
    auto *media = qobject_cast<MediaController *>(controller);
    if (media == m_mediaController) {
        return;
    }
    if (m_mediaController) {
        disconnect(m_mediaController, nullptr, this, nullptr);
    }
    m_mediaController = media;
    if (m_mediaController) {
        connect(m_mediaController, &MediaController::stickRemoved, this, &EditSessionRegistry::onStickRemoved);
        connect(m_mediaController, &MediaController::stickReturned, this, &EditSessionRegistry::onStickReturned);
    }
    emit mediaControllerChanged();
}

LibraryEditSession *EditSessionRegistry::findSession(const QString &libraryId) const
{
    auto it = m_sessions.find(libraryId);
    return it == m_sessions.end() ? nullptr : it->second;
}

LibraryEditSession *EditSessionRegistry::ensureSession(const QString &libraryId, const QString &stickLabel)
{
    if (LibraryEditSession *existing = findSession(libraryId)) {
        // A controller may have created the session before the page's
        // host, which is the one that knows the label.
        if (existing->stickLabel().isEmpty() && !stickLabel.isEmpty()) {
            existing->setStickLabel(stickLabel);
        }
        return existing;
    }
    QString mountPoint;
    if (m_mediaController) {
        if (auto stick = m_mediaController->stickForLibraryId(libraryId); stick && stick->mounted) {
            mountPoint = QString::fromStdString(stick->mountPoint);
        }
    }
    auto *session = new LibraryEditSession(this, libraryId, stickLabel, mountPoint);
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);
    connect(session, &LibraryEditSession::stateChanged, this, &EditSessionRegistry::stateChanged);
    connect(session, &LibraryEditSession::pendingChanged, this, &EditSessionRegistry::stateChanged);
    connect(session, &LibraryEditSession::saveFinished, this,
            [this, libraryId](const QVariantMap &summary) { emit saveFinished(libraryId, summary); });
    m_sessions.emplace(libraryId, session);
    emit sessionsChanged();
    return session;
}

LibraryEditSession *EditSessionRegistry::openSession(const QString &libraryId, const QString &stickLabel,
                                                     const QString &rekordboxPath, const QString &enginePath)
{
    if (libraryId.isEmpty()) {
        return nullptr;
    }
    LibraryEditSession *session = ensureSession(libraryId, stickLabel);
    session->setLibraryPaths(rekordboxPath, enginePath);
    session->addRef();
    return session;
}

void EditSessionRegistry::closeSession(const QString &libraryId)
{
    LibraryEditSession *session = findSession(libraryId);
    if (!session) {
        return;
    }
    session->release();
    // An idle, unreferenced, unlocked session is just memory: drop it so
    // a later open starts fresh. One that is dirty, writing or still
    // holding the lock stays (the stick-removed flow relies on that).
    if (session->refs() == 0 && !session->dirty() && !session->writing() && !session->lockHeld()
        && session != m_stickRemovedSession) {
        m_sessions.erase(libraryId);
        session->deleteLater();
        emit sessionsChanged();
        emit stateChanged();
    }
}

LibraryEditSession *EditSessionRegistry::sessionFor(const QString &libraryId, const QString &stickLabel)
{
    if (libraryId.isEmpty()) {
        return nullptr;
    }
    return ensureSession(libraryId, stickLabel);
}

bool EditSessionRegistry::hasSession(const QString &libraryId) const
{
    return findSession(libraryId) != nullptr;
}

bool EditSessionRegistry::isReadOnlyLibrary(const QString &libraryId) const
{
    if (m_mediaController) {
        if (const auto stick = m_mediaController->stickForLibraryId(libraryId)) {
            return stick->isBrowsedBackup;
        }
    }
    // No row for this library -- the list has not refreshed yet, the row
    // was dropped, or there is no media controller at all. Fall back to
    // what is on disk, which is what stage() has always asked. Without
    // this the two disagree: a staged edit is refused while a direct
    // write into the same cache proceeds.
    if (const auto it = m_sessions.find(libraryId); it != m_sessions.end() && it->second) {
        return it->second->editsBrowsedBackup();
    }
    return false;
}

void EditSessionRegistry::reportReadOnlyRefusal(const QString &libraryId, const QString &label)
{
    // The label is often empty: a controller may create the session
    // before the page that knows the stick's name. Resolved here so no
    // caller can render '"" is a stick backup being browsed'.
    QString name = label;
    if (name.isEmpty() && m_mediaController) {
        if (const auto stick = m_mediaController->stickForLibraryId(libraryId)) {
            name = QString::fromStdString(stick->label);
        }
    }
    if (name.isEmpty()) {
        name = libraryId;
    }
    emit directWriteRefused(libraryId,
                            QString::fromStdString(infrastructure::local::browsedBackupRefusal(name.toStdString())));
}

QString EditSessionRegistry::mountPointForPath(const QString &anyLibraryPath) const
{
    if (anyLibraryPath.isEmpty()) {
        return {};
    }
    fs::path p(anyLibraryPath.toStdString());
    std::string name = p.filename().string();
    // A catalog folder ("PIONEER", "Engine Library") sits directly under
    // the stick root; anything else is taken to be the root itself.
    if (name == "PIONEER" || name == "Engine Library") {
        return QString::fromStdString(p.parent_path().string());
    }
    return anyLibraryPath;
}

QString EditSessionRegistry::libraryIdForPath(const QString &anyLibraryPath)
{
    QString mountPoint = mountPointForPath(anyLibraryPath);
    if (mountPoint.isEmpty()) {
        return {};
    }
    if (m_mediaController) {
        QString id = m_mediaController->libraryIdForMountPoint(mountPoint);
        if (!id.isEmpty()) {
            return id;
        }
    }
    // No media controller (tests) or a mount point it never saw: the same
    // filesystem-UUID-else-label+size rule, read straight from the mount.
    std::string label = fs::path(mountPoint.toStdString()).filename().string();
    auto info = infrastructure::system::readStickHardwareInfo(mountPoint.toStdString(), label);
    return QString::fromStdString(info.stickIdentifier);
}

bool EditSessionRegistry::isLockedByOther(const QString &libraryId)
{
    auto probe = m_store->probe(libraryId.toStdString(), m_instanceId);
    return probe.status == application::EditLockStatus::HeldByOther;
}

QVariantMap EditSessionRegistry::holderToVariant(const std::optional<application::LibraryEditLock> &holder)
{
    QVariantMap map;
    if (!holder) {
        return map;
    }
    map["instanceId"] = QString::fromStdString(holder->instanceId);
    map["hostname"] = QString::fromStdString(holder->hostname);
    map["pid"] = static_cast<qlonglong>(holder->pid);
    map["stickLabel"] = QString::fromStdString(holder->stickLabel);
    map["mountPoint"] = QString::fromStdString(holder->mountPoint);
    map["startedAtUtc"] = QString::fromStdString(holder->startedAtUtc);
    return map;
}

QVariantMap EditSessionRegistry::lockHolder(const QString &libraryId)
{
    auto probe = m_store->probe(libraryId.toStdString(), m_instanceId);
    return holderToVariant(probe.holder);
}

void EditSessionRegistry::removeLock(const QString &libraryId)
{
    m_store->forceRemove(libraryId.toStdString());
    refreshLocks();
}

application::LibraryEditLock EditSessionRegistry::lockTemplate(const QString &libraryId, const QString &stickLabel,
                                                               const QString &mountPoint) const
{
    application::LibraryEditLock lock;
    lock.libraryId = libraryId.toStdString();
    lock.instanceId = m_instanceId;
    lock.hostname = m_hostName;
    lock.stickLabel = stickLabel.toStdString();
    lock.mountPoint = mountPoint.toStdString();
    lock.pid = infrastructure::system::currentPid();
    lock.processStartId = infrastructure::system::currentProcessStartId();
    return lock;
}

bool EditSessionRegistry::tryEnterDirectWrite(const QString &libraryId, const QString &stickLabel)
{
    return !enterDirectWrite(libraryId, stickLabel).has_value();
}

std::optional<EditSessionRegistry::Refusal> EditSessionRegistry::enterDirectWrite(const QString &libraryId,
                                                                                  const QString &stickLabel)
{
    if (libraryId.isEmpty()) {
        return std::nullopt;  // nothing to lock against (a blank drive)
    }
    if (isReadOnlyLibrary(libraryId)) {
        reportReadOnlyRefusal(libraryId, stickLabel);
        return Refusal{Refusal::Kind::ReadOnly, {}};
    }
    LibraryEditSession *session = findSession(libraryId);
    bool ownsAlready = (session && session->lockHeld()) || m_directWrites[libraryId] > 0;
    if (!ownsAlready) {
        QString mountPoint;
        if (session) {
            mountPoint = session->mountPoint();
        }
        if (!m_store->tryAcquire(lockTemplate(libraryId, stickLabel, mountPoint))) {
            return Refusal{Refusal::Kind::Locked, lockHolder(libraryId)};
        }
    }
    ++m_directWrites[libraryId];
    emit stateChanged();
    return std::nullopt;
}

void EditSessionRegistry::leaveDirectWrite(const QString &libraryId)
{
    auto it = m_directWrites.find(libraryId);
    if (it == m_directWrites.end() || it->second == 0) {
        return;
    }
    if (--it->second == 0) {
        LibraryEditSession *session = findSession(libraryId);
        if (!(session && session->lockHeld())) {
            m_store->release(libraryId.toStdString(), m_instanceId);
        }
    }
    emit stateChanged();
}

void EditSessionRegistry::refreshLocks()
{
    QStringList locked;
    for (const auto &lock : m_store->listAll()) {
        auto probe = m_store->probe(lock.libraryId, m_instanceId);
        switch (probe.status) {
        case application::EditLockStatus::HeldByOther:
            locked << QString::fromStdString(lock.libraryId);
            break;
        case application::EditLockStatus::Stale:
            m_store->forceRemove(lock.libraryId);  // provably dead owner: tidy up
            break;
        default:
            break;
        }
    }
    locked.sort();
    if (locked != m_lockedByOther) {
        m_lockedByOther = locked;
        emit locksChanged();
    }
}

void EditSessionRegistry::saveAll()
{
    for (auto &[id, session] : m_sessions) {
        if (session->dirty() && !session->writing()) {
            session->save();
        }
    }
}

void EditSessionRegistry::discardAll()
{
    for (auto &[id, session] : m_sessions) {
        if (session->dirty()) {
            session->discard();
        }
    }
}

void EditSessionRegistry::sessionStateChanged()
{
    emit stateChanged();
}

void EditSessionRegistry::onStickRemoved(const QString &libraryId, const QString &label)
{
    Q_UNUSED(label);
    LibraryEditSession *session = findSession(libraryId);
    if (!session || !(session->lockHeld() || session->dirty() || session->writing())) {
        return;
    }
    session->setStickPresent(false, QString());
    if (m_stickRemovedSession != session) {
        m_stickRemovedSession = session;
        emit stickRemovedSessionChanged();
    }
}

void EditSessionRegistry::onStickReturned(const QString &libraryId, const QString &identityStrength)
{
    LibraryEditSession *session = findSession(libraryId);
    if (!session) {
        return;
    }
    session->setStickPresent(true, identityStrength);
}

void EditSessionRegistry::setQuitAfterSave(bool value)
{
    if (m_quitAfterSave == value) {
        return;
    }
    m_quitAfterSave = value;
    emit quitAfterSaveChanged();
}

void EditSessionRegistry::acknowledgeStickReturned()
{
    if (!m_stickRemovedSession) {
        return;
    }
    m_stickRemovedSession = nullptr;
    emit stickRemovedSessionChanged();
}

}  // namespace seabass::gui
