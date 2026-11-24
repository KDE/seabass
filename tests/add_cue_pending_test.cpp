// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// AddCueController's staged cues name their track by its library and its
// id together, with the real edit session:
//
// 1. The same id and slot on two sticks are two cues: staging the second
//    does not replace the first, in the controller's list or in the
//    session. The id alone would not do: each stick numbers its tracks
//    from its own export.pdb.
// 2. pendingCuesFor() hands each track only its own: not another stick's
//    cue on the same id, not another format's on the same stick (Engine's
//    786 and export.pdb's 786 are different tracks).
//
// No catalog is read: staging writes nothing, and the cue is checked
// against the stick only when a save applies it.

#include <QCoreApplication>
#include <QVariantMap>

#include <cassert>
#include <filesystem>
#include <iostream>

#include "gui/add_cue_controller.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path scratch = testing::scratchRoot() / "add_cue_pending_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    // Two sticks' PIONEER folders. Whether they get one edit session or
    // two here, the sessions together must keep both cues.
    fs::create_directories(scratch / "A" / "PIONEER" / "rekordbox");
    fs::create_directories(scratch / "B" / "PIONEER" / "rekordbox");
    const QString stickA = pathToQString(scratch / "A" / "PIONEER");
    const QString stickB = pathToQString(scratch / "B" / "PIONEER");
    const QString engineA = pathToQString(scratch / "A" / "Engine Library");
    const QString rekordbox = QStringLiteral("rekordbox");
    const QString id = QStringLiteral("786");

    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(stickA));
    assert(session);

    // 1.
    AddCueController controller;
    controller.addCue(rekordbox, stickA, id, 10000, QStringLiteral("hot"), 1, QString(), QString(), false, 0,
                      QStringLiteral("On A"));
    assert(controller.errorMessage().isEmpty());
    controller.addCue(rekordbox, stickB, id, 20000, QStringLiteral("hot"), 1, QString(), QString(), false, 0,
                      QStringLiteral("On B"));
    assert(controller.errorMessage().isEmpty());
    const QVariantList onA = controller.pendingCuesFor(rekordbox, stickA, id);
    const QVariantList onB = controller.pendingCuesFor(rekordbox, stickB, id);
    LibraryEditSession *sessionB = registry->sessionFor(registry->libraryIdForPath(stickB));
    assert(sessionB);
    const int staged = sessionB == session ? session->pendingCount()
                                           : session->pendingCount() + sessionB->pendingCount();
    std::cerr << "stick A " << onA.size() << ", stick B " << onB.size() << ", staged " << staged << "\n";
    assert(onA.size() == 1 && onA.front().toMap().value(QStringLiteral("positionMs")).toDouble() == 10000);
    assert(onB.size() == 1 && onB.front().toMap().value(QStringLiteral("positionMs")).toDouble() == 20000);
    assert(staged == 2);
    std::cout << "case 1 (one id and slot on two sticks: two cues) OK\n";

    // 2.
    assert(controller.pendingCuesFor(QStringLiteral("engine"), engineA, id).isEmpty());
    assert(controller.pendingCuesFor(QStringLiteral("engine"), stickA, id).isEmpty());
    assert(controller.pendingCuesFor(rekordbox, stickA, QStringLiteral("870")).isEmpty());
    controller.addCue(QStringLiteral("engine"), engineA, id, 30000, QStringLiteral("hot"), 1, QString(), QString(),
                      false, 0, QStringLiteral("Engine's"));
    assert(controller.errorMessage().isEmpty());
    const QVariantList onEngine = controller.pendingCuesFor(QStringLiteral("engine"), engineA, id);
    assert(onEngine.size() == 1 && onEngine.front().toMap().value(QStringLiteral("positionMs")).toDouble() == 30000);
    assert(controller.pendingCuesFor(rekordbox, stickA, id).size() == 1);
    std::cout << "case 2 (each track only its own staged cues) OK\n";
    return 0;
}
