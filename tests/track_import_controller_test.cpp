// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// TrackImportController with the real session, tag reader and stick
// reads: files and a folder offered as a drop offers them, staged, saved
// by the session, and found in all three libraries.
//
// 1. A folder of three WAV files and a text file, into a new playlist:
//    three staged with the playlist, the text file named with its reason.
//    The folder's files come as a file manager lists them: "a", "B",
//    then "Deeper/c", not capitals first.
// 2. A second offer while the first waits is refused.
// 3. The session's save: files on the stick, the playlist in order in
//    all three libraries, the page told to rescan.
// 4. The same folder again: nothing staged, each file named as on the
//    stick already.
// 5. One file into that playlist at its top, and taken back: nothing
//    staged any more.
// 6. An offer while other edits wait is refused.
// 7. A playlist no library has, not asked to be made: refused, nothing
//    staged.
//
// The audio is made here: PCM WAV files of silence with one differing
// sample, so that no two are the same bytes or the same length.

#include <QCoreApplication>
#include <QSignalSpy>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/track_import_controller.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

class SkippingChange : public PendingChange
{
public:
    QString id() const override { return QStringLiteral("addcue:test-skip"); }
    QString owner() const override { return QStringLiteral("addcue"); }
    QString description() const override { return QStringLiteral("skips"); }
    QString unit() const override { return QStringLiteral("tracks"); }
    QStringList formatsTouched() const override { return {}; }
    ChangeOutcome apply(SaveContext &) override { return ChangeOutcome::skip(); }
};

// A 16 bit, 44.1 kHz, mono WAV of `frames` samples of silence, `mark`
// in its first.
void writeWav(const fs::path &file, std::uint32_t frames, std::int16_t mark)
{
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    const std::uint32_t bytes = frames * 2;
    out << "RIFF";
    u32(36 + bytes);
    out << "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(44100);
    u32(88200);
    u16(2);
    u16(16);
    out << "data";
    u32(bytes);
    std::vector<std::int16_t> samples(frames, 0);
    samples[0] = mark;
    out.write(reinterpret_cast<const char *>(samples.data()), bytes);
}

template <typename Reader>
std::vector<std::string> playlistOf(Reader &&reader, const std::string &name)
{
    std::map<int, std::string> byPosition;
    for (const auto &track : reader.readAll()) {
        for (const auto &membership : track.playlists) {
            if (membership.name == name) {
                assert(byPosition.emplace(membership.position, track.filePath).second && "one track per place");
            }
        }
    }
    std::vector<std::string> ordered;
    for (const auto &[position, file] : byPosition) {
        ordered.push_back(pathToUtf8(pathFromUtf8(file).filename()));
    }
    return ordered;
}

