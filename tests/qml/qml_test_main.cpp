// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QQmlContext>
#include <QQmlEngine>
#include <QDir>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/backup/interrupted_save.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "application/use_cases/scan_library.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/work_counters.hpp"
#include "infrastructure/local/metadata_store.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/app_color_scheme.hpp"
#include "gui/controls_style.hpp"
#include "gui/interface_font.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "../scratch_path.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/sync_controller.hpp"
#include "gui/metadata_restore_controller.hpp"
#include "gui/stick_catalogs.hpp"
#include "application/use_cases/collapse_catalog_rows.hpp"
#include "domain/metadata_restore.hpp"
#include "gui/format_usb_controller.hpp"
#include "gui/library_catalog_cache.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "gui/library_consistency_controller.hpp"
#include "gui/scan_controller.hpp"
#include <QColor>
#include <QFile>
#include <QImage>
#include <QSettings>
#include <QThreadPool>
#include <QString>
#include <QStringList>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtQuickTest/quicktest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <thread>
#include <string>
#include <system_error>

#include <sqlite3.h>

// Runs every tst_*.qml file found under the directory passed via -input
// (see the add_test() call in CMakeLists.txt) against a real QQmlEngine --
// TestCase, SignalSpy, mouseClick() etc. all work exactly as they would
// driving the real app, just against QML components in isolation rather
// than the full running application.
//
// `screenshotDir` is exposed to the tests from SEABASS_SCREENSHOT_DIR:
// when set, tests that render a whole page also save it as a PNG there
// (grabImage(page).save(...)), so a layout can be looked at for real
// rather than only asserted about. Empty (the default, and under ctest)
// means no files are written.
// Fills a Sync Cue Points page's own controller with a small fixed
// analysis, so tst_SyncPage.qml can measure the page and screenshot it
// without a stick: two decisions (one side of the first carries a 0:00
// memory cue) and five tracks ready to sync, one of which already has one
// of the cues it is about to receive. The page builds its controller
// itself, so the test finds it by objectName and hands it here.
// Copies of the committed Engine library for the cover-art tests, and the
// one place they are cleaned up again.
//
// Copies, not the fixture itself: pointing a page at a library opens an
// edit session on the stick it sits on, which writes Seabass/backups and a
// .write.lock beside it. Run against tests/fixtures/anonymized_library
// that lands in the source tree -- it did, once, before this existed.
//
// eraseImages() additionally turns every AlbumArt row into a proper hash
// and empties Artwork/, leaving a library whose only cover-art fault is
// deleted image files. The fixture as committed carries the other fault as
// well (1467 imported paths beside 95 of these), so it cannot show what
// Library Health says about this one on its own -- and that is the branch
// which used to offer a rekordbox re-import for pictures no import ever
// wrote. A copy of the real library rather than a hand-made database,
// because the page runs the whole Engine scan, which wants an Engine
// library and reports an error for anything less: an error would leave the
// artwork counts at zero, which is the state the test would then be
// asserting.
class ArtworkFixture : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    ~ArtworkFixture() override
    {
        std::error_code ec;
        for (const QString &root : m_roots) {
            std::filesystem::remove_all(seabass::gui::pathFromQString(root), ec);
        }
    }

    // The "Engine Library" path to point a page at, or empty on failure
    // (which the test then fails on, rather than skipping).
    Q_INVOKABLE QString libraryCopy(const QString &fromLibrary) { return copy(fromLibrary, false); }
    Q_INVOKABLE QString libraryWithMissingImages(const QString &fromLibrary) { return copy(fromLibrary, true); }

    // The same, and then `emptyRows` of the art rows emptied outright, so
    // the library carries a missing-image fault and a hash-less-row fault
    // at once and neither imported paths nor anything repairable. That is
    // the shape where a closing sentence picked by whichever count was
    // zero attached itself to the wrong fault.
    Q_INVOKABLE QString libraryWithMissingImagesAndEmptyRows(const QString &fromLibrary, int emptyRows)
    {
        const QString library = copy(fromLibrary, true);
        if (library.isEmpty()) {
            return {};
        }
        sqlite3 *db = nullptr;
        const std::string file = seabass::pathToUtf8(seabass::gui::pathFromQString(library) / "Database2" / "m.db");
        if (sqlite3_open(file.c_str(), &db) != SQLITE_OK) {
            sqlite3_close(db);
            return {};
        }
        // Never id 1, which is Engine's own "no cover" row: emptying that
        // one would be indistinguishable from a library with no art.
        const std::string sql = "UPDATE AlbumArt SET hash = NULL WHERE id IN (SELECT id FROM AlbumArt WHERE id > 1 "
            "ORDER BY id LIMIT " + std::to_string(emptyRows) + ");";
        const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(db);
        return ok ? library : QString();
    }

    // A copy whose imported art paths have their images beside it, under
    // the stick's own PIONEER/Artwork -- the one shape where the page has
    // something to offer, and so the only one that can show that pressing
    // its button stages rather than writes.
    Q_INVOKABLE QString libraryWithRepairableArt(const QString &fromLibrary)
    {
        namespace fs = std::filesystem;
        const QString library = copy(fromLibrary, false);
        if (library.isEmpty()) {
            return {};
        }
        const fs::path stick = seabass::gui::pathFromQString(library).parent_path();
        sqlite3 *db = nullptr;
        if (sqlite3_open(seabass::pathToUtf8(seabass::gui::pathFromQString(library) / "Database2" / "m.db").c_str(), &db)
            != SQLITE_OK) {
            sqlite3_close(db);
            return {};
        }
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT hash FROM AlbumArt;", -1, &stmt, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return {};
        }
        int written = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const void *blob = sqlite3_column_blob(stmt, 0);
            const int size = sqlite3_column_bytes(stmt, 0);
            if (blob == nullptr || size <= 0) {
                continue;
            }
            const std::string reference(static_cast<const char *>(blob), static_cast<size_t>(size));
            const auto at = reference.find("PIONEER/Artwork");
            if (at == std::string::npos) {
                continue;
            }
            const fs::path image = stick / reference.substr(at);
            std::error_code ec;
            fs::create_directories(image.parent_path(), ec);
            std::ofstream out(image, std::ios::binary | std::ios::trunc);
            // A real JPEG header: the audit reads the first bytes and
            // refuses to name a repair after a file that is not an image.
            // The name follows it, so each image hashes to its own row.
            out << "\xFF\xD8\xFF" << seabass::pathToUtf8(image.filename());
            written++;
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return written > 0 ? library : QString();
    }

private:
    QString copy(const QString &fromLibrary, bool eraseImages)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = seabass::testing::scratchRoot()
            / ("seabass_qml_artwork_" + std::to_string(QCoreApplication::applicationPid()) + "_"
               + std::to_string(m_roots.size()));
        fs::remove_all(root, ec);
        const fs::path library = root / "Engine Library";
        fs::create_directories(root, ec);
        fs::copy(seabass::gui::pathFromQString(fromLibrary), library, fs::copy_options::recursive, ec);
        if (ec) {
            return {};
        }
        m_roots.append(seabass::gui::pathToQString(root));
        if (!eraseImages) {
            return seabass::gui::pathToQString(library);
        }

        // Every image the copy might have had, gone.
        fs::remove_all(library / "Artwork", ec);
        fs::create_directories(library / "Artwork", ec);
        sqlite3 *db = nullptr;
        if (sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &db) != SQLITE_OK) {
            sqlite3_close(db);
            return {};
        }
        // Proper 20-byte hashes, the width Engine's own rows use: the rows
        // are right, and only their files are missing.
        const bool ok =
            sqlite3_exec(db, "UPDATE AlbumArt SET hash = randomblob(20);", nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(db);
        return ok ? seabass::gui::pathToQString(library) : QString();
    }

    QStringList m_roots;
};

