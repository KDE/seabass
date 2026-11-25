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

#include <QBuffer>
#include <QCoreApplication>
#include <QColor>
#include <QImage>
#include <QSettings>
#include <QSignalSpy>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_set>

#include "gui/analysis_settings.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/track_import_controller.hpp"
#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/anlz_builder.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
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

// A 16 bit, 44.1 kHz, mono WAV of `seconds` with a kick on every beat at
// `bpm`, the first at 0.25 s: a 60 Hz thump that dies away in 150 ms.
void writeBeatWav(const fs::path &file, int seconds, double bpm)
{
    fs::create_directories(file.parent_path());
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds) * 44100;
    std::vector<std::int16_t> samples(frames, 0);
    const double beat = 60.0 / bpm * 44100.0;
    for (double at = 0.25 * 44100.0; at < frames; at += beat) {
        for (std::uint32_t i = 0; i < 6615 && static_cast<std::uint32_t>(at) + i < frames; ++i) {
            const double t = i / 44100.0;
            samples[static_cast<std::uint32_t>(at) + i] =
                static_cast<std::int16_t>(24000.0 * std::exp(-t * 30.0) * std::sin(2.0 * 3.14159265358979 * 60.0 * t));
        }
    }
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    out << "RIFF";
    u32(36 + frames * 2);
    out << "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(44100);
    u32(88200);
    u16(2);
    u16(16);
    out << "data";
    u32(frames * 2);
    out.write(reinterpret_cast<const char *>(samples.data()), frames * 2);
}

// A WAV like writeWav()'s with an ID3v2.3 tag in a chunk of its own,
// holding `picture` as the front cover.
void writeWavWithCover(const fs::path &file, std::uint32_t frames, std::int16_t mark, const std::string &picture)
{
    writeWav(file, frames, mark);
    const auto be32 = [](std::uint32_t v) {
        return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
    };
    std::string body = std::string(1, '\0') + "image/png" + std::string(1, '\0') + std::string(1, '\3') + std::string(1, '\0')
                       + picture;
    const std::string frame = "APIC" + be32(static_cast<std::uint32_t>(body.size())) + std::string(2, '\0') + body;
    const std::uint32_t n = static_cast<std::uint32_t>(frame.size());
    const std::string synchsafe{static_cast<char>((n >> 21) & 0x7f), static_cast<char>((n >> 14) & 0x7f),
                                static_cast<char>((n >> 7) & 0x7f), static_cast<char>(n & 0x7f)};
    std::string tag = std::string("ID3") + '\3' + '\0' + '\0' + synchsafe + frame;
    if (tag.size() % 2 != 0) {
        tag.push_back('\0');  // a frame of padding; chunks are of even length
    }
    std::string wav;
    {
        std::ifstream in(file, std::ios::binary);
        wav.assign(std::istreambuf_iterator<char>(in), {});
    }
    const std::uint32_t chunk = static_cast<std::uint32_t>(tag.size());
    wav += "id3 ";
    wav.append(reinterpret_cast<const char *>(&chunk), 4);
    wav += tag;
    const std::uint32_t riff = static_cast<std::uint32_t>(wav.size() - 8);
    wav.replace(4, 4, reinterpret_cast<const char *>(&riff), 4);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << wav;
}

