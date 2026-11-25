// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// EditSessionRegistry on its own, over a lock directory of its own: the
// refcounted sessions pages open and close, the process guard
// (anyEditing / anyWriting / anyDirty) the window's quit flow reads, the
// direct-write hold every full-stick writer takes, what another
// instance's lock looks like from here, and a stick pulled from under a
// session with work in it.
//
// Until this test the registry only ran inside the QML suites, where a
// page opens one session and nothing ever asks for the same library
// twice, closes the last reference, or pulls the stick.

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QSignalSpy>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <thread>

#include "application/path_key.hpp"
#include "domain/sync_planning.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/direct_write_hold.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/media_controller.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/local/file_library_edit_lock_store.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "nothing_else_running.hpp"
#include "scratch_path.hpp"

using seabass::gui::EditSessionRegistry;
using seabass::gui::LibraryEditSession;
using seabass::gui::PendingChange;
namespace domain = seabass::domain;
using seabass::infrastructure::local::FileLibraryEditLockStore;
namespace fs = std::filesystem;

namespace
{

// A staged change that writes nothing. apply() waits for `release` when
// one is given, so a test can look at a session while its save is
// really running.
class NoopChange : public seabass::gui::PendingChange
{
public:
    explicit NoopChange(QString id, std::atomic<bool> *release = nullptr) : m_id(std::move(id)), m_release(release)
    {
    }
    QString id() const override { return m_id; }
    QString description() const override { return QStringLiteral("nothing"); }
    QString unit() const override { return QStringLiteral("things"); }
    QStringList formatsTouched() const override { return {}; }
    seabass::gui::ChangeOutcome apply(seabass::gui::SaveContext &) override
    {
        while (m_release != nullptr && !m_release->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return seabass::gui::ChangeOutcome::success();
    }

private:
    QString m_id;
    std::atomic<bool> *m_release;
};

std::unique_ptr<FileLibraryEditLockStore> lockStoreIn(const fs::path &dir,
                                                      FileLibraryEditLockStore::LivenessFn liveness = {})
{
    return std::make_unique<FileLibraryEditLockStore>(dir, std::move(liveness));
}

// deleteLater() only runs from the event loop.
void flushDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

}  // namespace

int main(int argc, char **argv)
{
    seabass::tests::assumeNothingElseRunning();
    // No real stick may reach the MediaController this test builds, and
    // no setting it writes may reach the developer's own.
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_edit_session_registry_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    seabass::testing::sandboxSeabassHome(scratch / "home");
    seabass::testing::sandboxSettings(scratch / "config");

    QCoreApplication app(argc, argv);
    const fs::path locks = scratch / "locks";

    // Case 1: the same library asked for twice is one session, counted
    // twice; the label a later caller knows fills in one an earlier
    // caller did not; an empty id has no session.
    {
        EditSessionRegistry registry(lockStoreIn(locks));
        QSignalSpy sessionsChanged(&registry, &EditSessionRegistry::sessionsChanged);

        LibraryEditSession *early = registry.sessionFor(QStringLiteral("lib-a"));
        assert(early != nullptr);
        assert(early->stickLabel().isEmpty());
        assert(early->refs() == 0);  // a lookup takes no reference

        LibraryEditSession *first = registry.openSession(QStringLiteral("lib-a"), QStringLiteral("RV2"));
        LibraryEditSession *second = registry.openSession(QStringLiteral("lib-a"), QStringLiteral("OTHER"));
        assert(first == early && second == early);
        assert(first->refs() == 2);
        assert(first->stickLabel() == "RV2");  // filled once, not overwritten
        assert(sessionsChanged.count() == 1);
        assert(registry.hasSession(QStringLiteral("lib-a")));

        assert(registry.openSession(QString(), QStringLiteral("RV2")) == nullptr);
        assert(registry.sessionFor(QString()) == nullptr);
        assert(!registry.hasSession(QString()));
        std::cout << "case 1 (one session per library, refcounted, label filled in) OK\n";

        // Case 2: closing one of two references keeps it; closing the
        // last destroys it; closing a library with no session is nothing.
        QPointer<LibraryEditSession> watched(first);
        registry.closeSession(QStringLiteral("lib-a"));
        assert(registry.hasSession(QStringLiteral("lib-a")));
        assert(first->refs() == 1);
        registry.closeSession(QStringLiteral("lib-a"));
        assert(!registry.hasSession(QStringLiteral("lib-a")));
        assert(sessionsChanged.count() == 2);
        flushDeferredDeletes();
        assert(watched.isNull());
        registry.closeSession(QStringLiteral("never-opened"));
        assert(sessionsChanged.count() == 2);

        // A fresh open after that is a fresh session with one reference.
        LibraryEditSession *again = registry.openSession(QStringLiteral("lib-a"), QStringLiteral("RV2"));
        assert(again->refs() == 1);
        std::cout << "case 2 (the last close destroys the session, an earlier one does not) OK\n";
    }

    // Case 3: a session with work staged holds its library's lock and is
    // not dropped when its page closes; discarding lets it go.
    {
        EditSessionRegistry registry(lockStoreIn(locks));
        LibraryEditSession *session = registry.openSession(QStringLiteral("lib-b"), QStringLiteral("B"));
        assert(!registry.anyEditing() && !registry.anyDirty() && !registry.anyWriting());
        assert(session->stage(std::make_unique<NoopChange>(QStringLiteral("test:1"))));
        assert(session->lockHeld());
        assert(registry.anyEditing());
        assert(registry.anyDirty());
        assert(!registry.anyWriting());

        registry.closeSession(QStringLiteral("lib-b"));
        assert(registry.hasSession(QStringLiteral("lib-b")));  // dirty: kept
        assert(session->refs() == 0);

        registry.discardAll();
        assert(!session->dirty());
        assert(!session->lockHeld());  // no page, no work: the cookie went
        assert(!registry.anyEditing());
        std::cout << "case 3 (a dirty session outlives its page; discarding lets it go) OK\n";
    }

    // Case 4: the direct-write hold, across two libraries and nested on
    // one: anyWriting stays on until the last one is left, and leaving a
    // library never entered changes nothing.
    {
        EditSessionRegistry registry(lockStoreIn(locks));
        QSignalSpy stateChanged(&registry, &EditSessionRegistry::stateChanged);
        assert(!registry.enterDirectWrite(QStringLiteral("lib-c"), QStringLiteral("C")).has_value());
        assert(registry.tryEnterDirectWrite(QStringLiteral("lib-c"), QStringLiteral("C")));  // nested
        assert(registry.tryEnterDirectWrite(QStringLiteral("lib-d"), QStringLiteral("D")));
        assert(registry.anyWriting());
        assert(registry.anyEditing());  // writing counts as editing
        assert(!registry.anyDirty());

        registry.leaveDirectWrite(QStringLiteral("lib-c"));
        assert(registry.anyWriting());  // lib-c once more, and lib-d
        registry.leaveDirectWrite(QStringLiteral("lib-c"));
        assert(registry.anyWriting());  // lib-d
        registry.leaveDirectWrite(QStringLiteral("lib-d"));
        assert(!registry.anyWriting());
        const int signalsSoFar = stateChanged.count();
        registry.leaveDirectWrite(QStringLiteral("lib-d"));    // already left
        registry.leaveDirectWrite(QStringLiteral("unknown"));  // never entered
        assert(stateChanged.count() == signalsSoFar);

        // An empty id is a blank drive: nothing to lock, nothing refused.
        assert(!registry.enterDirectWrite(QString(), QString()).has_value());
        assert(!registry.anyWriting());
        std::cout << "case 4 (direct writes nest and span libraries; the last leave ends them) OK\n";
    }

    // Case 5: anyWriting while a session's save is really running, with a
    // second, idle session beside it: the one writing session is enough.
    {
        const fs::path stick = scratch / "stick-e";
        fs::create_directories(stick / "PIONEER");
        EditSessionRegistry registry(lockStoreIn(locks));
        LibraryEditSession *idle = registry.openSession(QStringLiteral("lib-idle"), QStringLiteral("IDLE"));
        LibraryEditSession *busy = registry.openSession(QStringLiteral("lib-e"), QStringLiteral("E"),
                                                        seabass::gui::pathToQString(stick / "PIONEER"));
        std::atomic<bool> release{false};
        assert(busy->stage(std::make_unique<NoopChange>(QStringLiteral("test:save"), &release)));
        QSignalSpy finished(busy, &LibraryEditSession::saveFinished);
        busy->save();
        assert(busy->writing());
        assert(!idle->writing());
        assert(registry.anyWriting());

        // Closing the page of a session mid-save must not drop it.
        registry.closeSession(QStringLiteral("lib-e"));
        assert(registry.hasSession(QStringLiteral("lib-e")));

        release = true;
        assert(finished.wait(20000));
        assert(!busy->writing());
        assert(!registry.anyWriting());
        const QVariantMap summary = finished.takeFirst().at(0).toMap();
        // Said before the assert: this failed once under the full suite
        // (2026-09-29, 0.8) and passed on every rerun, with nothing to
        // say which error the save reported.
        if (!summary.value("error").toString().isEmpty()) {
            std::cerr << "the save reported: " << summary.value("error").toString().toStdString() << "\n";
        }
        assert(summary.value("error").toString().isEmpty());
        assert(summary.value("written").toInt() == 1);
        std::cout << "case 5 (anyWriting follows a running save, not the idle session beside it) OK\n";
    }

    // Case 6: another instance's lock, seen from here. Two registries in
    // one process are two instances with two ids over one lock directory.
    {
        EditSessionRegistry holder(lockStoreIn(locks));
        EditSessionRegistry other(lockStoreIn(locks));
        assert(holder.tryEnterDirectWrite(QStringLiteral("lib-f"), QStringLiteral("F")));

        QSignalSpy locksChanged(&other, &EditSessionRegistry::locksChanged);
        const auto refusal = other.enterDirectWrite(QStringLiteral("lib-f"), QStringLiteral("F"));
        assert(refusal.has_value());
        assert(refusal->kind == EditSessionRegistry::Refusal::Kind::Locked);
        assert(refusal->showsLockedDialog());
        assert(!refusal->isReadOnly());
        assert(refusal->holder.value("instanceId").toString().toStdString() == holder.instanceId());
        assert(refusal->holder.value("stickLabel").toString() == "F");
        assert(!other.anyWriting());  // refused: no hold was counted

        assert(other.isLockedByOther(QStringLiteral("lib-f")));
        assert(!holder.isLockedByOther(QStringLiteral("lib-f")));  // its own
        other.refreshLocks();
        assert(other.lockedByOther() == QStringList{QStringLiteral("lib-f")});
        assert(locksChanged.count() == 1);
        other.refreshLocks();  // unchanged: no second signal
        assert(locksChanged.count() == 1);

        // Remove Lock drops it whoever holds it.
        other.removeLock(QStringLiteral("lib-f"));
        assert(other.lockedByOther().isEmpty());
        assert(locksChanged.count() == 2);
        assert(other.lockHolder(QStringLiteral("lib-f")).isEmpty());
        assert(other.tryEnterDirectWrite(QStringLiteral("lib-f"), QStringLiteral("F")));
        other.leaveDirectWrite(QStringLiteral("lib-f"));
        std::cout << "case 6 (another instance's lock refuses, is listed, and can be removed) OK\n";
    }

    // Case 7: a registry going away gives back the direct-write holds it
    // still had, so the next instance is not locked out by a dead one.
    {
        {
            EditSessionRegistry leaving(lockStoreIn(locks));
            assert(leaving.tryEnterDirectWrite(QStringLiteral("lib-g"), QStringLiteral("G")));
        }
        EditSessionRegistry next(lockStoreIn(locks));
        assert(!next.isLockedByOther(QStringLiteral("lib-g")));
        assert(next.tryEnterDirectWrite(QStringLiteral("lib-g"), QStringLiteral("G")));
        next.leaveDirectWrite(QStringLiteral("lib-g"));
        std::cout << "case 7 (a destroyed registry releases its direct-write holds) OK\n";
    }

    // Case 8: a lock whose owner is provably dead is tidied away by
    // refreshLocks() rather than listed.
    {
        EditSessionRegistry holder(lockStoreIn(locks));
        assert(holder.tryEnterDirectWrite(QStringLiteral("lib-h"), QStringLiteral("H")));
        EditSessionRegistry seesItDead(lockStoreIn(locks, [](std::int64_t, std::uint64_t) { return false; }));
        seesItDead.refreshLocks();
        assert(seesItDead.lockedByOther().isEmpty());
        assert(!fs::exists(FileLibraryEditLockStore(locks).pathFor("lib-h")));
        std::cout << "case 8 (a stale lock is removed, not listed) OK\n";
    }

    // Case 9: a stick backup being browsed is read-only, asked of the
    // disk when no media controller knows the library; the refusal names
    // the label, or the library id when there is none.
    {
        const fs::path browsed = scratch / "browsed";
        fs::create_directories(browsed / "PIONEER");
        assert(seabass::infrastructure::local::writeBrowsedBackupMarker(browsed, scratch / "backup.zip", browsed));
        EditSessionRegistry registry(lockStoreIn(locks));
        registry.openSession(QStringLiteral("lib-ro"), QString(), seabass::gui::pathToQString(browsed / "PIONEER"));
        assert(registry.isReadOnlyLibrary(QStringLiteral("lib-ro")));
        assert(!registry.isReadOnlyLibrary(QStringLiteral("no-such-library")));

        QSignalSpy refused(&registry, &EditSessionRegistry::directWriteRefused);
        const auto refusal = registry.enterDirectWrite(QStringLiteral("lib-ro"), QString());
        assert(refusal.has_value() && refusal->isReadOnly() && !refusal->showsLockedDialog());
        assert(!registry.anyWriting());
        assert(refused.count() == 1);
        const QString withoutLabel = refused.takeFirst().at(1).toString();
        assert(withoutLabel.toStdString() == seabass::infrastructure::local::browsedBackupRefusal("lib-ro"));
        assert(!registry.tryEnterDirectWrite(QStringLiteral("lib-ro"), QStringLiteral("Friday set")));
        const QString withLabel = refused.takeFirst().at(1).toString();
        assert(withLabel.toStdString() == seabass::infrastructure::local::browsedBackupRefusal("Friday set"));
        std::cout << "case 9 (a browsed backup refuses a direct write, named) OK\n";
    }

    // Case 10: a stick pulled from under a session. One with work in it
    // is marked gone and offered to the window; the session stays even
    // with no page and nothing staged; the stick coming back marks it
    // present; an idle session is left alone.
    {
        seabass::gui::MediaController media;
        EditSessionRegistry registry(lockStoreIn(locks));
        QSignalSpy controllerChanged(&registry, &EditSessionRegistry::mediaControllerChanged);
        registry.setMediaController(&media);
        registry.setMediaController(&media);  // the same one again: no signal
        assert(controllerChanged.count() == 1);
        assert(registry.mediaController() == &media);

        LibraryEditSession *idle = registry.openSession(QStringLiteral("lib-idle"), QStringLiteral("IDLE"));
        LibraryEditSession *working = registry.openSession(QStringLiteral("lib-gone"), QStringLiteral("GONE"));
        assert(working->stage(std::make_unique<NoopChange>(QStringLiteral("test:gone"))));

        QSignalSpy removedChanged(&registry, &EditSessionRegistry::stickRemovedSessionChanged);
        emit media.stickRemoved(QStringLiteral("lib-idle"), QStringLiteral("IDLE"));
        assert(idle->stickPresent());
        assert(registry.stickRemovedSession() == nullptr);
        emit media.stickRemoved(QStringLiteral("unknown"), QStringLiteral("X"));
        assert(registry.stickRemovedSession() == nullptr);

        emit media.stickRemoved(QStringLiteral("lib-gone"), QStringLiteral("GONE"));
        assert(!working->stickPresent());
        assert(registry.stickRemovedSession() == working);
        assert(removedChanged.count() == 1);
        emit media.stickRemoved(QStringLiteral("lib-gone"), QStringLiteral("GONE"));  // told twice
        assert(removedChanged.count() == 1);

        // The window's dialog is up for it: neither the page closing nor
        // the work being discarded may take the session from under it.
        registry.closeSession(QStringLiteral("lib-gone"));
        working->discard();
        registry.closeSession(QStringLiteral("lib-gone"));
        assert(registry.hasSession(QStringLiteral("lib-gone")));

        emit media.stickReturned(QStringLiteral("lib-gone"), QStringLiteral("uuid"), QString());
        assert(working->stickPresent());
        assert(working->stickIdentityStrength() == "uuid");
        emit media.stickReturned(QStringLiteral("unknown"), QStringLiteral("uuid"), QString());  // no session: nothing

        registry.acknowledgeStickReturned();
        assert(registry.stickRemovedSession() == nullptr);
        assert(removedChanged.count() == 2);
        registry.acknowledgeStickReturned();  // nothing left to acknowledge
        assert(removedChanged.count() == 2);

        registry.setMediaController(nullptr);
        assert(registry.mediaController() == nullptr);
        emit media.stickRemoved(QStringLiteral("lib-idle"), QStringLiteral("IDLE"));  // disconnected
        assert(registry.stickRemovedSession() == nullptr);
        std::cout << "case 10 (a pulled stick flags the session with work in it, and only that one) OK\n";
    }

    // Case 11: where a library path puts the stick root, and the quit
    // flag's signal.
    {
        EditSessionRegistry registry(lockStoreIn(locks));
        const fs::path root = scratch / "stick-k";
        assert(registry.mountPointForPath(seabass::gui::pathToQString(root / "PIONEER"))
               == seabass::gui::pathToQString(root));
        assert(registry.mountPointForPath(seabass::gui::pathToQString(root / "Engine Library"))
               == seabass::gui::pathToQString(root));
        assert(registry.mountPointForPath(seabass::gui::pathToQString(root)) == seabass::gui::pathToQString(root));
        assert(registry.mountPointForPath(QString()).isEmpty());
        assert(registry.libraryIdForPath(QString()).isEmpty());

        QSignalSpy quitChanged(&registry, &EditSessionRegistry::quitAfterSaveChanged);
        registry.setQuitAfterSave(true);
        registry.setQuitAfterSave(true);
        assert(registry.quitAfterSave());
        assert(quitChanged.count() == 1);
        std::cout << "case 11 (catalog folders map to their stick root; quit flag) OK\n";
    }

    // Case 12: where a returned stick is, against where the session's
    // catalogs are. The same root spelled with a trailing separator is
    // the same place (filename() of ".../PIONEER/" is empty, and used to
    // give the wrong root); another mount point is not, and the session
    // stays where it was rather than following it.
    {
        seabass::gui::MediaController media;
        EditSessionRegistry registry(lockStoreIn(locks));
        registry.setMediaController(&media);
        const fs::path was = scratch / "return-sep-a";
        const QString id = QStringLiteral("lib-sep");
        LibraryEditSession *session = registry.openSession(
            id, QStringLiteral("SEP"), seabass::gui::pathToQString(was / "PIONEER") + "/",
            seabass::gui::pathToQString(was / "Engine Library") + "/");
        assert(seabass::application::samePath(session->stickRoot().toStdString(), seabass::pathToUtf8(was)));
        assert(session->stage(std::make_unique<NoopChange>(QStringLiteral("test:sep"))));
        emit media.stickRemoved(id, QStringLiteral("SEP"));
        emit media.stickReturned(id, QStringLiteral("hardware"), seabass::gui::pathToQString(was) + "/");
        assert(session->stickPresent() && !session->stickBackElsewhere() && "the same place, trailing separator or not");
        emit media.stickRemoved(id, QStringLiteral("SEP"));
        emit media.stickReturned(id, QStringLiteral("hardware"), seabass::gui::pathToQString(scratch / "return-sep-b"));
        assert(session->stickPresent() && session->stickBackElsewhere());
        assert(seabass::application::samePath(session->stickRoot().toStdString(), seabass::pathToUtf8(was))
               && "the session stays where it was");
        assert(session->notTheStickReason().contains(QStringLiteral("at another place")));
        registry.acknowledgeStickReturned();
        session->discard();
        registry.closeSession(id);
        registry.setMediaController(nullptr);
        std::cout << "case 12 (a stick back elsewhere is not followed; the same root with a separator is the same) OK\n";
    }

    // Case 13: stick X with changes staged is pulled, and stick Y, another
    // one with the same label, is mounted where X was. Whatever path the
    // page holds, Save writes nothing to Y and backs nothing up there: the
    // stick at the session's root is not X. Y with another filesystem id is
    // told apart outright; Y matched to X by label and size alone (a weak
    // return) may be X or not, and is refused too. A Sync Cue Points plan,
    // then a playlist edit (one page's changes per save).
    {
        const fs::path source = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library";
        // A stick's identity, in this test: a file at its root naming it.
        // On the developer's disk every folder is the same filesystem.
        const auto stickCopy = [&](const fs::path &root, const char *identity) {
            fs::remove_all(root);
            fs::create_directories(root);
            fs::copy(source / "rekordbox", root / "PIONEER", fs::copy_options::recursive);
            fs::copy(source / "engine", root / "Engine Library", fs::copy_options::recursive);
            std::ofstream(root / ".test-stick-id") << identity;
        };
        // "strong:<id>" or "weak:<id>", as the stick list would know it.
        EditSessionRegistry::setStickIdentityProbeForTesting([](const QString &stickRoot) {
            std::ifstream in(seabass::gui::pathFromQString(stickRoot) / ".test-stick-id");
            std::string identity;
            std::getline(in, identity);
            const QString text = QString::fromStdString(identity);
            return EditSessionRegistry::ProbedIdentity{text.section(QLatin1Char(':'), 1),
                                                       text.startsWith(QStringLiteral("strong:"))};
        });
        const auto snapshot = [](const fs::path &root) {
            std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> files;
            for (const auto &entry : fs::recursive_directory_iterator(root)) {
                if (entry.is_regular_file()) {
                    files[seabass::pathToUtf8(entry.path().lexically_relative(root))] = {entry.file_size(),
                                                                                        entry.last_write_time()};
                }
            }
            return files;
        };

        const auto refusedOnY = [&](const char *what, const char *yIdentity, const char *strength,
                                    const char *expected, auto makeChange) {
            const fs::path mount = scratch / "swap-a3";
            const fs::path xAway = scratch / "swap-x-away";
            fs::remove_all(xAway);
            stickCopy(mount, "strong:X");
            seabass::gui::MediaController media;
            EditSessionRegistry registry(lockStoreIn(locks));
            registry.setMediaController(&media);
            const QString id = QStringLiteral("lib-swap");
            LibraryEditSession *session = registry.openSession(id, QStringLiteral("A3"),
                                                               seabass::gui::pathToQString(mount / "PIONEER"),
                                                               seabass::gui::pathToQString(mount / "Engine Library"));
            assert(session->stage(makeChange(mount)));

            // X pulled and kept away; Y mounted where X was.
            emit media.stickRemoved(id, QStringLiteral("A3"));
            fs::rename(mount, xAway);
            stickCopy(mount, yIdentity);
            if (std::string(strength) == "weak") {
                emit media.stickReturned(id, QStringLiteral("weak"), seabass::gui::pathToQString(mount));
            }
            registry.acknowledgeStickReturned();
            const auto yBefore = snapshot(mount);
            const auto xBefore = snapshot(xAway);

            QSignalSpy finished(session, &LibraryEditSession::saveFinished);
            session->save();
            if (finished.count() == 0) {
                finished.wait(120000);
            }
            assert(finished.count() == 1);
            const QVariantMap summary = finished.takeFirst().at(0).toMap();
            const QString error = summary.value("error").toString();
            if (!error.contains(QString::fromUtf8(expected))) {
                std::cerr << what << ": expected a refusal saying \"" << expected << "\", got \"" << error.toStdString()
                          << "\"\n";
                assert(false);
            }
            assert(summary.value("written").toInt() == 0);
            assert(snapshot(mount) == yBefore && "nothing written into stick Y");
            assert(snapshot(xAway) == xBefore && "nor into X");
            assert(!fs::exists(mount / "Seabass") && !fs::exists(xAway / "Seabass") && "no backup taken");
            assert(session->pendingCount() == 1 && "the change stays staged, to be discarded");
            session->discard();
            registry.closeSession(id);
            registry.setMediaController(nullptr);
            fs::remove_all(mount);
            fs::remove_all(xAway);
        };

        const auto syncPlan = [](const fs::path &root) -> std::unique_ptr<PendingChange> {
            domain::SyncPlan plan;
            plan.direction = domain::SyncPlan::Direction::ToB;
            plan.match.trackA.format = "engine";
            plan.match.trackB.format = "rekordbox";
            return std::make_unique<seabass::gui::SyncPlanChange>(seabass::gui::pathToQString(root / "PIONEER"),
                                                                  seabass::gui::pathToQString(root / "Engine Library"),
                                                                  plan, 1);
        };
        const auto playlistEdit = [](const fs::path &root) -> std::unique_ptr<PendingChange> {
            return std::make_unique<seabass::gui::CreatePlaylistChange>(
                seabass::gui::pathToQString(root / "PIONEER"), seabass::gui::pathToQString(root / "Engine Library"),
                std::string("Swapped"), QStringList{QStringLiteral("rekordbox"), QStringLiteral("engine")});
        };
        refusedOnY("a Sync Cue Points plan, Y another stick", "strong:Y", "", "another stick is there now", syncPlan);
        refusedOnY("a playlist edit, Y another stick", "strong:Y", "", "another stick is there now", playlistEdit);
        refusedOnY("a playlist edit, Y matched by label only", "weak:A3-16", "weak", "may be a different stick",
                   playlistEdit);
        std::cout << "case 13 (another stick where the session's was gets nothing written) OK\n";

        // Case 14: the same for a direct write (Clean Up's prune, Delete
        // Orphaned Files, a clone, Recordings): the page passes the id it
        // read the stick under, and another stick at the path refuses it
        // with nothing taken. The stick it read is written to.
        {
            const fs::path mount = scratch / "direct-a3";
            fs::create_directories(mount);
            std::ofstream(mount / ".test-stick-id") << "strong:Y";
            seabass::gui::DirectWriteHold stale;
            const auto refused = stale.acquire({QStringLiteral("lib-direct")}, QStringLiteral("A3"), {},
                                               {seabass::gui::pathToQString(mount / "PIONEER")}, {QStringLiteral("X")});
            assert(refused && refused->kind == EditSessionRegistry::Refusal::Kind::NotTheStick);
            assert(refused->message.contains(QStringLiteral("another stick is there now")));
            assert(!stale.held() && "nothing taken");
            seabass::gui::DirectWriteHold current;
            assert(!current.acquire({QStringLiteral("lib-direct")}, QStringLiteral("A3"), {},
                                    {seabass::gui::pathToQString(mount / "PIONEER")}, {QStringLiteral("Y")}));
            assert(current.held());
            current.release();
            // A page that read a stick with no strong id is not refused on it.
            seabass::gui::DirectWriteHold unknown;
            assert(!unknown.acquire({QStringLiteral("lib-direct")}, QStringLiteral("A3"), {},
                                    {seabass::gui::pathToQString(mount / "PIONEER")}, {QString()}));
            unknown.release();
            fs::remove_all(mount);
            std::cout << "case 14 (a direct write meets the stick it read, or nothing) OK\n";
        }
        // Case 15: a stick known only weakly when the session recorded it
        // (the fallback's label and size) and strongly when it saves (the
        // stick list's filesystem id): two sources that do not agree on one
        // and the same stick. Not refused: identity refuses only on two
        // strong ids that differ.
        {
            const fs::path mount = scratch / "weak-then-strong";
            fs::remove_all(mount);
            fs::create_directories(mount);
            std::ofstream(mount / ".test-stick-id") << "weak:A3-16000000000";
            EditSessionRegistry registry(lockStoreIn(locks));
            const QString id = QStringLiteral("lib-weak-strong");
            LibraryEditSession *session = registry.openSession(id, QStringLiteral("A3"),
                                                               seabass::gui::pathToQString(mount / "PIONEER"));
            assert(session->stage(std::make_unique<NoopChange>(QStringLiteral("test:weak"))));
            std::ofstream(mount / ".test-stick-id", std::ios::trunc) << "strong:0123-4567";
            QSignalSpy finished(session, &LibraryEditSession::saveFinished);
            session->save();
            if (finished.count() == 0) {
                finished.wait(60000);
            }
            const QString error = finished.takeFirst().at(0).toMap().value("error").toString();
            if (!error.isEmpty()) {
                std::cerr << "a weak id then a strong one was refused: " << error.toStdString() << "\n";
                assert(false);
            }
            registry.closeSession(id);
            fs::remove_all(mount);
            std::cout << "case 15 (a weak id on either side never refuses) OK\n";
        }

        // Case 16: matched by label and size only on its return, then
        // discarded: the stick at the root verifies as this one by its
        // strong id, and the page stages again and saves.
        {
            const fs::path mount = scratch / "weak-return";
            fs::remove_all(mount);
            fs::create_directories(mount);
            std::ofstream(mount / ".test-stick-id") << "strong:X";
            seabass::gui::MediaController media;
            EditSessionRegistry registry(lockStoreIn(locks));
            registry.setMediaController(&media);
            const QString id = QStringLiteral("lib-weak-return");
            LibraryEditSession *session = registry.openSession(id, QStringLiteral("A3"),
                                                               seabass::gui::pathToQString(mount / "PIONEER"));
            assert(session->stage(std::make_unique<NoopChange>(QStringLiteral("test:first"))));
            emit media.stickRemoved(id, QStringLiteral("A3"));
            emit media.stickReturned(id, QStringLiteral("weak"), seabass::gui::pathToQString(mount));
            registry.acknowledgeStickReturned();
            assert(session->notTheStickReason().contains(QStringLiteral("may be a different stick")));
            session->discard();
            assert(session->notTheStickReason().isEmpty() && "verified on discard");
            assert(session->stage(std::make_unique<NoopChange>(QStringLiteral("test:again"))));
            QSignalSpy finished(session, &LibraryEditSession::saveFinished);
            session->save();
            if (finished.count() == 0) {
                finished.wait(60000);
            }
            const QString error = finished.takeFirst().at(0).toMap().value("error").toString();
            if (!error.isEmpty()) {
                std::cerr << "staging again after a verified return was refused: " << error.toStdString() << "\n";
                assert(false);
            }
            registry.closeSession(id);
            registry.setMediaController(nullptr);
            fs::remove_all(mount);
            std::cout << "case 16 (a discard after a verified return lets the page stage again) OK\n";
        }
        EditSessionRegistry::setStickIdentityProbeForTesting({});
    }

    // Case 17: opened folders, through the real stick list. Folder A has
    // changes staged, then folder B is opened, which takes A off the list.
    // A unlisted is unknown, not another stick: its save goes through.
    {
        const fs::path folderA = scratch / "folder-a";
        const fs::path folderB = scratch / "folder-b";
        for (const fs::path &folder : {folderA, folderB}) {
            fs::remove_all(folder);
            fs::create_directories(folder / "PIONEER" / "rekordbox");
            std::ofstream(folder / "PIONEER" / "rekordbox" / "export.pdb") << "x";
            fs::create_directories(folder / "Engine Library" / "Database2");
            std::ofstream(folder / "Engine Library" / "Database2" / "m.db") << "x";
        }
        seabass::gui::MediaController media;
        EditSessionRegistry registry(lockStoreIn(locks));
        registry.setMediaController(&media);
        assert(media.openFolder(seabass::gui::pathToQString(folderA)).isEmpty());
        const QString idA = registry.libraryIdForPath(seabass::gui::pathToQString(folderA / "PIONEER"));
        assert(!idA.isEmpty());
        LibraryEditSession *session = registry.openSession(idA, QStringLiteral("A"),
                                                           seabass::gui::pathToQString(folderA / "PIONEER"),
                                                           seabass::gui::pathToQString(folderA / "Engine Library"));
        assert(!registry.strongStickIdentityAt(seabass::gui::pathToQString(folderA)).isEmpty()
               && "precondition: a listed folder has a strong id");
        assert(session->stage(std::make_unique<NoopChange>(QStringLiteral("test:folder"))));
        assert(media.openFolder(seabass::gui::pathToQString(folderB)).isEmpty());
        QSignalSpy finished(session, &LibraryEditSession::saveFinished);
        session->save();
        if (finished.count() == 0) {
            finished.wait(60000);
        }
        const QString error = finished.takeFirst().at(0).toMap().value("error").toString();
        if (!error.isEmpty()) {
            std::cerr << "folder A was refused after folder B was opened: " << error.toStdString() << "\n";
            assert(false);
        }
        registry.closeSession(idA);
        media.closeFolder(seabass::gui::pathToQString(folderB));
        registry.setMediaController(nullptr);
        std::cout << "case 17 (an opened folder no longer listed is unknown, not another stick) OK\n";
    }

    std::cout << "edit_session_registry_test: all cases passed\n";
    return 0;
}