// A whole stick built from the committed real-scale library: PIONEER and
// "Engine Library" side by side, as a page is handed them. A copy for the
// reason ArtworkFixture gives (a page opens an edit session on the stick,
// which writes beside it), and a fresh one per call on purpose: the
// catalog cache is keyed on the path, so a scan of a new copy really
// reads the library rather than answering from memory, and takes long
// enough to be seen running.
//
// Plus the two questions the stray-cue tests ask of the rekordbox side:
// which track ids it holds, and how many times export.pdb has been parsed
// end to end so far in this process (see WorkCounters).
class StickFixture : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    ~StickFixture() override
    {
        std::error_code ec;
        for (const QString &root : m_roots) {
            std::filesystem::remove_all(seabass::pathFromUtf8(root.toStdString()), ec);
        }
    }

    // The stick's root, or empty on failure (which the test then fails
    // on, rather than skipping).
    Q_INVOKABLE QString stickCopy(const QString &fixtureRoot)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = seabass::testing::scratchRoot()
            / ("seabass_qml_stick_" + std::to_string(QCoreApplication::applicationPid()) + "_"
               + std::to_string(m_roots.size()));
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        const fs::path from = seabass::pathFromUtf8(fixtureRoot.toStdString());
        fs::copy(from / "rekordbox", root / "PIONEER", fs::copy_options::recursive, ec);
        if (ec) {
            return {};
        }
        fs::copy(from / "engine", root / "Engine Library", fs::copy_options::recursive, ec);
        if (ec) {
            return {};
        }
        m_roots.append(QString::fromStdString(seabass::pathToUtf8(root)));
        return m_roots.last();
    }

    Q_INVOKABLE QStringList rekordboxTrackIds(const QString &pioneerRoot, int count)
    {
        QStringList ids;
        try {
            seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneerRoot.toStdString());
            for (const auto &track : reader.readAll()) {
                if (ids.size() >= count) {
                    break;
                }
                ids << QString::fromStdString(track.sourceId);
            }
        } catch (const std::exception &) {
            ids.clear();
        }
        return ids;
    }

    // Moves a file's modification time a minute on, which is what any
    // rewrite of it does, without changing a byte.
    Q_INVOKABLE bool touch(const QString &file)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path path = seabass::pathFromUtf8(file.toStdString());
        const auto modified = fs::last_write_time(path, ec);
        if (ec) {
            return false;
        }
        fs::last_write_time(path, modified + std::chrono::minutes(1), ec);
        return !ec;
    }

    Q_INVOKABLE double trackDatabaseParses() const
    {
        return static_cast<double>(seabass::infrastructure::WorkCounters::instance().snapshot().trackDatabaseParses);
    }

private:
    QStringList m_roots;
};

// Browse's two phases without a stick. While held, every ScanController
// scans through a cache of its own: a rekordbox catalog of made-up tracks
// whose cue pass waits for releaseCues() (or a cancel), and an Engine
// catalog of the same songs whose art is named but missing, beside a
// rekordbox catalog whose art for them is a real image. The caches live
// until the fixture does, because a cancelled scan can still be finishing
// in one after its test is over.
class BrowseFixture : public QObject
{
    Q_OBJECT

    struct Gate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool released = false;
        std::atomic<int> cuePasses{0};
        std::atomic<int> waiting{0};
    };

    std::shared_ptr<Gate> m_gate;
    std::vector<std::unique_ptr<seabass::gui::LibraryCatalogCache>> m_caches;
    QTemporaryDir m_artDir;

public:
    using QObject::QObject;
    ~BrowseFixture() override
    {
        restore();
        QThreadPool::globalInstance()->waitForDone();
    }

    // A real cover on disk, for the fallback to find.
    Q_INVOKABLE QString presentArtwork()
    {
        const QString file = m_artDir.filePath(QStringLiteral("cover.png"));
        if (!QFile::exists(file)) {
            QImage image(8, 8, QImage::Format_RGB32);
            image.fill(QColor(200, 40, 40));
            image.save(file);
        }
        return file;
    }

    Q_INVOKABLE QString missingArtwork() const { return m_artDir.filePath(QStringLiteral("not-there.jpg")); }

    Q_INVOKABLE void holdCues(int trackCount)
    {
        restore();
        auto gate = std::make_shared<Gate>();
        const std::string present = presentArtwork().toStdString();
        const std::string missing = missingArtwork().toStdString();
        auto stage = [gate, trackCount, present, missing](seabass::gui::LibraryCatalogCache::Detail detail,
                                                          const std::string &format, const std::string &,
                                                          std::vector<seabass::domain::Track> &tracks,
                                                          seabass::gui::LibraryCatalogCache::StageNotes &,
                                                          seabass::application::ProgressReporter &,
                                                          seabass::application::CancellationToken cancel) {
            using Detail = seabass::gui::LibraryCatalogCache::Detail;
            if (detail == Detail::Tracks) {
                tracks.clear();
                for (int i = 0; i < trackCount; ++i) {
                    seabass::domain::Track track;
                    track.format = format;
                    track.sourceId = std::to_string(i + 1);
                    track.title = "Held Track " + std::to_string(1000 + i);
                    track.artist = "Held Artist";
                    track.durationSeconds = 200 + i;
                    track.playlists.push_back({"Held Playlist", i});
                    // Engine names art that is not on the stick; the
                    // rekordbox copy of the same song has a real one.
                    track.artworkPath = format == "engine" ? missing : present;
                    if (format == "engine") {
                        // Engine keeps its cues in the catalog.
                        seabass::domain::CuePoint cue;
                        cue.kind = seabass::domain::CuePoint::Kind::Hot;
                        cue.hotCueNumber = 1;
                        track.cues.push_back(cue);
                    }
                    tracks.push_back(std::move(track));
                }
                return;
            }
            if (detail == Detail::Cues && format == "rekordbox") {
                ++gate->cuePasses;
                ++gate->waiting;
                {
                    std::unique_lock<std::mutex> lock(gate->mutex);
                    while (!gate->released && !cancel.cancelled()) {
                        gate->cv.wait_for(lock, std::chrono::milliseconds(5));
                    }
                }
                --gate->waiting;
                cancel.throwIfCancelled();
                // Track i gets i % 5 hot cues: sorting by cues reorders.
                for (size_t i = 0; i < tracks.size(); ++i) {
                    for (size_t n = 0; n < i % 5; ++n) {
                        seabass::domain::CuePoint cue;
                        cue.kind = seabass::domain::CuePoint::Kind::Hot;
                        cue.hotCueNumber = static_cast<int>(n) + 1;
                        cue.positionMs = 1000.0 * static_cast<double>(n);
                        tracks[i].cues.push_back(cue);
                    }
                }
            }
        };
        auto mtime = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        m_caches.push_back(std::make_unique<seabass::gui::LibraryCatalogCache>(stage, mtime));
        m_gate = gate;
        seabass::gui::ScanController::setCatalogCacheForTesting(m_caches.back().get());
    }

    Q_INVOKABLE void releaseCues()
    {
        if (!m_gate) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_gate->mutex);
            m_gate->released = true;
        }
        m_gate->cv.notify_all();
    }

    // How many cue passes the held cache has started, and whether one is
    // waiting at the gate right now.
    Q_INVOKABLE int cuePasses() const { return m_gate ? m_gate->cuePasses.load() : 0; }
    Q_INVOKABLE bool cuePassWaiting() const { return m_gate && m_gate->waiting.load() > 0; }

    // Every scan task has returned, and so has everything it reported
    // (delivery still needs the event loop to run).
    Q_INVOKABLE bool waitForScans() { return QThreadPool::globalInstance()->waitForDone(10000); }

    Q_INVOKABLE void restore()
    {
        releaseCues();
        m_gate.reset();
        seabass::gui::ScanController::setCatalogCacheForTesting(nullptr);
    }
};

// Test seams on controllers whose real work touches hardware: a
// filesystem repair unmounts and checks a drive, a format rewrites one.
// Each Q_INVOKABLE puts a stand-in in place of that work, and each has a
// matching restore; the stand-ins never reach a device.
class ControllerFixture : public QObject
{
    Q_OBJECT
    int m_stickCounter = 0;

public:
    using QObject::QObject;
    ~ControllerFixture() override
    {
        seabass::gui::LibraryConsistencyController::setFilesystemRepairForTesting({});
        seabass::gui::FormatUsbController::setFormatTaskForTesting({});
    }

