// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaProperty>
#include <QObject>
#include <QProcess>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QVariant>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../fixture_copy.hpp"
#include "../scratch_path.hpp"
#include "application/ports/removable_media_locator.hpp"
#include "gui/async_request.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "domain/track.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/interrupted_save.hpp"
#include "infrastructure/local/metadata_store.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/media/media_factory.hpp"
#include "infrastructure/media/stick_root_scan.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

// The storm's side of the world (tests/qml-storm, docs/testing.md): sticks
// that go in and come out, and a stick that answers slowly, not at all, or
// with an unreadable file. Everything it does, the app can meet for real.
//
// Sticks are copies of the anonymized fixture under the scratch tree. They
// reach the app the way a real one does: the media factory's locator lists
// them (only while SEABASS_IGNORE_REMOVABLE_MEDIA is set, so a real stick
// never gets in beside them) and a hotplug is announced through the
// monitor, after which MediaController re-detects on its own debounce. A
// pull renames the stick's directory away, so its mount point is gone and
// every open by path fails, as on a pulled stick; a re-plug renames it
// back.
//
// Reads are the real readers, run through a catalog cache whose passes the
// weather wraps: most pass, some are slowed, some held until released (and
// some of those cannot see their token, a hung device), some fail. Which
// pass does what is decided from the seed and the pass's own key and
// number, so a seed replays the same weather. Every track read gets its
// stick's tag ("[S2]") at the end of its title, so a page showing another
// stick's data can be caught by what it displays.
class StormFixture : public QObject
{
    Q_OBJECT

    enum class Weather { Pass, Delay, Hold, Fail };

    struct Gate
    {
        std::mutex mutex;
        std::condition_variable cv;
        // Holds by number; released numbers are erased.
        std::map<int, bool> held;
        int nextHold = 0;
        bool releaseEverything = false;
        std::atomic<int> waiting{0};
        std::atomic<int> passes{0};
    };

    struct Stick
    {
        std::filesystem::path root;     // where it is mounted
        std::filesystem::path outside;  // where it waits while pulled
        std::string label;
        std::string uuid;
        bool plugged = true;
    };

    // Shared with the stand-in locator and the stage function, both of
    // which run on other threads, and with workers let go of that may
    // outlive this fixture: never freed while one could run.
    struct World
    {
        std::mutex mutex;
        std::vector<Stick> sticks;
        std::uint64_t seed = 0;
        int passPct = 100, delayPct = 0, holdPct = 0, failPct = 0, maxDelayMs = 0, stuckPct = 0;
        std::map<std::string, int> passCount;
        std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    };

public:
    explicit StormFixture(QObject *parent = nullptr) : QObject(parent) {}

    ~StormFixture() override
    {
        m_stopWatch.store(true);
        if (m_watchdog.joinable()) {
            m_watchdog.join();
        }
        stopCapture();
        seabass::infrastructure::media::setStandInSticksForTesting(nullptr);
        seabass::gui::LibraryCatalogCache::setInstanceForTesting(nullptr);
        if (m_quitLeg) {
            // The end of the process decides what happens to a read still
            // held: nothing here may wait for it, or free what it uses. The
            // scratch tree is removed at exit, when there is one, and needs
            // the sticks' directory writable for that.
            if (!m_base.empty()) {
                mediaWritable(m_base / "media", true);
            }
            return;
        }
        releaseAll();
        QThreadPool::globalInstance()->waitForDone();
        seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
        removeSticks();
    }

    // The child of runQuitLeg(): at the end nothing is waited for or freed.
    Q_INVOKABLE void markQuitLeg() { m_quitLeg = true; }
    Q_INVOKABLE bool quitLeg() const { return m_quitLeg; }