template <typename Reader>
std::string coverOf(Reader &&reader, const std::string &fileName)
{
    for (const auto &track : reader.readAll()) {
        if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == fileName) {
            return track.artworkPath;
        }
    }
    return "no such track";
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
        already += line.toMap().value("why").toString().contains(QLatin1String("is in \"Dropped\" already")) ? 1 : 0;
    }
    assert(already == 3 && result.value("addedToPlaylist").toInt() == 0);
    std::cout << "case 4 (the same folder again on its playlist: nothing staged, each file named as in it already) OK\n";

    // 5.
    writeWav(computer / "d four.wav", 4 * 44100, 4);
    const QVariantList one{pathToQString(computer / "d four.wav")};
    assert(offer(one, QStringLiteral("Dropped"), false, 0));
    assert(controller.stagedCount() == 1 && session->pendingCount() == 3 && !controller.stagedPlaylistIsNew());
    // 5b. One more for the same playlist while that one waits: the two
    //     are one batch, in the order offered, with the first's place.
    writeWav(computer / "e five.wav", 5 * 44100, 5);
    assert(offer(QVariantList{pathToQString(computer / "e five.wav")}, QStringLiteral("Dropped"), false));
    assert(controller.errorMessage().isEmpty());
    assert(controller.stagedCount() == 2 && session->pendingCount() == 2 * 2 + 1);
    assert(controller.trackCountChange(QStringLiteral("Dropped")) == 2);
    // The same file once more joins nothing new.
    assert(offer(QVariantList{pathToQString(computer / "e five.wav")}, QStringLiteral("Dropped"), false));
    assert(controller.stagedCount() == 2 && session->pendingCount() == 5);
    // A joining offer that is cancelled costs the batch that waits
    // nothing.
    writeWav(computer / "e six.wav", 44100, 6);
    assert(controller.importFiles(pioneer, engine, QVariantList{pathToQString(computer / "e six.wav")},
                                  QStringLiteral("Dropped"), false));
    controller.cancel();
    assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
    assert(controller.stagedCount() == 2 && session->pendingCount() == 5);
    assert(controller.trackCountChange(QStringLiteral("Dropped")) == 2);
    // The batch that waits taken back while more is being added to it:
    // the joined one, which holds its tracks, is not staged in its place.
    assert(controller.importFiles(pioneer, engine, QVariantList{pathToQString(computer / "e six.wav")},
                                  QStringLiteral("Dropped"), false));
    controller.dropStaged();
    assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
    assert(controller.stagedCount() == 0 && session->pendingCount() == 0);
    assert(controller.errorMessage().contains(QLatin1String("Offer the new files again")));
    // And while one is prepared, the possible duplicates of the last
    // are not asked for at the cost of what waits.
    assert(offer(one, QStringLiteral("Dropped"), false, 0));
    assert(offer(QVariantList{pathToQString(computer / "e five.wav")}, QStringLiteral("Dropped"), false));
    assert(controller.stagedCount() == 2 && session->pendingCount() == 5);
    // Into another playlist it has to wait.
    assert(!controller.importFiles(pioneer, engine, one, QString(), false));
    assert(controller.errorMessage().contains(QLatin1String("another playlist")) && session->pendingCount() == 5);
    std::cout << "case 5b (a second offer for the playlist of the batch that waits joins it; one for elsewhere waits) OK\n";
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

    // 7b. A row that names an analysis file is longer: the title that
    //     just fits without one does not fit with one, and staging asks
    //     with it when the tracks will be analysed.
    {
        domain::ImportedTrack track;
        track.stickPath = "/Contents/A/B/long.wav";
        track.date = "2026-11-25";
        track.sampleRateHz = 44100;
        const QStringList pdbOnly{QStringLiteral("rekordbox")};
        std::size_t fits = 0;
        for (std::size_t length = 1; length < 5000; ++length) {
            track.title.assign(length, 'x');
            if (!ImportTrackChange::rowRefusal(track, pdbOnly, 1000).empty()) {
                break;
            }
            fits = length;
        }
        assert(fits > 100 && fits < 4999);
        track.title.assign(fits, 'x');
        assert(ImportTrackChange::rowRefusal(track, pdbOnly, 1000).empty());
        assert(!ImportTrackChange::rowRefusal(track, pdbOnly, 1000, true).empty());
        std::cout << "case 7b (a row with an analysis file's name is checked at its real length) OK\n";
    }

    // 8. Files the stick already has, dropped on a playlist with a new
    //    one: none is copied again, all are in the playlist in the order
    //    offered, in every library.
    controller.dropStaged();
    assert(session->pendingCount() == 0);
    const QVariantList mixed{pathToQString(computer / "d four.wav"), QUrl::fromLocalFile(pathToQString(computer / "Crate"))};
    assert(offer(mixed, QStringLiteral("Mixed"), true));
    result = controller.lastResult();
    assert(result.value("staged").toInt() == 1 && result.value("addedToPlaylist").toInt() == 3);
    assert(result.value("notImported").toList().isEmpty());
    assert(session->pendingCount() == 1 + 2 + 3 && controller.trackCountChange(QStringLiteral("Mixed")) == 4);
    const auto audioFiles = [&] {
        int count = 0;
        for (const auto &entry : fs::recursive_directory_iterator(stick.root / "Contents")) {
            count += entry.is_regular_file() ? 1 : 0;
        }
        return count;
    };
    const int filesBefore = audioFiles();
    finished.clear();
    session->save();
    assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
    if (session->lastSummary().value("written").toInt() != 6) {
        std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
    }
    assert(session->lastSummary().value("written").toInt() == 6 && session->pendingCount() == 0);
    assert(audioFiles() == filesBefore + 1 && controller.trackCountChange(QStringLiteral("Mixed")) == 0);
    const std::vector<std::string> mixedOrder{"d four.wav", "a one.wav", "B two.wav", "c three.wav"};
    assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "Mixed") == mixedOrder);
    assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "Mixed") == mixedOrder);
    assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), "Mixed") == mixedOrder);
    // The first playlist still has its three.
    assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "Dropped") == expected);
    std::cout << "case 8 (one new file and three the stick has, on a new playlist: one copied, four listed in order in "
                 "all three libraries) OK\n";

    // 9. A file with a picture in its tags: the cover in all three
    //    libraries, as rekordbox's two squares under the ids of the rows
    //    made for it and as Engine's own; a file without one has none.
    //    Undo takes the pictures away with the track.
    {
        QImage drawn(300, 200, QImage::Format_RGB32);
        drawn.fill(QColor(200, 40, 40));
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 50; ++x) {
                drawn.setPixelColor(x, y, QColor(20, 20, 220));  // the left edge, cut away by the square
            }
        }
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        assert(drawn.save(&buffer, "PNG"));
        writeWavWithCover(computer / "f covered.wav", 44100, 6, png.toStdString());
        writeWav(computer / "g bare.wav", 44100, 7);
        const QVariantList two{pathToQString(computer / "f covered.wav"), pathToQString(computer / "g bare.wav")};
        assert(offer(two, QStringLiteral("Mixed"), false));
        assert(controller.lastResult().value("staged").toInt() == 2);
        const auto pictures = [&] {
            std::vector<std::string> names;
            std::error_code ec;
            for (const auto &entry : fs::recursive_directory_iterator(stick.pioneer / "Artwork", ec)) {
                if (entry.is_regular_file()) {
                    names.push_back(pathToUtf8(entry.path().filename()));
                }
            }
            std::sort(names.begin(), names.end());
            return names;
        };
        const std::vector<std::string> picturesBefore = pictures();
        const auto enginePictures = [&] {
            std::vector<std::string> files;
            std::error_code ec;
            for (const auto &entry : fs::recursive_directory_iterator(pathFromUtf8(stick.engineUtf8()) / "Artwork", ec)) {
                if (entry.is_regular_file()) {
                    files.push_back(pathToUtf8(entry.path()));
                }
            }
            return files;
        };
        const std::vector<std::string> enginePicturesBefore = enginePictures();
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        if (!session->lastSummary().value("error").toString().isEmpty()) {
            std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
        }
        assert(session->lastSummary().value("written").toInt() == 4);
        const std::string a = coverOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "f covered.wav");
        const std::string b = coverOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "f covered.wav");
        // Engine's: the one file new under its Artwork folder.
        std::string e;
        for (const std::string &file : enginePictures()) {
            if (std::find(enginePicturesBefore.begin(), enginePicturesBefore.end(), file) == enginePicturesBefore.end()) {
                assert(e.empty() && "one cover for the one track that has one");
                e = file;
            }
        }
        assert(!e.empty() && "Engine keeps the cover as a file in this library");
        const auto small = [](const std::string &file) { return QImage(QString::fromStdString(file)); };
        const auto medium = [](const std::string &file) {
            return QImage(QString::fromStdString(file.substr(0, file.size() - 4) + "_m.jpg"));
        };
        const auto nameOf = [](const std::string &file) { return pathToUtf8(pathFromUtf8(file).filename()); };
        // The rows name files of their own id, each library its letter.
        assert(nameOf(a).front() == 'a' && nameOf(b).front() == 'b' && a.find("/PIONEER/Artwork/0") != std::string::npos);
        for (const std::string &file : {a, b}) {
            assert(small(file).size() == QSize(80, 80) || medium(file).size() == QSize(80, 80));
            const std::string smallFile = nameOf(file).find("_m") == std::string::npos ? file : file.substr(0, file.size() - 6) + ".jpg";
            assert(small(smallFile).size() == QSize(80, 80) && medium(smallFile).size() == QSize(240, 240));
            // The middle square: red to its left edge, the blue strip gone.
            const QColor edge = medium(smallFile).pixelColor(2, 120);
            assert(edge.red() > 150 && edge.blue() < 100);
        }
        const QImage engines(QString::fromStdString(e));
        assert(engines.size() == QSize(200, 200) && engines.pixelColor(2, 100).red() > 150);
        assert(coverOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "g bare.wav").empty());
        assert(coverOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "g bare.wav").empty());
        assert(pictures().size() == picturesBefore.size() + 4);

        finished.clear();
        session->undoLastSave();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        for (const std::string &name : pictures()) {
            if (std::find(picturesBefore.begin(), picturesBefore.end(), name) == picturesBefore.end()) {
                std::cerr << "left after Undo: " << name << std::endl;
            }
        }
        if (fs::exists(pathFromUtf8(e))) {
            std::cerr << "left after Undo: " << e << std::endl;
        }
        assert(pictures() == picturesBefore && !fs::exists(pathFromUtf8(e)));
        assert(coverOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "f covered.wav") == "no such track");
        std::cout << "case 9 (a picture in the tags: 80 and 240 pixel squares for both rekordbox libraries, Engine's own "
                     "cover, none for a file without; Undo takes them away) OK\n";
    }

    // 9b. Two files with a picture each in one save: each its own rows
    //     and files, each its own picture.
    {
        const auto pngOf = [](const QColor &colour) {
            QImage drawn(120, 120, QImage::Format_RGB32);
            drawn.fill(colour);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            assert(drawn.save(&buffer, "PNG"));
            return png.toStdString();
        };
        writeWavWithCover(computer / "i red.wav", 44100, 8, pngOf(QColor(220, 30, 30)));
        writeWavWithCover(computer / "j green.wav", 44100, 9, pngOf(QColor(30, 200, 30)));
        assert(offer(QVariantList{pathToQString(computer / "i red.wav"), pathToQString(computer / "j green.wav")}, QString(),
                     false));
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 2);
        const auto colourOf = [](const std::string &file) {
            return QImage(QString::fromStdString(file.substr(0, file.size() - 4) + "_m.jpg")).pixelColor(120, 120);
        };
        for (const bool one : {false, true}) {
            std::string red, green;
            if (one) {
                infrastructure::onelibrary::OneLibraryReader reader(pathToUtf8(stick.pioneer));
                red = coverOf(reader, "i red.wav");
                green = coverOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), "j green.wav");
            } else {
                red = coverOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "i red.wav");
                green = coverOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), "j green.wav");
            }
            assert(!red.empty() && !green.empty() && red != green);
            assert(colourOf(red).red() > 150 && colourOf(red).green() < 100);
            assert(colourOf(green).green() > 150 && colourOf(green).red() < 100);
        }
        finished.clear();
        session->undoLastSave();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        std::cout << "case 9b (two covers in one save: two artwork rows and two pictures in each rekordbox library) OK\n";
    }

    // 10. Seabass's own analysis: a 30 s track with a kick at 125 BPM
    //     gets a beat grid and waveforms in every library, the rows the
    //     grid's tempo, and new analysis files that Undo takes away.
    {
        writeBeatWav(computer / "h beat.wav", 30, 125.0);
        assert(offer(QVariantList{pathToQString(computer / "h beat.wav")}, QString(), false));
        result = controller.lastResult();
        std::cerr << "  analysed " << result.value("analysed").toInt() << ", with grid " << result.value("withGrid").toInt()
                  << std::endl;
        assert(result.value("staged").toInt() == 1 && result.value("analysed").toInt() == 1);
        assert(result.value("withGrid").toInt() == 1);
        const auto analysisFiles = [&] {
            std::vector<std::string> files;
            std::error_code ec;
            for (const auto &entry : fs::recursive_directory_iterator(stick.pioneer / "USBANLZ", ec)) {
                if (entry.is_regular_file()) {
                    files.push_back(pathToUtf8(entry.path()));
                }
            }
            std::sort(files.begin(), files.end());
            return files;
        };
        // The folder the track's analysis would go to first is taken by
        // a file nothing names: left as it is, the track goes elsewhere.
        std::unordered_set<std::string> inUse;
        for (const std::string &file : analysisFiles()) {
            inUse.insert("/PIONEER/" + pathToGenericUtf8(fs::relative(pathFromUtf8(file), stick.pioneer)));
        }
        const std::string first =
            infrastructure::rekordbox::newAnlzPathFor("/Contents/UnknownArtist/UnknownAlbum/h beat.wav", inUse);
        const fs::path planted = stick.root / pathFromUtf8(first.substr(1, first.size() - 4) + "EXT");
        fs::create_directories(planted.parent_path());
        std::ofstream(planted, std::ios::binary) << "somebody else's";
        const std::vector<std::string> analysisBefore = analysisFiles();
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        if (!session->lastSummary().value("error").toString().isEmpty()) {
            std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
        }
        assert(session->lastSummary().value("written").toInt() == 1);
        assert(analysisFiles().size() == analysisBefore.size() + 3);
        {
            std::ifstream in(planted, std::ios::binary);
            const std::string kept((std::istreambuf_iterator<char>(in)), {});
            assert(kept == "somebody else's" && fs::is_regular_file(planted));
            assert(!fs::exists(planted.parent_path() / "ANLZ0000.DAT") && "nothing was written beside it");
        }

        const auto rowOf = [](auto &&reader) {
            for (const auto &track : reader.readAll()) {
                if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == "h beat.wav") {
                    return track;
                }
            }
            assert(false && "the track is in the library");
            return domain::Track{};
        };
        // Beats 480 ms apart, the first on the first kick, to the end.
        const auto checkGrid = [](const std::vector<domain::Beat> &beats, const char *library) {
            std::cerr << "  " << library << ": " << beats.size() << " beats, first at "
                      << (beats.empty() ? -1.0 : beats.front().timeMs) << " ms" << std::endl;
            assert(beats.size() >= 55 && beats.size() <= 64);
            for (std::size_t i = 1; i < beats.size(); ++i) {
                assert(std::abs(beats[i].timeMs - beats[i - 1].timeMs - 480.0) < 2.0);
            }
            // On a kick: 250 ms and every 480 after it.
            const double phase = std::fmod(beats.front().timeMs - 250.0 + 4800.0, 480.0);
            assert(std::min(phase, 480.0 - phase) < 12.0);
        };
        const domain::Track rekordboxRow = rowOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)));
        const domain::Track oneRow = rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)));
        const domain::Track engineRow = rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()));
        std::cerr << "  bpm " << rekordboxRow.bpm << " | " << oneRow.bpm << " | " << engineRow.bpm << std::endl;
        assert(std::abs(rekordboxRow.bpm - 125.0) < 0.3 && std::abs(oneRow.bpm - 125.0) < 0.3
               && std::abs(engineRow.bpm - 125.0) < 0.3);
        checkGrid(infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), rekordboxRow.sourceId), "rekordbox");
        checkGrid(infrastructure::engine::readBeatGrid(stick.engineUtf8(), engineRow.sourceId), "Engine");
        const auto sections = infrastructure::rekordbox::readWaveformSections(pathToUtf8(stick.pioneer), rekordboxRow.sourceId);
        assert(sections.threeBandPreview && sections.colourPreview && sections.scrollEntries
               && *sections.scrollEntries == 30 * 150);
        // A kick is bass: the low band of the three-band preview is its
        // loudest.
        {
            long low = 0, mid = 0, high = 0;
            const std::string &body = *sections.threeBandPreview;
            for (std::size_t i = 0; i + 2 < body.size(); i += 3) {
                low += static_cast<unsigned char>(body[i]);
                mid += static_cast<unsigned char>(body[i + 1]);
                high += static_cast<unsigned char>(body[i + 2]);
            }
            std::cerr << "  preview low/mid/high " << low << " " << mid << " " << high << std::endl;
            assert(low > 0 && low > mid && low > high);
        }
        finished.clear();
        session->undoLastSave();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(analysisFiles() == analysisBefore);
        std::cout << "case 10 (a 125 BPM click track: analysed, the grid on the kicks in rekordbox's files and in Engine, "
                     "the tempo in all three rows, a bass-heavy preview; Undo takes the analysis files away) OK\n";
    }

    // 11. An MP3: rekordbox counts the decoder's lead-in as audio, so
    //     what the decoder hears at 1.5 s is later in rekordbox's files,
    //     by the file's own offset. The waveform is written there (and a
    //     grid would be, with it). Three seconds of two clicks have no
    //     grid, so the row keeps no tempo and Engine is left to the
    //     player.
    {
        const fs::path click = fixture.parent_path() / "mp3-click" / "click-D-lame-44100.mp3";
        assert(fs::is_regular_file(click));
        fs::copy_file(click, computer / "k click.mp3");
        const auto leadIn = infrastructure::audio::readMp3LeadIn(pathToUtf8(computer / "k click.mp3"));
        assert(leadIn.has_value());
        const double offsetMs = domain::engineToRekordboxOffsetMs(*leadIn);
        std::cerr << "  the MP3's offset: " << offsetMs << " ms" << std::endl;
        assert(offsetMs > 10.0 && "an offset of several waveform entries, or this case shows nothing");
        assert(offer(QVariantList{pathToQString(computer / "k click.mp3")}, QString(), false));
        result = controller.lastResult();
        assert(result.value("staged").toInt() == 1 && result.value("analysed").toInt() == 1
               && result.value("withGrid").toInt() == 0);
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 1);
        std::string id;
        for (const auto &track : infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll()) {
            if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == "k click.mp3") {
                id = track.sourceId;
            }
        }
        assert(!id.empty());
        const domain::DetailWaveform detail = infrastructure::rekordbox::readDetailWaveform(pathToUtf8(stick.pioneer), id);
        assert(detail.columns.size() > 400);
        std::size_t loudest = 0;
        const auto level = [](const domain::WaveformColumn &c) { return c.low + c.mid + c.high; };
        for (std::size_t i = 1; i < detail.columns.size(); ++i) {
            if (level(detail.columns[i]) > level(detail.columns[loudest])) {
                loudest = i;
            }
        }
        const long expected = std::lround((1500.0 + offsetMs) * 150.0 / 1000.0);
        std::cerr << "  the click is in entry " << loudest << ", expected " << expected << " (225 as decoded)" << std::endl;
        assert(std::labs(static_cast<long>(loudest) - expected) <= 1);
        assert(infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), id).empty());
        finished.clear();
        session->undoLastSave();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        std::cout << "case 11 (an MP3: its waveform on rekordbox's timeline, the file's own offset later than decoded) OK\n";
    }

    // 12. The tempo range of Preferences decides between a tempo and its
    //     double: a kick at 80 BPM is 80 in the default range and 160
    //     once the range is 88 to 175, read when the import starts.
    {
        // Another length each: the same bytes twice would be one track.
        const auto tempoOf = [&](const char *name, int seconds) {
            writeBeatWav(computer / name, seconds, 80.0);
            assert(offer(QVariantList{pathToQString(computer / name)}, QString(), false));
            assert(controller.lastResult().value("withGrid").toInt() == 1);
            finished.clear();
            session->save();
            assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
            assert(session->lastSummary().value("written").toInt() == 1);
            for (const auto &track : infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll()) {
                if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == name) {
                    return track.bpm;
                }
            }
            return 0.0;
        };
        const double asDefault = tempoOf("l slow.wav", 30);
        {
            QSettings settings = openSeabassSettings();
            settings.setValue(QLatin1String(AnalysisTempoRangeKey), QStringLiteral("88-175"));
            settings.sync();
        }
        const double inRange = tempoOf("m slow.wav", 31);
        std::cerr << "  80 BPM kicks: " << asDefault << " by default, " << inRange << " in 88 to 175" << std::endl;
        assert(std::abs(asDefault - 80.0) < 0.3 && std::abs(inRange - 160.0) < 0.6);
        std::cout << "case 12 (the tempo range of Preferences: 80 BPM by default, 160 in 88 to 175) OK\n";
    }

    // 13. Several files at once, each analysed in a thread of its own:
    //     every track gets its own tempo, not a neighbour's.
    {
        const std::vector<std::pair<std::string, double>> tempos{
            {"n 100.wav", 100.0}, {"n 110.wav", 110.0}, {"n 120.wav", 120.0}, {"n 130.wav", 130.0}, {"n 140.wav", 140.0}};
        QVariantList files;
        int seconds = 32;
        for (const auto &[name, bpm] : tempos) {
            writeBeatWav(computer / name, seconds++, bpm);
            files.push_back(pathToQString(computer / name));
        }
        assert(offer(files, QString(), false));
        assert(controller.lastResult().value("withGrid").toInt() == 5);
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 5);
        std::size_t found = 0;
        for (const auto &track : infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll()) {
            for (const auto &[name, bpm] : tempos) {
                if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == name) {
                    assert(std::abs(track.bpm - bpm) < 0.3);
                    ++found;
                }
            }
        }
        assert(found == tempos.size());
        std::cout << "case 13 (five files analysed at once: each track its own tempo) OK\n";
    }

    std::cout << "track_import_controller_test: all cases passed\n";
    return 0;
}