    // The next filesystem repair throws `message` from its worker thread.
    Q_INVOKABLE void makeFilesystemRepairThrow(const QString &message)
    {
        const std::string text = message.toStdString();
        seabass::gui::LibraryConsistencyController::setFilesystemRepairForTesting(
            [text](const std::string &) -> seabass::infrastructure::media::FilesystemRepairResult {
                throw std::runtime_error(text);
            });
    }
    Q_INVOKABLE void restoreFilesystemRepair()
    {
        seabass::gui::LibraryConsistencyController::setFilesystemRepairForTesting({});
    }

    // What an edit session knows about its stick's catalogs after a page
    // named one of them: {rekordbox, engine}. A fresh stick is made under
    // the scratch root with export.pdb and/or Engine's m.db present --
    // catalogPathFor() answers by presence only. See
    // tst_EditSessionCatalogs.qml for why this matters.
    Q_INVOKABLE QVariantMap sessionCatalogsAfter(bool hasRekordbox, bool hasEngine, const QString &openedAs,
                                                 const QString &engineGiven = QString())
    {
        namespace fs = std::filesystem;
        const fs::path stick = seabass::testing::scratchRoot()
            / ("seabass_session_catalogs_" + std::to_string(++m_stickCounter));
        std::error_code ec;
        fs::remove_all(stick, ec);
        auto touch = [](const fs::path &file) {
            fs::create_directories(file.parent_path());
            std::ofstream(file) << "x";
        };
        if (hasRekordbox) {
            touch(stick / "PIONEER" / "rekordbox" / "export.pdb");
        }
        if (hasEngine) {
            touch(stick / "Engine Library" / "Database2" / "m.db");
        }
        const QString root = seabass::gui::pathToQString(stick);
        seabass::gui::LibraryEditSession session(seabass::gui::EditSessionRegistry::instance(),
                                                 QStringLiteral("session-catalogs-%1").arg(m_stickCounter),
                                                 QStringLiteral("TEST"), root);
        if (openedAs == QLatin1String("engine")) {
            session.setLibraryPaths(QString(), root + QStringLiteral("/Engine Library"));
        } else {
            session.setLibraryPaths(root + QStringLiteral("/PIONEER"), engineGiven);
        }
        QVariantMap result;
        result[QStringLiteral("root")] = root;
        result[QStringLiteral("rekordbox")] = session.rekordboxPath();
        result[QStringLiteral("engine")] = session.enginePath();
        return result;
    }

    // A stick a save was pulled from (#48): a backup record the save
    // finished making, and the note naming it that the save leaves before
    // writing. `note` is "complete" (the note names that record), "missing"
    // (it names one the save never finished) or "none". Returns what a
    // session opened on the stick afterwards offers: {canUndo, interruptedSave}.
    Q_INVOKABLE QVariantMap sessionAfterInterruptedSave(const QString &note)
    {
        namespace fs = std::filesystem;
        namespace backup = seabass::infrastructure::backup;
        const fs::path stick = seabass::testing::scratchRoot()
            / ("seabass_interrupted_save_" + std::to_string(++m_stickCounter));
        std::error_code ec;
        fs::remove_all(stick, ec);
        fs::create_directories(stick / "PIONEER" / "rekordbox");
        std::ofstream(stick / "PIONEER" / "rekordbox" / "export.pdb") << "pdb";
        std::ofstream(stick / "PIONEER" / "rekordbox" / "exportLibrary.db") << "onelibrary";
        const std::string backupDir = backup::backupDirForStickRoot(seabass::pathToUtf8(stick));
        backup::FilesystemBackupStore store(backupDir);
        const auto record =
            store.backup({seabass::pathToUtf8(stick / "PIONEER" / "rekordbox" / "exportLibrary.db")}, "sync");
        if (note == QLatin1String("complete")) {
            backup::noteSaveInProgress(backupDir, {record.id});
        } else if (note == QLatin1String("missing")) {
            backup::noteSaveInProgress(backupDir, {"20260924T195039-sync"});
        }
        seabass::gui::LibraryEditSession session(seabass::gui::EditSessionRegistry::instance(),
                                                 QStringLiteral("interrupted-save-%1").arg(m_stickCounter),
                                                 QStringLiteral("PULLED"), seabass::gui::pathToQString(stick));
        QVariantMap result;
        result[QStringLiteral("canUndo")] = session.canUndo();
        result[QStringLiteral("interruptedSave")] = session.interruptedSave();
        return result;
    }

    // A backup folder that takes a while to list: `count` files named
    // *.zip that are not archives. Each is opened and refused, which is
    // the same walk a folder of real backups gets, only cheaper to make;
    // 20000 of them list in about half a second here. Recreated on every
    // call, so a count from an earlier run never lingers.
    Q_INVOKABLE QString slowBackupFolder(int count)
    {
        namespace fs = std::filesystem;
        const fs::path folder = seabass::testing::scratchRoot() / "seabass_slow_backup_folder";
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        if (ec) {
            return {};
        }
        for (int i = 0; i < count; ++i) {
            std::ofstream(folder / ("backup-" + std::to_string(i) + ".zip")) << "x";
        }
        return seabass::gui::pathToQString(folder);
    }
    Q_INVOKABLE void removeSlowBackupFolder()
    {
        std::error_code ec;
        std::filesystem::remove_all(seabass::testing::scratchRoot() / "seabass_slow_backup_folder", ec);
    }

    // Scans a stick copy with a real LibraryConsistencyController and stops
    // it once per entry of `offsetsMs`: that long after the `format` leg
    // has started (the Engine leg's catalog read, then its audits), either
    // with cancelScan() or by destroying the controller, which is what
    // leaving the page does. Timed in C++ because QML's destroy() is
    // deferred to the event loop, and what Back costs is the `delete`.
    //
    // One map per stop: {offsetMs, stopMs, wasRunning, formatAtStop,
    // tasksAfter, outlivedMs}. stopMs is how long the stop took to be over (cancel:
    // until busy went false; destroy: the delete itself). tasksAfter is how
    // many scan tasks were still running once it was: a destroy must leave
    // none. wasRunning says the scan had not finished on its own before the
    // stop, without which the sample measures nothing. outlivedMs is how
    // long a task went on once the controller was gone.
    Q_INVOKABLE QVariantList stopScanAt(const QString &stickRoot, const QString &format, const QVariantList &offsetsMs,
                                        bool destroy)
    {
        QVariantList samples;
        for (const QVariant &offset : offsetsMs) {
            // Every sample reads the stick, not the catalog cache the one
            // before it filled: a cached leg is over before a stop lands.
            seabass::gui::LibraryCatalogCache::instance().invalidateEveryCatalogOn(stickRoot.toStdString());
            auto controller = std::make_unique<seabass::gui::LibraryConsistencyController>();
            controller->scan(stickRoot + QStringLiteral("/PIONEER"), stickRoot + QStringLiteral("/Engine Library"));
            QElapsedTimer clock;
            clock.start();
            // "engine:audits" is the Engine leg once its catalog has been
            // read, which is where the audits run: the reader has ticked
            // its last track.
            const bool afterRead = format.endsWith(QStringLiteral(":audits"));
            const QString leg = afterRead ? format.section(QLatin1Char(':'), 0, 0) : format;
            const auto reached = [&]() {
                return controller->scanningFormat() == leg
                    && (!afterRead || (controller->scanTotal() > 0 && controller->scanCurrent() >= controller->scanTotal()));
            };
            while (controller->busy() && !reached() && clock.elapsed() < 120000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            }
            clock.restart();
            while (controller->busy() && clock.elapsed() < offset.toInt()) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
            QVariantMap sample;
            sample[QStringLiteral("offsetMs")] = offset.toInt();
            sample[QStringLiteral("wasRunning")] = controller->busy();
            sample[QStringLiteral("formatAtStop")] = controller->scanningFormat();
            clock.restart();
            if (destroy) {
                controller.reset();
            } else {
                controller->cancelScan();
                while (controller->busy() && clock.elapsed() < 120000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
                }
            }
            sample[QStringLiteral("stopMs")] = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            sample[QStringLiteral("tasksAfter")] = seabass::gui::LibraryConsistencyController::runningScanTasksForTesting();
            controller.reset();
            // Whatever a stop left running must not be counted against the
            // next sample; how long it went on is outlivedMs.
            clock.restart();
            while (seabass::gui::LibraryConsistencyController::runningScanTasksForTesting() > 0
                   && clock.elapsed() < 120000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            }
            sample[QStringLiteral("outlivedMs")] = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            samples << sample;
        }
        return samples;
    }