template <typename Object, typename Signal>
bool waitFor(Object *object, Signal signal, const std::function<bool()> &done)
{
    QSignalSpy spy(object, signal);
    for (int i = 0; i < 24 && !done(); ++i) {
        spy.wait(5000);
    }
    return done();
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: track_import_controller_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "track_import_controller_home");
    testing::sandboxSettings(testing::scratchRoot() / "track_import_controller_config");
    QCoreApplication app(argc, argv);
    const fs::path computer = testing::scratchRoot() / "track_import_controller_computer";
    fs::remove_all(computer);
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "track_import_controller_stick");
    testing::levelImportCounter(stick);
    const QString pioneer = stick.pioneerPath();
    const QString engine = stick.enginePath();

    writeWav(computer / "Crate" / "a one.wav", 44100, 1);
    writeWav(computer / "Crate" / "B two.wav", 2 * 44100, 2);
    writeWav(computer / "Crate" / "Deeper" / "c three.wav", 3 * 44100, 3);
    std::ofstream(computer / "notes.txt") << "not audio";
    const QVariantList dropped{QUrl::fromLocalFile(pathToQString(computer / "Crate")),
                               QUrl::fromLocalFile(pathToQString(computer / "notes.txt"))};

    TrackImportController controller;
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(pioneer));
    assert(session);
    QSignalSpy results(&controller, &TrackImportController::resultReady);
    QSignalSpy imported(&controller, &TrackImportController::tracksImported);
    const auto offer = [&](const QVariantList &files, const QString &playlist, bool create, int atIndex = -1) {
        const int before = results.count();
        const QString errorBefore = controller.errorMessage();
        if (!controller.importFiles(pioneer, engine, files, playlist, create, atIndex)) {
            return false;
        }
        assert(controller.busy());
        assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
        return results.count() > before;
    };

    // 1.
    assert(offer(dropped, QStringLiteral("Dropped"), true));
    if (!controller.errorMessage().isEmpty()) {
        std::cerr << controller.errorMessage().toStdString() << "\n";
    }
    QVariantMap result = controller.lastResult();
    assert(result.value("staged").toInt() == 3 && controller.stagedCount() == 3);
    assert(result.value("playlist").toString() == QLatin1String("Dropped") && result.value("playlistIsNew").toBool());
    assert(result.value("notImported").toList().size() == 1);
    assert(result.value("notImported").toList()[0].toMap().value("name").toString() == QLatin1String("notes.txt"));
    assert(result.value("notImported").toList()[0].toMap().value("why").toString().contains(QLatin1String("not a format")));
    assert(session->pendingCount() == 1 + 3 * 2);
    assert(controller.trackCountChange(QStringLiteral("Dropped")) == 3 && controller.trackCountChange(QStringLiteral("Other")) == 0);
    assert(controller.stagedPlaylist() == QLatin1String("Dropped") && controller.stagedPlaylistIsNew());
    std::cout << "case 1 (a folder and a text file dropped on a new playlist: three staged, the text file named) OK\n";

    // 2.
    assert(!controller.importFiles(pioneer, engine, dropped, QString(), false));
    assert(controller.errorMessage().contains(QLatin1String("waiting to be saved")));
    assert(session->pendingCount() == 7);
    std::cout << "case 2 (a second offer while one waits: refused) OK\n";

    // 3.
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    session->save();
    assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
    if (session->lastSummary().value("written").toInt() != 7) {
        std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
    }
    assert(session->lastSummary().value("written").toInt() == 7);
    assert(imported.count() == 1 && controller.stagedCount() == 0 && session->pendingCount() == 0);
    assert(controller.trackCountChange(QStringLiteral("Dropped")) == 0);
    const std::vector<std::string> expected{"a one.wav", "B two.wav", "c three.wav"};
    assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "Dropped") == expected);
    assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "Dropped") == expected);
    assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "Dropped") == expected);
    assert(fs::file_size(stick.root / "Contents" / "UnknownArtist" / "UnknownAlbum" / "B two.wav")
           == fs::file_size(computer / "Crate" / "B two.wav"));
    std::cout << "case 3 (saved: the files on the stick, the playlist in order in all three libraries) OK\n";

    // 4.
    assert(offer(dropped, QStringLiteral("Dropped"), false));
    result = controller.lastResult();
    assert(result.value("staged").toInt() == 0 && session->pendingCount() == 0);
    assert(result.value("notImported").toList().size() == 4);
    int already = 0;
    for (const QVariant &line : result.value("notImported").toList()) {
        already += line.toMap().value("why").toString().contains(QLatin1String("on the stick already")) ? 1 : 0;
    }
    assert(already == 3);
    std::cout << "case 4 (the same folder again: nothing staged, each file named as on the stick already) OK\n";

    // 5.
    writeWav(computer / "d four.wav", 4 * 44100, 4);
    const QVariantList one{pathToQString(computer / "d four.wav")};
    assert(offer(one, QStringLiteral("Dropped"), false, 0));
    assert(controller.stagedCount() == 1 && session->pendingCount() == 3 && !controller.stagedPlaylistIsNew());
    controller.dropStaged();
    assert(controller.stagedCount() == 0 && session->pendingCount() == 0);
    std::cout << "case 5 (one file at the top of the playlist, a path and not a url; taken back, nothing is staged) OK\n";

    // 6.
    assert(session->stage(std::make_unique<SkippingChange>()));
    assert(!controller.importFiles(pioneer, engine, one, QString(), false));
    assert(controller.errorMessage().contains(QLatin1String("Save or discard")));
    session->discard();
    assert(session->pendingCount() == 0);
    std::cout << "case 6 (an offer while other edits wait: refused) OK\n";

    // 7.
    assert(!offer(one, QStringLiteral("Never Made"), false));
    assert(controller.errorMessage().contains(QLatin1String("is not in every library")));
    assert(session->pendingCount() == 0 && controller.stagedCount() == 0);
    // And into no playlist at all it goes.
    assert(offer(one, QString(), false));
    assert(controller.stagedCount() == 1 && session->pendingCount() == 1 && controller.stagedPlaylist().isEmpty());
    std::cout << "case 7 (a playlist no library has: refused; no playlist: staged alone) OK\n";

    std::cout << "track_import_controller_test: all cases passed\n";
    return 0;
}