    // Fresh sticks for a seed, all plugged in, and the weather calm.
    // Returns an error, or "".
    Q_INVOKABLE QString prepare(int seed, int stickCount, const QString &fixtureRoot)
    {
        namespace fs = std::filesystem;
        releaseAll();
        // What the last seed let go of has to be over before its sticks go.
        QThreadPool::globalInstance()->waitForDone();
        const bool nothingRuns = seabass::gui::AsyncWorkers::instance().waitForAll(std::chrono::seconds(30));
        removeSticks();
        // The last seed's cache holds three sticks' catalogs; a hunt of a
        // hundred seeds in one process kept every one of them. Freed only
        // when no read is running, since one let go of may still be in it.
        seabass::gui::LibraryCatalogCache::setInstanceForTesting(nullptr);
        if (nothingRuns) {
            for (seabass::gui::LibraryCatalogCache *cache : m_caches) {
                delete cache;
            }
            m_caches.clear();
        }
        auto world = std::make_shared<World>();
        world->seed = static_cast<std::uint64_t>(seed);
        const fs::path base = seabass::testing::scratchRoot()
            / ("storm_" + std::to_string(QCoreApplication::applicationPid()) + "_" + std::to_string(seed) + "_"
               + std::to_string(++m_prepares));
        std::error_code ec;
        fs::remove_all(base, ec);
        const fs::path from = seabass::pathFromUtf8(fixtureRoot.toStdString());
        for (int i = 0; i < stickCount; ++i) {
            Stick stick;
            stick.label = "STORM" + std::to_string(i);
            stick.uuid = "5702-" + std::to_string(seed) + "-" + std::to_string(i);
            stick.root = base / "media" / stick.label;
            stick.outside = base / "pulled" / stick.label;
            fs::create_directories(stick.root, ec);
            fs::create_directories(stick.outside.parent_path(), ec);
            seabass::testing::copyPioneerFixture(from / "rekordbox", stick.root / "PIONEER", ec);
            if (ec) {
                return QStringLiteral("could not copy the rekordbox fixture: ") + QString::fromStdString(ec.message());
            }
            fs::copy(from / "engine", stick.root / "Engine Library", fs::copy_options::recursive, ec);
            if (ec) {
                return QStringLiteral("could not copy the Engine fixture: ") + QString::fromStdString(ec.message());
            }
            world->sticks.push_back(stick);
        }
        mediaWritable(base / "media", false);
        m_base = base;
        m_world = world;
        {
            std::lock_guard<std::mutex> lock(m_worldsMutex);
            m_worlds.push_back(world);
        }

        seabass::infrastructure::media::setStandInSticksForTesting([world]() {
            std::vector<seabass::application::DetectedStick> found;
            std::lock_guard<std::mutex> lock(world->mutex);
            for (const Stick &stick : world->sticks) {
                if (!stick.plugged) {
                    continue;
                }
                seabass::application::DetectedStick detected;
                detected.mountPoint = seabass::pathToUtf8(stick.root);
                detected.label = stick.label;
                detected.mounted = true;
                // No device path: nothing may ever run a device command on
                // a stand-in (a mount, a repair, a format).
                detected.capacityBytes = 16ull * 1024 * 1024 * 1024;
                detected.identity.filesystemUuid = stick.uuid;
                detected.identity.label = stick.label;
                detected.identity.capacityBytes = detected.capacityBytes;
                seabass::infrastructure::media::scanMountedRoot(detected.mountPoint, detected);
                found.push_back(std::move(detected));
            }
            return found;
        });

        auto realStage = seabass::gui::LibraryCatalogCache::realStageForTesting();
        auto stage = [world, realStage](seabass::gui::LibraryCatalogCache::Detail detail, const std::string &format,
                                        const std::string &path, std::vector<seabass::domain::Track> &tracks,
                                        seabass::gui::LibraryCatalogCache::StageNotes &notes,
                                        seabass::application::ProgressReporter &progress,
                                        seabass::application::CancellationToken cancel) {
            weatherFor(*world, detail, format, path, cancel);
            realStage(detail, format, path, tracks, notes, progress, cancel);
            if (detail == seabass::gui::LibraryCatalogCache::Detail::Tracks) {
                const std::string tag = " [" + tagFor(*world, path) + "]";
                for (seabass::domain::Track &track : tracks) {
                    track.title += tag;
                }
            }
        };
        // Freed by a later prepare() once nothing reads, never before.
        auto *cache = new seabass::gui::LibraryCatalogCache(stage,
                                                            seabass::gui::LibraryCatalogCache::realMtimeForTesting());
        m_caches.push_back(cache);
        seabass::gui::LibraryCatalogCache::setInstanceForTesting(cache);
        return {};
    }