    // How long opening this stick's OneLibrary takes to its first row:
    // SQLCipher derives the key there, inside one library call that no
    // token can interrupt. A stop landing just before it waits it out, so
    // the lifetime tests judge a stop against this, measured at the time.
    Q_INVOKABLE double oneLibraryOpenMs(const QString &stickRoot)
    {
        namespace onelibrary = seabass::infrastructure::onelibrary;
        const std::string pioneer = (stickRoot + QStringLiteral("/PIONEER")).toStdString();
        QElapsedTimer clock;
        clock.start();
        try {
            onelibrary::SqlCipherLibrary lib;
            onelibrary::SqlCipherDb db(lib, onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), /*readOnly=*/true);
            db.exec("PRAGMA key = '" + onelibrary::deriveOneLibraryKey() + "';");
            onelibrary::SqlCipherStatement first(db, "SELECT count(*) FROM playlist");
            first.step();
        } catch (const std::exception &) {
            return -1.0;
        }
        return static_cast<double>(clock.nsecsElapsed()) / 1e6;
    }

    // How long the whole scan of a stick copy takes, uninterrupted: what a
    // destructor that waited without a stop landing would cost Back.
    Q_INVOKABLE double fullScanMs(const QString &stickRoot)
    {
        seabass::gui::LibraryConsistencyController controller;
        QElapsedTimer clock;
        clock.start();
        controller.scan(stickRoot + QStringLiteral("/PIONEER"), stickRoot + QStringLiteral("/Engine Library"));
        while (controller.busy() && clock.elapsed() < 180000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        return static_cast<double>(clock.nsecsElapsed()) / 1e6;
    }

    // Starts a filesystem repair and destroys the controller while it
    // runs. The repair unmounts and remounts the stick and cannot be
    // stopped, so the destructor must wait for it. The stand-in takes half
    // a second and marks when it is done. Empty when the destructor
    // waited, otherwise what went wrong.
    Q_INVOKABLE QString leaveThePageMidRepair()
    {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        seabass::gui::LibraryConsistencyController::setFilesystemRepairForTesting(
            [finished](const std::string &) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                finished->store(true);
                return seabass::infrastructure::media::FilesystemRepairResult{};
            });
        QString verdict;
        {
            auto controller = std::make_unique<seabass::gui::LibraryConsistencyController>();
            // A stick that is not there: the scan ends at once with an
            // error, and the repair it leaves possible is the stand-in.
            controller->scan(QStringLiteral("/nonexistent/seabass-repair-test/PIONEER"), QString());
            QElapsedTimer clock;
            clock.start();
            while (controller->busy() && clock.elapsed() < 30000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
            controller->repairStickFilesystem();
            if (!controller->repairingFilesystem()) {
                verdict = QStringLiteral("the repair never started");
            }
        }
        if (verdict.isEmpty() && !finished->load()) {
            verdict = QStringLiteral("the controller was destroyed while its filesystem repair was still running");
        }
        seabass::gui::LibraryConsistencyController::setFilesystemRepairForTesting({});
        return verdict;
    }

    // Starts a format and leaves the page while it runs, which destroys
    // the controller mid-format. The controller holds the library's edit
    // lock as a member, so its destructor must wait for the format:
    // returning early releases the lock while the partition is still
    // being rewritten. The stand-in format takes half a second and marks
    // when it is done.
    //
    // Returns an empty string when the destructor waited, otherwise what
    // went wrong. Done in C++ because QML's destroy() is deferred to the
    // event loop, and what matters is the order inside `delete`.
    Q_INVOKABLE QString leaveThePageMidFormat()
    {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        seabass::gui::FormatUsbController::setFormatTaskForTesting([finished]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            finished->store(true);
            return seabass::gui::FormatUsbTaskResult{};
        });
        QString verdict;
        {
            auto controller = std::make_unique<seabass::gui::FormatUsbController>();
            // A path that names no device: the stand-in is what runs, and
            // should it ever not be, the real formatter finds nothing here.
            const QString noDisk = QStringLiteral("/dev/seabass-test-not-a-disk");
            controller->chooseDrive(noDisk);
            controller->format(noDisk, QStringLiteral("exfat"), QStringLiteral("TEST"));
            if (!controller->busy()) {
                verdict = QStringLiteral("the format never started: ") + controller->errorMessage();
            }
        }
        if (verdict.isEmpty() && !finished->load()) {
            verdict = QStringLiteral("the controller was destroyed while its format was still running");
        }
        seabass::gui::FormatUsbController::setFormatTaskForTesting({});
        return verdict;
    }
};

class SyncPageFixture : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    Q_INVOKABLE bool fill(QObject *controller)
    {
        auto *sync = qobject_cast<seabass::gui::SyncController *>(controller);
        if (sync == nullptr) {
            return false;
        }
        using namespace seabass::domain;
        const auto hot = [](int pad, double seconds, const char *color) {
            CuePoint cue;
            cue.kind = CuePoint::Kind::Hot;
            cue.hotCueNumber = pad;
            cue.positionMs = seconds * 1000.0;
            cue.color = color;
            return cue;
        };
        const auto memory = [](double seconds) {
            CuePoint cue;
            cue.kind = CuePoint::Kind::Memory;
            cue.positionMs = seconds * 1000.0;
            return cue;
        };
        const auto track = [](const char *format, const char *id, const char *title, const char *artist,
                              const char *filename, double seconds, double bpm, const char *key,
                              std::vector<CuePoint> cues) {
            Track t;
            t.format = format;
            t.sourceId = id;
            t.title = title;
            t.artist = artist;
            t.filename = filename;
            // Never read: it only makes Play look the way it does for a
            // real track, which is what the screenshots are for.
            t.filePath = std::string("/nonexistent/TESTSTICK/Contents/") + filename;
            t.durationSeconds = seconds;
            t.bpm = bpm;
            t.key = key;
            t.cues = std::move(cues);
            return t;
        };
        const auto readyPlan = [](Track source, Track target) {
            SyncPlan plan;
            plan.kind = SyncPlan::Kind::AOnly;
            plan.direction = SyncPlan::Direction::ToB;
            plan.cuesToApply = source.cues;
            plan.match.trackA = std::move(source);
            plan.match.trackB = std::move(target);
            return plan;
        };
        const auto decision = [](Track a, Track b, bool junkOnB) {
            CrossSourceSyncConflict conflict;
            conflict.samePair = true;
            conflict.target = b;
            conflict.cuesFromA = a.cues;
            conflict.cuesFromB = b.cues;
            conflict.sourceBHasJunkCue = junkOnB;
            conflict.sourceA = std::move(a);
            conflict.sourceB = std::move(b);
            return conflict;
        };

        std::vector<CrossSourceSyncConflict> conflicts;
        conflicts.push_back(decision(
            track("rekordbox", "10", "Flaschenpost", "Kollektiv Turmstrasse",
                  "05_Kollektiv Turmstrasse-Flaschenpost.mp3", 432, 124, "8A",
                  {hot(1, 8, "#e03c3c"), hot(2, 120, "#ff9b1a"), hot(3, 200, "#ffe13b"), hot(4, 260, "#39d353"),
                   hot(5, 330, "#2ec4f0")}),
            track("engine", "110", "Flaschenpost", "Kollektiv Turmstrasse",
                  "05_Kollektiv Turmstrasse-Flaschenpost.mp3", 432, 124, "8A",
                  {hot(1, 8, "#e03c3c"), hot(2, 150, "#ff9b1a"), hot(3, 200, "#ffe13b"), hot(4, 300, "#39d353"),
                   memory(0)}),
            true));
        conflicts.push_back(decision(
            track("engine", "111", "Diary of a Lost Girl", "Roman Fl\u00fcgel",
                  "17_Roman Flugel-Diary of a Lost Girl.mp3", 418, 122, "5A",
                  {hot(1, 16, "#e03c3c"), hot(2, 140, "#ffe13b"), hot(3, 290, "#2ec4f0")}),
            track("rekordbox", "11", "Diary of a Lost Girl", "Roman Fl\u00fcgel",
                  "17_Roman Flugel-Diary of a Lost Girl.mp3", 418, 122, "5A",
                  {hot(1, 16, "#e03c3c"), hot(2, 132, "#ffe13b"), hot(3, 290, "#2ec4f0")}),
            false));

        std::vector<SyncPlan> plans;
        plans.push_back(readyPlan(
            track("engine", "112", "Rej", "\u00c2me", "12_Ame-Rej.mp3", 521, 124, "11B",
                  {hot(1, 20, "#e03c3c"), hot(2, 150, "#ff9b1a"), hot(3, 290, "#39d353"), hot(4, 430, "#2ec4f0")}),
            track("rekordbox", "12", "Rej", "\u00c2me", "12_Ame-Rej.mp3", 521, 124, "11B",
                  {hot(1, 20, "#e03c3c")})));
        plans.push_back(readyPlan(
            track("rekordbox", "13", "Bloom", "Nils Hoffmann", "02_Nils Hoffmann-Bloom.mp3", 389, 120, "3A",
                  {hot(1, 4, "#e03c3c"), hot(2, 64, "#ff9b1a"), hot(3, 128, "#ffe13b"), hot(4, 192, "#39d353"),
                   hot(5, 256, "#2ec4f0"), hot(6, 320, "#7b61ff"), memory(4), memory(256)}),
            track("engine", "113", "Bloom", "Nils Hoffmann", "02_Nils Hoffmann-Bloom.mp3", 389, 120, "3A", {})));
        plans.push_back(readyPlan(
            track("rekordbox", "14", "Loop In Loop", "Sven V\u00e4th", "09_Sven Vath-Loop In Loop.mp3", 468, 127,
                  "6A", {hot(1, 32, "#e03c3c"), hot(2, 180, "#ffe13b"), hot(3, 360, "#2ec4f0")}),
            track("engine", "114", "Loop In Loop", "Sven V\u00e4th", "09_Sven Vath-Loop In Loop.mp3", 468, 127,
                  "6A", {})));
        plans.push_back(readyPlan(
            track("onelibrary", "15", "Sisters", "Recondite", "21_Recondite-Sisters.mp3", 402, 123, "9A",
                  {hot(1, 12, "#e03c3c"), hot(2, 90, "#ff9b1a"), hot(3, 180, "#ffe13b"), hot(4, 270, "#39d353"),
                   hot(5, 350, "#2ec4f0"), memory(12)}),
            track("engine", "115", "Sisters", "Recondite", "21_Recondite-Sisters.mp3", 402, 123, "9A", {})));
        plans.push_back(readyPlan(
            track("rekordbox", "16", "Flaschenpost (Edit)", "Kollektiv Turmstrasse",
                  "33_Kollektiv Turmstrasse-Flaschenpost Edit.mp3", 245, 124, "8A",
                  {hot(1, 8, "#e03c3c"), hot(2, 120, "#ff9b1a")}),
            track("engine", "116", "Flaschenpost (Edit)", "Kollektiv Turmstrasse",
                  "33_Kollektiv Turmstrasse-Flaschenpost Edit.mp3", 245, 124, "8A", {})));

        sync->plansModel()->setAnalysis(std::move(plans), std::move(conflicts));
        return true;
    }
};

