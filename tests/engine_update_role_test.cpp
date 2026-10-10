// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// DetectedStickListModel's engineUpdate and syncNeeded roles, the stick
// card badge of Sync after Rekordbox Export, through the real
// MediaController on a copy of the anonymized fixture opened as a folder
// (openFolder runs detect(), which starts the stick's request), never on
// the fixture itself and never on a stick.
//
// What it pins: the roles come from one request per stick and data()
// reads nothing (the catalog cache's passes are counted around a sweep of
// every role of every row); a current baseline is answered without a
// catalog read; and the answer follows the stick after refreshSyncNeeded()
// (a recorded baseline, a moved pdb sequence, a levelled counter), and
// after export.pdb changes outside Seabass while the stick stays listed,
// through the stat timer alone.
//
// The fixture copy ships in "Sync Needed" state (pdb 15132, Engine
// counter 14204) with no baseline, and rekordbox playlist paths Engine
// lacks (seabass-cli sync-after-export proposes 1 create and 925 adds on
// it), so it starts at "library".

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSettings>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/rekordbox_baseline.hpp"
#include "fixture_copy.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/media_controller.hpp"
#include "gui/qt_path.hpp"
#include "gui/seabass_settings.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/local/rekordbox_baseline_file.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::gui::DetectedStickListModel;
using seabass::gui::LibraryCatalogCache;
using seabass::gui::MediaController;
using seabass::gui::pathToQString;
using seabass::pathFromUtf8;
using seabass::pathToGenericUtf8;
using seabass::pathToUtf8;