    // Backs up `count` of stick i's tracks into the metadata store with a
    // hot cue the stick does not have, as a backup taken before a player
    // lost a cue would: the Restore Metadata page then has something to
    // propose for that stick. Read through the storm's cache, so the
    // titles carry the stick's tag like every other read. Returns how many
    // were stored.
    Q_INVOKABLE int seedStore(int i, int count)
    {
        if (!valid(i)) {
            return 0;
        }
        const Stick stick = m_world->sticks[static_cast<size_t>(i)];
        try {
            std::vector<seabass::domain::Track> tracks = seabass::gui::LibraryCatalogCache::instance().tracksFor(
                "rekordbox", seabass::pathToUtf8(stick.root / "PIONEER"),
                seabass::gui::LibraryCatalogCache::Detail::Cues);
            const std::vector<seabass::domain::Track> whole = tracks;
            if (static_cast<int>(tracks.size()) > count) {
                tracks.resize(static_cast<size_t>(count));
            }
            for (seabass::domain::Track &track : tracks) {
                seabass::domain::CuePoint cue;
                cue.kind = seabass::domain::CuePoint::Kind::Hot;
                cue.hotCueNumber = 7;
                cue.positionMs = 99000.0;
                track.cues.push_back(cue);
            }
            seabass::infrastructure::local::MetadataSource source;
            source.stickRoot = stick.root;
            source.libraryId = stick.uuid;
            source.stickLabel = stick.label;
            source.wholeStick = whole;
            seabass::infrastructure::local::MetadataStore store;
            store.store(tracks, source, seabass::application::NullProgressReporter::instance(),
                        seabass::application::CancellationToken::none());
            return static_cast<int>(tracks.size());
        } catch (const std::exception &e) {
            std::fprintf(stderr, "storm: could not seed the store from %s: %s\n", stick.label.c_str(), e.what());
            return 0;
        }
    }