// Fills Restore Metadata's own controller without a stick or a store,
// through the one door a finished scan uses (applyScanResult), so
// tst_MetadataRestorePage.qml can drive the stick and playlist pickers,
// open rows, and photograph them.
//
// fill() is a handful of hand-made proposals from two sticks that share
// the label NO NAME and one called RV2, in three playlists: enough to
// show that the pickers narrow by stick identity and by playlist.
//
// prepareFromLibrary() + applyPrepared() is the real-scale case: every
// track of a stick-shaped copy of tests/fixtures/anonymized_library, as
// the store would offer them back to a stick that lost its cues. Split in
// two so a test can time the list opening without timing the catalog
// read in front of it.
class MetadataRestoreFixture : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    ~MetadataRestoreFixture() override
    {
        std::error_code ec;
        if (!m_root.empty()) {
            std::filesystem::remove_all(m_root, ec);
        }
    }

    Q_INVOKABLE bool fill(QObject *controller) { return fillWith(controller, false, false); }

    // fill(), plus a sixth proposal from a stick the store recorded
    // neither an id nor a label for.
    Q_INVOKABLE bool fillWithAnUnnamedStick(QObject *controller) { return fillWith(controller, true, false); }

    // fill(), with Rej (the second NO NAME) and Sisters (RV2) each a
    // conflict the stick's own cues won: offered for their rating, their
    // cues left alone.
    Q_INVOKABLE bool fillWithConflictsLeftAlone(QObject *controller) { return fillWith(controller, false, true); }

    // fill(), plus two rows from backups taken before the store recorded
    // a stick's id: one labelled RV2, which only one recorded stick is
    // called, and one labelled NO NAME, which two are.
    Q_INVOKABLE bool fillWithUnstampedRows(QObject *controller) { return fillWith(controller, false, false, true); }

    bool fillWith(QObject *controller, bool unnamedStick, bool conflictsLeftAlone, bool unstampedRows = false)
    {
        auto *restore = qobject_cast<seabass::gui::MetadataRestoreController *>(controller);
        if (restore == nullptr) {
            return false;
        }
        using namespace seabass::domain;
        const auto hot = [](int pad, double seconds, const char *color) {
            CuePoint cue;
            cue.kind = CuePoint::Kind::Hot;
            cue.hotCueNumber = pad;
            cue.positionMs = seconds * 1000.0;
            cue.color = color;
            return cue;
        };
        int nextId = 500;
        const auto proposal = [&nextId](const char *title, const char *artist, double seconds, const char *libraryId,
                                        const char *label, std::vector<std::string> playlists,
                                        std::vector<CuePoint> cues) {
            MetadataRestoreProposal p;
            p.stickTrack.format = "rekordbox";
            p.stickTrack.sourceId = std::to_string(nextId);
            p.stickTrack.title = title;
            p.stickTrack.artist = artist;
            p.stickTrack.filename = std::string(title) + ".mp3";
            p.stickTrack.filePath = std::string("/nonexistent/TESTSTICK/Contents/") + title + ".mp3";
            p.stickTrack.durationSeconds = seconds;
            CatalogRowRef row;
            row.format = "rekordbox";
            row.sourceId = std::to_string(nextId++);
            p.stickTrack.catalogRows.push_back(row);
            p.storedId = std::to_string(nextId);
            p.storedFrom = label;
            p.storedFromLibraryId = libraryId;
            p.storedPlaylists = std::move(playlists);
            p.cues = std::move(cues);
            p.cuesOffered = true;
            p.cuesFillAGap = true;
            return p;
        };
        seabass::gui::MetadataRestoreTaskResult result;
        result.libraryPath = QStringLiteral("/nonexistent/TESTSTICK/PIONEER");
        result.proposals.push_back(proposal("Flaschenpost", "Kollektiv Turmstrasse", 432, "uuid-one", "NO NAME",
                                            {"Warm Up"},
                                            {hot(1, 8, "#e03c3c"), hot(2, 120, "#ff9b1a"), hot(3, 200, "#ffe13b")}));
        result.proposals.push_back(proposal("Diary of a Lost Girl", "Roman Fl\u00fcgel", 418, "uuid-one", "NO NAME",
                                            {"Warm Up", "Closing"}, {hot(1, 16, "#e03c3c"), hot(2, 290, "#2ec4f0")}));
        result.proposals.push_back(proposal("Rej", "\u00c2me", 521, "uuid-two", "NO NAME", {"Peak Time"},
                                            {hot(1, 20, "#e03c3c"), hot(2, 150, "#ff9b1a"), hot(4, 430, "#2ec4f0")}));
        result.proposals.push_back(proposal("Bloom", "Nils Hoffmann", 389, "uuid-rv2", "RV2", {"Warm Up"},
                                            {hot(1, 4, "#e03c3c"), hot(2, 64, "#ff9b1a"), hot(3, 128, "#ffe13b"),
                                             hot(4, 192, "#39d353")}));
        result.proposals.push_back(proposal("Sisters", "Recondite", 402, "uuid-rv2", "RV2", {"Closing"},
                                            {hot(1, 12, "#e03c3c")}));
        if (conflictsLeftAlone) {
            for (const std::size_t index : {std::size_t{2}, std::size_t{4}}) {
                auto &p = result.proposals[index];
                p.cuesConflict = true;
                p.cuesOffered = false;
                p.cuesFillAGap = false;
                p.ratingOffered = true;
                p.rating = 4;
            }
        }
        if (unnamedStick) {
            result.proposals.push_back(proposal("Nameless", "Nobody", 300, "", "", {"Warm Up"},
                                                {hot(1, 10, "#e03c3c")}));
        }
        if (unstampedRows) {
            result.proposals.push_back(proposal("Older RV2 Row", "Somebody", 300, "", "RV2", {"Closing"},
                                                {hot(1, 10, "#e03c3c")}));
            result.proposals.push_back(proposal("Older NO NAME Row", "Somebody", 300, "", "NO NAME", {"Closing"},
                                                {hot(1, 10, "#e03c3c")}));
        }
        result.recordedIdsByLabel = {{"NO NAME", {"uuid-one", "uuid-two"}}, {"RV2", {"uuid-rv2"}}};
        result.stickTrackCount = static_cast<int>(result.proposals.size());
        result.storedTrackCount = static_cast<int>(result.proposals.size());
        restore->applyScanResult(std::move(result));
        return true;
    }

    // Marks the proposal at `index` staged without an edit session, the
    // way a real stage leaves it, so a test can watch a scope change take
    // it off again. The change id names nothing a session holds.
    Q_INVOKABLE void markStaged(QObject *controller, int index)
    {
        auto *restore = qobject_cast<seabass::gui::MetadataRestoreController *>(controller);
        if (restore == nullptr) {
            return;
        }
        restore->proposals()->setStagedChanges(index, {QStringLiteral("fixture:%1").arg(index)});
        emit restore->analysisChanged();
    }

    // The proposal count, or -1 when the copy or the read failed.
    Q_INVOKABLE int prepareFromLibrary(const QString &fixtureRoot)
    {
        namespace fs = std::filesystem;
        using namespace seabass;
        std::error_code ec;
        m_root = testing::scratchRoot()
            / ("seabass_qml_metadata_restore_" + std::to_string(QCoreApplication::applicationPid()));
        fs::remove_all(m_root, ec);
        const fs::path stick = m_root / "FIXTURE";
        const fs::path from = gui::pathFromQString(fixtureRoot);
        fs::create_directories(stick, ec);
        // A copy, never the fixture itself: nothing here writes, but the
        // readers are handed a stick, and a stick is somewhere a later
        // change could decide to put a lock file.
        fs::copy(from / "rekordbox", stick / "PIONEER", fs::copy_options::recursive, ec);
        if (ec) {
            return -1;
        }
        fs::copy(from / "engine", stick / "Engine Library", fs::copy_options::recursive, ec);
        if (ec) {
            return -1;
        }
        const std::string pioneer = pathToUtf8(stick / "PIONEER");
        const auto read = gui::readAllStickCatalogs(pioneer, application::NullProgressReporter::instance(),
                                                     application::CancellationToken::none());
        std::vector<domain::Track> rows;
        for (const auto *catalog : {&read.catalogs.rekordbox, &read.catalogs.oneLibrary, &read.catalogs.engine}) {
            if (*catalog) {
                rows.insert(rows.end(), (*catalog)->begin(), (*catalog)->end());
            }
        }
        std::vector<domain::Track> stickTracks = application::collapseCatalogRows(rows);
        // The store's side: every track as it was, cues and all, with at
        // least one cue so every track has something to offer back, and
        // the stick's side with its cues gone -- the stick a re-export
        // left behind, which is the case this page exists for.
        std::vector<domain::Track> stored;
        stored.reserve(stickTracks.size());
        for (std::size_t i = 0; i < stickTracks.size(); ++i) {
            domain::Track copy = stickTracks[i];
            copy.format = "metadata-store";
            copy.sourceId = std::to_string(i + 1);
            copy.filePath.clear();
            copy.catalogRows.clear();
            // The fixture ships without its cover images; a path to one
            // would only fill the log with failed image loads.
            copy.artworkPath.clear();
            copy.metadataModifiedAt = 1'800'000'000;
            if (copy.cues.empty()) {
                domain::CuePoint cue;
                cue.kind = domain::CuePoint::Kind::Hot;
                cue.hotCueNumber = 1;
                cue.positionMs = 30'000.0;
                copy.cues.push_back(cue);
            }
            stored.push_back(std::move(copy));
            stickTracks[i].cues.clear();
            for (auto &row : stickTracks[i].catalogRows) {
                row.cues.clear();
            }
        }
        m_prepared = gui::MetadataRestoreTaskResult();
        m_prepared.proposals = domain::planMetadataRestore(stickTracks, stored, 1'700'000'000);
        for (std::size_t i = 0; i < m_prepared.proposals.size(); ++i) {
            m_prepared.proposals[i].storedFrom = i % 2 == 0 ? "RV2" : "A4";
            m_prepared.proposals[i].storedFromLibraryId = i % 2 == 0 ? "uuid-rv2" : "uuid-a4";
        }
        m_prepared.recordedIdsByLabel = {{"RV2", {"uuid-rv2"}}, {"A4", {"uuid-a4"}}};
        m_prepared.libraryPath = QString::fromStdString(pioneer);
        m_prepared.stickTrackCount = static_cast<int>(stickTracks.size());
        m_preparedStickTracks = m_prepared.stickTrackCount;
        m_prepared.storedTrackCount = static_cast<int>(stored.size());
        return static_cast<int>(m_prepared.proposals.size());
    }

    // How many tracks the stick side of the last prepare had, after the
    // catalogs were folded into files.
    Q_INVOKABLE int preparedStickTrackCount() const { return m_preparedStickTracks; }

    // The stick-shaped copy prepareFromLibrary() made, for a page that
    // scans a stick itself (Metadata Backup). Empty before it ran.
    Q_INVOKABLE QString stickRoot() const
    {
        return m_root.empty() ? QString() : seabass::gui::pathToQString(m_root / "FIXTURE");
    }

    Q_INVOKABLE bool applyPrepared(QObject *controller)
    {
        auto *restore = qobject_cast<seabass::gui::MetadataRestoreController *>(controller);
        if (restore == nullptr) {
            return false;
        }
        restore->applyScanResult(std::move(m_prepared));
        m_prepared = {};
        return true;
    }

private:
    std::filesystem::path m_root;
    seabass::gui::MetadataRestoreTaskResult m_prepared;
    int m_preparedStickTracks = 0;
};

