// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QQmlContext>
#include <QQmlEngine>
#include <QDir>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include "application/use_cases/scan_library.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/local/metadata_store.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/controls_style.hpp"
#include "gui/interface_font.hpp"
#include "../scratch_path.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/sync_controller.hpp"
#include "gui/format_usb_controller.hpp"
#include "gui/library_consistency_controller.hpp"
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtQuickTest/quicktest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
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
            std::filesystem::remove_all(root.toStdString(), ec);
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
        const std::string file = (std::filesystem::path(library.toStdString()) / "Database2" / "m.db").string();
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
        const fs::path stick = fs::path(library.toStdString()).parent_path();
        sqlite3 *db = nullptr;
        if (sqlite3_open((fs::path(library.toStdString()) / "Database2" / "m.db").string().c_str(), &db)
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
            out << "\xFF\xD8\xFF" << image.filename().string();
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
        fs::copy(fromLibrary.toStdString(), library, fs::copy_options::recursive, ec);
        if (ec) {
            return {};
        }
        m_roots.append(QString::fromStdString(root.string()));
        if (!eraseImages) {
            return QString::fromStdString(library.string());
        }

        // Every image the copy might have had, gone.
        fs::remove_all(library / "Artwork", ec);
        fs::create_directories(library / "Artwork", ec);
        sqlite3 *db = nullptr;
        if (sqlite3_open((library / "Database2" / "m.db").string().c_str(), &db) != SQLITE_OK) {
            sqlite3_close(db);
            return {};
        }
        // Proper 20-byte hashes, the width Engine's own rows use: the rows
        // are right, and only their files are missing.
        const bool ok =
            sqlite3_exec(db, "UPDATE AlbumArt SET hash = randomblob(20);", nullptr, nullptr, nullptr) == SQLITE_OK;
        sqlite3_close(db);
        return ok ? QString::fromStdString(library.string()) : QString();
    }

    QStringList m_roots;
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
        const QString root = QString::fromStdString(stick.string());
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
            const std::filesystem::path root = liveStick.toStdString();
            const std::filesystem::path pioneer = root / "PIONEER";
            if (std::filesystem::exists(pioneer / "rekordbox" / "export.pdb")) {
                infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer.string());
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
            qCritical("seabass_qml_tests: could not create a settings sandbox (%s) -- refusing "
                      "to run rather than fall back to the real store.",
                      qPrintable(sandbox.errorString()));
            std::abort();
        }
        const std::filesystem::path sandboxRoot(sandbox.path().toStdString());
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
                           QString::fromStdString((sandboxRoot / "config").string()));

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
                          "sandbox at %s -- refusing to run rather than touch the real store.",
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
        const char *dir = std::getenv("SEABASS_SCREENSHOT_DIR");
        engine->rootContext()->setContextProperty(QStringLiteral("screenshotDir"),
                                                  dir != nullptr ? QString::fromLocal8Bit(dir) : QString());
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
            QString::fromLocal8Bit(qgetenv("SEABASS_SHADER_EXPECTED")) == QStringLiteral("1"));
        // A writable directory a QML test may point a controller at: the
        // same per-pid scratch tree the C++ fixtures use, already created.
        // Without it a live test that needs a folder of its own has to
        // invent an absolute path, which means either the developer's real
        // ~/Seabass or a path that does not exist.
        engine->rootContext()->setContextProperty(
            QStringLiteral("testScratchDir"), QString::fromStdString(seabass::testing::scratchRoot().string()));
        // tests/qml-live/: the mount point of a real (scratch) stick to
        // drive the real pages and controllers against. Empty under
        // ctest, and every live test skips itself then.
        const char *stick = std::getenv("SEABASS_LIVE_STICK");
        engine->rootContext()->setContextProperty(QStringLiteral("liveStickRoot"),
                                                  stick != nullptr ? QString::fromLocal8Bit(stick) : QString());
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
        const char *referenceDir = std::getenv("SEABASS_RIG_REFERENCE_DIR");
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigReferenceDir"),
                                                  referenceDir != nullptr ? QString::fromLocal8Bit(referenceDir)
                                                                          : QString());
        // tests/qml-live/tst_LiveFullStick.qml, F4: the runner has filled
        // the stick to within a few MB of full and will delete the filler
        // afterwards. Without this the check skips, because a save that
        // fits proves nothing about a stick that is out of room.
        engine->rootContext()->setContextProperty(QStringLiteral("liveRigFullStick"),
                                                  qEnvironmentVariableIsSet("SEABASS_RIG_FULL_STICK"));
        // A second (scratch) stick, for the flows that read one stick and
        // write another -- metadata backed up from it, restored onto
        // liveStickRoot. Empty: those tests skip themselves.
        const char *secondStick = std::getenv("SEABASS_LIVE_SECOND_STICK");
        engine->rootContext()->setContextProperty(QStringLiteral("liveSecondStickRoot"),
                                                  secondStick != nullptr ? QString::fromLocal8Bit(secondStick) : QString());
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
        engine->rootContext()->setContextProperty(QStringLiteral("artworkFixture"), new ArtworkFixture(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("controllerFixture"), new ControllerFixture(engine));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(SeabassGuiQmlTests, Setup)

#include "qml_test_main.moc"