    Q_INVOKABLE int stickCount() const { return m_world ? static_cast<int>(m_world->sticks.size()) : 0; }
    Q_INVOKABLE QString stickRoot(int i) const
    {
        return valid(i) ? seabass::gui::pathToQString(m_world->sticks[static_cast<size_t>(i)].root) : QString();
    }
    Q_INVOKABLE QString stickLabel(int i) const
    {
        return valid(i) ? QString::fromStdString(m_world->sticks[static_cast<size_t>(i)].label) : QString();
    }
    Q_INVOKABLE bool plugged(int i) const
    {
        if (!valid(i)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_world->mutex);
        return m_world->sticks[static_cast<size_t>(i)].plugged;
    }
    // Which stick a path is on, or -1.
    Q_INVOKABLE int stickOf(const QString &path) const
    {
        if (!m_world || path.isEmpty()) {
            return -1;
        }
        const std::string p = seabass::pathToGenericUtf8(seabass::gui::pathFromQString(path));
        for (size_t i = 0; i < m_world->sticks.size(); ++i) {
            const std::string root = seabass::pathToGenericUtf8(m_world->sticks[i].root);
            if (p == root || (p.size() > root.size() && p.compare(0, root.size(), root) == 0 && p[root.size()] == '/')) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Pulls stick i: its mount point goes away, and the OS says so.
    Q_INVOKABLE bool pull(int i)
    {
        if (!valid(i)) {
            return false;
        }
        std::error_code ec;
        {
            std::lock_guard<std::mutex> lock(m_world->mutex);
            Stick &stick = m_world->sticks[static_cast<size_t>(i)];
            if (!stick.plugged) {
                return false;
            }
            mediaWritable(stick.root.parent_path(), true);
            std::filesystem::rename(stick.root, stick.outside, ec);
            mediaWritable(stick.root.parent_path(), false);
            if (ec) {
                std::fprintf(stderr, "storm: could not pull %s: %s\n", stick.label.c_str(), ec.message().c_str());
                return false;
            }
            stick.plugged = false;
        }
        seabass::infrastructure::media::announceMediaChangeForTesting();
        return true;
    }

    // Plugs stick i back in at the same mount point.
    Q_INVOKABLE bool insert(int i)
    {
        if (!valid(i)) {
            return false;
        }
        std::error_code ec;
        {
            std::lock_guard<std::mutex> lock(m_world->mutex);
            Stick &stick = m_world->sticks[static_cast<size_t>(i)];
            if (stick.plugged) {
                return false;
            }
            mediaWritable(stick.root.parent_path(), true);
            if (std::filesystem::exists(stick.root)) {
                // Something wrote to the mount point while the stick was out.
                std::string what;
                std::error_code walkEc;
                int n = 0;
                for (auto it = std::filesystem::recursive_directory_iterator(stick.root, walkEc);
                     !walkEc && it != std::filesystem::recursive_directory_iterator() && n < 12; it.increment(walkEc), ++n) {
                    what += " " + seabass::pathToUtf8(std::filesystem::relative(it->path(), stick.root));
                }
                std::fprintf(stderr, "storm: written to %s while it was out:%s\n", stick.label.c_str(), what.c_str());
                m_writtenWhileOut << QString::fromStdString(stick.label + ":" + what);
                std::filesystem::rename(stick.root,
                                        stick.outside.parent_path() / (stick.label + ".written-" + std::to_string(++m_prepares)), ec);
            }
            std::filesystem::rename(stick.outside, stick.root, ec);
            mediaWritable(stick.root.parent_path(), false);
            if (ec) {
                std::fprintf(stderr, "storm: could not insert %s: %s\n", stick.label.c_str(), ec.message().c_str());
                return false;
            }
            stick.plugged = true;
        }
        seabass::infrastructure::media::announceMediaChangeForTesting();
        return true;
    }

    // The watchdog: the walk beats on every tick of its clock, and a window
    // that has not beaten for `freezeMs` is frozen -- the GUI thread stuck
    // waiting for something, which no invariant checked from that thread
    // can see. The watchdog then says so with the steps that led there,
    // and lets every held read go, as a hung stick that finally answers
    // would; if that thaws the window, the walk fails the seed on its next
    // tick (takeFreeze()). A window still frozen `freezeMs` later ends the
    // process, with the seed on record, rather than the whole run's time.
    Q_INVOKABLE void beat(int seed, int step)
    {
        m_lastBeat.store(std::chrono::steady_clock::now().time_since_epoch().count());
        m_beatSeed.store(seed);
        m_beatStep.store(step);
        if (!m_watchdog.joinable()) {
            m_watchdog = std::thread([this]() { watch(); });
        }
    }
    // Between walks nothing beats, and nothing is frozen.
    Q_INVOKABLE void disarm() { m_lastBeat.store(0); }
    Q_INVOKABLE void noteStep(const QString &line)
    {
        std::lock_guard<std::mutex> lock(m_stepsMutex);
        m_steps << line;
        if (m_steps.size() > 60) {
            m_steps.removeFirst();
        }
    }
    Q_INVOKABLE void clearSteps()
    {
        std::lock_guard<std::mutex> lock(m_stepsMutex);
        m_steps.clear();
    }
    Q_INVOKABLE QString takeFreeze()
    {
        std::lock_guard<std::mutex> lock(m_stepsMutex);
        const QString freeze = m_freeze;
        m_freeze.clear();
        return freeze;
    }

    // What was written to a stick's mount point while it was out, since
    // the last call: a write aimed at a stick that is gone.
    Q_INVOKABLE QStringList takeWrittenWhileOut()
    {
        QStringList taken = m_writtenWhileOut;
        m_writtenWhileOut.clear();
        return taken;
    }

    // What a player does to a stick while it is out: its catalogs are
    // rewritten (here, only their modification times move on).
    Q_INVOKABLE void changeWhileOut(int i)
    {
        if (!valid(i)) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_world->mutex);
        const Stick &stick = m_world->sticks[static_cast<size_t>(i)];
        const std::filesystem::path at = stick.plugged ? stick.root : stick.outside;
        for (const auto &file : {at / "PIONEER" / "rekordbox" / "export.pdb", at / "Engine Library" / "Database2" / "m.db"}) {
            std::error_code ec;
            const auto modified = std::filesystem::last_write_time(file, ec);
            if (!ec) {
                std::filesystem::last_write_time(file, modified + std::chrono::minutes(1), ec);
            }
        }
    }

    // The weather from here on, in percent of passes (the rest pass).
    // stuckPct: of the held passes, how many cannot see their token.
    Q_INVOKABLE void setWeather(int delayPct, int holdPct, int failPct, int maxDelayMs, int stuckPct)
    {
        if (!m_world) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_world->gate->mutex);
            m_world->gate->releaseEverything = false;
        }
        std::lock_guard<std::mutex> lock(m_world->mutex);
        m_world->delayPct = delayPct;
        m_world->holdPct = holdPct;
        m_world->failPct = failPct;
        m_world->maxDelayMs = maxDelayMs;
        m_world->stuckPct = stuckPct;
    }

    Q_INVOKABLE int held() const { return m_world ? m_world->gate->waiting.load() : 0; }
    Q_INVOKABLE int passes() const { return m_world ? m_world->gate->passes.load() : 0; }

    // Lets the `n`th oldest held pass go on (0 is the oldest).
    Q_INVOKABLE void releaseOne(int n)
    {
        if (!m_world) {
            return;
        }
        auto gate = m_world->gate;
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (gate->held.empty()) {
                return;
            }
            auto it = gate->held.begin();
            std::advance(it, n % static_cast<int>(gate->held.size()));
            gate->held.erase(it);
        }
        gate->cv.notify_all();
    }

    // Every held pass goes on, and none is held until the weather says so again.
    Q_INVOKABLE void releaseAll()
    {
        std::lock_guard<std::mutex> worldsLock(m_worldsMutex);
        for (const auto &world : m_worlds) {
            auto gate = world->gate;
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                gate->held.clear();
                gate->releaseEverything = true;
            }
            gate->cv.notify_all();
            std::lock_guard<std::mutex> lock(world->mutex);
            world->holdPct = 0;
        }
    }

    // Reads and writes still running anywhere in the process.
    Q_INVOKABLE int liveWorkers() const { return seabass::gui::AsyncWorkers::instance().live(); }
    Q_INVOKABLE int activeWrites() const { return QThreadPool::globalInstance()->activeThreadCount(); }

    // Every "is working" property set on `root` and the objects under it
    // (QObject children, which is where a page's controllers live), as
    // "Type(objectName).property" strings. Stops at child pages: each
    // page is asked about on its own.
    Q_INVOKABLE QStringList busyNow(QObject *root) const
    {
        QStringList busy;
        collectBusy(root, busy, 0);
        return busy;
    }

    // The stick tags ("S1") shown by visible text under `root`, each once.
    Q_INVOKABLE QStringList shownTags(QQuickItem *root) const
    {
        QStringList tags;
        collectTags(root, tags);
        return tags;
    }

    // Objects under `root` (itself included) of a kind: a C++ class it
    // inherits ("QQuickComboBox"), a QML type's name ("BackBreadcrumb"),
    // or an objectName. visibleOnly: items that are shown and enabled.
    Q_INVOKABLE QVariantList findObjects(QObject *root, const QString &kind, bool visibleOnly) const
    {
        QVariantList found;
        collectKind(root, kind.toUtf8(), visibleOnly, found, 0);
        return found;
    }
    Q_INVOKABLE QObject *findObject(QObject *root, const QString &kind) const
    {
        const QVariantList found = findObjects(root, kind, false);
        return found.isEmpty() ? nullptr : found.first().value<QObject *>();
    }

    // Popups that are open under `root`, anywhere in its object tree.
    Q_INVOKABLE QVariantList openPopups(QObject *root) const
    {
        QVariantList popups;
        collectPopups(root, popups);
        return popups;
    }

    // Whether another Seabass holds stick `root`'s write lock right now:
    // the lock is an flock, so a second open file description in this
    // process conflicts just as another process would.
    Q_INVOKABLE bool lockHeld(const QString &root) const
    {
#if defined(_WIN32)
        Q_UNUSED(root);
        return false;
#else
        const std::string lock = seabass::infrastructure::backup::writeLockPathForBackupDir(
            seabass::infrastructure::backup::backupDirForStickRoot(root.toStdString()));
        const int fd = ::open(lock.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
        if (!held) {
            ::flock(fd, LOCK_UN);
        }
        ::close(fd);
        return held;
#endif
    }

    // The records of a save that never finished on stick `root`, as the
    // recovery path reads them. restorableOnly: only those it could undo
    // from; a record the save was still writing when the stick went is
    // not one, since the save changes nothing before its record is whole.
    Q_INVOKABLE QStringList interruptedSave(const QString &root, bool restorableOnly = false) const
    {
        const std::string backupDir = seabass::infrastructure::backup::backupDirForStickRoot(root.toStdString());
        seabass::infrastructure::backup::FilesystemBackupStore store(backupDir);
        QStringList ids;
        for (const std::string &id : seabass::infrastructure::backup::interruptedSaveRecords(backupDir)) {
            if (!restorableOnly || store.isRestorable(id)) {
                ids << QString::fromStdString(id);
            }
        }
        return ids;
    }

    // Every warning and error the process logged since the last call.
    Q_INVOKABLE void startCapture()
    {
        if (!s_capturing.exchange(true)) {
            s_previous = qInstallMessageHandler(&StormFixture::onMessage);
        }
    }
    Q_INVOKABLE void stopCapture()
    {
        if (s_capturing.exchange(false)) {
            qInstallMessageHandler(s_previous);
        }
    }
    Q_INVOKABLE QStringList takeWarnings()
    {
        std::lock_guard<std::mutex> lock(warnings().mutex);
        QStringList taken = warnings().lines;
        warnings().lines.clear();
        return taken;
    }

    Q_INVOKABLE void log(const QString &line) const
    {
        std::fprintf(stderr, "%s\n", qPrintable(line));
        std::fflush(stderr);
    }

    // Appends a line to the file SEABASS_STORM_FAILURES names, if any.
    Q_INVOKABLE void recordFailure(const QString &line) const
    {
        const QString file = qEnvironmentVariable("SEABASS_STORM_FAILURES");
        if (file.isEmpty()) {
            return;
        }
        QFile out(file);
        if (out.open(QIODevice::Append | QIODevice::Text)) {
            out.write(line.toUtf8() + "\n");
        }
    }

    Q_INVOKABLE int envInt(const QString &name, int fallback) const
    {
        bool ok = false;
        const int value = qEnvironmentVariableIntValue(name.toUtf8().constData(), &ok);
        return ok ? value : fallback;
    }
    Q_INVOKABLE QString env(const QString &name) const { return qEnvironmentVariable(name.toUtf8().constData()); }

    // The quit leg: this binary run again as the app (--storm-quit), a
    // seed's walk cut short by the window closing at `quitStep`, and the
    // process has to end on its own within `boundMs` and exit 0. Returns
    // "" or what went wrong, with the child's log.
    Q_INVOKABLE QString runQuitLeg(const QString &driver, int seed, int steps, int quitStep, int boundMs)
    {
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SEABASS_STORM_SEED"), QString::number(seed));
        env.insert(QStringLiteral("SEABASS_STORM_STEPS"), QString::number(steps));
        env.insert(QStringLiteral("SEABASS_STORM_QUIT_STEP"), QString::number(quitStep));
        child.setProcessEnvironment(env);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--storm-quit"), driver});
        if (!child.waitForStarted(10000)) {
            return QStringLiteral("the quit leg did not start: ") + child.errorString();
        }
        if (!child.waitForFinished(boundMs)) {
            child.kill();
            child.waitForFinished(5000);
            return QStringLiteral("the process did not end within %1 ms of the walk:\n").arg(boundMs)
                + QString::fromUtf8(child.readAll()).right(6000);
        }
        const QString out = QString::fromUtf8(child.readAll());
        if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
            return QStringLiteral("the process ended badly (status %1, code %2):\n")
                       .arg(child.exitStatus())
                       .arg(child.exitCode())
                + out.right(6000);
        }
        if (!out.contains(QStringLiteral("STORM QUIT LEG WALKED"))) {
            return QStringLiteral("the child never finished its walk:\n") + out.right(6000);
        }
        return {};
    }