class Setup : public QObject
{
    Q_OBJECT
public slots:
    // Screenshot mode only: the app runs under the desktop's own Qt Quick
    // style with Material's dark palette exported for popups (see
    // gui/main.cpp); offscreen there is no desktop, so without this the
    // pages render in the light Basic style over Theme.qml's dark palette
    // and every contrast judgement is wrong. Material Dark is the closest
    // stand-in that needs no platform theme. The plain test run (no
    // screenshot dir) is left exactly as it was.

// Two rows in the metadata store, so the page that lists them has
// something to list.
//
// Four cases skipped without this -- three about leaving the page with
// staged work, and one screenshot -- on "no stored tracks in this run's
// metadata store". A skip proves nothing, and these are the cases that
// cover walking away from work you cannot see, which is the whole
// reason the page grew a guard.
//
// Seeded here rather than in the test, because the page reads the
// DEFAULT store and there is no way to hand it another path from QML.
// SEABASS_HOME is already sandboxed per test binary by the time this
// runs, so the rows land in the run's own directory and nothing
// reaches a real store.
//
// Only when the store is empty. A store with rows in it is a store some
// other setup meant to arrange, and overwriting it would replace a
// deliberate fixture with this one.
void seedMetadataStoreForTests()
{
    using namespace seabass;
    try {
        infrastructure::local::MetadataStore store;
        if (!store.readAll().empty()) {
            return;
        }
        // From the stick's own catalog when there is a stick, because
        // the Restore page MATCHES stored tracks against what is on the
        // stick. Invented rows match nothing, so the page produces no
        // proposals and its screenshot case fails on "the scan must
        // produce proposals against this stick" -- which is the seed
        // being wrong, not the page.
        std::vector<domain::Track> tracks;
        infrastructure::local::MetadataSource source;
        const QByteArray liveStick = qgetenv("SEABASS_LIVE_STICK");
        if (!liveStick.isEmpty()) {
            const std::filesystem::path root = seabass::gui::pathFromQString(liveStick);
            const std::filesystem::path pioneer = root / "PIONEER";
            if (std::filesystem::exists(pioneer / "rekordbox" / "export.pdb")) {
                infrastructure::rekordbox::KaitaiRekordboxReader reader(seabass::pathToUtf8(pioneer));
                std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
                // A handful, not the library: this runs before every
                // test in the binary and a full store costs seconds.
                if (read.size() > 5) {
                    read.resize(5);
                }
                // With a cue the stick does not have, so there is
                // something to RESTORE. Seeding the stick's tracks
                // unchanged stores rows identical to what is already
                // there, the Restore page has nothing to propose, and
                // its screenshot case fails on "the scan must produce
                // proposals against this stick" -- which is true, and is
                // the seed's fault rather than the page's. It passed
                // when the stick happened to differ from the store and
                // failed as soon as a round restored the stick to its
                // reference first.
                for (domain::Track &track : read) {
                    domain::CuePoint cue;
                    cue.kind = domain::CuePoint::Kind::Hot;
                    cue.hotCueNumber = 7;
                    cue.positionMs = 99000.0;
                    track.cues.push_back(cue);
                }
                tracks = std::move(read);
            }
            source.stickRoot = root;
            source.libraryId = "harness-seed";
            source.stickLabel = "SEEDED";
            source.catalogModifiedAt = 0;
        }
        if (tracks.empty()) {
            for (int i = 0; i < 2; ++i) {
                domain::Track track;
                track.sourceId = "seed-" + std::to_string(i);
                track.format = "rekordbox";
                track.title = "Seeded Track " + std::to_string(i + 1);
                track.artist = "Harness";
                track.durationSeconds = 180.0 + i;
                track.filePath = "/seeded/Contents/seed-" + std::to_string(i) + ".mp3";
                domain::CuePoint cue;
                cue.kind = domain::CuePoint::Kind::Hot;
                cue.hotCueNumber = 0;
                cue.positionMs = 1000.0 * (i + 1);
                track.cues.push_back(cue);
                tracks.push_back(track);
            }
            source.stickRoot = "/seeded";
            source.libraryId = "harness-seed";
            source.stickLabel = "SEEDED";
            source.catalogModifiedAt = 0;
        }
        store.store(tracks, source, application::NullProgressReporter::instance(),
                    application::CancellationToken::none());
    } catch (const std::exception &) {
        // Not fatal: the four cases go back to skipping and say so,
        // which is what they did before this existed.
    }
}