namespace
{

std::atomic<int> g_passes{0};

int rowOf(const DetectedStickListModel &model, const std::string &mountPoint)
{
    const auto &sticks = model.sticks();
    for (size_t i = 0; i < sticks.size(); ++i) {
        if (sticks[i].mountPoint == mountPoint) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Every role of every row, the way a view and get() ask: the passes the
// cache ran before and after must be the same.
void readEveryRole(DetectedStickListModel &model)
{
    const auto roles = model.roleNames();
    for (int row = 0; row < model.rowCount(); ++row) {
        for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
            (void)model.data(model.index(row), it.key());
        }
        (void)model.get(row);
    }
}

// Waits for the next answer of the stick's request: a dataChanged on its
// row naming the engineUpdate role.
void waitForAnswer(DetectedStickListModel &model, const std::string &mountPoint, int &answers, int expected)
{
    QElapsedTimer timer;
    timer.start();
    while (answers < expected && timer.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    assert(answers == expected && "the request answered");
    (void)model;
    (void)mountPoint;
}

QString roleText(DetectedStickListModel &model, int row, int role)
{
    return model.data(model.index(row), role).toString();
}

void writePdbSequence(const fs::path &pioneer, std::uint32_t sequence)
{
    std::fstream pdb(pioneer / "rekordbox" / "export.pdb", std::ios::binary | std::ios::in | std::ios::out);
    assert(pdb);
    pdb.seekp(5 * sizeof(std::uint32_t));
    const unsigned char bytes[4] = {static_cast<unsigned char>(sequence), static_cast<unsigned char>(sequence >> 8),
                                    static_cast<unsigned char>(sequence >> 16), static_cast<unsigned char>(sequence >> 24)};
    pdb.write(reinterpret_cast<const char *>(bytes), 4);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_update_role_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    QCoreApplication app(argc, argv);
    const fs::path fixture = pathFromUtf8(argv[1]);

    const fs::path scratch = seabass::testing::scratchRoot() / "engine-update-role-test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    seabass::testing::sandboxSeabassHome(scratch / "home");
    // openFolder() remembers the folder in the settings store: never the
    // real one (see open_folder_test.cpp for why both belts).
    seabass::testing::sandboxSettings(scratch / "config");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, pathToQString(scratch / "config"));
    {
        QSettings probe = seabass::gui::openSeabassSettings();
        assert(probe.fileName().toStdString().rfind(pathToGenericUtf8(scratch / "config"), 0) == 0);
    }

    // The real passes, counted.
    LibraryCatalogCache counting(
        [real = LibraryCatalogCache::realStageForTesting()](
            LibraryCatalogCache::Detail stage, const std::string &format, const std::string &path,
            std::vector<seabass::domain::Track> &tracks, LibraryCatalogCache::StageNotes &notes,
            seabass::application::ProgressReporter &progress, seabass::application::CancellationToken cancel) {
            ++g_passes;
            real(stage, format, path, tracks, notes, progress, cancel);
        },
        LibraryCatalogCache::realMtimeForTesting());
    LibraryCatalogCache::setInstanceForTesting(&counting);

    const fs::path root = scratch / "stick";
    const fs::path pioneer = root / "PIONEER";
    const fs::path engine = root / "Engine Library";
    std::error_code ec;
    seabass::testing::copyPioneerFixture(fixture / "rekordbox", pioneer, ec);
    assert(!ec);
    fs::copy(fixture / "engine", engine, fs::copy_options::recursive, ec);
    assert(!ec);
    const std::string mountPoint = pathToUtf8(root);

    {
        MediaController controller;
        DetectedStickListModel &model = *controller.sticksModel();
        int answers = 0;
        QObject::connect(&model, &DetectedStickListModel::dataChanged,
                         [&](const QModelIndex &from, const QModelIndex &, const QList<int> &roles) {
                             const int row = rowOf(model, mountPoint);
                             if (row >= 0 && from.row() == row && roles.contains(DetectedStickListModel::EngineUpdateRole)) {
                                 ++answers;
                             }
                         });
        const auto roles = model.roleNames();
        assert(roles.value(DetectedStickListModel::EngineUpdateRole) == "engineUpdate");
        assert(roles.value(DetectedStickListModel::SyncNeededRole) == "syncNeeded");

        assert(controller.openFolder(pathToQString(root)).isEmpty());
        const int row = rowOf(model, mountPoint);
        assert(row >= 0);
        // Asked before the answer: nothing known yet, and nothing read.
        assert(roleText(model, row, DetectedStickListModel::EngineUpdateRole).isEmpty());

        waitForAnswer(model, mountPoint, answers, 1);
        const int afterInsert = g_passes.load();
        assert(afterInsert == 2 && "the Tracks stage of rekordbox and Engine, once each");
        assert(roleText(model, row, DetectedStickListModel::EngineUpdateRole) == QStringLiteral("library"));
        assert(model.data(model.index(row), DetectedStickListModel::SyncNeededRole).toBool()
               && "15132 against 14204: the player would offer the import");

        // data() reads nothing: with both catalogs dropped from the cache,
        // so a read in data() would have to run a pass, a hundred sweeps of
        // every role run none.
        counting.invalidate("rekordbox", model.data(model.index(row), DetectedStickListModel::RekordboxPathRole)
                                             .toString()
                                             .toStdString());
        counting.invalidate("engine", model.data(model.index(row), DetectedStickListModel::EnginePathRole)
                                          .toString()
                                          .toStdString());
        for (int i = 0; i < 100; ++i) {
            readEveryRole(model);
        }
        assert(g_passes.load() == afterInsert && "no catalog read in data()");
        std::cout << "case 1 (the request answers \"library\" and data() reads nothing) OK\n";

        // A baseline recorded from rekordbox as it is now, at its sequence:
        // current, so the answer is "" without a catalog read, and the
        // counter test still says what it said.
        {
            seabass::infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(pioneer));
            const auto tracks = reader.readAll();
            const auto tree = seabass::infrastructure::rekordbox::rekordboxPlaylistTree(pathToUtf8(pioneer));
            const std::string rootText = pathToUtf8(root);
            const auto baseline = seabass::domain::baselineFrom(
                tracks, tree, 15132,
                [&](const std::string &p) { return seabass::application::stickRelativePathOf(p, rootText); },
                [](const std::string &p) { return seabass::application::normalizedPathKey(p); });
            std::string error;
            const bool written = seabass::infrastructure::local::writeRekordboxBaseline(
                root, baseline, [](const std::string &) {}, &error);
            assert(written && error.empty());
            (void)written;
        }
        controller.refreshSyncNeeded();
        waitForAnswer(model, mountPoint, answers, 2);
        assert(roleText(model, row, DetectedStickListModel::EngineUpdateRole).isEmpty());
        assert(model.data(model.index(row), DetectedStickListModel::SyncNeededRole).toBool());
        assert(g_passes.load() == afterInsert && "a current baseline is answered without reading a catalog");
        std::cout << "case 2 (a current baseline: \"\" with no catalog read) OK\n";

        // rekordbox exported: the sequence moved, the library did not. The
        // role flips to "cues" on the next refresh, not before.
        writePdbSequence(pioneer, 15133);
        assert(roleText(model, row, DetectedStickListModel::EngineUpdateRole).isEmpty()
               && "the model holds the last answer until it is asked again");
        controller.refreshSyncNeeded();
        waitForAnswer(model, mountPoint, answers, 3);
        assert(roleText(model, row, DetectedStickListModel::EngineUpdateRole) == QStringLiteral("cues"));
        assert(g_passes.load() == afterInsert + 1 && "rekordbox's Tracks stage again (its file moved), not Engine's");
        std::cout << "case 3 (the pdb sequence moved: \"cues\" after refreshSyncNeeded) OK\n";

        // The player's counter levelled (what any save with both catalogs
        // does): syncNeeded goes quiet on the next refresh, the
        // baseline-based answer stays.
        {
            std::string error;
            const bool marked = seabass::infrastructure::engine::markRekordboxLibraryImported(pathToUtf8(engine), 15133,
                                                                                               &error);
            assert(marked && error.empty());
            (void)marked;
        }
        assert(model.data(model.index(row), DetectedStickListModel::SyncNeededRole).toBool()
               && "data() did not look: the old answer until the request says otherwise");
        controller.refreshSyncNeeded();
        waitForAnswer(model, mountPoint, answers, 4);
        assert(!model.data(model.index(row), DetectedStickListModel::SyncNeededRole).toBool());
        assert(roleText(model, row, DetectedStickListModel::EngineUpdateRole) == QStringLiteral("cues"));
        std::cout << "case 4 (syncNeeded follows the counter through the same request) OK\n";

        // A track rekordbox no longer lists against the record: "library".
        // The baseline is rewritten with one extra track it does not have,
        // as though rekordbox had dropped it since.
        {
            std::string error;
            auto baseline = seabass::infrastructure::local::readRekordboxBaseline(root, &error);
            assert(baseline && error.empty());
            seabass::domain::BaselineTrack gone;
            gone.pathKey = "contents/removed since.mp3";
            gone.stickRelativePath = "Contents/Removed Since.mp3";
            baseline->tracks.push_back(gone);
            const bool written = seabass::infrastructure::local::writeRekordboxBaseline(
                root, *baseline, [](const std::string &) {}, &error);
            assert(written && error.empty());
            (void)written;
        }
        controller.refreshSyncNeeded();
        waitForAnswer(model, mountPoint, answers, 5);
        assert(roleText(model, row, DetectedStickListModel::EngineUpdateRole) == QStringLiteral("library"));
        std::cout << "case 5 (a track gone from rekordbox since the record: \"library\") OK\n";

        // export.pdb rewritten outside Seabass while the stick stays
        // listed (rekordbox exported onto it): no save, no replug, no
        // refreshSyncNeeded(). Untouched, the stat timer asks nothing.
        {
            QElapsedTimer quiet;
            quiet.start();
            while (quiet.elapsed() < MediaController::CatalogStampIntervalMs + 1500) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            assert(answers == 5 && "catalogs untouched: the timer restarts nothing");
        }
        assert(!model.data(model.index(row), DetectedStickListModel::SyncNeededRole).toBool());
        writePdbSequence(pioneer, 15134);
        waitForAnswer(model, mountPoint, answers, 6);
        assert(model.data(model.index(row), DetectedStickListModel::SyncNeededRole).toBool()
               && "15134 against 15133: the timer saw export.pdb change and asked again");
        std::cout << "case 6 (export.pdb changed outside Seabass: syncNeeded flips on the stat timer) OK\n";

        // Closing the folder forgets the answer with the row.
        controller.closeFolder(pathToQString(root));
        assert(rowOf(model, mountPoint) < 0);
    }
    LibraryCatalogCache::setInstanceForTesting(nullptr);
    fs::remove_all(scratch, ec);
    std::cout << "engine_update_role_test OK\n";
    return 0;
}