private:
    // The directory the sticks are mounted in is not writable, as /media
    // and /Volumes are not: a write aimed at a pulled stick's mount point
    // fails there, where it would otherwise quietly make the directory
    // again (and the stick could not go back in). Opened only for the
    // moment a stick goes in or out.
    static void mediaWritable(const std::filesystem::path &media, bool writable)
    {
        std::error_code ec;
        std::filesystem::permissions(media,
                                     writable ? std::filesystem::perms::owner_all | std::filesystem::perms::group_read
                                                    | std::filesystem::perms::group_exec
                                              : std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec
                                                    | std::filesystem::perms::group_read | std::filesystem::perms::group_exec,
                                     std::filesystem::perm_options::replace, ec);
    }

    bool valid(int i) const { return m_world && i >= 0 && static_cast<size_t>(i) < m_world->sticks.size(); }

    void watch()
    {
        const auto freezeAfter = std::chrono::milliseconds(envMs("SEABASS_STORM_FREEZE_MS", 30000));
        bool reported = false;
        std::chrono::steady_clock::time_point reportedAt;
        while (!m_stopWatch.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const auto beat = m_lastBeat.load();
            if (beat == 0) {
                reported = false;
                continue;
            }
            const auto last = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(beat));
            const auto now = std::chrono::steady_clock::now();
            if (now - last < freezeAfter) {
                reported = false;
                continue;
            }
            if (!reported) {
                reported = true;
                reportedAt = now;
                QString steps;
                {
                    std::lock_guard<std::mutex> lock(m_stepsMutex);
                    steps = m_steps.join(QStringLiteral("\n    "));
                    m_freeze = QStringLiteral("the window froze for %1 s at step %2 (released every held read to see "
                                              "whether it thaws)")
                                   .arg(freezeAfter.count() / 1000)
                                   .arg(m_beatStep.load());
                }
                std::fprintf(stderr, "STORM FROZEN seed %d at step %d, the last steps:\n    %s\n", m_beatSeed.load(),
                             m_beatStep.load(), qPrintable(steps));
                std::fflush(stderr);
                dumpStacks();
                releaseAllFromAnyThread();
            } else if (now - reportedAt > freezeAfter) {
                std::fprintf(stderr, "STORM FAILED seed %d, step %d: the window is still frozen with every read "
                             "released; ending the process\n",
                             m_beatSeed.load(), m_beatStep.load());
                recordFailure(QStringLiteral("seed %1: the window froze at step %2 and did not thaw")
                                  .arg(m_beatSeed.load())
                                  .arg(m_beatStep.load()));
                std::fflush(nullptr);
                std::_Exit(3);
            }
        }
    }

    // Where every thread of this process is while the window is frozen,
    // through gdb when there is one (and ptrace lets it attach), into a
    // file beside SEABASS_STORM_FAILURES, or the scratch tree.
    void dumpStacks()
    {
#if !defined(_WIN32)
        const QString failures = qEnvironmentVariable("SEABASS_STORM_FAILURES");
        const std::string dir = failures.isEmpty() ? seabass::pathToUtf8(seabass::testing::scratchRoot())
                                                   : seabass::pathToUtf8(std::filesystem::path(failures.toStdString()).parent_path());
        const std::string file = dir + "/frozen-seed" + std::to_string(m_beatSeed.load()) + "-step"
            + std::to_string(m_beatStep.load()) + "-pid" + std::to_string(QCoreApplication::applicationPid()) + ".stacks";
        const std::string command = "gdb -p " + std::to_string(QCoreApplication::applicationPid())
            + " -batch -ex 'thread apply all bt 30' > '" + file + "' 2>&1";
        if (std::system(command.c_str()) == 0) {
            std::fprintf(stderr, "STORM FROZEN stacks: %s\n", file.c_str());
        }
#endif
    }

    static int envMs(const char *name, int fallback)
    {
        bool ok = false;
        const int value = qEnvironmentVariableIntValue(name, &ok);
        return ok ? value : fallback;
    }

    // releaseAll() for the watchdog, which cannot touch m_worlds while the
    // GUI thread might.
    void releaseAllFromAnyThread()
    {
        std::vector<std::shared_ptr<World>> worlds;
        {
            std::lock_guard<std::mutex> lock(m_worldsMutex);
            worlds = m_worlds;
        }
        for (const auto &world : worlds) {
            auto gate = world->gate;
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                gate->held.clear();
                gate->releaseEverything = true;
            }
            gate->cv.notify_all();
        }
    }

    static std::uint64_t fnv(const std::string &text)
    {
        std::uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : text) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    static std::string tagFor(World &world, const std::string &path)
    {
        const std::string p = seabass::pathToGenericUtf8(seabass::pathFromUtf8(path));
        for (size_t i = 0; i < world.sticks.size(); ++i) {
            const std::string root = seabass::pathToGenericUtf8(world.sticks[i].root);
            if (p.compare(0, root.size(), root) == 0) {
                return "S" + std::to_string(i);
            }
        }
        return "S?";
    }

    // Decided from the seed, the pass's key and how many passes that key
    // had before: the same seed meets the same weather.
    static void weatherFor(World &world, seabass::gui::LibraryCatalogCache::Detail detail, const std::string &format,
                           const std::string &path, seabass::application::CancellationToken cancel)
    {
        std::uint64_t roll = 0;
        int delayPct = 0, holdPct = 0, failPct = 0, maxDelayMs = 0, stuckPct = 0;
        {
            std::lock_guard<std::mutex> lock(world.mutex);
            const std::string key = format + "|" + tagFor(world, path) + "|" + std::to_string(static_cast<int>(detail));
            const int n = world.passCount[key]++;
            roll = fnv(std::to_string(world.seed) + "|" + key + "|" + std::to_string(n));
            delayPct = world.delayPct;
            holdPct = world.holdPct;
            failPct = world.failPct;
            maxDelayMs = world.maxDelayMs;
            stuckPct = world.stuckPct;
        }
        auto gate = world.gate;
        ++gate->passes;
        const int pick = static_cast<int>(roll % 100);
        const int second = static_cast<int>((roll / 100) % 100);
        if (pick < holdPct) {
            const bool honourCancel = second >= stuckPct;
            int number = 0;
            {
                std::lock_guard<std::mutex> lock(gate->mutex);
                if (gate->releaseEverything) {
                    return;
                }
                number = gate->nextHold++;
                gate->held[number] = true;
            }
            ++gate->waiting;
            {
                std::unique_lock<std::mutex> lock(gate->mutex);
                while (gate->held.count(number) != 0 && !gate->releaseEverything
                       && !(honourCancel && cancel.cancelled())) {
                    gate->cv.wait_for(lock, std::chrono::milliseconds(5));
                }
                gate->held.erase(number);
            }
            --gate->waiting;
            cancel.throwIfCancelled();
        } else if (pick < holdPct + failPct) {
            throw std::runtime_error("storm: a file on the stick could not be read");
        } else if (pick < holdPct + failPct + delayPct && maxDelayMs > 0) {
            // A slow stick, which still looks at its token between files.
            const auto until = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(static_cast<int>(second * maxDelayMs / 100));
            while (std::chrono::steady_clock::now() < until) {
                cancel.throwIfCancelled();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    static bool isBusyName(const char *name)
    {
        static const char *names[] = {"busy",     "writing",   "previewing", "writeBusy",  "wearBusy",
                                      "restoring", "mounting", "listing",    "listingBackups", "deleting",
                                      "cuesPending", "cloning", "backingUp", "analyzing"};
        for (const char *candidate : names) {
            if (qstrcmp(name, candidate) == 0) {
                return true;
            }
        }
        return false;
    }

    void collectBusy(QObject *object, QStringList &busy, int depth) const
    {
        if (object == nullptr || depth > 40) {
            return;
        }
        const QMetaObject *meta = object->metaObject();
        // Only the app's own C++ controllers: a QML item's "busy" (a
        // BusyIndicator's running, a page's own alias) is what they show.
        const QString className = QString::fromLatin1(meta->className());
        if (className.startsWith(QLatin1String("seabass::gui::"))) {
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                const QMetaProperty property = meta->property(i);
                if (property.typeId() == QMetaType::Bool && isBusyName(property.name())
                    && property.read(object).toBool()) {
                    busy << className.mid(14) + QLatin1Char('(') + object->objectName() + QLatin1String(").")
                            + QString::fromLatin1(property.name());
                }
            }
        }
        for (QObject *child : object->children()) {
            collectBusy(child, busy, depth + 1);
        }
    }

    static void collectTags(QQuickItem *item, QStringList &tags)
    {
        if (item == nullptr || !item->isVisible() || item->opacity() <= 0.0) {
            return;
        }
        const QVariant text = item->property("text");
        if (text.isValid() && text.canConvert<QString>()) {
            const QString shown = text.toString();
            int at = 0;
            while ((at = shown.indexOf(QLatin1String("[S"), at)) >= 0) {
                const int end = shown.indexOf(QLatin1Char(']'), at);
                if (end < 0) {
                    break;
                }
                const QString tag = shown.mid(at + 1, end - at - 1);
                if (!tags.contains(tag)) {
                    tags << tag;
                }
                at = end;
            }
        }
        for (QQuickItem *child : item->childItems()) {
            collectTags(child, tags);
        }
    }

    static void collectKind(QObject *object, const QByteArray &kind, bool visibleOnly, QVariantList &found, int depth)
    {
        if (object == nullptr || depth > 60) {
            return;
        }
        const QByteArray className = object->metaObject()->className();
        const bool matches = object->inherits(kind.constData()) || className == kind
            || className.startsWith(kind + "_QMLTYPE_") || className.startsWith(kind + "_QML_")
            || object->objectName() == QString::fromUtf8(kind);
        if (matches) {
            auto *item = qobject_cast<QQuickItem *>(object);
            if (!visibleOnly || (item != nullptr && item->isVisible() && item->isEnabled())) {
                found << QVariant::fromValue(object);
            }
        }
        for (QObject *child : object->children()) {
            collectKind(child, kind, visibleOnly, found, depth + 1);
        }
    }

    static void collectPopups(QObject *object, QVariantList &popups)
    {
        if (object == nullptr) {
            return;
        }
        if (object->inherits("QQuickPopup") && object->property("opened").toBool()) {
            popups << QVariant::fromValue(object);
        }
        for (QObject *child : object->children()) {
            collectPopups(child, popups);
        }
    }

    void removeSticks()
    {
        if (m_base.empty()) {
            return;
        }
        std::error_code ec;
        mediaWritable(m_base / "media", true);
        std::filesystem::remove_all(m_base, ec);
        m_base.clear();
    }

    struct Warnings
    {
        std::mutex mutex;
        QStringList lines;
    };
    static Warnings &warnings()
    {
        static Warnings *w = new Warnings();
        return *w;
    }
    static void onMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
            std::lock_guard<std::mutex> lock(warnings().mutex);
            warnings().lines << message;
        }
        if (s_previous) {
            s_previous(type, context, message);
        }
    }

    static inline std::atomic<bool> s_capturing{false};
    static inline QtMessageHandler s_previous = nullptr;

    std::shared_ptr<World> m_world;
    // Every world ever made: a worker let go of may still be in one.
    std::vector<std::shared_ptr<World>> m_worlds;
    std::filesystem::path m_base;
    int m_prepares = 0;
    bool m_quitLeg = false;
    QStringList m_writtenWhileOut;
    std::vector<seabass::gui::LibraryCatalogCache *> m_caches;
    std::mutex m_worldsMutex;
    std::thread m_watchdog;
    std::atomic<bool> m_stopWatch{false};
    std::atomic<std::chrono::steady_clock::duration::rep> m_lastBeat{0};
    std::atomic<int> m_beatSeed{0};
    std::atomic<int> m_beatStep{0};
    std::mutex m_stepsMutex;
    QStringList m_steps;
    QString m_freeze;
};