    void applicationAvailable()
    {
        // The same text renderer the app picks in gui/main.cpp, for the
        // same reason, and set here as well so a screenshot taken from
        // this binary is of the text Seabass actually draws. A harness
        // that renders glyphs differently from the app is a harness that
        // cannot be used to judge how they look.
#ifdef Q_OS_LINUX
        QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
#endif

        // The same interface font the app names for itself in
        // gui/main.cpp, and for the same reason: a harness whose labels
        // resolve to a different font from the app's is a harness that
        // cannot be used to judge them. This is also where the fault was
        // first visible -- the Windows rig's labels asked for the
        // generic "Sans Serif" and were handed the bundled symbol
        // subset, which has no Latin coverage.
        // The same style the app picks, for the same reason it picks
        // it. ctest pins QT_QUICK_CONTROLS_STYLE on Linux and that still
        // wins; where nothing is pinned, this is what ships.
        seabass::gui::applyDefaultControlsStyle();

        seedMetadataStoreForTests();

        QGuiApplication::setFont(seabass::gui::interfaceFont());

        // AppSettingsController, main.cpp's exportMaterialPalette() and
        // media_controller.cpp's opened-folders store all construct their
        // QSettings the same way -- QSettings("seabass", "seabass"),
        // i.e. QSettings::defaultFormat() at QSettings::UserScope -- and
        // several QML tests build the real controller rather than a fake
        // one (see tst_AppSettingsPage.qml's own comment). CMakeLists.txt
        // sets XDG_CONFIG_HOME to a build-local directory specifically so
        // those tests read and write there instead of the developer's
        // real settings, but that redirection is a Linux/XDG convention:
        // QSettings::NativeFormat (the default) ignores XDG_CONFIG_HOME
        // entirely on Windows and always resolves to the registry
        // (HKCU\Software\seabass\seabass) regardless of it. On Windows
        // this comment's whole reason for existing silently did nothing
        // -- every run of this binary wrote real test fixture values
        // (a fake stickBackupDirectory among them) straight into the
        // real registry, which the real app then read back as if a user
        // had set them. Forcing IniFormat and pointing UserScope at the
        // same XDG_CONFIG_HOME directory makes the redirect actually
        // apply, identically, on every platform -- the registry (or
        // equivalent) is never touched by a test run again.
        //
        // Unconditional, and it trusts nothing it was handed. The
        // redirect used to happen only when XDG_CONFIG_HOME was ALREADY
        // set -- true under ctest, which sets it, and false for the
        // command docs/testing.md tells you to run:
        //
        //     SEABASS_SCREENSHOT_DIR=<dir> QT_QPA_PLATFORM=offscreen \
        //         build/seabass_qml_tests -input tests/qml
        //
        // Plasma does not export XDG_CONFIG_HOME (it is a default, not a
        // setting), so on a normal KDE desktop that guard fell straight
        // through and every direct run wrote the suite's own fixtures
        // into the real ~/.config/seabass/seabass.conf -- including
        // tst_AppSettingsPage's fake "/home/somebody/Music/..." backup
        // directory, which the app then read back and showed as the
        // user's own choice.
        //
        // Honouring the variable when it IS set would leave the same
        // hole open from the other side: plenty of setups export
        // XDG_CONFIG_HOME="$HOME/.config" from a dotfile or an
        // environment.d drop-in, and then the "sandbox" is the real
        // store and every check below passes while the fixtures land in
        // it. So this makes its own directory every run and points the
        // platform's own variables at it -- via the same helpers the C++
        // tests use, which also covers APPDATA for Windows, where the
        // native store is the registry and no XDG variable is read.
        //
        // SEABASS_HOME too: sandboxSeabassHome() leaves an inherited one
        // alone (ctest sets one per test), but on a direct run there is
        // none, and AppSettingsController would otherwise point the
        // local root at the developer's real ~/Seabass -- which is what
        // tst_StickListPage's home-backup probes would then be reading.
        static QTemporaryDir sandbox;
        if (!sandbox.isValid()) {
            qCritical("seabass_qml_tests: could not create a settings sandbox (%s), refusing "
                      "to run rather than fall back to the real store.",
                      qPrintable(sandbox.errorString()));
            std::abort();
        }
        const std::filesystem::path sandboxRoot = seabass::gui::pathFromQString(sandbox.path());
        // Named "Seabass" rather than "home": SEABASS_HOME stands in for
        // the real ~/Seabass, and pages that show the user where they
        // write show this path. tst_MetadataBackupPage asserts the label
        // names a Seabass location, which under ctest passed only
        // because the build directory happens to sit under ~/Seabass --
        // an accident this would otherwise have turned into a failure.
        seabass::testing::sandboxSeabassHome(sandboxRoot / "Seabass");
        seabass::testing::sandboxSettings(sandboxRoot / "config");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           seabass::gui::pathToQString(sandboxRoot / "config"));

        // The colour scheme the app hands KDE's style (gui/main.cpp), for
        // the reason the style and the font are matched above: without it
        // the desktop-style lane draws unstyled Labels in whatever scheme
        // the machine has, which is not the ink the app ships. Written
        // into the sandbox, never the real cache.
        seabass::gui::applyAppColorScheme(false, seabass::gui::pathToQString(sandboxRoot / "color-schemes"));

