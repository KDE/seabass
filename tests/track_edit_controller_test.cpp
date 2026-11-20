// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// TrackEditController (#105) with the real edit session, on a copy of the
// fixture:
//
//   track_edit_controller_test <tests/fixtures/one_stick_two_catalogs>
//
// 1. Nothing is staged before the track's lookup has answered: no read of
//    the stick on the GUI thread stands in for it.
// 2. A save tells the page first (saveStarting), so an edit still open in
//    a field can be staged before anything is written.
// 3. A change the save reached and skipped is taken off the list but not
//    reported as saved: the pane must not show a value never written.

#include <QCoreApplication>
#include <QSignalSpy>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include "application/use_cases/scan_library.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/qt_path.hpp"
#include "gui/track_edit_controller.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include "fixture_copy.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

class SkippingChange : public PendingChange
{
public:
    explicit SkippingChange(QString id) : m_id(std::move(id)) {}
    QString id() const override { return m_id; }
    QString description() const override { return QStringLiteral("skips"); }
    QString unit() const override { return QStringLiteral("tracks"); }
    QStringList formatsTouched() const override { return {}; }
    ChangeOutcome apply(SaveContext &) override { return ChangeOutcome::skip(); }

private:
    QString m_id;
};

// Waits for `done` by the signal that can make it true, never by sleeping:
// each wait returns as soon as `signal` fires, and gives up after a minute.
template <typename Object, typename Signal>
bool waitFor(Object *object, Signal signal, const std::function<bool()> &done)
{
    QSignalSpy spy(object, signal);
    for (int i = 0; i < 12 && !done(); ++i) {
        spy.wait(5000);
    }
    return done();
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: track_edit_controller_test <tests/fixtures/one_stick_two_catalogs>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path scratch = testing::scratchRoot() / "track_edit_controller_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    const fs::path stick = scratch / "stick";
    std::error_code ec;
    testing::copyPioneerFixture(fixture / "rekordbox", stick / "PIONEER", ec);
    assert(!ec);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive, ec);
    assert(!ec);
    const QString pioneer = pathToQString(stick / "PIONEER");
    const QString engine = pathToQString(stick / "Engine Library");
    infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(stick / "PIONEER"));
    const auto tracks = application::ScanLibrary(reader).execute();
    assert(!tracks.empty());
    const auto &track = tracks.front();
    const QString file = QString::fromStdString(track.filePath);
    const int original = track.rating.value_or(-1);
    const int stars = original == 4 ? 2 : 4;

    TrackEditController controller;
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(pioneer));
    assert(session);

    // 1.
    assert(!controller.setRating(pioneer, engine, file, QStringLiteral("T"), original, stars));
    assert(controller.errorMessage().contains(QStringLiteral("still reading")));
    assert(!controller.setComment(pioneer, engine, file, QStringLiteral("T"), QString(), QStringLiteral("x")));
    assert(session->pendingCount() == 0);
    controller.lookUpTrack(pioneer, engine, file);
    assert(waitFor(&controller, &TrackEditController::commentLimitChanged,
                   [&] { return controller.commentLimit().value(QStringLiteral("known")).toBool(); }));
    assert(controller.commentLimit().value(QStringLiteral("editable")).toBool());
    assert(controller.setRating(pioneer, engine, file, QStringLiteral("T"), original, stars));
    assert(session->pendingCount() == 1 && controller.hasStagedRating(file));
    std::cout << "case 1 (nothing staged before the lookup answers) OK\n";

    // 2 and 3. The staged rating's id taken over by a change that skips.
    const QString id = trackFieldChangeId(TrackField::Rating, file.toStdString());
    assert(session->stage(std::make_unique<SkippingChange>(id)));
    QSignalSpy starting(&controller, &TrackEditController::saveStarting);
    QSignalSpy saved(&controller, &TrackEditController::fieldSaved);
    QSignalSpy tracksSaved(&controller, &TrackEditController::tracksSaved);
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    session->save();
    assert(starting.count() == 1 && "the page hears of the save before it starts");
    assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
    assert(session->lastSkippedIds().contains(id));
    assert(saved.isEmpty() && "a skipped change is not reported as saved");
    assert(tracksSaved.isEmpty());
    assert(!controller.hasStagedRating(file) && "and it is off the list");
    std::cout << "case 2 (a save tells the page first) OK\n";
    std::cout << "case 3 (a skipped change is not shown as saved) OK\n";

    // 4. After a save the track still shown can be edited again: the
    //    index is read again (known goes false, then true) and the next
    //    star stages, without the track being shown again.
    assert(waitFor(&controller, &TrackEditController::commentLimitChanged,
                   [&] { return controller.commentLimit().value(QStringLiteral("known")).toBool(); }));
    assert(controller.setRating(pioneer, engine, file, QStringLiteral("T"), original, 3 == original ? 2 : 3));
    QSignalSpy finishedAgain(session, &LibraryEditSession::saveFinished);
    session->save();
    assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finishedAgain.isEmpty(); }));
    assert(finishedAgain.takeFirst().at(0).toMap().value(QStringLiteral("error")).toString().isEmpty());
    assert(!controller.commentLimit().value(QStringLiteral("known")).toBool()
           && "the index is read again after the save");
    assert(waitFor(&controller, &TrackEditController::commentLimitChanged,
                   [&] { return controller.commentLimit().value(QStringLiteral("known")).toBool(); }));
    assert(controller.setRating(pioneer, engine, file, QStringLiteral("T"), 3 == original ? 2 : 3, 4)
           && "a second edit of the same track stages");
    assert(controller.hasStagedRating(file));
    std::cout << "case 4 (after a save the track shown can be edited again) OK\n";
    registry->discardAll();
    return 0;
}