        // And proof -- against the real store, computed independently,
        // rather than against the string just handed to setPath(), which
        // would agree with itself whatever it pointed at. What has to be
        // true is that nothing lands under the user's own config
        // directory; the sandbox lives in the temp tree, so it cannot.
        //
        // Probed via openSeabassSettings(), the same call production
        // code makes: a bare QSettings("seabass","seabass") here would
        // check a different, uninteresting thing on Windows, where that
        // two-argument constructor keeps resolving to NativeFormat (the
        // registry) regardless of the setDefaultFormat()/setPath() calls
        // above -- confirmed directly, not merely suspected -- so the
        // probe would find nothing wrong while AppSettingsController's
        // real four-argument-constructed QSettings still opened the
        // registry underneath it.
        {
            QSettings probe = seabass::gui::openSeabassSettings();
            const QString realConfigRoot = QDir::homePath() + QStringLiteral("/.config");
            if (probe.fileName().startsWith(realConfigRoot)
                || !probe.fileName().startsWith(sandbox.path())) {
                qCritical("seabass_qml_tests: settings would be written to %s, outside the "
                          "sandbox at %s, refusing to run rather than touch the real store.",
                          qPrintable(probe.fileName()), qPrintable(sandbox.path()));
                std::abort();
            }
        }

        const char *dir = std::getenv("SEABASS_SCREENSHOT_DIR");
        if (dir == nullptr || *dir == '\0') {
            return;
        }
        if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_STYLE")) {
            qputenv("QT_QUICK_CONTROLS_STYLE", "Material");
        }
        qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
        qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "#3daee9");
        qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#123a52");
    }

    void qmlEngineAvailable(QQmlEngine *engine)
    {
        engine->rootContext()->setContextProperty(QStringLiteral("screenshotDir"),
                                                  qEnvironmentVariable("SEABASS_SCREENSHOT_DIR"));
        // Whether this run ended up under a Qt Quick Controls style other
        // than "Basic", the one the pixel-measuring tests are calibrated
        // against: the screenshot mode forces Material, a run under the
        // desktop's own style forces that, and -- the case an env-var
        // check alone missed -- a plain ctest run on Windows still lands
        // on "Windows" with nothing forced at all, because that style is
        // this platform's native default rather than something QML asks
        // for. Comparing the resolved name instead of the env var catches
        // every one of those the same way.
        engine->rootContext()->setContextProperty(QStringLiteral("controlsStyleForced"),
                                                  QQuickStyle::name() != QStringLiteral("Basic"));
        // Whether unstyled text takes its ink from KDE's colour scheme
        // (KDE_COLOR_SCHEME_PATH, gui/app_color_scheme.hpp) rather than
        // from the platform palette: true only under KDE's own style.
        engine->rootContext()->setContextProperty(QStringLiteral("kdeDesktopStyle"),
                                                  QQuickStyle::name() == QStringLiteral("org.kde.desktop"));
        // Whether unstyled text takes its ink from Material's theme, the
        // style macOS runs (gui/controls_style.hpp): the ink then follows
        // whichever Material.theme the nearest item up the tree sets.
        engine->rootContext()->setContextProperty(QStringLiteral("materialStyle"),
                                                  QQuickStyle::name() == QStringLiteral("Material"));
        // Whether this run is entitled to a working shader. The suite is
        // registered on a real display with Qt6 ShaderTools present, and
        // sets SEABASS_SHADER_EXPECTED=1 to say so; a test that asks the
        // platform "can you draw?" and accepts no for an answer would
        // otherwise pass on a build where the shader never got compiled
        // in, which is the whole ring going untested under a green run.
        // Where it is unset -- somebody running the binary by hand, or a
        // platform without a display -- the tests still cover both paths.
        engine->rootContext()->setContextProperty(
            QStringLiteral("shaderExpected"),
            qEnvironmentVariable("SEABASS_SHADER_EXPECTED") == QStringLiteral("1"));
        // A writable directory a QML test may point a controller at: the
        // same per-pid scratch tree the C++ fixtures use, already created.
        // Without it a live test that needs a folder of its own has to
        // invent an absolute path, which means either the developer's real
        // ~/Seabass or a path that does not exist.
        engine->rootContext()->setContextProperty(
            QStringLiteral("testScratchDir"), seabass::gui::pathToQString(seabass::testing::scratchRoot()));
        // tests/qml-live/: the mount point of a real (scratch) stick to
        // drive the real pages and controllers against. Empty under
        // ctest, and every live test skips itself then.
        engine->rootContext()->setContextProperty(QStringLiteral("liveStickRoot"),
                                                  qEnvironmentVariable("SEABASS_LIVE_STICK"));
        // Which of the orchestrated live scenarios this run is (see
        // tests/qml-live/run-live.sh); each file skips itself otherwise.
        engine->rootContext()->setContextProperty(QStringLiteral("liveLockPlanted"),
                                                  qEnvironmentVariableIsSet("SEABASS_LIVE_LOCKED"));
        engine->rootContext()->setContextProperty(QStringLiteral("liveGuardRun"),
                                                  qEnvironmentVariableIsSet("SEABASS_LIVE_GUARD"));
        // A mounted disk image standing in for a stick (docs/testing.md):
        // it has no hardware serial, so checks that assert how a stick was
        // re-identified have to expect one step less.
        engine->rootContext()->setContextProperty(QStringLiteral("liveStickIsDiskImage"),
                                                  qgetenv("SEABASS_ACCEPT_DISK_IMAGES") == "1");
        engine->rootContext()->setContextProperty(QStringLiteral("liveStickPullRun"),
                                                  qEnvironmentVariableIsSet("SEABASS_LIVE_STICK_PULL"));
        // tools/rig-clones.sh: where tst_LiveEditMode::test_11_rigKeepCue
        // adds a memory cue it keeps, in ms (0: the test skips itself).
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigKeepCueMs"),
                                                  qEnvironmentVariableIntValue("SEABASS_RIG_KEEP_CUE_MS"));
        // tools/rig-edits.sh, after tools/rig_plant_repairable planted an
        // issue: test_10 must find something to repair. A skip exits 0, so
        // without this a plant Library Health did not see still read PASS.
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigRequireRepairable"),
                                                  qEnvironmentVariableIsSet("SEABASS_RIG_REQUIRE_REPAIRABLE"));
        // tools/rig-shakedown.sh, W8: the delete-orphaned-files check
        // removes audio files for good and leaves the stick changed, so it
        // only runs where the runner restores the stick afterwards.
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigDeleteOrphans"),
                                                  qEnvironmentVariableIsSet("SEABASS_RIG_DELETE_ORPHANS"));
        // tests/qml-live/tst_LivePages.qml, R5: the folder whose entries
        // link to the reference backups, so Manage Backups can be pointed
        // at them without naming a path in the test.
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigReferenceDir"),
                                                  qEnvironmentVariable("SEABASS_RIG_REFERENCE_DIR"));
        // tests/qml-live/tst_LiveFullStick.qml, F4: the runner has filled
        // the stick to within a few MB of full and will delete the filler
        // afterwards. Without this the check skips, because a save that
        // fits proves nothing about a stick that is out of room.
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigFullStick"),
                                                  qEnvironmentVariableIsSet("SEABASS_RIG_FULL_STICK"));
        // A second (scratch) stick, for the flows that read one stick and
        // write another -- metadata backed up from it, restored onto
        // liveStickRoot. Empty: those tests skip themselves.
        engine->rootContext()->setContextProperty(QStringLiteral("liveSecondStickRoot"),
                                                  qEnvironmentVariable("SEABASS_LIVE_SECOND_STICK"));
        // The Breeze icons compiled into this binary, by name, read from
        // the resources rather than listed: tst_SeabassIcon loads each one
        // through Theme.iconUrl(), so a file registered under a different
        // path than the one QML asks for fails there.
        QStringList bundledIcons;
        for (const QString &file : QDir(QStringLiteral(":/qt/qml/SeabassGui/qml/icons/breeze"))
                                       .entryList({QStringLiteral("*.svg")}, QDir::Files)) {
            bundledIcons.append(file.chopped(4));
        }
        engine->rootContext()->setContextProperty(QStringLiteral("bundledIcons"), bundledIcons);
        engine->rootContext()->setContextProperty(QStringLiteral("syncPageFixture"), new SyncPageFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("metadataRestoreFixture"),
                                                  new MetadataRestoreFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("artworkFixture"), new ArtworkFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("stickFixture"), new StickFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("controllerFixture"), new ControllerFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("browseFixture"), new BrowseFixture(engine));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(SeabassGuiQmlTests, Setup)

#include "qml_test_main.moc"
