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
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_set>

#include "gui/analysis_settings.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/changes/analysis_ledger_record.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/async_request.hpp"
#include "gui/edit/changes/reanalyse_track_change.hpp"
#include "gui/seabass_settings.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/track_import_analysis.hpp"
#include "gui/track_import_controller.hpp"
#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/rekordbox/anlz_builder.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "application/path_key.hpp"
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "musicanalyze/version.hpp"

#include <djinterop/engine/v3/track_data_blob.hpp>

#include "engine_change_fixture.hpp"
#include "rekordbox_baseline_fixture.hpp"

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

// A 16 bit, 44.1 kHz, mono WAV of `samples`.
void writeMonoWav(const fs::path &file, const std::vector<std::int16_t> &samples, std::uint32_t rate = 44100)
{
    fs::create_directories(file.parent_path());
    const std::uint32_t frames = static_cast<std::uint32_t>(samples.size());
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    out << "RIFF";
    u32(36 + frames * 2);
    out << "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    out << "data";
    u32(frames * 2);
    out.write(reinterpret_cast<const char *>(samples.data()), frames * 2);
}

// A 16 bit, 44.1 kHz, mono WAV of `seconds` with a kick on every beat at
// `bpm`, the first at 0.25 s: a 60 Hz thump that dies away in 150 ms.
void writeBeatWav(const fs::path &file, int seconds, double bpm)
{
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
    writeMonoWav(file, samples);
}

// A 16 bit, 44.1 kHz, mono WAV of `seconds` with a burst of noise on
// every beat at `bpm`, the first at 0.25 s: a tempo and no pitch at all.
void writeNoiseBeatWav(const fs::path &file, int seconds, double bpm)
{
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds) * 44100;
    std::vector<std::int16_t> samples(frames, 0);
    const double beat = 60.0 / bpm * 44100.0;
    std::uint32_t noise = 12345;
    for (double at = 0.25 * 44100.0; at < frames; at += beat) {
        for (std::uint32_t i = 0; i < 2205 && static_cast<std::uint32_t>(at) + i < frames; ++i) {
            noise = noise * 1664525u + 1013904223u;
            const double t = i / 44100.0;
            samples[static_cast<std::uint32_t>(at) + i] =
                static_cast<std::int16_t>(20000.0 * std::exp(-t * 60.0) * (static_cast<double>(noise >> 16) / 32768.0 - 1.0));
        }
    }
    writeMonoWav(file, samples);
}

// A WAV of `seconds` of the chord A minor (A, C and E over three
// octaves, each with two overtones), struck twice a second: something
// with a key in it.
void writeChordWav(const fs::path &file, int seconds)
{
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds) * 44100;
    std::vector<std::int16_t> samples(frames, 0);
    const double notes[] = {110.0, 130.81, 164.81, 220.0, 261.63, 329.63, 440.0, 523.25, 659.26};
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double t = i / 44100.0;
        const double struck = std::exp(-std::fmod(t, 0.5) * 4.0);
        double sum = 0.0;
        for (const double note : notes) {
            for (int overtone = 1; overtone <= 3; ++overtone) {
                sum += std::sin(2.0 * 3.14159265358979 * note * overtone * t) / overtone;
            }
        }
        samples[i] = static_cast<std::int16_t>(1500.0 * struck * sum);
    }
    writeMonoWav(file, samples);
}

// A WAV of `seconds` with a kick on every beat at `bpm` (as writeBeatWav(),
// the first at `firstBeatSeconds`) over a chord of `notes` (Hz, each with
// two overtones): a tempo and a key to hear. `rate` samples a second.
void writeBeatChordWav(const fs::path &file, int seconds, double bpm, const std::vector<double> &notes,
                       double firstBeatSeconds = 0.25, std::uint32_t rate = 44100)
{
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds) * rate;
    std::vector<double> mix(frames, 0.0);
    const double beat = 60.0 / bpm * rate;
    for (double at = firstBeatSeconds * rate; at < frames; at += beat) {
        for (std::uint32_t i = 0; i < rate * 3 / 20 && static_cast<std::uint32_t>(at) + i < frames; ++i) {
            const double t = i / static_cast<double>(rate);
            mix[static_cast<std::uint32_t>(at) + i] += 20000.0 * std::exp(-t * 30.0) * std::sin(2.0 * 3.14159265358979 * 60.0 * t);
        }
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double t = i / static_cast<double>(rate);
        double sum = 0.0;
        for (const double note : notes) {
            for (int overtone = 1; overtone <= 3; ++overtone) {
                sum += std::sin(2.0 * 3.14159265358979 * note * overtone * t) / overtone;
            }
        }
        mix[i] += 600.0 * sum;
    }
    std::vector<std::int16_t> samples(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        samples[i] = static_cast<std::int16_t>(std::clamp(mix[i], -32000.0, 32000.0));
    }
    writeMonoWav(file, samples, rate);
}

// Adds an ID3v2.3 tag of one frame to a WAV, in a chunk of its own.
void appendId3Frame(const fs::path &file, const std::string &id, const std::string &body)
{
    const auto be32 = [](std::uint32_t v) {
        return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
    };
    const std::string frame = id + be32(static_cast<std::uint32_t>(body.size())) + std::string(2, '\0') + body;
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

// A WAV like writeWav()'s with an ID3v2.3 tag in a chunk of its own,
// holding `picture` as the front cover.
void writeWavWithCover(const fs::path &file, std::uint32_t frames, std::int16_t mark, const std::string &picture)
{
    writeWav(file, frames, mark);
    std::string body = std::string(1, '\0') + "image/png" + std::string(1, '\0') + std::string(1, '\3') + std::string(1, '\0')
                       + picture;
    appendId3Frame(file, "APIC", body);
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

    // 14. The key the analyser hears goes into the rows of all three
    //     libraries where the file's tags name none; a tag's key stays.
    {
        writeChordWav(computer / "o chord.wav", 20);
        writeChordWav(computer / "o tagged.wav", 21);
        appendId3Frame(computer / "o tagged.wav", "TKEY", std::string(1, '\0') + "Dm");
        const auto heard = analyseForImport(pathToUtf8(computer / "o chord.wav"), application::CancellationToken{});
        assert(heard && !heard->key.empty());
        assert(heard->key != "Dm");
        assert(offer(QVariantList{pathToQString(computer / "o chord.wav"), pathToQString(computer / "o tagged.wav")}, QString(),
                     false));
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 2);
        const auto keysOf = [&](auto &&reader) {
            std::map<std::string, std::string> keys;
            for (const auto &track : reader.readAll()) {
                keys[pathToUtf8(pathFromUtf8(track.filePath).filename())] = track.key;
            }
            return keys;
        };
        const auto check = [&](const char *library, const std::map<std::string, std::string> &keys) {
            std::cerr << "  " << library << ": heard " << keys.at("o chord.wav") << ", tagged " << keys.at("o tagged.wav")
                      << std::endl;
            assert(keys.at("o chord.wav") == heard->key);
            assert(keys.at("o tagged.wav") == "Dm");
        };
        check("DeviceLibrary", keysOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer))));
        check("OneLibrary", keysOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer))));
        check("Engine", keysOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8())));
        std::cout << "case 14 (the analyser's key in all three libraries, a tag's key kept) OK\n";
    }


    // 15. Analyse again. Five tracks imported into a playlist and saved,
    //     one given a hot cue; then three files on the stick are replaced
    //     by audio of another tempo and chord (the same size), one by a
    //     longer file, and one is taken away. Analyse again on the
    //     playlist stages one change for each of the three and leaves the
    //     other two as they are, counted (the longer file is not the one
    //     its rows describe); staging again replaces, Take back takes them
    //     all. Saved: the new tempo and grid in all three libraries, the
    //     hot cue still there, every key a row has kept (the analyser's
    //     and a tag's), the analyser's key only in rows that name none and
    //     whose writer can take it, a .2EX that was gone made again. A
    //     second Analyse again is not taken for a grid moved on the
    //     player.
    {
        const std::vector<double> aMinor{110.0, 130.81, 164.81, 220.0, 261.63, 329.63};
        const std::vector<double> eMinor{82.41, 98.0, 123.47, 164.81, 196.0, 246.94};
        writeBeatChordWav(computer / "p plain.wav", 30, 120.0, aMinor);
        writeBeatChordWav(computer / "p tagged.wav", 31, 120.0, aMinor);
        appendId3Frame(computer / "p tagged.wav", "TKEY", std::string(1, '\0') + "Dm");
        writeBeatChordWav(computer / "p gone.wav", 32, 120.0, aMinor);
        writeBeatChordWav(computer / "p bigger.wav", 34, 120.0, aMinor);
        assert(offer(QVariantList{pathToQString(computer / "p plain.wav"), pathToQString(computer / "p tagged.wav"),
                                  pathToQString(computer / "p gone.wav"), pathToQString(computer / "p bigger.wav")},
                     QStringLiteral("Again"), true));
        assert(controller.lastResult().value("withGrid").toInt() == 4);
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 4 + 1 + 4);
        // One more whose rows name no key: imported with a grid and the
        // analysis's key left out, through the import's own worker.
        {
            writeBeatChordWav(computer / "p keyless.wav", 33, 120.0, aMinor);
            ImportRequest request;
            request.offered = {pathToUtf8(computer / "p keyless.wav")};
            request.rekordboxPath = pioneer;
            request.enginePath = engine;
            request.playlist = ImportPlaylist{"Again", false, std::nullopt};
            request.date = "2026-11-26";
            request.analyser = [](const std::string &file, const application::CancellationToken &cancel) {
                auto made = analyseForImport(file, cancel, domain::TempoRange{88.0, 175.0});
                auto keyless = std::make_shared<ImportAnalysis>(*made);
                keyless->key.clear();
                return std::shared_ptr<const ImportAnalysis>(keyless);
            };
            const PreparedImport prepared = prepareTrackImport(request, application::CancellationToken{});
            assert(prepared.refusal.empty() && prepared.withGrid == 1);
            assert(session->stageAll(prepared.staged.changes));
            finished.clear();
            session->save();
            assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
            assert(session->lastSummary().value("written").toInt() == 2);
        }

        const auto rowOf = [](auto &&reader, const std::string &name) {
            for (const auto &track : reader.readAll()) {
                if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == name) {
                    return track;
                }
            }
            assert(false && "the track is in the library");
            return domain::Track{};
        };
        const auto pdbRow = [&](const std::string &name) {
            return rowOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), name);
        };
        const auto oneRow = [&](const std::string &name) {
            return rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), name);
        };
        const auto engineRow = [&](const std::string &name) {
            return rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), name);
        };
        const domain::Track plainBefore = pdbRow("p plain.wav");
        const std::string engineKeyBefore = engineRow("p plain.wav").key;
        assert(!plainBefore.key.empty() && !engineKeyBefore.empty());
        std::cerr << "  keyless after the import: '" << pdbRow("p keyless.wav").key << "' '" << oneRow("p keyless.wav").key
                  << "' '" << engineRow("p keyless.wav").key << "'" << std::endl;
        assert(pdbRow("p keyless.wav").key.empty() && oneRow("p keyless.wav").key.empty()
               && engineRow("p keyless.wav").key.empty());

        // A hot cue through the app's own cue writer.
        assert(session->stage(std::make_unique<AddCueChange>(
            QStringLiteral("rekordbox"), pioneer, QString::fromStdString(plainBefore.sourceId), 1234.0,
            QStringLiteral("hot"), 2, QStringLiteral("#FF0000"), QStringLiteral("drop"), false, 0.0,
            QStringLiteral("p plain"))));
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 1);
        const auto hasTheCue = [](const domain::Track &track) {
            for (const auto &cue : track.cues) {
                if (cue.kind == domain::CuePoint::Kind::Hot && cue.hotCueNumber == 2
                    && std::abs(cue.positionMs - 1234.0) < 1.0) {
                    return true;
                }
            }
            return false;
        };
        assert(hasTheCue(pdbRow("p plain.wav")) && hasTheCue(oneRow("p plain.wav")));

        // The files on the stick, replaced; one gone; one .2EX gone.
        const fs::path onStick = stick.root / "Contents" / "UnknownArtist" / "UnknownAlbum";
        writeBeatChordWav(onStick / "p plain.wav", 30, 140.0, eMinor);
        writeBeatChordWav(onStick / "p tagged.wav", 31, 140.0, eMinor);
        appendId3Frame(onStick / "p tagged.wav", "TKEY", std::string(1, '\0') + "Dm");
        fs::remove(onStick / "p gone.wav");
        writeBeatChordWav(onStick / "p keyless.wav", 33, 140.0, eMinor);
        writeBeatChordWav(onStick / "p bigger.wav", 35, 140.0, eMinor);
        const std::string plainDat = plainBefore.analysisFile;
        assert(!plainDat.empty());
        const fs::path plainTwoEx = stick.root / pathFromUtf8(plainDat.substr(1, plainDat.size() - 4) + "2EX");
        assert(fs::is_regular_file(plainTwoEx));
        fs::remove(plainTwoEx);
        const auto heard = analyseForImport(pathToUtf8(onStick / "p plain.wav"), application::CancellationToken{});
        assert(heard && !heard->key.empty() && heard->key != plainBefore.key && heard->key != "Dm");
        std::cerr << "  key before " << plainBefore.key << ", heard now " << heard->key << std::endl;

        QSignalSpy reanalysed(&controller, &TrackImportController::reanalysisReady);
        const auto analyseAgain = [&] {
            const int before = reanalysed.count();
            assert(controller.analyseAgain(pioneer, engine, QStringLiteral("Again")));
            assert(controller.busy() && controller.reanalysingPlaylist() == QLatin1String("Again"));
            assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
            if (!controller.errorMessage().isEmpty()) {
                std::cerr << controller.errorMessage().toStdString() << "\n";
            }
            return reanalysed.count() > before;
        };
        assert(analyseAgain());
        QVariantMap again = controller.lastReanalysis();
        std::cerr << "  analysed " << again.value("analysed").toInt() << ", left " << again.value("leftAlone").toInt()
                  << std::endl;
        assert(again.value("analysed").toInt() == 3 && again.value("leftAlone").toInt() == 2);
        assert(again.value("playlist").toString() == QLatin1String("Again"));
        assert(session->pendingCount() == 3 && controller.reanalysisCount(QStringLiteral("Again")) == 3);
        // While it runs, an import waits.
        assert(controller.analyseAgain(pioneer, engine, QStringLiteral("Again")));
        assert(!controller.importFiles(pioneer, engine, one, QString(), false));
        assert(controller.errorMessage().contains(QLatin1String("analysed again")));
        assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
        // Twice: still one change a track.
        assert(session->pendingCount() == 3 && controller.reanalysisCount(QStringLiteral("Again")) == 3);
        controller.dropReanalysis(QStringLiteral("Again"));
        assert(session->pendingCount() == 0 && controller.reanalysisCount(QStringLiteral("Again")) == 0);
        assert(analyseAgain() && session->pendingCount() == 3);
        // An import waits for them, and says so.
        assert(!controller.importFiles(pioneer, engine, one, QString(), false));
        std::cerr << "  import: " << controller.errorMessage().toStdString() << std::endl;
        assert(controller.errorMessage().contains(QLatin1String("Tracks analysed again are waiting to be saved")));
        std::cout << "case 15a (Analyse again: three staged, the missing file and the one of another size left as they "
                     "are and counted, again replaces, Take back takes them all) OK\n";

        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        if (!session->lastSummary().value("error").toString().isEmpty()) {
            std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
        }
        assert(session->lastSummary().value("written").toInt() == 3);
        assert(session->pendingCount() == 0 && controller.reanalysisCount(QStringLiteral("Again")) == 0);

        const auto checkGrid = [](const std::vector<domain::Beat> &beats, double spacing, const char *library) {
            std::cerr << "  " << library << ": " << beats.size() << " beats, "
                      << (beats.size() > 1 ? beats[1].timeMs - beats[0].timeMs : 0.0) << " ms apart" << std::endl;
            assert(beats.size() > 20);
            for (std::size_t i = 1; i < beats.size(); ++i) {
                assert(std::abs(beats[i].timeMs - beats[i - 1].timeMs - spacing) < 2.0);
            }
        };
        for (const char *name : {"p plain.wav", "p tagged.wav", "p keyless.wav"}) {
            const domain::Track d = pdbRow(name);
            const domain::Track o = oneRow(name);
            const domain::Track e = engineRow(name);
            std::cerr << "  " << name << " bpm " << d.bpm << " | " << o.bpm << " | " << e.bpm << std::endl;
            assert(std::abs(d.bpm - 140.0) < 0.3 && std::abs(o.bpm - 140.0) < 0.3 && std::abs(e.bpm - 140.0) < 0.3);
            checkGrid(infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), d.sourceId), 60000.0 / 140.0,
                      "rekordbox");
            checkGrid(infrastructure::engine::readBeatGrid(stick.engineUtf8(), e.sourceId), 60000.0 / 140.0, "Engine");
        }
        // The cue stayed, in both rekordbox libraries.
        assert(hasTheCue(pdbRow("p plain.wav")) && hasTheCue(oneRow("p plain.wav")));
        // The key: every key a row has stays, the analyser's of the import
        // as a tag's.
        assert(pdbRow("p plain.wav").key == plainBefore.key && oneRow("p plain.wav").key == plainBefore.key);
        assert(engineRow("p plain.wav").key == engineKeyBefore);
        assert(pdbRow("p tagged.wav").key == "Dm" && oneRow("p tagged.wav").key == "Dm");
        assert(engineRow("p tagged.wav").key == "Dm");
        // Rows with none take the analyser's where the writer can: not
        // Engine's analysed row, whose key lives in the analysis record
        // (ReanalyseTrackChange says why).
        const auto heardKeyless = analyseForImport(pathToUtf8(onStick / "p keyless.wav"), application::CancellationToken{});
        assert(heardKeyless && !heardKeyless->key.empty());
        std::cerr << "  keyless now: " << pdbRow("p keyless.wav").key << " | " << oneRow("p keyless.wav").key << " | '"
                  << engineRow("p keyless.wav").key << "', heard " << heardKeyless->key << std::endl;
        assert(pdbRow("p keyless.wav").key == heardKeyless->key && oneRow("p keyless.wav").key == heardKeyless->key);
        assert(engineRow("p keyless.wav").key.empty());
        assert(fs::is_regular_file(plainTwoEx));
        // The track that is gone, and the one of another size, are as they
        // were.
        assert(std::abs(pdbRow("p gone.wav").bpm - 120.0) < 0.3);
        assert(std::abs(pdbRow("p bigger.wav").bpm - 120.0) < 0.3 && std::abs(engineRow("p bigger.wav").bpm - 120.0) < 0.3);
        // The save's log names the track and what each library got.
        {
            std::ifstream in(stick.root / "Seabass" / "seabass.log", std::ios::binary);
            std::string line;
            std::string found;
            while (std::getline(in, line)) {
                if (line.find("track-reanalysis: analysed") != std::string::npos
                    && line.find("p plain.wav") != std::string::npos) {
                    found = line;
                }
            }
            std::cerr << "  log: " << found << std::endl;
            assert(found.find("export.pdb: tempo of 1 row(s), keys left as they are") != std::string::npos);
            assert(found.find("exportLibrary.db: tempo of 1 row(s), keys left as they are") != std::string::npos);
            assert(found.find("Engine id") != std::string::npos && found.find("grid written") != std::string::npos);
            assert(found.find(".2EX made") != std::string::npos);
        }
        std::cout << "case 15b (saved: 140 BPM and its grid in all three libraries, the hot cue kept, every key a row has "
                     "kept, the analyser's only into rows with none, a missing .2EX made) OK\n";

        // Once more at another tempo: the grid Seabass wrote last time is
        // known for its own, not taken for one moved on the player.
        writeBeatChordWav(onStick / "p plain.wav", 30, 125.0, eMinor);
        // A tempo out of step with a grid that is the new one already: the
        // tempo goes in all the same.
        const std::string taggedEngineId = engineRow("p tagged.wav").sourceId;
        assert(testing::sqlExec(pathToUtf8(stick.engine / "Database2" / "m.db"),
                                "UPDATE Track SET bpm = 100, bpmAnalyzed = 100 WHERE id = " + taggedEngineId));
        assert(std::abs(engineRow("p tagged.wav").bpm - 100.0) < 0.01);
        assert(analyseAgain() && controller.lastReanalysis().value("analysed").toInt() == 3);
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 3);
        const domain::Track e = engineRow("p plain.wav");
        std::cerr << "  again at 125: Engine bpm " << e.bpm << std::endl;
        assert(std::abs(e.bpm - 125.0) < 0.3 && std::abs(pdbRow("p plain.wav").bpm - 125.0) < 0.3);
        checkGrid(infrastructure::engine::readBeatGrid(stick.engineUtf8(), e.sourceId), 480.0, "Engine");
        assert(hasTheCue(pdbRow("p plain.wav")));
        std::cerr << "  tagged, its grid the same already: Engine bpm " << engineRow("p tagged.wav").bpm << std::endl;
        assert(std::abs(engineRow("p tagged.wav").bpm - 140.0) < 0.3);
        std::cout << "case 15c (Analyse again twice: Seabass's own grid replaced the second time too, and a tempo "
                     "beside a grid that is the same already put right) OK\n";
    }


    // 16. Cues on the old grid, carried onto the new one when the person
    //     asks. Two tracks, each with a hot cue on a beat and a memory cue
    //     off the beats in all three libraries; their audio replaced by
    //     the same music 40 ms later. Analyse again counts them and the
    //     review shows each cue where it is and where it would go, every
    //     switch off; all on and all off reach the changes. Saved with
    //     one track switched on: its hot cue is 40 ms later in all three
    //     libraries, its memory cue where it was; the other track's cues
    //     are untouched.
    {
        const std::vector<double> aMinor{110.0, 130.81, 164.81, 220.0, 261.63, 329.63};
        writeBeatChordWav(computer / "r on.wav", 30, 120.0, aMinor);
        writeBeatChordWav(computer / "r off.wav", 31, 120.0, aMinor);
        assert(offer(QVariantList{pathToQString(computer / "r on.wav"), pathToQString(computer / "r off.wav")},
                     QStringLiteral("Cues"), true));
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        assert(session->lastSummary().value("written").toInt() == 2 + 1 + 2);
        const auto rowOf = [](auto &&reader, const std::string &name) {
            for (const auto &track : reader.readAll()) {
                if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == name) {
                    return track;
                }
            }
            assert(false && "the track is in the library");
            return domain::Track{};
        };
        const auto rowsOf = [&](const std::string &name) {
            return std::vector<domain::Track>{
                rowOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), name),
                rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), name),
                rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), name)};
        };
        std::map<std::string, std::pair<double, double>> placed;  // name: on a beat, off the beats
        for (const char *name : {"r on.wav", "r off.wav"}) {
            const std::vector<domain::Track> rows = rowsOf(name);
            const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), rows[0].sourceId);
            assert(grid.size() > 20);
            const double onBeat = std::round(grid[8].timeMs);
            const double offBeat = std::round(grid[10].timeMs) + 180.0;
            placed[name] = {onBeat, offBeat};
            for (const auto &[format, path, row] :
                 {std::tuple{QStringLiteral("rekordbox"), pioneer, rows[0]}, std::tuple{QStringLiteral("engine"), engine, rows[2]}}) {
                assert(session->stage(std::make_unique<AddCueChange>(format, path, QString::fromStdString(row.sourceId),
                                                                     onBeat, QStringLiteral("hot"), 1,
                                                                     QStringLiteral("#00FF00"), QStringLiteral("drop"),
                                                                     false, 0.0, QStringLiteral("r"))));
                assert(session->stage(std::make_unique<AddCueChange>(format, path, QString::fromStdString(row.sourceId),
                                                                     offBeat, QStringLiteral("memory"), 0, QString(),
                                                                     QString(), false, 0.0, QStringLiteral("r"))));
            }
        }
        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        if (!session->lastSummary().value("error").toString().isEmpty()) {
            std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
        }
        // The hot cue and the memory cue of a track, in each library, by
        // position: what each library holds now.
        const auto cueAt = [](const domain::Track &track, bool hotCue) {
            for (const auto &cue : track.cues) {
                if ((cue.kind == domain::CuePoint::Kind::Hot) == hotCue && !cue.isLoop) {
                    return cue.positionMs;
                }
            }
            return -1.0;
        };
        for (const char *name : {"r on.wav", "r off.wav"}) {
            for (const domain::Track &row : rowsOf(name)) {
                assert(std::abs(cueAt(row, true) - placed[name].first) < 1.0);
                assert(std::abs(cueAt(row, false) - placed[name].second) < 1.0);
            }
        }

        const fs::path onStick = stick.root / "Contents" / "UnknownArtist" / "UnknownAlbum";
        writeBeatChordWav(onStick / "r on.wav", 30, 120.0, aMinor, 0.29);
        writeBeatChordWav(onStick / "r off.wav", 31, 120.0, aMinor, 0.29);
        assert(controller.analyseAgain(pioneer, engine, QStringLiteral("Cues")));
        assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
        const QVariantMap again = controller.lastReanalysis();
        std::cerr << "  cues: " << again.value("cueTracks").toInt() << " tracks, " << again.value("cuesToMove").toInt()
                  << " to move, " << again.value("gridChangedTooMuch").toInt() << " too different" << std::endl;
        assert(again.value("analysed").toInt() == 2 && again.value("cueTracks").toInt() == 2);
        assert(again.value("cuesToMove").toInt() == 2 && again.value("gridChangedTooMuch").toInt() == 0);
        assert(controller.cueReviewTooDifferent().isEmpty());

        // The review: per track its one cue on a beat, every switch off.
        QVariantList review = controller.cueReview();
        assert(review.size() == 2);
        QString onId;
        for (const QVariant &entry : review) {
            const QVariantMap track = entry.toMap();
            const bool isOn = track.value("title").toString().startsWith(QLatin1String("r on"));
            if (isOn) {
                onId = track.value("id").toString();
            }
            const auto &[onBeat, offBeat] = placed[isOn ? "r on.wav" : "r off.wav"];
            std::cerr << "  " << track.value("title").toString().toStdString() << ": grid moved "
                      << track.value("gridShiftMs").toInt() << " ms" << std::endl;
            assert(std::abs(track.value("gridShiftMs").toInt() - 40) <= 3);
            assert(!track.value("moveCues").toBool() && track.value("offBeat").toInt() == 1);
            const QVariantList rows = track.value("rows").toList();
            assert(rows.size() == 1);
            const QVariantMap row = rows[0].toMap();
            std::cerr << "  row: " << row.value("label").toString().toStdString() << " \""
                      << row.value("name").toString().toStdString() << "\" " << row.value("oldText").toString().toStdString()
                      << " to " << row.value("newText").toString().toStdString() << std::endl;
            assert(row.value("label").toString() == QLatin1String("Hot cue 1"));
            assert(row.value("oldText").toString() == ReanalyseTrackChange::clockText(onBeat));
            const int diff = row.value("diffMs").toInt();
            assert(std::abs(diff - 40) <= 3);
            assert(row.value("newText").toString() == ReanalyseTrackChange::clockText(onBeat + diff)
                   || std::abs(diff - 40) <= 3);
            // The strip: in its window, in order, the old place before the
            // new one by the move.
            const QVariantMap strip = row.value("strip").toMap();
            const double span = strip.value("spanMs").toDouble();
            assert(span > 1000.0 && strip.value("columns").toList().size() > 100);
            for (const char *grid : {"oldBeats", "newBeats"}) {
                const QVariantList beats = strip.value(grid).toList();
                assert(beats.size() >= 3);
                for (int i = 0; i < beats.size(); ++i) {
                    assert(beats[i].toDouble() >= 0.0 && beats[i].toDouble() <= span);
                    assert(i == 0 || beats[i].toDouble() > beats[i - 1].toDouble());
                }
            }
            const double oldCue = strip.value("oldCueMs").toDouble();
            const double newCue = strip.value("newCueMs").toDouble();
            assert(oldCue > 0.0 && newCue < span && std::abs(newCue - oldCue - diff) < 1.0);
        }
        assert(!onId.isEmpty());
        const auto switches = [&] {
            QStringList on;
            for (const QVariant &entry : controller.cueReview()) {
                if (entry.toMap().value("moveCues").toBool()) {
                    on << entry.toMap().value("id").toString();
                }
            }
            return on;
        };
        controller.setMoveCuesForAll(true);
        assert(switches().size() == 2);
        controller.setMoveCuesForAll(false);
        assert(switches().isEmpty());
        {
            // One switch turned is that one switch: the review is not
            // rebuilt (the QML follows moveCuesChanged alone).
            QSignalSpy rebuilt(&controller, &TrackImportController::cueReviewChanged);
            QSignalSpy choice(&controller, &TrackImportController::moveCuesChanged);
            controller.setMoveCues(onId, true);
            assert(choice.count() == 1 && choice.at(0).at(0).toString() == onId && choice.at(0).at(1).toBool());
            controller.setMoveCuesForAll(false);
            controller.setMoveCuesForAll(true);
            assert(choice.count() == 1 + 1 + 2 && rebuilt.count() == 0);
            controller.setMoveCuesForAll(false);
            controller.setMoveCues(onId, true);
        }
        assert(switches() == QStringList{onId});
        // A second Analyse again that stages nothing leaves the review of
        // the changes that still wait, switches and all.
        assert(controller.analyseAgain(pioneer, engine, QStringLiteral("No Such Playlist")));
        assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
        assert(controller.lastReanalysis().value("analysed").toInt() == 0);
        assert(controller.cueReview().size() == 2 && switches() == QStringList{onId});
        std::cout << "case 16a (cues on the old grid: counted, reviewed with old and new times and a strip, every switch "
                     "off until turned on) OK\n";

        finished.clear();
        session->save();
        assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        if (!session->lastSummary().value("error").toString().isEmpty()) {
            std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
        }
        assert(session->lastSummary().value("written").toInt() == 2);
        assert(controller.cueReview().isEmpty());
        const char *libraries[] = {"DeviceLibrary", "OneLibrary", "Engine"};
        for (const char *name : {"r on.wav", "r off.wav"}) {
            const bool moved = std::string(name) == "r on.wav";
            const std::vector<domain::Track> rows = rowsOf(name);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const double hotCue = cueAt(rows[i], true);
                std::cerr << "  " << name << " " << libraries[i] << ": hot cue " << placed[name].first << " to " << hotCue
                          << ", memory cue " << placed[name].second << " to " << cueAt(rows[i], false) << std::endl;
                assert(std::abs(hotCue - (placed[name].first + (moved ? 40.0 : 0.0))) < (moved ? 4.0 : 1.0));
                assert(std::abs(cueAt(rows[i], false) - placed[name].second) < 1.0);
            }
        }
        // OneLibrary's cue table, kept in step with its analysis file.
        for (const char *name : {"r on.wav", "r off.wav"}) {
            const bool moved = std::string(name) == "r on.wav";
            infrastructure::onelibrary::OneLibraryCueWriter table(pathToUtf8(stick.pioneer));
            const auto ids = table.contentIdsForFile(pathToUtf8(onStick / name));
            assert(ids.size() == 1);
            double hotCue = -1.0;
            for (const auto &entry : table.cueTableOf(ids.front())) {
                if (entry.understood() && entry.cue.kind == domain::CuePoint::Kind::Hot) {
                    hotCue = entry.cue.positionMs;
                }
            }
            std::cerr << "  " << name << " OneLibrary cue table: hot cue at " << hotCue << std::endl;
            assert(std::abs(hotCue - (placed[name].first + (moved ? 40.0 : 0.0))) < (moved ? 4.0 : 1.0));
        }
        {
            std::ifstream in(stick.root / "Seabass" / "seabass.log", std::ios::binary);
            std::string line;
            bool named = false;
            while (std::getline(in, line)) {
                named = named
                        || (line.find("track-reanalysis: analysed \"r on\"") != std::string::npos
                            && line.find("moved Hot cue 1 from " + ReanalyseTrackChange::clockText(placed["r on.wav"].first).toStdString())
                                   != std::string::npos);
            }
            assert(named);
        }
        std::cout << "case 16b (saved: the switched-on track's cue on a beat moved 40 ms in all three libraries, the one "
                     "off the beats kept; the other track's cues untouched) OK\n";

        // Shared by the cases below.
        const auto saveNow = [&]() {
            finished.clear();
            session->save();
            assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
            if (!session->lastSummary().value("error").toString().isEmpty()) {
                std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
            }
            return session->lastSummary();
        };
        const auto stageCue = [&](const QString &format, const domain::Track &row, double ms, bool hot, int pad) {
            const QString path = format == QLatin1String("engine") ? engine : pioneer;
            assert(session->stage(std::make_unique<AddCueChange>(
                format, path, QString::fromStdString(row.sourceId), ms, hot ? QStringLiteral("hot") : QStringLiteral("memory"),
                hot ? pad : 0, hot ? QStringLiteral("#00FF00") : QString(), QString(), false, 0.0, QStringLiteral("t"))));
        };
        // Tracks imported into `playlist`, saved, each given hot cue 1 on its
        // ninth beat in all three libraries; that beat's time, by name.
        const auto cuedTracks = [&](const std::vector<std::pair<std::string, int>> &tracks, const QString &playlist) {
            QVariantList files;
            for (const auto &[name, seconds] : tracks) {
                writeBeatChordWav(computer / name, seconds, 120.0, aMinor);
                files.push_back(pathToQString(computer / name));
            }
            assert(offer(files, playlist, true));
            assert(controller.lastResult().value("staged").toInt() == static_cast<int>(tracks.size()));
            assert(saveNow().value("error").toString().isEmpty());
            std::map<std::string, double> onBeat;
            const auto [name, seconds] = tracks.front();
            const std::vector<domain::Track> rows = rowsOf(name);
            const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), rows[0].sourceId);
            onBeat[name] = std::round(grid[8].timeMs);
            stageCue(QStringLiteral("rekordbox"), rows[0], onBeat[name], true, 1);
            stageCue(QStringLiteral("engine"), rows[2], onBeat[name], true, 1);
            assert(saveNow().value("written").toInt() == 2);
            return onBeat;
        };
        const auto analyseAgainOn = [&](const QString &playlist) {
            assert(controller.analyseAgain(pioneer, engine, playlist));
            assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
            assert(controller.errorMessage().isEmpty());
        };
        const auto cuesOf = [&](const domain::Track &row) {
            std::vector<std::pair<bool, double>> cues;
            for (const auto &cue : row.cues) {
                cues.emplace_back(cue.kind == domain::CuePoint::Kind::Hot, cue.positionMs);
            }
            return cues;
        };
        const auto logSince = [&](std::size_t from) {
            std::ifstream in(stick.root / "Seabass" / "seabass.log", std::ios::binary);
            const std::string all((std::istreambuf_iterator<char>(in)), {});
            return all.size() > from ? all.substr(from) : std::string();
        };
        // Whether the log, from `text` on, says that a save's Engine
        // session opened by `label` put m.db behind a scratch copy.
        const auto scratchBy = [](const std::string &text, const std::string &label) {
            const auto at = text.find(label + ": applying up to ");
            if (at == std::string::npos) {
                return false;
            }
            const std::string line = text.substr(at, text.find('\n', at) - at);
            std::cerr << "  " << line.substr(0, 90) << std::endl;
            return line.find(" engine update(s) to a local scratch copy") != std::string::npos;
        };
        const auto logSize = [&]() {
            std::error_code ec;
            return static_cast<std::size_t>(fs::file_size(stick.root / "Seabass" / "seabass.log", ec));
        };

        // 16c. One save: Analyse again with the cues moved, and a memory
        //      cue added to the same track after it in all three
        //      libraries. Both are there in all three, the hot cue at its
        //      new place. The catalog cache is made as blind as a FAT32
        //      stick makes it (one mtime for every write, the analysis
        //      files' check window never over), so only a change that says
        //      it wrote lets the next one see it.
        {
            const auto onBeat = cuedTracks({{"s same.wav", 36}}, QStringLiteral("Same"));
            const double hotAt = onBeat.at("s same.wav");
            writeBeatChordWav(onStick / "s same.wav", 36, 120.0, aMinor, 0.29);
            analyseAgainOn(QStringLiteral("Same"));
            assert(controller.lastReanalysis().value("cuesToMove").toInt() == 1);
            controller.setMoveCuesForAll(true);
            const std::vector<domain::Track> rows = rowsOf("s same.wav");
            const double memoryAt = hotAt + 700.0;  // between two beats
            stageCue(QStringLiteral("rekordbox"), rows[0], memoryAt, false, 0);
            stageCue(QStringLiteral("engine"), rows[2], memoryAt, false, 0);
            {
                LibraryCatalogCache blind(LibraryCatalogCache::realStageForTesting(),
                                          [](const std::string &, const std::string &) {
                                              return std::chrono::system_clock::time_point{};
                                          });
                const auto frozen = std::chrono::steady_clock::now();
                blind.setNowFnForTesting([frozen] { return frozen; });
                LibraryCatalogCache::setInstanceForTesting(&blind);
                assert(saveNow().value("written").toInt() == 3);
                LibraryCatalogCache::setInstanceForTesting(nullptr);
            }
            const char *libraries[] = {"DeviceLibrary", "OneLibrary", "Engine"};
            const std::vector<domain::Track> after = rowsOf("s same.wav");
            for (std::size_t i = 0; i < after.size(); ++i) {
                std::cerr << "  s same " << libraries[i] << ": hot " << cueAt(after[i], true) << " (was " << hotAt
                          << "), memory " << cueAt(after[i], false) << std::endl;
                assert(std::abs(cueAt(after[i], true) - (hotAt + 40.0)) < 4.0);
                assert(std::abs(cueAt(after[i], false) - memoryAt) < 1.0);
            }
            std::cout << "case 16c (a cue added after Analyse again in the same save, the cache blind as on FAT32: the "
                         "moved cue and the new one in all three libraries) OK\n";
        }

        // 16d. Fifteen tracks analysed again put m.db behind a scratch
        //      copy for the save. A memory cue added to another track in
        //      that save, before its own Analyse again change, is in the
        //      copy only: the move reads Engine there, and every library
        //      ends up with the moved cue and the new one.
        {
            const auto onBeat = cuedTracks({{"z last.wav", 27}}, QStringLiteral("Last"));
            const double hotAt = onBeat.at("z last.wav");
            QVariantList files;
            std::vector<std::pair<std::string, int>> many;
            for (int i = 0; i < 15; ++i) {
                many.emplace_back((i < 10 ? "m 0" : "m ") + std::to_string(i) + ".wav", 12 + i);
                writeBeatChordWav(computer / many.back().first, many.back().second, 120.0, aMinor);
                files.push_back(pathToQString(computer / many.back().first));
            }
            assert(offer(files, QStringLiteral("Many"), true));
            assert(saveNow().value("error").toString().isEmpty());
            for (const auto &[name, seconds] : many) {
                writeBeatChordWav(onStick / name, seconds, 120.0, aMinor, 0.29);
            }
            writeBeatChordWav(onStick / "z last.wav", 27, 120.0, aMinor, 0.29);
            analyseAgainOn(QStringLiteral("Many"));
            assert(controller.lastReanalysis().value("analysed").toInt() == 15);
            const std::vector<domain::Track> rows = rowsOf("z last.wav");
            const double memoryAt = hotAt + 700.0;
            stageCue(QStringLiteral("rekordbox"), rows[0], memoryAt, false, 0);
            stageCue(QStringLiteral("engine"), rows[2], memoryAt, false, 0);
            analyseAgainOn(QStringLiteral("Last"));
            assert(controller.lastReanalysis().value("cuesToMove").toInt() == 1);
            controller.setMoveCuesForAll(true);
            const std::size_t from = logSize();
            assert(saveNow().value("written").toInt() == 18);
            assert(scratchBy(logSince(from), "track-reanalysis"));
            for (const domain::Track &row : rowsOf("z last.wav")) {
                std::cerr << "  z last " << row.format << ": hot " << cueAt(row, true) << ", memory " << cueAt(row, false)
                          << std::endl;
                assert(std::abs(cueAt(row, true) - (hotAt + 40.0)) < 4.0);
                assert(std::abs(cueAt(row, false) - memoryAt) < 1.0);
            }
            std::cout << "case 16d (m.db behind a scratch copy, a cue added there before the move: the moved cue and "
                         "the new one in all three libraries) OK\n";
        }

        // 16e. All or nothing. One track's Engine row holds a cue on a beat
        //      the others lack; another track's cue is put on another beat
        //      after the review. With every switch on, no cue of either moves in any
        //      library, their grids move all the same, and the save says
        //      why.
        {
            auto onBeat = cuedTracks({{"t extra.wav", 37}}, QStringLiteral("Differ"));
            writeBeatChordWav(computer / "t later.wav", 38, 120.0, aMinor);
            assert(offer(QVariantList{pathToQString(computer / "t later.wav")}, QStringLiteral("Differ"), false));
            assert(saveNow().value("error").toString().isEmpty());
            {
                const std::vector<domain::Track> rows = rowsOf("t later.wav");
                const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), rows[0].sourceId);
                onBeat["t later.wav"] = std::round(grid[8].timeMs);
                stageCue(QStringLiteral("rekordbox"), rows[0], onBeat["t later.wav"], true, 1);
                stageCue(QStringLiteral("engine"), rows[2], onBeat["t later.wav"], true, 1);
                const domain::Track extra = rowsOf("t extra.wav")[2];
                const auto extraGrid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer),
                                                                               rowsOf("t extra.wav")[0].sourceId);
                stageCue(QStringLiteral("engine"), extra, std::round(extraGrid[12].timeMs), true, 2);
                assert(saveNow().value("written").toInt() == 3);
            }
            const auto before = [&](const std::string &name) {
                std::vector<std::vector<std::pair<bool, double>>> all;
                for (const domain::Track &row : rowsOf(name)) {
                    all.push_back(cuesOf(row));
                }
                return all;
            };
            const auto extraBefore = before("t extra.wav");
            const auto gridBefore = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer),
                                                                            rowsOf("t extra.wav")[0].sourceId);
            writeBeatChordWav(onStick / "t extra.wav", 37, 120.0, aMinor, 0.29);
            writeBeatChordWav(onStick / "t later.wav", 38, 120.0, aMinor, 0.29);
            analyseAgainOn(QStringLiteral("Differ"));
            assert(controller.lastReanalysis().value("cuesToMove").toInt() == 2);
            controller.setMoveCuesForAll(true);
            // After the review, its hot cue put on another beat in
            // rekordbox's analysis file (shared by OneLibrary), as another
            // program would: as many cues as the review showed, not the same.
            {
                const domain::Track later = rowsOf("t later.wav")[0];
                std::vector<domain::CuePoint> cues = later.cues;
                const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), later.sourceId);
                assert(cues.size() == 1);
                cues[0].positionMs = std::round(grid[14].timeMs);
                infrastructure::rekordbox::RekordboxCueWriter(pathToUtf8(stick.pioneer)).writeHotCues(later.sourceId, cues);
                LibraryCatalogCache::instance().invalidateWithOneLibraryMirror("rekordbox", pathToUtf8(stick.pioneer));
            }
            const auto laterBefore = before("t later.wav");
            const QVariantMap summary = saveNow();
            std::cerr << "  notes: " << summary.value("notes").toString().toStdString() << std::endl;
            assert(summary.value("written").toInt() == 2);
            assert(before("t extra.wav") == extraBefore && before("t later.wav") == laterBefore);
            const auto gridAfter = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer),
                                                                           rowsOf("t extra.wav")[0].sourceId);
            assert(std::abs(gridAfter[8].timeMs - gridBefore[8].timeMs - 40.0) < 4.0 && "the grid moved all the same");
            assert(summary.value("notes").toString().contains(
                QLatin1String("The cue points of 2 tracks analysed again were left where they are")));
            std::cout << "case 16e (a cue only Engine holds, and one added after the review: no cue moves anywhere, the "
                         "grids do, and the save says why) OK\n";
        }

        // 16f. A row whose grid is locked on the player is left whole. With
        //      Engine the only library the change has, nothing is written
        //      and the track is not counted as analysed again; with the
        //      rekordbox side as well, that is written and the save names a
        //      library left as it was.
        {
            writeBeatChordWav(onStick / "t extra.wav", 37, 120.0, aMinor, 0.33);
            const domain::Track row = rowsOf("t extra.wav")[2];
            const std::string mdb = pathToUtf8(stick.engine / "Database2" / "m.db");
            assert(testing::sqlExec(mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = " + row.sourceId));
            const auto engineGridBefore = infrastructure::engine::readBeatGrid(stick.engineUtf8(), row.sourceId);
            const auto analysis = analyseForImport(pathToUtf8(onStick / "t extra.wav"), application::CancellationToken{},
                                                   domain::TempoRange{88.0, 175.0});
            auto change = std::make_unique<ReanalyseTrackChange>(
                pathToUtf8(onStick / "t extra.wav"), "/Contents/UnknownArtist/UnknownAlbum/t extra.wav",
                pathToQString(stick.root), QString(), engine, std::vector<ReanalyseTrackChange::AnalysisFileOf>{}, analysis,
                std::string(), QStringLiteral("t extra"), QString(), 1);
            const QString changeId = change->id();
            assert(session->stage(std::move(change)));
            const std::size_t from = logSize();
            const QVariantMap summary = saveNow();
            assert(summary.value("written").toInt() == 0 && session->lastSkippedIds().contains(changeId));
            // Skipped for the lock, not for want of a row.
            assert(logSince(from).find("nothing written: Engine id " + row.sourceId + ": grid locked on the player")
                   != std::string::npos);
            assert(infrastructure::engine::readBeatGrid(stick.engineUtf8(), row.sourceId).front().timeMs
                   == engineGridBefore.front().timeMs);
            analyseAgainOn(QStringLiteral("Differ"));
            const QVariantMap mixed = saveNow();
            std::cerr << "  notes: " << mixed.value("notes").toString().toStdString() << std::endl;
            assert(mixed.value("written").toInt() == 2);
            assert(mixed.value("notes").toString().contains(QLatin1String("a library was left as it was")));
            assert(infrastructure::engine::readBeatGrid(stick.engineUtf8(), row.sourceId).front().timeMs
                   == engineGridBefore.front().timeMs);
            std::cout << "case 16f (a grid locked on the player: Engine alone, nothing written and not counted; beside "
                         "rekordbox, the save names the library left) OK\n";
        }

        // 16g. A save that fails after a change of Analyse again landed puts
        //      every library and analysis file back byte for byte, and Undo
        //      Last Save after one that landed, with cues moved, does too.
        //      The failure: a folder where the save's next track is to go.
        {
            const auto snapshot = [&]() {
                std::map<std::string, std::string> files;
                for (const fs::path &top : {stick.pioneer, stick.engine}) {
                    for (const auto &entry : fs::recursive_directory_iterator(top)) {
                        // Not SQLite's shared-memory index beside a WAL
                        // database: no library content, rebuilt by every
                        // connection that opens the database.
                        if (entry.is_regular_file() && entry.path().extension() != ".db-shm") {
                            std::ifstream in(entry.path(), std::ios::binary);
                            files[pathToUtf8(entry.path())] = std::string((std::istreambuf_iterator<char>(in)), {});
                        }
                    }
                }
                return files;
            };
            const auto differs = [](const std::map<std::string, std::string> &a,
                                    const std::map<std::string, std::string> &b) {
                std::vector<std::string> names;
                for (const auto &[file, bytes] : a) {
                    const auto other = b.find(file);
                    if (other == b.end() || other->second != bytes) {
                        names.push_back(file);
                    }
                }
                for (const auto &[file, bytes] : b) {
                    if (a.count(file) == 0) {
                        names.push_back(file + " (new)");
                    }
                }
                for (const std::string &name : names) {
                    std::cerr << "  differs: " << name << std::endl;
                }
                return names;
            };
            writeBeatChordWav(onStick / "s same.wav", 36, 120.0, aMinor, 0.33);
            analyseAgainOn(QStringLiteral("Same"));
            assert(controller.lastReanalysis().value("cuesToMove").toInt() == 1);
            controller.setMoveCuesForAll(true);
            writeBeatChordWav(computer / "u blocked.wav", 39, 120.0, aMinor);
            ImportRequest request;
            request.offered = {pathToUtf8(computer / "u blocked.wav")};
            request.rekordboxPath = pioneer;
            request.enginePath = engine;
            request.date = "2026-11-26";
            const PreparedImport prepared = prepareTrackImport(request, application::CancellationToken{});
            assert(prepared.refusal.empty() && !prepared.staged.changes.empty());
            assert(session->stageAll(prepared.staged.changes));
            QStringList importIds;
            fs::path destination;
            for (const auto &staged : prepared.staged.changes) {
                importIds << staged->id();
                if (const auto import = std::dynamic_pointer_cast<ImportTrackChange>(staged)) {
                    destination = pathFromUtf8(import->destinationFile());
                }
            }
            assert(!destination.empty());
            fs::create_directories(destination);  // in the way
            const auto beforeSave = snapshot();
            const QVariantMap failed = saveNow();
            assert(!failed.value("error").toString().isEmpty());
            assert(differs(beforeSave, snapshot()).empty() && "a failed save leaves the stick as it was");
            fs::remove(destination);
            session->unstageAll(importIds);
            // The rekordbox baseline, so the save records Seabass's writes.
            {
                const auto tracks = infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll();
                const std::string root = testing::stickRootOf(stick);
                testing::plantBaseline(stick, domain::baselineFrom(
                                                  tracks, {}, testing::pdbSequence(stick),
                                                  [&](const std::string &file) { return baselineStickRelativePath(root, file); },
                                                  [](const std::string &relative) { return baselinePathKey(relative); }));
            }
            const double hotBefore = cueAt(rowsOf("s same.wav")[0], true);
            assert(saveNow().value("written").toInt() == 1);
            {
                const domain::Track moved = rowsOf("s same.wav")[0];
                std::cerr << "  s same: hot cue " << hotBefore << " to " << cueAt(moved, true) << std::endl;
                assert(std::abs(cueAt(moved, true) - hotBefore - 40.0) < 4.0);
                const auto baseline = testing::readBaseline(stick);
                const domain::BaselineTrack *row = baseline->findTrack(testing::keyOf(stick, moved));
                assert(row != nullptr);
                bool recorded = false;
                for (const auto &cue : row->cues) {
                    recorded = recorded
                               || (cue.cue.kind == domain::CuePoint::Kind::Hot && cue.origin == domain::ValueOrigin::Seabass
                                   && std::abs(cue.cue.positionMs - cueAt(moved, true)) < 1.0);
                }
                assert(recorded && "the moved cue is Seabass's own write in the rekordbox baseline");
            }
            assert(!differs(beforeSave, snapshot()).empty());
            assert(std::abs(cueAt(rowsOf("s same.wav")[2], true) - (cueAt(rowsOf("s same.wav")[0], true))) < 4.0);
            finished.clear();
            session->undoLastSave();
            assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
            assert(differs(beforeSave, snapshot()).empty() && "Undo puts every file back");
            std::cout << "case 16g (a failed save, and Undo Last Save after one with cues moved: every library and "
                         "analysis file as it was; the move recorded as Seabass's in the rekordbox baseline) OK\n";

            // 16h. A refusal inside the change after its first cue write (a
            //      step the test fails there): the save goes back whole.
            {
                // The audio 40 ms later than the grid Undo brought back.
                writeBeatChordWav(onStick / "s same.wav", 36, 120.0, aMinor, 0.33);
                analyseAgainOn(QStringLiteral("Same"));
                assert(controller.lastReanalysis().value("cuesToMove").toInt() == 1);
                controller.setMoveCuesForAll(true);
                const auto before = snapshot();
                bool reachedCues = false;
                ReanalyseTrackChange::setStepHookForTesting([&](const std::string &step) {
                    if (step.rfind("rekordbox-cues:", 0) == 0) {
                        reachedCues = true;
                        throw std::runtime_error("refused after the first cue write");
                    }
                });
                const QVariantMap failed = saveNow();
                ReanalyseTrackChange::setStepHookForTesting({});
                assert(reachedCues && failed.value("error").toString().contains(QLatin1String("refused after the first cue write")));
                assert(differs(before, snapshot()).empty() && "the save went back whole");
                std::cout << "case 16h (a refusal after the first cue write: every library and analysis file as it "
                             "was) OK\n";
            }

            // 16i. An Engine row the plan moved cues on that does not take
            //      the new grid when the save writes it (its grid locked
            //      between the preview and the write): the change fails,
            //      and the save goes back whole.
            {
                const domain::Track row = rowsOf("s same.wav")[2];
                const std::string mdb = pathToUtf8(stick.engine / "Database2" / "m.db");
                const auto before = snapshot();
                bool locked = false;
                ReanalyseTrackChange::setStepHookForTesting([&](const std::string &step) {
                    if (step == "engine-grid:" + row.sourceId) {
                        locked = testing::sqlExec(mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = " + row.sourceId);
                    }
                });
                const QVariantMap failed = saveNow();
                ReanalyseTrackChange::setStepHookForTesting({});
                std::cerr << "  failed: " << failed.value("error").toString().toStdString() << std::endl;
                assert(locked && failed.value("error").toString().contains(QLatin1String("did not take the new grid")));
                // The lock was the test's own write, made inside the save:
                // the save puts m.db back as it was before it, without it.
                assert(differs(before, snapshot()).empty() && "the save went back whole");
                controller.dropReanalysis(QStringLiteral("Same"));
                std::cout << "case 16i (an Engine row that does not take the grid the plan counted on: the change fails "
                             "and the save goes back whole) OK\n";
            }
        }

        // 16j. Each library compared for the kinds it holds. A track with a
        //      hot cue, two memory cues and a memory loop on beats in
        //      rekordbox's libraries, the hot cue in Engine too, and
        //      Engine's main cue on a beat rekordbox has no memory cue at:
        //      everything moves but Engine's main cue, which stays and is
        //      no reason to refuse. Another track whose Engine hot cue sits
        //      on another beat than rekordbox's, and one whose Engine row
        //      lacks a pad rekordbox moves: nothing of theirs moves.
        {
            auto onBeat = cuedTracks({{"k all.wav", 40}}, QStringLiteral("Kinds"));
            writeBeatChordWav(computer / "k differ.wav", 41, 120.0, aMinor);
            writeBeatChordWav(computer / "k missing.wav", 43, 120.0, aMinor);
            assert(offer(QVariantList{pathToQString(computer / "k differ.wav"), pathToQString(computer / "k missing.wav")},
                         QStringLiteral("Kinds"), false));
            assert(saveNow().value("error").toString().isEmpty());
            const std::vector<domain::Track> all = rowsOf("k all.wav");
            const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), all[0].sourceId);
            const auto beatAt = [&](int i) { return std::round(grid[static_cast<std::size_t>(i)].timeMs); };
            stageCue(QStringLiteral("rekordbox"), all[0], beatAt(10), false, 0);
            stageCue(QStringLiteral("rekordbox"), all[0], beatAt(12), false, 0);
            assert(session->stage(std::make_unique<AddCueChange>(QStringLiteral("rekordbox"), pioneer,
                                                                 QString::fromStdString(all[0].sourceId), beatAt(14),
                                                                 QStringLiteral("memory"), 0, QString(), QString(), true,
                                                                 beatAt(16), QStringLiteral("k"))));
            stageCue(QStringLiteral("engine"), all[2], beatAt(20), false, 0);  // Engine's main cue
            const std::vector<domain::Track> differ = rowsOf("k differ.wav");
            const auto differGrid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), differ[0].sourceId);
            onBeat["k differ.wav"] = std::round(differGrid[8].timeMs);
            stageCue(QStringLiteral("rekordbox"), differ[0], onBeat["k differ.wav"], true, 1);
            stageCue(QStringLiteral("engine"), differ[2], std::round(differGrid[9].timeMs), true, 1);
            // Pads 1 and 2 in rekordbox, only pad 1 in Engine.
            const std::vector<domain::Track> missing = rowsOf("k missing.wav");
            const auto missingGrid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), missing[0].sourceId);
            stageCue(QStringLiteral("rekordbox"), missing[0], std::round(missingGrid[8].timeMs), true, 1);
            stageCue(QStringLiteral("rekordbox"), missing[0], std::round(missingGrid[12].timeMs), true, 2);
            stageCue(QStringLiteral("engine"), missing[2], std::round(missingGrid[8].timeMs), true, 1);
            assert(saveNow().value("written").toInt() == 9);
            const auto cuesBefore = [&](const std::string &name) {
                std::vector<std::vector<domain::CuePoint>> rows;
                for (const domain::Track &row : rowsOf(name)) {
                    rows.push_back(row.cues);
                }
                return rows;
            };
            const auto allBefore = cuesBefore("k all.wav");
            const auto differBefore = cuesBefore("k differ.wav");
            const auto missingBefore = cuesBefore("k missing.wav");
            assert(allBefore[0].size() == 4 && allBefore[1].size() == 4 && allBefore[2].size() == 2);
            writeBeatChordWav(onStick / "k all.wav", 40, 120.0, aMinor, 0.29);
            writeBeatChordWav(onStick / "k differ.wav", 41, 120.0, aMinor, 0.29);
            writeBeatChordWav(onStick / "k missing.wav", 43, 120.0, aMinor, 0.29);
            analyseAgainOn(QStringLiteral("Kinds"));
            controller.setMoveCuesForAll(true);
            const std::size_t from = logSize();
            const QVariantMap summary = saveNow();
            // The log says what Engine itself moved: its pad, not rekordbox's
            // memory cues it does not hold, nor its main cue that stayed.
            {
                const std::string log = logSince(from);
                const std::string engineRow = "Engine id " + all[2].sourceId + ": moved ";
                std::size_t lines = 0;
                for (std::size_t at = log.find(engineRow); at != std::string::npos; at = log.find(engineRow, at + 1)) {
                    ++lines;
                }
                std::cerr << "  Engine moves logged: " << lines << std::endl;
                assert(lines == 1 && log.find(engineRow + "Hot cue 1") != std::string::npos);
            }
            std::cerr << "  notes: " << summary.value("notes").toString().toStdString() << std::endl;
            assert(summary.value("written").toInt() == 3);
            const std::vector<domain::Track> after = rowsOf("k all.wav");
            for (std::size_t library = 0; library < after.size(); ++library) {
                for (const domain::CuePoint &was : allBefore[library]) {
                    bool found = false;
                    const bool stays = library == 2 && was.kind == domain::CuePoint::Kind::Memory;
                    for (const domain::CuePoint &now : after[library].cues) {
                        if (now.kind == was.kind && now.hotCueNumber == was.hotCueNumber && now.isLoop == was.isLoop) {
                            const double by = now.positionMs - was.positionMs;
                            std::cerr << "  k all " << after[library].format << ": " << (was.isLoop ? "loop " : "cue ")
                                      << was.positionMs << " to " << now.positionMs << std::endl;
                            found = found || (stays ? std::abs(by) < 1.0 : std::abs(by - 40.0) < 4.0);
                        }
                    }
                    assert(found);
                }
            }
            assert(cuesBefore("k differ.wav").size() == 3);
            for (const auto &[name, wasBefore] :
                 {std::pair{std::string("k differ.wav"), differBefore}, std::pair{std::string("k missing.wav"), missingBefore}}) {
                const auto now = cuesBefore(name);
                for (std::size_t library = 0; library < now.size(); ++library) {
                    assert(now[library].size() == wasBefore[library].size());
                    for (std::size_t i = 0; i < now[library].size(); ++i) {
                        assert(std::abs(now[library][i].positionMs - wasBefore[library][i].positionMs) < 1.0);
                    }
                }
            }
            assert(summary.value("notes").toString().contains(
                QLatin1String("The cue points of 2 tracks analysed again were left where they are")));
            std::cout << "case 16j (memory cues and a memory loop move beside Engine's pads; Engine's own main cue "
                         "stays and blocks nothing; a differing hot cue still blocks) OK\n";
        }

        // 16k. A cue change asking first, with hint 0, does not decide for
        //      the bulk Engine write behind it in the same save: m.db goes
        //      behind a scratch copy all the same.
        {
            stageCue(QStringLiteral("engine"), rowsOf("m 00.wav")[2], 1000.0, false, 0);
            analyseAgainOn(QStringLiteral("Many"));
            const std::size_t from = logSize();
            assert(saveNow().value("error").toString().isEmpty());
            assert(scratchBy(logSince(from), "add-cue"));
            std::cout << "case 16k (a cue first, fifteen analyses behind it: m.db behind a scratch copy) OK\n";
        }

        // 16l. An Engine row the player never analysed that names a key: the
        //      completed row's analysis record carries that key.
        {
            writeBeatChordWav(computer / "n unanalysed.wav", 42, 120.0, aMinor);
            appendId3Frame(computer / "n unanalysed.wav", "TKEY", std::string(1, '\0') + "Dm");
            ImportRequest request;
            request.offered = {pathToUtf8(computer / "n unanalysed.wav")};
            request.rekordboxPath = pioneer;
            request.enginePath = engine;
            request.playlist = ImportPlaylist{"Unanalysed", true, std::nullopt};
            request.date = "2026-11-26";
            const PreparedImport prepared = prepareTrackImport(request, application::CancellationToken{});
            assert(prepared.refusal.empty() && prepared.analysed == 0);
            assert(session->stageAll(prepared.staged.changes));
            assert(saveNow().value("error").toString().isEmpty());
            const std::string id = rowsOf("n unanalysed.wav")[2].sourceId;
            const std::string mdb = pathToUtf8(stick.engine / "Database2" / "m.db");
            assert(testing::sqlScalar(mdb, "SELECT isAnalyzed FROM Track WHERE id = " + id) == 0);
            const std::int64_t column = testing::sqlScalar(mdb, "SELECT key FROM Track WHERE id = " + id);
            assert(column > 0);
            analyseAgainOn(QStringLiteral("Unanalysed"));
            assert(saveNow().value("written").toInt() == 1);
            assert(testing::sqlScalar(mdb, "SELECT isAnalyzed FROM Track WHERE id = " + id) == 1);
            const auto rows = testing::sqlRows(mdb, "SELECT trackData FROM PerformanceData WHERE trackId = " + id);
            const std::string &hex = rows.at(0).at(0);  // x'...', as sqlRows gives a blob
            assert(hex.size() > 3 && hex.rfind("x'", 0) == 0);
            std::vector<std::byte> blob;
            for (std::size_t i = 2; i + 1 < hex.size() - 1; i += 2) {
                blob.push_back(static_cast<std::byte>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            }
            const auto record = djinterop::engine::v3::track_data_blob::from_blob(blob);
            std::cerr << "  key column " << column << ", record " << record.key << std::endl;
            assert(record.key == column && testing::sqlScalar(mdb, "SELECT key FROM Track WHERE id = " + id) == column);
            std::cout << "case 16l (a completed row keeps its key in the column and the analysis record alike) OK\n";
        }

        // 16m. A save of twenty Analyse-again changes reads the stick's
        //      rekordbox catalogs a bounded number of times, with cues
        //      moved or not: never once per change. The catalog cache
        //      counts its passes; the save is timed.
        {
            std::vector<std::pair<std::string, int>> twenty;
            for (int i = 0; i < 20; ++i) {
                twenty.emplace_back((i < 10 ? "v 0" : "v ") + std::to_string(i) + ".wav", 10 + i);
            }
            const auto onBeat = cuedTracks(twenty, QStringLiteral("Twenty"));
            const double hotAt = onBeat.at("v 00.wav");
            const auto countedSave = [&](const char *what) {
                std::map<std::string, int> passes;
                std::mutex counting;
                const auto real = LibraryCatalogCache::realStageForTesting();
                LibraryCatalogCache counted(
                    [&](LibraryCatalogCache::Detail stage, const std::string &format, const std::string &path,
                        std::vector<domain::Track> &tracks, LibraryCatalogCache::StageNotes &notes,
                        application::ProgressReporter &progress, application::CancellationToken cancel) {
                        {
                            const std::lock_guard<std::mutex> lock(counting);
                            ++passes[format];
                        }
                        real(stage, format, path, tracks, notes, progress, cancel);
                    },
                    LibraryCatalogCache::realMtimeForTesting());
                LibraryCatalogCache::setInstanceForTesting(&counted);
                const auto started = std::chrono::steady_clock::now();
                const QVariantMap summary = saveNow();
                const auto seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                LibraryCatalogCache::setInstanceForTesting(nullptr);
                std::cerr << "  twenty, " << what << ": " << seconds << " s, catalog passes rekordbox "
                          << passes["rekordbox"] << ", onelibrary " << passes["onelibrary"] << ", engine "
                          << passes["engine"] << std::endl;
                assert(summary.value("written").toInt() == 20);
                assert(passes["rekordbox"] <= 3 && passes["onelibrary"] <= 3 && passes["engine"] <= 3);
            };
            for (const auto &[name, seconds] : twenty) {
                writeBeatChordWav(onStick / name, seconds, 120.0, aMinor, 0.29);
            }
            analyseAgainOn(QStringLiteral("Twenty"));
            assert(controller.lastReanalysis().value("cuesToMove").toInt() == 1);
            controller.setMoveCuesForAll(true);
            countedSave("one cue moved");
            for (const domain::Track &row : rowsOf("v 00.wav")) {
                assert(std::abs(cueAt(row, true) - (hotAt + 40.0)) < 4.0);
            }
            for (const auto &[name, seconds] : twenty) {
                writeBeatChordWav(onStick / name, seconds, 120.0, aMinor, 0.33);
            }
            analyseAgainOn(QStringLiteral("Twenty"));
            countedSave("no cue moved");
            std::cout << "case 16m (twenty tracks analysed again in one save: the catalogs read a bounded number of "
                         "times, cues moved or not) OK\n";
        }

        // 16n. A cue only OneLibrary holds, off the beats, beside a cue on a
        //      beat both rekordbox libraries hold: OneLibrary's row names
        //      an analysis file of its own. Nothing moves, and OneLibrary's
        //      cue is still there.
        {
            const auto onBeat = cuedTracks({{"o split.wav", 44}}, QStringLiteral("Split"));
            const double hotAt = onBeat.at("o split.wav");
            const domain::Track one = rowsOf("o split.wav")[1];
            // A copy of the track's analysis files in a folder of their own,
            // named by OneLibrary's row alone.
            const std::string ownDat = "/PIONEER/USBANLZ/P07E/0000AAAA/ANLZ0000.DAT";
            const fs::path ownFolder = stick.root / "PIONEER" / "USBANLZ" / "P07E" / "0000AAAA";
            fs::create_directories(ownFolder);
            for (const char *extension : {"DAT", "EXT", "2EX"}) {
                const fs::path from =
                    stick.root / pathFromUtf8(one.analysisFile.substr(1, one.analysisFile.size() - 4) + extension);
                if (fs::is_regular_file(from)) {
                    fs::copy_file(from, ownFolder / (std::string("ANLZ0000.") + extension));
                }
            }
            {
                infrastructure::onelibrary::SqlCipherLibrary lib;
                infrastructure::onelibrary::SqlCipherDb raw(lib, pathToUtf8(stick.pioneer / "rekordbox" / "exportLibrary.db"),
                                                           false);
                raw.exec("PRAGMA key = '" + infrastructure::onelibrary::deriveOneLibraryKey() + "';");
                raw.exec("UPDATE content SET analysisDataFilePath = '" + ownDat + "' WHERE content_id = " + one.sourceId
                         + ";");
            }
            std::vector<domain::CuePoint> cues = one.cues;
            const double offBeatAt = hotAt + 700.0;
            cues.push_back(domain::CuePoint{domain::CuePoint::Kind::Memory, 0, offBeatAt, "", ""});
            {
                infrastructure::onelibrary::OneLibraryCueWriter writer(pathToUtf8(stick.pioneer));
                writer.writeCuesForPath(pathToUtf8(onStick / "o split.wav"), cues);
                writer.finishWriting();
            }
            LibraryCatalogCache::instance().invalidateEveryCatalogOn(pathToUtf8(stick.root));
            assert(rowsOf("o split.wav")[1].cues.size() == 2 && rowsOf("o split.wav")[0].cues.size() == 1);
            writeBeatChordWav(onStick / "o split.wav", 44, 120.0, aMinor, 0.29);
            analyseAgainOn(QStringLiteral("Split"));
            assert(controller.lastReanalysis().value("cuesToMove").toInt() == 1);
            controller.setMoveCuesForAll(true);
            const std::size_t from = logSize();
            const QVariantMap summary = saveNow();
            assert(summary.value("written").toInt() == 1);
            assert(logSince(from).find("DeviceLibrary and OneLibrary hold different cue points") != std::string::npos);
            assert(summary.value("notes").toString().contains(
                QLatin1String("The cue points of one track analysed again were left where they are")));
            const std::vector<domain::Track> after = rowsOf("o split.wav");
            for (const domain::Track &row : after) {
                assert(std::abs(cueAt(row, true) - hotAt) < 1.0);
            }
            assert(after[1].cues.size() == 2 && std::abs(cueAt(after[1], false) - offBeatAt) < 1.0);
            std::cout << "case 16n (a cue only OneLibrary holds: nothing moves, and it is still there) OK\n";
        }

        // 16o. A 48 kHz track whose Engine row the player has not analysed
        //      (no rate stored) holds a hot cue on a beat: read at the
        //      file's rate it is the cue rekordbox holds, and moves with it.
        {
            writeBeatChordWav(computer / "q 48k.wav", 45, 120.0, aMinor, 0.25, 48000);
            assert(offer(QVariantList{pathToQString(computer / "q 48k.wav")}, QStringLiteral("Fortyeight"), true));
            assert(saveNow().value("error").toString().isEmpty());
            std::vector<domain::Track> rows = rowsOf("q 48k.wav");
            const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), rows[0].sourceId);
            const double hotAt = std::round(grid[8].timeMs);
            stageCue(QStringLiteral("rekordbox"), rows[0], hotAt, true, 1);
            assert(saveNow().value("written").toInt() == 1);
            // Engine's row as the player leaves one it has not analysed,
            // and its hot cue put there at the file's own rate.
            const std::string mdb = pathToUtf8(stick.engine / "Database2" / "m.db");
            assert(testing::sqlExec(mdb, "UPDATE Track SET isAnalyzed = 0 WHERE id = " + rows[2].sourceId));
            assert(testing::sqlExec(mdb, "UPDATE PerformanceData SET trackData = NULL, beatData = NULL, "
                                         "overviewWaveFormData = NULL WHERE trackId = " + rows[2].sourceId));
            {
                infrastructure::engine::LibdjinteropEngineCueWriter writer(stick.engineUtf8());
                writer.writeHotCuesAtSampleRate(
                    rows[2].sourceId, {domain::CuePoint{domain::CuePoint::Kind::Hot, 1, hotAt, "", ""}}, 48000.0);
            }
            LibraryCatalogCache::instance().invalidateEveryCatalogOn(pathToUtf8(stick.root));
            writeBeatChordWav(onStick / "q 48k.wav", 45, 120.0, aMinor, 0.29, 48000);
            analyseAgainOn(QStringLiteral("Fortyeight"));
            assert(controller.lastReanalysis().value("cuesToMove").toInt() == 1);
            controller.setMoveCuesForAll(true);
            const QVariantMap summary = saveNow();
            std::cerr << "  notes: " << summary.value("notes").toString().toStdString() << std::endl;
            assert(summary.value("written").toInt() == 1);
            for (const domain::Track &row : rowsOf("q 48k.wav")) {
                std::cerr << "  q 48k " << row.format << ": hot " << cueAt(row, true) << " (was " << hotAt << ")"
                          << std::endl;
                assert(std::abs(cueAt(row, true) - (hotAt + 40.0)) < 4.0);
            }
            std::cout << "case 16o (a 48 kHz row the player has not analysed: its cue read at the file's rate, and "
                         "moved with the others) OK\n";
        }

        // 16p. A cancel landing inside an Analyse again change stops the
        //      save after it, as after any change: what landed stays, and
        //      nothing is called a failure.
        {
            const auto onBeat = cuedTracks({{"w one.wav", 46}, {"w two.wav", 47}, {"w three.wav", 48}},
                                           QStringLiteral("Cancel"));
            for (const auto &[name, seconds] :
                 std::vector<std::pair<std::string, int>>{{"w one.wav", 46}, {"w two.wav", 47}, {"w three.wav", 48}}) {
                writeBeatChordWav(onStick / name, seconds, 120.0, aMinor, 0.29);
            }
            analyseAgainOn(QStringLiteral("Cancel"));
            controller.setMoveCuesForAll(true);
            bool asked = false;
            ReanalyseTrackChange::setStepHookForTesting([&](const std::string &step) {
                if (!asked && step.rfind("engine-grid:", 0) == 0) {
                    asked = true;
                    QMetaObject::invokeMethod(session, "cancelWrite", Qt::BlockingQueuedConnection);
                }
            });
            const QVariantMap summary = saveNow();
            ReanalyseTrackChange::setStepHookForTesting({});
            std::cerr << "  cancelled: written " << summary.value("written").toInt() << ", error '"
                      << summary.value("error").toString().toStdString() << "'" << std::endl;
            assert(asked && summary.value("cancelled").toBool() && summary.value("error").toString().isEmpty());
            assert(summary.value("written").toInt() == 1);
            assert(std::abs(cueAt(rowsOf("w one.wav")[0], true) - (onBeat.at("w one.wav") + 40.0)) < 4.0);
            controller.dropReanalysis(QStringLiteral("Cancel"));
            std::cout << "case 16p (a cancel inside a change: that change lands, the rest wait, no failure) OK\n";
        }

        // 16q. The threshold: two tracks, their audio replaced by the same
        //      music about 5 ms and 40 ms later. Under the default 10 ms
        //      the 5 ms track's cue follows on its own (its switch on,
        //      counted apart), the 40 ms one is asked (off). Apply to all
        //      at 50 turns both on, at 0 both off, each switch told and
        //      the list not made again; the threshold is kept and the next
        //      run takes it. Saved at 10: the 5 ms track's cue moved in all
        //      three libraries and the log says it moved on its own, the
        //      40 ms track's cue where it was.
        {
            assert(controller.cueFollowThresholdMs() == DefaultCueFollowThresholdMs);
            const std::vector<std::pair<std::string, double>> tracks{{"q near.wav", 0.255}, {"q far.wav", 0.29}};
            QVariantList files;
            int seconds = 51;
            for (const auto &[name, firstBeat] : tracks) {
                (void)firstBeat;
                writeBeatChordWav(computer / name, seconds++, 120.0, aMinor);
                files.push_back(pathToQString(computer / name));
            }
            assert(offer(files, QStringLiteral("Threshold"), true));
            assert(controller.lastResult().value("staged").toInt() == 2);
            assert(saveNow().value("error").toString().isEmpty());
            std::map<std::string, double> onBeat;
            for (const auto &[name, firstBeat] : tracks) {
                (void)firstBeat;
                const std::vector<domain::Track> rows = rowsOf(name);
                const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), rows[0].sourceId);
                onBeat[name] = std::round(grid[8].timeMs);
                stageCue(QStringLiteral("rekordbox"), rows[0], onBeat[name], true, 1);
                stageCue(QStringLiteral("engine"), rows[2], onBeat[name], true, 1);
            }
            assert(saveNow().value("written").toInt() == 4);
            seconds = 51;
            for (const auto &[name, firstBeat] : tracks) {
                writeBeatChordWav(onStick / name, seconds++, 120.0, aMinor, firstBeat);
            }
            analyseAgainOn(QStringLiteral("Threshold"));
            QVariantMap again = controller.lastReanalysis();
            std::cerr << "  threshold " << again.value("cueFollowThresholdMs").toInt() << ": "
                      << again.value("autoCueTracks").toInt() << " on their own (" << again.value("autoCues").toInt()
                      << " cues), " << again.value("askedCueTracks").toInt() << " asked ("
                      << again.value("askedCues").toInt() << " cues)" << std::endl;
            assert(again.value("cueFollowThresholdMs").toInt() == 10);
            assert(again.value("cueTracks").toInt() == 2 && again.value("cuesToMove").toInt() == 2);
            assert(again.value("autoCueTracks").toInt() == 1 && again.value("autoCues").toInt() == 1);
            assert(again.value("askedCueTracks").toInt() == 1 && again.value("askedCues").toInt() == 1);
            QString nearId;
            QString farId;
            double nearMove = 0.0;
            for (const QVariant &entry : controller.cueReview()) {
                const QVariantMap track = entry.toMap();
                const double largest = track.value("largestMoveMs").toDouble();
                std::cerr << "  " << track.value("title").toString().toStdString() << ": largest move " << largest
                          << " ms, switch " << track.value("moveCues").toBool() << std::endl;
                if (track.value("title").toString().startsWith(QLatin1String("q near"))) {
                    nearId = track.value("id").toString();
                    nearMove = largest;
                    assert(largest >= 1.0 && largest < 10.0 && track.value("moveCues").toBool());
                } else if (track.value("title").toString().startsWith(QLatin1String("q far"))) {
                    farId = track.value("id").toString();
                    assert(std::abs(largest - 40.0) <= 3.0 && !track.value("moveCues").toBool());
                }
            }
            assert(!nearId.isEmpty() && !farId.isEmpty());
            assert(controller.movesCues(nearId) && !controller.movesCues(farId));
            const auto storedThreshold = [] { return storedCueFollowThresholdMs(openSeabassSettings()); };
            {
                QSignalSpy rebuilt(&controller, &TrackImportController::cueReviewChanged);
                QSignalSpy choice(&controller, &TrackImportController::moveCuesChanged);
                QSignalSpy threshold(&controller, &TrackImportController::cueFollowThresholdChanged);
                controller.applyCueThreshold(50);
                assert(controller.movesCues(nearId) && controller.movesCues(farId));
                assert(choice.count() == 1 && choice.at(0).at(0).toString() == farId && choice.at(0).at(1).toBool());
                assert(threshold.count() == 1 && controller.cueFollowThresholdMs() == 50 && storedThreshold() == 50);
                controller.applyCueThreshold(0);
                assert(!controller.movesCues(nearId) && !controller.movesCues(farId));
                assert(choice.count() == 3 && storedThreshold() == 0);
                assert(rebuilt.count() == 0);
            }
            // Out of range is taken to its end.
            controller.applyCueThreshold(500);
            assert(storedThreshold() == MaxCueFollowThresholdMs);
            // The next run takes the threshold kept: at 50, both follow on
            // their own.
            controller.applyCueThreshold(50);
            analyseAgainOn(QStringLiteral("Threshold"));
            again = controller.lastReanalysis();
            assert(again.value("cueFollowThresholdMs").toInt() == 50);
            assert(again.value("autoCueTracks").toInt() == 2 && again.value("askedCueTracks").toInt() == 0);
            assert(controller.movesCues(nearId) && controller.movesCues(farId));
            controller.applyCueThreshold(DefaultCueFollowThresholdMs);
            assert(controller.movesCues(nearId) && !controller.movesCues(farId));

            const std::size_t logFrom = logSize();
            assert(saveNow().value("written").toInt() == 2);
            const char *libraries[] = {"DeviceLibrary", "OneLibrary", "Engine"};
            for (const auto &[name, firstBeat] : tracks) {
                (void)firstBeat;
                const bool near = name == "q near.wav";
                const std::vector<domain::Track> rows = rowsOf(name);
                assert(rows.size() == 3);
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    const double hot = cueAt(rows[i], true);
                    std::cerr << "  " << name << " " << libraries[i] << ": hot cue " << onBeat[name] << " to " << hot
                              << std::endl;
                    // The rekordbox side keeps whole ms.
                    assert(std::abs(hot - (onBeat[name] + (near ? nearMove : 0.0))) < 1.5);
                    assert(!near || hot - onBeat[name] >= 1.0);
                }
            }
            const std::string log = logSince(logFrom);
            const auto lineOf = [&](const std::string &title) {
                const auto at = log.find("track-reanalysis: analysed \"" + title + "\"");
                assert(at != std::string::npos);
                return log.substr(at, log.find('\n', at) - at);
            };
            const std::string nearLine = lineOf("q near");
            std::cerr << "  q near: ..." << nearLine.substr(nearLine.find("; its cue points"), 220) << std::endl;
            assert(nearLine.find("its cue points moved on their own, every move under 10 ms") != std::string::npos);
            assert(nearLine.find("moved Hot cue 1 from " + ReanalyseTrackChange::clockText(onBeat["q near.wav"]).toStdString())
                   != std::string::npos);
            assert(lineOf("q far").find("on their own") == std::string::npos);
            assert(lineOf("q far").find("moved Hot cue 1") == std::string::npos);
            std::cout << "case 16q (the threshold: a 5 ms track follows on its own and says so in the log, a 40 ms one "
                         "is asked; Apply to all at 50 and 0, kept for the next run) OK\n";
        }

        // Tracks imported into `playlist` and saved, each given hot cue 1 on
        // its ninth beat in all three libraries, then their audio on the
        // stick replaced by the same music with its first beat at the
        // given second (0.25 before) and analysed again. The review's ids
        // by name, and the cue's place before.
        struct ShiftedTrack
        {
            std::string name;
            int seconds = 0;
            double firstBeat = 0.25;
        };
        const auto shiftedTracks = [&](const std::vector<ShiftedTrack> &tracks, const QString &playlist) {
            QVariantList files;
            for (const ShiftedTrack &track : tracks) {
                writeBeatChordWav(computer / track.name, track.seconds, 120.0, aMinor);
                files.push_back(pathToQString(computer / track.name));
            }
            assert(offer(files, playlist, true));
            assert(controller.lastResult().value("staged").toInt() == static_cast<int>(tracks.size()));
            assert(saveNow().value("error").toString().isEmpty());
            std::map<std::string, std::pair<QString, double>> placed;
            for (const ShiftedTrack &track : tracks) {
                const std::vector<domain::Track> rows = rowsOf(track.name);
                const auto grid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), rows[0].sourceId);
                placed[track.name].second = std::round(grid[8].timeMs);
                stageCue(QStringLiteral("rekordbox"), rows[0], placed[track.name].second, true, 1);
                stageCue(QStringLiteral("engine"), rows[2], placed[track.name].second, true, 1);
            }
            assert(saveNow().value("written").toInt() == 2 * static_cast<int>(tracks.size()));
            for (const ShiftedTrack &track : tracks) {
                writeBeatChordWav(onStick / track.name, track.seconds, 120.0, aMinor, track.firstBeat);
            }
            analyseAgainOn(playlist);
            for (const QVariant &entry : controller.cueReview()) {
                const QVariantMap review = entry.toMap();
                for (const ShiftedTrack &track : tracks) {
                    const std::string stem = track.name.substr(0, track.name.size() - 4);
                    if (review.value("title").toString().startsWith(QString::fromStdString(stem))) {
                        placed[track.name].first = review.value("id").toString();
                        std::cerr << "  " << stem << ": largest move " << review.value("largestMoveMs").toDouble()
                                  << " ms" << std::endl;
                    }
                }
            }
            for (const ShiftedTrack &track : tracks) {
                assert(!placed[track.name].first.isEmpty());
            }
            return placed;
        };
        const auto logLineOf = [](const std::string &log, const std::string &title) {
            const auto at = log.find("track-reanalysis: analysed \"" + title + "\"");
            assert(at != std::string::npos);
            return log.substr(at, log.find('\n', at) - at);
        };

        // 16r. The person's own switches. Two tracks as in 16q, about 5 ms
        //      and 40 ms, at the threshold of 10: the near one switched off
        //      by hand, the far one on. Apply to all leaves both as the
        //      person set them, at 50, 0 and 10, and says how many kept a
        //      choice the threshold would have turned; Move none and Move
        //      all reach them all the same, and are the person's choice
        //      too. While the save writes, every switch change is refused.
        //      Saved: the near track's cue exactly where it was in all
        //      three libraries and its log line without "on their own";
        //      the far one moved, by the person's choice.
        {
            assert(controller.cueFollowThresholdMs() == DefaultCueFollowThresholdMs);
            const auto placed = shiftedTracks({{"x near.wav", 71, 0.255}, {"x far.wav", 72, 0.29}}, QStringLiteral("Hand"));
            const QString nearId = placed.at("x near.wav").first;
            const QString farId = placed.at("x far.wav").first;
            assert(controller.movesCues(nearId) && !controller.movesCues(farId));
            assert(controller.setMoveCues(nearId, false) && controller.setMoveCues(farId, true));
            {
                QSignalSpy choice(&controller, &TrackImportController::moveCuesChanged);
                // At 50 the threshold would turn the near one on, at 0 the
                // far one off, at 10 both.
                assert(controller.applyCueThreshold(50) == 1);
                assert(!controller.movesCues(nearId) && controller.movesCues(farId));
                assert(controller.applyCueThreshold(0) == 1);
                assert(!controller.movesCues(nearId) && controller.movesCues(farId));
                assert(controller.applyCueThreshold(DefaultCueFollowThresholdMs) == 2);
                assert(!controller.movesCues(nearId) && controller.movesCues(farId));
                assert(choice.count() == 0);
            }
            assert(controller.setMoveCuesForAll(false));
            assert(!controller.movesCues(nearId) && !controller.movesCues(farId));
            assert(controller.setMoveCuesForAll(true));
            assert(controller.movesCues(nearId) && controller.movesCues(farId));
            // Move all was the person's: at 10 the far one stays on.
            assert(controller.applyCueThreshold(DefaultCueFollowThresholdMs) == 1);
            assert(controller.movesCues(nearId) && controller.movesCues(farId));
            assert(controller.setMoveCues(nearId, false));

            const std::vector<domain::Track> before = rowsOf("x near.wav");
            assert(before.size() == 3);
            // While the save writes (inside its first change, on the GUI
            // thread as the review would): every change refused, nothing
            // turned, the threshold kept.
            bool tried = false;
            bool refused = false;
            ReanalyseTrackChange::setStepHookForTesting([&](const std::string &step) {
                if (tried || step.rfind("engine-grid:", 0) != 0) {
                    return;
                }
                tried = true;
                QMetaObject::invokeMethod(
                    &controller,
                    [&] {
                        refused = controller.saveWriting() && !controller.setMoveCues(nearId, true)
                                  && !controller.setMoveCuesForAll(true) && controller.applyCueThreshold(50) == -1
                                  && !controller.movesCues(nearId) && controller.movesCues(farId)
                                  && controller.cueFollowThresholdMs() == DefaultCueFollowThresholdMs;
                    },
                    Qt::BlockingQueuedConnection);
            });
            const std::size_t logFrom = logSize();
            const QVariantMap saved = saveNow();
            ReanalyseTrackChange::setStepHookForTesting({});
            assert(tried && refused);
            assert(!controller.saveWriting());
            assert(saved.value("written").toInt() == 2);
            const char *libraries[] = {"DeviceLibrary", "OneLibrary", "Engine"};
            const std::vector<domain::Track> after = rowsOf("x near.wav");
            assert(after.size() == 3);
            for (std::size_t i = 0; i < after.size(); ++i) {
                std::cerr << "  x near " << libraries[i] << ": hot cue " << cueAt(before[i], true) << " to "
                          << cueAt(after[i], true) << std::endl;
                assert(!cuesOf(before[i]).empty() && cuesOf(after[i]) == cuesOf(before[i]));
            }
            const std::vector<domain::Track> far = rowsOf("x far.wav");
            assert(far.size() == 3);
            for (const domain::Track &row : far) {
                assert(std::abs(cueAt(row, true) - (placed.at("x far.wav").second + 40.0)) < 4.0);
            }
            const std::string log = logSince(logFrom);
            const std::string nearLine = logLineOf(log, "x near");
            assert(nearLine.find("on their own") == std::string::npos);
            assert(nearLine.find("moved Hot cue") == std::string::npos);
            const std::string farLine = logLineOf(log, "x far");
            assert(farLine.find("moved Hot cue 1") != std::string::npos);
            assert(farLine.find("on their own") == std::string::npos);
            std::cout << "case 16r (switches set by hand: Apply to all leaves them and counts them, Move all and Move "
                         "none reach them; refused while the save writes; a small track switched off keeps its cues "
                         "in all three libraries) OK\n";
        }

        // 16s. A track the threshold switched on, then Move all: the move is
        //      the person's, and the log does not say it moved on its own.
        {
            const auto placed = shiftedTracks({{"y near.wav", 73, 0.255}}, QStringLiteral("Mine"));
            const QString nearId = placed.at("y near.wav").first;
            assert(controller.movesCues(nearId));
            assert(controller.setMoveCuesForAll(true));
            const std::size_t logFrom = logSize();
            assert(saveNow().value("written").toInt() == 1);
            const std::string line = logLineOf(logSince(logFrom), "y near");
            std::cerr << "  y near: ..." << line.substr(line.find("; its cue points") == std::string::npos ? 0 : line.find("; its cue points"), 160)
                      << std::endl;
            assert(line.find("moved Hot cue 1") != std::string::npos);
            assert(line.find("on their own") == std::string::npos);
            std::cout << "case 16s (a track the threshold switched on, then Move all: moved, and not said to have "
                         "moved on its own) OK\n";
        }

        // 16t. Near the track's start rekordbox keeps whole ms and Engine
        //      samples, so a grid moving its first beat from 2.2 ms to the
        //      start meets each library's copy of a cue differently: an
        //      intro loop over four beats at 2 ms in rekordbox and 2.4 ms in
        //      Engine (one start held at 0, the other not), and a hot cue
        //      at 1 ms in rekordbox and 0.8 ms in Engine (one a move of
        //      1 ms, the other under the minimum). No analyser puts a beat
        //      that near the start of a test file, so the review's two
        //      grids are set by hand: 120 BPM from 2.2 ms, and from 0. The
        //      save refuses nothing, and the cues move in all three
        //      libraries.
        {
            writeBeatChordWav(computer / "z start.wav", 49, 120.0, aMinor);
            assert(offer(QVariantList{pathToQString(computer / "z start.wav")}, QStringLiteral("Start"), true));
            assert(saveNow().value("error").toString().isEmpty());
            std::vector<domain::Track> rows = rowsOf("z start.wav");
            const double first = 2.0;
            const double fourth = 2002.0;
            const auto stageLoop = [&](const QString &format, const domain::Track &row, double from, double to) {
                const QString path = format == QLatin1String("engine") ? engine : pioneer;
                assert(session->stage(std::make_unique<AddCueChange>(
                    format, path, QString::fromStdString(row.sourceId), from, QStringLiteral("hot"), 1,
                    QStringLiteral("#00FF00"), QStringLiteral("intro"), true, to, QStringLiteral("t"))));
            };
            stageLoop(QStringLiteral("rekordbox"), rows[0], first, fourth);
            stageLoop(QStringLiteral("engine"), rows[2], first + 0.4, fourth + 0.4);
            stageCue(QStringLiteral("rekordbox"), rows[0], 1.0, true, 2);
            stageCue(QStringLiteral("engine"), rows[2], 0.8, true, 2);
            assert(saveNow().value("written").toInt() == 4);
            const std::vector<domain::Track> before = rowsOf("z start.wav");
            const auto padOf = [](const domain::Track &row, int pad) {
                for (const auto &cue : row.cues) {
                    if (cue.kind == domain::CuePoint::Kind::Hot && cue.hotCueNumber == pad) {
                        return cue;
                    }
                }
                assert(false && "the pad is set");
                return domain::CuePoint{};
            };
            for (const domain::Track &row : before) {
                std::cerr << "  z start " << row.format << ": loop " << padOf(row, 1).positionMs << " to "
                          << padOf(row, 1).loopEndMs << ", hot cue 2 at " << padOf(row, 2).positionMs << std::endl;
            }

            writeBeatChordWav(onStick / "z start.wav", 49, 120.0, aMinor, 0.29);
            analyseAgainOn(QStringLiteral("Start"));
            std::shared_ptr<ReanalyseTrackChange> change;
            for (const auto &pending : session->pendingChanges()) {
                if (auto again = std::dynamic_pointer_cast<ReanalyseTrackChange>(pending);
                    again && again->subject().startsWith(QLatin1String("z start"))) {
                    change = again;
                }
            }
            assert(change);
            ReanalyseTrackChange::CueReview review = change->cueReview();
            const double shift = -2.2;
            review.oldBeats.clear();
            review.newBeats.clear();
            for (int i = 0; i < 90; ++i) {
                review.oldBeats.push_back(2.2 + i * 500.0);
                review.newBeats.push_back(i * 500.0);
            }
            // The cues the review read from the stick are the ones placed.
            assert(review.cues.size() == 2);
            review.plan = application::planCueGridMoves(review.cues, review.oldBeats, review.newBeats);
            assert(review.plan.verdict == application::CueGridPlan::Verdict::Moves);
            change->setCueReview(review);
            change->setMoveCues(true);
            const std::size_t from = logSize();
            const QVariantMap summary = saveNow();
            const std::string log = logSince(from);
            assert(summary.value("written").toInt() == 1);
            assert(log.find("holds other cue points than the review showed") == std::string::npos
                   && "no library is taken for holding other cues");
            assert(!summary.value("notes").toString().contains(QLatin1String("were left where they are")));
            const std::vector<domain::Track> after = rowsOf("z start.wav");
            const char *libraries[] = {"DeviceLibrary", "OneLibrary", "Engine"};
            for (std::size_t i = 0; i < after.size(); ++i) {
                const domain::CuePoint loopWas = padOf(before[i], 1);
                const domain::CuePoint loopNow = padOf(after[i], 1);
                const domain::CuePoint hotNow = padOf(after[i], 2);
                std::cerr << "  z start " << libraries[i] << ": loop " << loopNow.positionMs << " to " << loopNow.loopEndMs
                          << ", hot cue 2 at " << hotNow.positionMs << std::endl;
                // The loop's start at the track's start, or 0.2 ms after it
                // in Engine; its end on its beat, moved by the grid's amount
                // (rekordbox's whole ms cut it by up to 1 ms more).
                assert(loopNow.positionMs >= 0.0 && loopNow.positionMs < 0.5);
                assert(loopNow.positionMs < loopWas.positionMs - 0.5 && "the loop moved");
                assert(std::abs(loopNow.loopEndMs - (loopWas.loopEndMs + shift)) < 1.0 + 1e-6);
                // Hot cue 2 at the start, within the millisecond rekordbox
                // keeps, in every library.
                assert(hotNow.positionMs >= 0.0 && hotNow.positionMs < 1.0);
            }
            assert(std::abs(padOf(after[0], 2).positionMs) < 1e-6 && "rekordbox's hot cue 2 moved to the start");
            std::cout << "case 16t (near the start, a loop held at 0 in one library and not in another, and a cue "
                         "moved in one and under a millisecond in another: the cues move in all three libraries) OK\n";
        }
    }


    // 17. The review's strip on one timeline: an MP3's analysis is on the
    //     decoder's, 26 ms earlier than rekordbox's where grid and cues
    //     are. A kick the decoder hears at 2.000 s sits under the new
    //     grid's beat at 2.026 s in the strip, the cue moved onto it.
    {
        auto analysis = std::make_shared<ImportAnalysis>();
        analysis->waveform.sampleRateHz = 44100;
        analysis->waveform.sampleCount = 10 * 44100;
        analysis->waveform.entries.assign(1500, domain::WaveformEntry{});
        analysis->waveform.entries[300] = {1.0f, 1.0f, 1.0f, 1.0f};  // 2.000 s as decoded
        for (int i = 0; i < 20; ++i) {
            analysis->beats.push_back({i * 500.0, i % 4 + 1});
        }
        analysis->bpm = 120.0;
        ReanalyseTrackChange change(pathToUtf8(stick.root / "Contents" / "x.mp3"), "/Contents/x.mp3", pathToQString(stick.root),
                                    pioneer, QString(), {}, analysis, {}, QStringLiteral("x"), QString(), 1);
        ReanalyseTrackChange::CueReview review;
        const double offset = 26.0;
        for (int i = 0; i < 20; ++i) {
            review.oldBeats.push_back(i * 500.0);           // the old grid, on rekordbox's timeline
            review.newBeats.push_back(i * 500.0 + offset);  // the analysis's, moved onto it
        }
        domain::CuePoint cue;
        cue.kind = domain::CuePoint::Kind::Hot;
        cue.hotCueNumber = 3;
        cue.positionMs = 2000.0;
        review.cues = {cue};
        review.plan = application::planCueGridMoves(review.cues, review.oldBeats, review.newBeats);
        review.waveformOffsetMs = offset;
        change.setCueReview(review);
        assert(review.plan.moves.size() == 1 && std::abs(review.plan.moves[0].newMs - 2026.0) < 1e-6);
        const QVariantMap strip = change.reviewStrip(review.plan.moves[0]);
        const QVariantList columns = strip.value("columns").toList();
        int loudest = -1;
        for (int i = 0; i < columns.size(); ++i) {
            if (columns[i].toMap().value("low").toDouble() > 0.5) {
                loudest = i;
            }
        }
        assert(loudest >= 0);
        const double columnMs = 1000.0 / 150.0;
        const double kickInStrip = loudest * columnMs;
        const double newCue = strip.value("newCueMs").toDouble();
        std::cerr << "  strip: the kick at " << kickInStrip << " ms, the new cue at " << newCue << " ms, the old at "
                  << strip.value("oldCueMs").toDouble() << std::endl;
        assert(std::abs(kickInStrip - newCue) < columnMs);
        assert(std::abs(newCue - strip.value("oldCueMs").toDouble() - offset) < 1e-6);
        bool onBeat = false;
        for (const QVariant &beat : strip.value("newBeats").toList()) {
            onBeat = onBeat || std::abs(beat.toDouble() - newCue) < 1e-6;
        }
        assert(onBeat && std::abs(change.gridShiftMs() - offset) < 1e-6);
        std::cout << "case 17 (the strip of an MP3: waveform, grids and cues on rekordbox's timeline, the kick under "
                     "the new beat) OK\n";
    }

    // 18. The analysis ledger (<stick>/Seabass/analysis.tsv.gz). An import
    //     records each track it analysed: the analyser's revisions, the
    //     tempo range, the file on the stick, and the grids the libraries
    //     hold afterwards, each fingerprint compared with the library's own.
    //     Analyse again updates the entry; a track it leaves alone gets
    //     none; a save that goes back, and Undo Last Save, leave the file as
    //     it was before; a damaged ledger is replaced by the next save, and
    //     one that cannot be read is left as it is, a warning on a save that
    //     landed, and so is one that can be read and not written. A grid
    //     that cannot be read back is "-", never a failed save; an import
    //     whose source was replaced after its analysis (another size, or
    //     a time more than 2 s from the one analysed) records nothing.
    //     Each entry holds the grid's confidence, whether it follows the
    //     tempo and the key's margin as the analyser reported them.
    {
        namespace local = infrastructure::local;
        const fs::path ledgerFile = infrastructure::paths::stickAnalysisLedger(stick.root);
        const fs::path onStick = stick.root / "Contents" / "UnknownArtist" / "UnknownAlbum";
        const auto bytesOf = [](const fs::path &file) {
            std::ifstream in(file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), {});
        };
        const auto readLedger = [&]() {
            std::string error;
            const local::AnalysisLedger ledger = local::readAnalysisLedger(stick.root, &error);
            if (!error.empty()) {
                std::cerr << "  ledger: " << error << std::endl;
            }
            assert(error.empty());
            return ledger;
        };
        const auto keyOf = [&](const std::string &name) {
            return application::normalizedPathKey("Contents/UnknownArtist/UnknownAlbum/" + name);
        };
        const auto saveNow = [&]() {
            finished.clear();
            session->save();
            assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
            if (!session->lastSummary().value("error").toString().isEmpty()) {
                std::cerr << "save: " << session->lastSummary().value("error").toString().toStdString() << "\n";
            }
            return session->lastSummary();
        };
        const auto undoNow = [&]() {
            finished.clear();
            session->undoLastSave();
            assert(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }));
        };
        const auto analyseAgainOn = [&](const QString &playlist) {
            assert(controller.analyseAgain(pioneer, engine, playlist));
            assert(waitFor(&controller, &TrackImportController::busyChanged, [&] { return !controller.busy(); }));
            assert(controller.errorMessage().isEmpty());
            return controller.lastReanalysis();
        };
        const auto rowOf = [](auto &&reader, const std::string &name) {
            for (const auto &track : reader.readAll()) {
                if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == name) {
                    return track;
                }
            }
            assert(false && "the track is in the library");
            return domain::Track{};
        };
        const auto logText = [&]() { return bytesOf(stick.root / "Seabass" / "seabass.log"); };
        // The entry against the stick and the libraries as they are now.
        const auto checkEntry = [&](const std::string &name, const local::AnalysisLedgerEntry &entry) {
            const fs::path file = onStick / name;
            const musicanalyze::Revisions revisions = musicanalyze::revisions();
            assert(entry.pathKey == keyOf(name));
            assert(entry.inputRevision == infrastructure::audio::AnalysisInputRevision);
            assert(entry.gridRevision == revisions.grid && entry.keyRevision == revisions.key
                   && entry.waveformRevision == revisions.waveform);
            assert(entry.libraryVersion == musicanalyze::version());
            QSettings settings = openSeabassSettings();
            const domain::TempoRange range = musicanalyze::usableTempoRange(storedAnalysisTempoRange(settings));
            std::cerr << "  " << name << ": tempo " << entry.tempoMin << " to " << entry.tempoMax << ", key '" << entry.key
                      << "' written " << entry.keyWritten << std::endl;
            assert(entry.tempoMin == local::formatTempoBpm(range.lowBpm)
                   && entry.tempoMax == local::formatTempoBpm(range.highBpm));
            assert(entry.fileSize == fs::file_size(file));
            assert(entry.fileMtimeUnix == infrastructure::stick_backup::toUnixSeconds(fs::last_write_time(file)));
            const domain::Track pdb = rowOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), name);
            const domain::Track engineRow = rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), name);
            assert(!pdb.analysisFile.empty());
            const std::string rekordboxNow = infrastructure::rekordbox::pqtzGridFingerprint(
                infrastructure::rekordbox::AnlzFile::readRaw(pathToUtf8(stick.root / pathFromUtf8(pdb.analysisFile.substr(1)))));
            const std::string engineNow =
                infrastructure::engine::LibdjinteropEngineCueWriter(stick.engineUtf8()).gridFingerprintOf(engineRow.sourceId);
            assert(!rekordboxNow.empty() && entry.rekordboxGrid == rekordboxNow);
            assert(!engineNow.empty() && entry.engineGrid == engineNow);
            assert(entry.keyWritten == (!entry.key.empty() && pdb.key == entry.key));
            assert(entry.analysedAtUnix > 0);
            // How sure the analyser was, as it reported it for this file:
            // the same file analysed again gives the same numbers.
            const auto heard = analyseForImport(pathToUtf8(file), application::CancellationToken{}, range);
            assert(heard && heard->provenance);
            std::cerr << "  " << name << ": grid confidence " << entry.gridConfidence << " (analyser "
                      << heard->confidence << "), follows tempo " << entry.gridFollowsTempo << ", key margin '"
                      << entry.keyMargin << "' (analyser " << heard->keyMargin << ")" << std::endl;
            assert(!heard->beats.empty() && "both ledger tracks have a grid");
            assert(!entry.gridConfidence.empty() && entry.gridConfidence == local::formatGridConfidence(heard->confidence));
            assert(entry.gridFollowsTempo == (heard->followsTempo ? "1" : "0"));
            assert(entry.key == heard->key);
            assert(entry.key.empty() ? entry.keyMargin.empty()
                                     : !entry.keyMargin.empty() && entry.keyMargin == local::formatKeyMargin(heard->keyMargin));
        };

        // 18a. The import. v one has a key to hear, and no tag naming one:
        //      the key goes into the rows and the entry says so.
        const std::vector<double> aMinor{110.0, 130.81, 164.81, 220.0, 261.63, 329.63};
        writeBeatChordWav(computer / "v one.wav", 37, 128.0, aMinor);
        writeBeatWav(computer / "v two.wav", 38, 128.0);
        assert(offer(QVariantList{pathToQString(computer / "v one.wav"), pathToQString(computer / "v two.wav")},
                     QStringLiteral("Ledger"), true));
        assert(controller.lastResult().value("withGrid").toInt() == 2);
        const local::AnalysisLedger beforeImport = readLedger();
        assert(beforeImport.count(keyOf("v one.wav")) == 0);
        assert(saveNow().value("error").toString().isEmpty());
        const local::AnalysisLedger imported = readLedger();
        assert(imported.size() == beforeImport.size() + 2);
        checkEntry("v one.wav", imported.at(keyOf("v one.wav")));
        checkEntry("v two.wav", imported.at(keyOf("v two.wav")));
        assert(!imported.at(keyOf("v one.wav")).key.empty() && imported.at(keyOf("v one.wav")).keyWritten
               && "a key heard and written is recorded as written");
        for (const auto &[key, entry] : beforeImport) {
            assert(imported.at(key) == entry && "the entries of other tracks stay as they were");
        }
        std::cout << "case 18a (an import records each track: revisions, tempo range, size and time, and the grids "
                     "the libraries hold) OK\n";

        // 18b. v two's entry taken out, then its file made longer and v one's
        //      replaced by other music of the same size: Analyse again
        //      stages v one alone. First in a save that goes back (an
        //      import blocked by a folder where its file is to go).
        {
            local::AnalysisLedger without = imported;
            without.erase(keyOf("v two.wav"));
            std::string error;
            assert(local::writeAnalysisLedger(stick.root, without, [](const std::string &) {}, &error));
        }
        const std::string ledgerBefore = bytesOf(ledgerFile);
        writeBeatWav(onStick / "v one.wav", 37, 140.0);
        writeBeatWav(onStick / "v two.wav", 41, 128.0);
        QVariantMap again = analyseAgainOn(QStringLiteral("Ledger"));
        assert(again.value("analysed").toInt() == 1 && again.value("leftAlone").toInt() == 1);
        {
            writeBeatWav(computer / "w blocked.wav", 42, 120.0);
            ImportRequest request;
            request.offered = {pathToUtf8(computer / "w blocked.wav")};
            request.rekordboxPath = pioneer;
            request.enginePath = engine;
            request.date = "2026-11-26";
            const PreparedImport prepared = prepareTrackImport(request, application::CancellationToken{});
            assert(prepared.refusal.empty() && !prepared.staged.changes.empty());
            assert(session->stageAll(prepared.staged.changes));
            QStringList importIds;
            fs::path destination;
            for (const auto &staged : prepared.staged.changes) {
                importIds << staged->id();
                if (const auto import = std::dynamic_pointer_cast<ImportTrackChange>(staged)) {
                    destination = pathFromUtf8(import->destinationFile());
                }
            }
            assert(!destination.empty());
            fs::create_directories(destination);  // in the way
            const std::size_t logBefore = logText().size();
            const QVariantMap failed = saveNow();
            assert(!failed.value("error").toString().isEmpty());
            // Analyse again was applied before the import failed: its
            // entry was noted, and the ledger as it was is the save going
            // back, not a save that never got that far.
            assert(logText().substr(logBefore).find("track-reanalysis: analysed ") != std::string::npos
                   && "Analyse again was applied in the save that went back");
            assert(bytesOf(ledgerFile) == ledgerBefore && "a save that went back leaves the ledger as it was");
            fs::remove(destination);
            session->unstageAll(importIds);
        }
        std::cout << "case 18b (a save that goes back: the ledger as it was) OK\n";

        // 18c. Saved: v one's entry is the new analysis, v two has none.
        {
            const QVariantMap saved = saveNow();
            assert(saved.value("error").toString().isEmpty() && saved.value("written").toInt() == 1);
            const local::AnalysisLedger after = readLedger();
            assert(after.size() == imported.size() - 1);
            assert(after.count(keyOf("v two.wav")) == 0 && "a track left alone gets no entry");
            const local::AnalysisLedgerEntry &one = after.at(keyOf("v one.wav"));
            checkEntry("v one.wav", one);
            assert(one.rekordboxGrid != imported.at(keyOf("v one.wav")).rekordboxGrid);
            assert(one.engineGrid != imported.at(keyOf("v one.wav")).engineGrid);
            assert(one.analysedAtUnix >= imported.at(keyOf("v one.wav")).analysedAtUnix);
            std::cout << "case 18c (Analyse again: the entry is the new analysis, the track left alone has none) OK\n";
        }

        // 18d. Undo Last Save: the ledger as it was before that save.
        undoNow();
        assert(bytesOf(ledgerFile) == ledgerBefore && "Undo Last Save puts the old ledger back");
        std::cout << "case 18d (Undo Last Save puts the ledger back) OK\n";

        // 18e. A damaged ledger: the save lands, and the ledger is replaced
        //      by one that holds what this save wrote.
        {
            {
                std::ofstream out(ledgerFile, std::ios::binary | std::ios::trunc);
                out << "not a ledger at all";
            }
            again = analyseAgainOn(QStringLiteral("Ledger"));
            assert(again.value("analysed").toInt() == 1);
            const std::size_t logBefore = logText().size();
            const QVariantMap saved = saveNow();
            assert(saved.value("error").toString().isEmpty() && saved.value("written").toInt() == 1);
            const local::AnalysisLedger replaced = readLedger();
            assert(replaced.size() == 1);
            checkEntry("v one.wav", replaced.at(keyOf("v one.wav")));
            assert(logText().substr(logBefore).find("replacing a damaged ledger") != std::string::npos);
            std::cout << "case 18e (a damaged ledger does not fail the save and is replaced) OK\n";
        }

        // 18f. A ledger that cannot be read (a folder where it belongs):
        //      left as it is, not replaced, and the save lands and says so
        //      in plain words.
        {
            fs::remove(ledgerFile);
            fs::create_directories(ledgerFile / "in the way");
            again = analyseAgainOn(QStringLiteral("Ledger"));
            assert(again.value("analysed").toInt() == 1);
            const std::size_t logBefore = logText().size();
            const QVariantMap saved = saveNow();
            const std::string logged = logText().substr(logBefore);
            assert(logged.find("track(s) not recorded, the ledger left as it is") != std::string::npos);
            assert(logged.find("replacing") == std::string::npos && logged.find("not written") == std::string::npos);
            std::cerr << "  warning: " << saved.value("warning").toString().toStdString() << std::endl;
            assert(saved.value("error").toString().isEmpty() && saved.value("written").toInt() == 1);
            assert(saved.value("warning").toString().contains(
                QLatin1String("Your changes are saved, but Seabass could not record which tracks it analysed")));
            assert(saved.value("warning").toString().contains(QLatin1String("seabass-cli analysis-ledger --reset"))
                   && "the warning says what the person can do");
            assert(fs::is_directory(ledgerFile / "in the way"));
            fs::remove_all(ledgerFile);
            std::cout << "case 18f (a ledger that cannot be read: left as it is, the save lands with a warning) OK\n";
        }

        // 18g. Analyse again, the analysis file not readable when the grid
        //      is read back for the ledger: the save lands, the entry
        //      records no rekordbox grid, Engine's as always.
        {
            again = analyseAgainOn(QStringLiteral("Ledger"));
            assert(again.value("analysed").toInt() == 1);
            int asked = 0;
            setBeforeLedgerGridReadForTesting([&asked](const std::string &) {
                ++asked;
                throw std::runtime_error("in the way");
            });
            const std::size_t logBefore = logText().size();
            const QVariantMap saved = saveNow();
            setBeforeLedgerGridReadForTesting({});
            assert(asked > 0);
            assert(saved.value("error").toString().isEmpty() && saved.value("written").toInt() == 1);
            assert(logText().substr(logBefore).find("could not be read for the analysis ledger (in the way)")
                   != std::string::npos);
            const local::AnalysisLedgerEntry &one = readLedger().at(keyOf("v one.wav"));
            assert(one.rekordboxGrid.empty() && !one.engineGrid.empty());
            std::cout << "case 18g (a grid that cannot be read back for the ledger: \"-\", the save lands) OK\n";
        }

        // 18h. A ledger that can be read and not written: a folder where
        //      the atomic write puts its temporary file. The save lands
        //      with a warning that says what to do, and the ledger stays
        //      as it was.
        {
            again = analyseAgainOn(QStringLiteral("Ledger"));
            assert(again.value("analysed").toInt() == 1);
            const std::string before = bytesOf(ledgerFile);
            assert(!before.empty());
            fs::path temporary = ledgerFile;
            temporary += ".tmp-seabass-write";
            fs::create_directories(temporary / "in the way");
            const std::size_t logBefore = logText().size();
            const QVariantMap saved = saveNow();
            fs::remove_all(temporary);
            std::cerr << "  warning: " << saved.value("warning").toString().toStdString() << std::endl;
            assert(saved.value("error").toString().isEmpty() && saved.value("written").toInt() == 1);
            assert(logText().substr(logBefore).find("analysis ledger: not written") != std::string::npos);
            assert(saved.value("warning").toString().contains(
                QLatin1String("Your changes are saved, but Seabass could not record which tracks it analysed")));
            assert(saved.value("warning").toString().contains(QLatin1String("free space")));
            assert(bytesOf(ledgerFile) == before && "a ledger that could not be written stays as it was");
            std::cout << "case 18h (a ledger that can be read and not written: the save lands with a warning, the "
                         "ledger as it was) OK\n";
        }

        // 18i. An import whose source is replaced by a file of another size
        //      after its analysis: the file is copied, and nothing calls
        //      the analysis the copied file's (import_held_rows_test case 4:
        //      none of it is written).
        {
            writeBeatWav(computer / "x swapped.wav", 30, 128.0);
            assert(offer(QVariantList{pathToQString(computer / "x swapped.wav")}, QStringLiteral("Ledger"), false));
            writeBeatWav(computer / "x swapped.wav", 32, 128.0);
            const std::size_t logBefore = logText().size();
            const QVariantMap saved = saveNow();
            assert(saved.value("error").toString().isEmpty());
            assert(fs::file_size(onStick / "x swapped.wav") == fs::file_size(computer / "x swapped.wav"));
            assert(readLedger().count(keyOf("x swapped.wav")) == 0 && "an analysis of another file is not recorded");
            assert(logText().substr(logBefore).find("x swapped.wav is not recorded in the analysis ledger: the file copied")
                   != std::string::npos);
            std::cout << "case 18i (an import whose source was replaced after its analysis: no ledger entry) OK\n";
        }

        // 18j. Sources rewritten at the same size after their analysis: one
        //      modified 10 s later is not recorded, one 1 s later is (FAT
        //      keeps times in 2 s steps).
        {
            writeBeatWav(computer / "x touched.wav", 30, 126.0);
            writeBeatWav(computer / "x close.wav", 31, 126.0);
            assert(offer(QVariantList{pathToQString(computer / "x touched.wav"), pathToQString(computer / "x close.wav")},
                         QStringLiteral("Ledger"), false));
            const auto later = [&](const fs::path &file, int seconds) {
                fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(seconds));
            };
            later(computer / "x touched.wav", 10);
            later(computer / "x close.wav", 1);
            const std::size_t logBefore = logText().size();
            const QVariantMap saved = saveNow();
            assert(saved.value("error").toString().isEmpty());
            const local::AnalysisLedger after = readLedger();
            assert(after.count(keyOf("x touched.wav")) == 0 && "a source modified since its analysis is not recorded");
            assert(after.count(keyOf("x close.wav")) == 1 && "within 2 s it is the file analysed");
            assert(logText().substr(logBefore).find("x touched.wav is not recorded in the analysis ledger: the file "
                                                    "copied")
                   != std::string::npos);
            std::cout << "case 18j (a source rewritten at its size after its analysis: not recorded; within 2 s it "
                         "is) OK\n";
        }
    }

    // 19 to 22, on a stick of their own: the import's playlist changes
    // beside the playlist editor's, the possible duplicates asked for
    // when the start is refused, experimental features switched off, and
    // a controller that goes while a file is analysed.
    {
        const testing::EngineChangeStick fresh = testing::makeEngineChangeStick(fixture, "track_import_controller_fixes");
        testing::levelImportCounter(fresh);
        const QString freshPioneer = fresh.pioneerPath();
        const QString freshEngine = fresh.enginePath();
        LibraryEditSession *freshSession = registry->sessionFor(registry->libraryIdForPath(freshPioneer));
        assert(freshSession && freshSession != session);
        const fs::path in = computer / "fixes";
        const QStringList allThree{QStringLiteral("rekordbox"), QStringLiteral("onelibrary"), QStringLiteral("engine")};
        TrackImportController fixes;
        QSignalSpy fixesResults(&fixes, &TrackImportController::resultReady);
        const auto offerFresh = [&](const QVariantList &files, const QString &playlist, bool create) {
            const int before = fixesResults.count();
            if (!fixes.importFiles(freshPioneer, freshEngine, files, playlist, create)) {
                return false;
            }
            assert(waitFor(&fixes, &TrackImportController::busyChanged, [&] { return !fixes.busy(); }));
            return fixesResults.count() > before;
        };
        const auto saveFresh = [&]() {
            QSignalSpy done(freshSession, &LibraryEditSession::saveFinished);
            freshSession->save();
            assert(waitFor(freshSession, &LibraryEditSession::saveFinished, [&] { return !done.isEmpty(); }));
            return freshSession->lastSummary();
        };
        // Until `done`, with the events going: the worker's progress and
        // its end reach the controller through them.
        const auto waitUntil = [](const std::function<bool()> &done) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!done() && std::chrono::steady_clock::now() < until) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return done();
        };

        // 19. The import's CreatePlaylist and AddToPlaylist changes have
        //     ids of their own: the playlist editor's for the same playlist
        //     and file, staged after them, stand beside them rather than
        //     over them, and the save holds both: the playlist made once,
        //     each file in it once.
        writeWav(in / "f base.wav", 44100, 41);
        assert(offerFresh(QVariantList{pathToQString(in / "f base.wav")}, QString(), false));
        assert(fixes.stagedCount() == 1);
        assert(saveFresh().value("error").toString().isEmpty());
        const fs::path baseOnStick = fresh.root / "Contents" / "UnknownArtist" / "UnknownAlbum" / "f base.wav";
        assert(fs::exists(baseOnStick));
        writeWav(in / "f one.wav", 44100, 42);
        assert(offerFresh(QVariantList{pathToQString(in / "f one.wav"), pathToQString(baseOnStick)},
                          QStringLiteral("Fresh"), true));
        assert(fixes.stagedCount() == 1 && fixes.lastResult().value("addedToPlaylist").toInt() == 1);
        // The playlist, the track, its entry, the entry of the stick's own.
        assert(freshSession->pendingCount() == 4);
        assert(freshSession->stage(std::make_unique<CreatePlaylistChange>(freshPioneer, freshEngine, "Fresh", allThree)));
        assert(freshSession->stage(std::make_unique<AddToPlaylistChange>(freshPioneer, freshEngine, "Fresh",
                                                                         pathToUtf8(baseOnStick),
                                                                         QStringLiteral("f base"), allThree)));
        assert(freshSession->pendingCount() == 6 && "the editor's changes are staged beside the import's");
        assert(fixes.stagedCount() == 1 && fixes.trackCountChange(QStringLiteral("Fresh")) == 2);
        const QVariantMap both = saveFresh();
        if (!both.value("error").toString().isEmpty()) {
            std::cerr << "save: " << both.value("error").toString().toStdString() << "\n";
        }
        assert(both.value("error").toString().isEmpty() && freshSession->pendingCount() == 0);
        const std::vector<std::string> freshOrder{"f one.wav", "f base.wav"};
        assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(fresh.pioneer)), "Fresh") == freshOrder);
        assert(playlistOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(fresh.pioneer)), "Fresh") == freshOrder);
        assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(fresh.engineUtf8()), "Fresh") == freshOrder);
        // Taken back, the import takes its own changes and not the
        // editor's.
        writeWav(in / "f two.wav", 44100, 43);
        assert(offerFresh(QVariantList{pathToQString(in / "f two.wav")}, QStringLiteral("Fresh Two"), true));
        assert(freshSession->stage(std::make_unique<CreatePlaylistChange>(freshPioneer, freshEngine, "Fresh Two", allThree)));
        assert(freshSession->pendingCount() == 4);
        fixes.dropStaged();
        assert(freshSession->pendingCount() == 1 && freshSession->hasChange(CreatePlaylistChange::idFor("Fresh Two")));
        freshSession->discard();
        std::cout << "case 19 (the import's playlist changes and the editor's for the same playlist stand side by side) OK\n";

        // 20. The possible duplicates asked for while other edits wait:
        //     refused, and the batch staged from the offer stays.
        writeWav(in / "other" / "f one.wav", 44100, 44);  // the name and length of one on the stick
        writeWav(in / "f three.wav", 44100, 45);
        assert(offerFresh(QVariantList{pathToQString(in / "f three.wav"), pathToQString(in / "other" / "f one.wav")},
                          QStringLiteral("Fresh"), false));
        if (fixes.lastResult().value("possibleDuplicates").toInt() != 1) {
            for (const QVariant &line : fixes.lastResult().value("notImported").toList()) {
                std::cerr << line.toMap().value("name").toString().toStdString() << ": "
                          << line.toMap().value("why").toString().toStdString() << "\n";
            }
        }
        assert(fixes.stagedCount() == 1 && fixes.lastResult().value("possibleDuplicates").toInt() == 1);
        assert(freshSession->pendingCount() == 2);
        assert(freshSession->stage(std::make_unique<SkippingChange>()));
        assert(!fixes.importPossibleDuplicates());
        assert(fixes.errorMessage().contains(QLatin1String("Save or discard")));
        assert(fixes.stagedCount() == 1 && freshSession->pendingCount() == 3 && "the batch is as it was");
        freshSession->unstage(SkippingChange().id());
        assert(fixes.importPossibleDuplicates());
        assert(waitFor(&fixes, &TrackImportController::busyChanged, [&] { return !fixes.busy(); }));
        assert(fixes.errorMessage().isEmpty());
        assert(fixes.stagedCount() == 2 && freshSession->pendingCount() == 4 && "the batch again, with the duplicate");
        fixes.dropStaged();
        assert(freshSession->pendingCount() == 0);
        std::cout << "case 20 (possible duplicates refused while other edits wait: the batch stays) OK\n";

        // 21. Experimental features switched off: nothing new is staged,
        //     neither an offer nor Analyse again, nor an offer that was
        //     being prepared when they went off. What waits stays staged.
        writeWav(in / "f four.wav", 44100, 46);
        const QVariantList four{pathToQString(in / "f four.wav")};
        fixes.setExperimentalEnabled(false);
        assert(!fixes.importFiles(freshPioneer, freshEngine, four, QStringLiteral("Fresh"), false));
        assert(fixes.errorMessage().contains(QLatin1String("switched off")));
        assert(!fixes.analyseAgain(freshPioneer, freshEngine, QStringLiteral("Fresh")));
        assert(!fixes.busy() && freshSession->pendingCount() == 0);
        fixes.setExperimentalEnabled(true);
        auto held = std::make_shared<std::atomic<bool>>(true);
        auto entered = std::make_shared<std::atomic<bool>>(false);
        fixes.setAnalyserForTesting([held, entered](const std::string &, const application::CancellationToken &) {
            entered->store(true);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (held->load() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return std::shared_ptr<const ImportAnalysis>();
        });
        assert(fixes.importFiles(freshPioneer, freshEngine, four, QStringLiteral("Fresh"), false));
        assert(waitUntil([&] { return entered->load(); }));
        fixes.setExperimentalEnabled(false);
        held->store(false);
        assert(waitUntil([&] { return !fixes.busy(); }));
        assert(fixes.stagedCount() == 0 && freshSession->pendingCount() == 0 && "switched off meanwhile: not staged");
        assert(fixes.errorMessage().contains(QLatin1String("switched off")));
        fixes.setExperimentalEnabled(true);
        assert(offerFresh(four, QStringLiteral("Fresh"), false));
        assert(fixes.stagedCount() == 1 && freshSession->pendingCount() == 2);
        fixes.setExperimentalEnabled(false);
        assert(fixes.stagedCount() == 1 && freshSession->pendingCount() == 2 && "what waits stays staged");
        fixes.dropStaged();
        assert(freshSession->pendingCount() == 0 && "and can be taken back");
        fixes.setExperimentalEnabled(true);
        fixes.setAnalyserForTesting(nullptr);
        std::cout << "case 21 (experimental features off: nothing new is staged, what waits stays) OK\n";

        // 22. A controller that goes (Browse closed, the stick pulled)
        //     while a file is analysed does not wait for an analysis that
        //     does not look at its token: it is let go, and finishes
        //     alone, staging nothing.
        {
            auto release = std::make_shared<std::atomic<bool>>(false);
            auto started = std::make_shared<std::atomic<bool>>(false);
            auto finished = std::make_shared<std::atomic<bool>>(false);
            auto *closing = new TrackImportController;
            closing->setAnalyserForTesting([release, started, finished](const std::string &,
                                                                       const application::CancellationToken &) {
                started->store(true);
                // Never asks the token: a decoder waiting for a file that
                // stopped answering.
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (!release->load() && std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                finished->store(true);
                return std::shared_ptr<const ImportAnalysis>();
            });
            writeWav(in / "f five.wav", 44100, 47);
            assert(closing->importFiles(freshPioneer, freshEngine, QVariantList{pathToQString(in / "f five.wav")},
                                        QStringLiteral("Fresh"), false));
            assert(waitUntil([&] { return started->load(); }));
            const auto before = std::chrono::steady_clock::now();
            delete closing;
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before);
            std::cout << "  the controller went in " << took.count() << " ms" << std::endl;
            assert(took < std::chrono::milliseconds(1500) && "going away waits a moment, not for the analysis");
            assert(!finished->load() && "the worker was let go, not waited for");
            release->store(true);
            assert(waitUntil([&] { return finished->load(); }));
            assert(AsyncWorkers::instance().waitForAll(std::chrono::seconds(10)) && "and it ended alone");
            QCoreApplication::processEvents();
            assert(freshSession->pendingCount() == 0 && "nothing of it was staged");
        }
        std::cout << "case 22 (a controller going while a file is analysed is not held up by it) OK\n";
    }

    // 23 to 27, on a stick of their own: what the track list shows of a
    // staged batch before Save (stagedImport) is the batch's changes as
    // the save will apply them, a row each, in their order; one row taken
    // back takes its unit and leaves the rest as they will be saved; and
    // the rows follow the save, a failed save and a discard.
    {
        const testing::EngineChangeStick rowsStick = testing::makeEngineChangeStick(fixture, "track_import_controller_rows");
        testing::levelImportCounter(rowsStick);
        const QString rowsPioneer = rowsStick.pioneerPath();
        const QString rowsEngine = rowsStick.enginePath();
        LibraryEditSession *rowsSession = registry->sessionFor(registry->libraryIdForPath(rowsPioneer));
        assert(rowsSession && rowsSession != session);
        const fs::path in = computer / "rows";
        TrackImportController shown;
        QSignalSpy shownResults(&shown, &TrackImportController::resultReady);
        // The one bar of each run, as the strip shows it.
        std::vector<double> bar;
        QObject::connect(&shown, &TrackImportController::progressChanged, [&] {
            if (shown.busy()) {
                bar.push_back(shown.progressFraction());
            }
        });
        const auto barWentWell = [&]() {
            for (std::size_t i = 1; i < bar.size(); ++i) {
                if (bar[i] < bar[i - 1]) {
                    std::cerr << "  the bar went back from " << bar[i - 1] << " to " << bar[i] << "\n";
                    return false;
                }
            }
            const auto ends = std::count_if(bar.begin(), bar.end(), [](double at) { return at >= 1.0; });
            return bar.size() > 3 && bar.back() == 1.0 && ends == 1;
        };
        const auto offerRows = [&](const QVariantList &files, const QString &playlist, bool create, int atIndex = -1) {
            const int before = shownResults.count();
            bar.clear();
            if (!shown.importFiles(rowsPioneer, rowsEngine, files, playlist, create, atIndex)) {
                return false;
            }
            assert(waitFor(&shown, &TrackImportController::busyChanged, [&] { return !shown.busy(); }));
            if (!shown.errorMessage().isEmpty()) {
                std::cerr << "  " << shown.errorMessage().toStdString() << "\n";
            }
            return shownResults.count() > before;
        };
        const auto saveRows = [&]() {
            QSignalSpy done(rowsSession, &LibraryEditSession::saveFinished);
            rowsSession->save();
            assert(waitFor(rowsSession, &LibraryEditSession::saveFinished, [&] { return !done.isEmpty(); }));
            return rowsSession->lastSummary();
        };
        const auto rowsOf = [&](const char *which) { return shown.stagedImport().value(QLatin1String(which)).toList(); };
        const auto idsOf = [](const QVariantList &rows) {
            QStringList ids;
            for (const QVariant &row : rows) {
                ids << row.toMap().value("changeId").toString();
            }
            return ids;
        };
        const auto nameOfRow = [](const QVariant &row) {
            return pathToUtf8(pathFromUtf8(row.toMap().value("filePath").toString().toStdString()).filename());
        };
        // The rows against the session's changes of the batch: one per
        // playlist entry (or per import, into no playlist), in the save's
        // order, each with what its change will write.
        const auto rowsMatchTheChanges = [&]() {
            const QVariantList rows = rowsOf("rows");
            std::map<std::string, std::shared_ptr<ImportTrackChange>> imports;
            std::vector<std::shared_ptr<PendingChange>> order;
            bool entries = false;
            int placeAt = -1;
            for (const auto &change : rowsSession->pendingChanges()) {
                if (const auto import = std::dynamic_pointer_cast<ImportTrackChange>(change)) {
                    imports[import->destinationFile()] = import;
                }
            }
            for (const auto &change : rowsSession->pendingChanges()) {
                if (const auto entry = std::dynamic_pointer_cast<AddToPlaylistChange>(change)) {
                    entries = true;
                    order.push_back(entry);
                } else if (const auto place = std::dynamic_pointer_cast<PlaceInPlaylistChange>(change)) {
                    placeAt = place->toIndex();
                }
            }
            if (!entries) {
                for (const auto &change : rowsSession->pendingChanges()) {
                    if (std::dynamic_pointer_cast<ImportTrackChange>(change)) {
                        order.push_back(change);
                    }
                }
            }
            if (static_cast<std::size_t>(rows.size()) != order.size()
                || shown.stagedImport().value("atIndex").toInt() != placeAt) {
                return false;
            }
            for (std::size_t i = 0; i < order.size(); ++i) {
                const QVariantMap row = rows[static_cast<qsizetype>(i)].toMap();
                std::shared_ptr<ImportTrackChange> import = std::dynamic_pointer_cast<ImportTrackChange>(order[i]);
                if (const auto entry = std::dynamic_pointer_cast<AddToPlaylistChange>(order[i])) {
                    const auto it = imports.find(entry->filePath());
                    if (it == imports.end()) {
                        // A track of the stick's.
                        if (!row.value("existing").toBool() || row.value("changeId").toString() != entry->id()
                            || row.value("filePath").toString().toStdString() != entry->filePath()) {
                            return false;
                        }
                        continue;
                    }
                    import = it->second;
                }
                const domain::ImportedTrack &track = import->track();
                if (row.value("existing").toBool() || row.value("changeId").toString() != import->id()
                    || row.value("filePath").toString().toStdString() != import->destinationFile()
                    || row.value("title").toString().toStdString() != track.title
                    || row.value("artist").toString().toStdString() != track.artist
                    || row.value("album").toString().toStdString() != track.album
                    || row.value("durationSeconds").toDouble() != track.durationSeconds
                    || row.value("bpm").toDouble() != track.bpm
                    || row.value("key").toString().toStdString() != track.key) {
                    return false;
                }
            }
            return true;
        };

        // 23. Into no playlist: the new track's row, and "All tracks"
        //     grows by it. Saved, the row is landed until the page reads
        //     the stick again.
        writeWav(in / "r base.wav", 44100, 61);
        assert(offerRows(QVariantList{pathToQString(in / "r base.wav")}, QString(), false));
        assert(rowsOf("rows").size() == 1 && rowsMatchTheChanges());
        assert(shown.stagedImport().value("playlist").toString().isEmpty());
        assert(shown.trackCountChange(QString()) == 1);
        const QVariant baseRow = rowsOf("rows")[0];
        assert(saveRows().value("error").toString().isEmpty());
        assert(rowsOf("rows").isEmpty() && shown.trackCountChange(QString()) == 0);
        assert(rowsOf("landed") == QVariantList{baseRow} && "the saved row is landed, as it was");
        const fs::path baseOnStick = pathFromUtf8(baseRow.toMap().value("filePath").toString().toStdString());
        assert(fs::exists(baseOnStick) && "the row said where the save puts the file");
        std::cout << "case 23 (a track into no playlist: its row is its change, and landed once saved) OK\n";

        // 24. Into a new playlist: a row per entry in the save's order,
        //     with the tags and what the analysis found, the cover of the
        //     one that has one.
        {
            QImage drawn(120, 120, QImage::Format_RGB32);
            drawn.fill(QColor(30, 160, 200));
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            assert(drawn.save(&buffer, "PNG"));
            writeWavWithCover(in / "r one.wav", 44100, 62, png.toStdString());
        }
        writeBeatWav(in / "r two.wav", 8, 124.0);
        writeChordWav(in / "r three.wav", 6);
        shown.setAnalyserForTesting(nullptr);
        assert(offerRows(QVariantList{pathToQString(in / "r one.wav"), pathToQString(in / "r two.wav"),
                                      pathToQString(in / "r three.wav")},
                         QStringLiteral("Rows"), true));
        assert(rowsMatchTheChanges());
        assert(barWentWell() && "one bar from the drop to the staging, never back, at the end once");
        QVariantList rows = rowsOf("rows");
        assert(rows.size() == 3 && shown.stagedImport().value("isNew").toBool());
        assert(shown.stagedImport().value("playlist").toString() == QLatin1String("Rows"));
        assert(nameOfRow(rows[0]) == "r one.wav" && nameOfRow(rows[1]) == "r two.wav" && nameOfRow(rows[2]) == "r three.wav");
        assert(rows[1].toMap().value("bpm").toDouble() > 100.0 && "the analysis's tempo, the file has no tag");
        std::cerr << "  r two: " << rows[1].toMap().value("bpm").toDouble() << " BPM; r three: key "
                  << rows[2].toMap().value("key").toString().toStdString() << std::endl;
        {
            const QString cover = rows[0].toMap().value("artworkPath").toString();
            assert(cover.startsWith(QLatin1String("data:image/jpeg;base64,")));
            const QImage picture = QImage::fromData(QByteArray::fromBase64(cover.mid(cover.indexOf(',') + 1).toLatin1()));
            assert(picture.width() == 80 && picture.height() == 80 && "the small square the libraries keep");
            assert(rows[1].toMap().value("artworkPath").toString().isEmpty());
        }
        assert(shown.trackCountChange(QStringLiteral("Rows")) == 3 && shown.trackCountChange(QString()) == 3);
        assert(rowsOf("landed").isEmpty() && "a new batch: what landed before is the scan's");
        const QVariantList newRows = rows;
        const QVariantMap saved = saveRows();
        assert(saved.value("error").toString().isEmpty());
        assert(rowsOf("rows").isEmpty() && rowsOf("landed") == newRows);
        {
            const std::vector<std::string> expected{"r one.wav", "r two.wav", "r three.wav"};
            assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(rowsStick.pioneer)), "Rows") == expected);
            // The tempo the row showed is the one the libraries have.
            for (const auto &track : infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(rowsStick.pioneer)).readAll()) {
                if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == "r two.wav") {
                    assert(std::abs(track.bpm - newRows[1].toMap().value("bpm").toDouble()) < 0.01);
                }
            }
        }
        // Analyse again on one bar as well, and its refusals are its own.
        bar.clear();
        assert(shown.analyseAgain(rowsPioneer, rowsEngine, QStringLiteral("Rows")));
        assert(!shown.analyseAgain(rowsPioneer, rowsEngine, QStringLiteral("Rows")));
        assert(shown.errorSource() == QLatin1String("reanalysis") && !shown.errorMessage().isEmpty());
        assert(!shown.importFiles(rowsPioneer, rowsEngine, QVariantList{pathToQString(in / "r one.wav")}, QString(), false));
        assert(shown.errorSource() == QLatin1String("import"));
        assert(waitFor(&shown, &TrackImportController::busyChanged, [&] { return !shown.busy(); }));
        assert(barWentWell() && "Analyse again: one bar too");
        assert(shown.reanalysisCount(QStringLiteral("Rows")) > 0);
        shown.dropReanalysis(QStringLiteral("Rows"));
        assert(rowsSession->pendingCount() == 0);
        std::cout << "case 24 (a batch's rows: its entries in order, tags, analysis and cover; saved as shown; one bar) OK\n";

        // 25. Into that playlist at its second place, with a track of the
        //     stick's: a row each, the run's place. "Don't import" for one
        //     takes it and its entry, the run moves without it; for the
        //     stick's track, its entry. More files joined later do not
        //     bring them back.
        writeWav(in / "r four.wav", 44100, 63);
        writeWav(in / "r five.wav", 44100, 64);
        writeWav(in / "r six.wav", 44100, 65);
        assert(offerRows(QVariantList{pathToQString(in / "r four.wav"), pathToQString(in / "r five.wav"),
                                      pathToQString(baseOnStick), pathToQString(in / "r six.wav")},
                         QStringLiteral("Rows"), false, 1));
        assert(rowsMatchTheChanges());
        rows = rowsOf("rows");
        assert(rows.size() == 4 && shown.stagedImport().value("atIndex").toInt() == 1);
        assert(rows[2].toMap().value("existing").toBool() && nameOfRow(rows[2]) == "r base.wav");
        assert(shown.trackCountChange(QStringLiteral("Rows")) == 4 && shown.trackCountChange(QString()) == 3);
        const QString fiveId = rows[1].toMap().value("changeId").toString();
        const std::string fiveFile = rows[1].toMap().value("filePath").toString().toStdString();
        const QString baseEntryId = rows[2].toMap().value("changeId").toString();
        assert(!shown.dropStagedTrack(QStringLiteral("addcue:import-track:/no/such.wav")));
        assert(shown.dropStagedTrack(fiveId));
        assert(!rowsSession->hasChange(fiveId) && "its import goes");
        assert(!rowsSession->hasChange(AddToPlaylistChange::idFor("Rows", fiveFile))
               && !rowsSession->hasChange(QStringLiteral("addcue:import:") + AddToPlaylistChange::idFor("Rows", fiveFile).mid(7))
               && "and its entry");
        for (const auto &change : rowsSession->pendingChanges()) {
            if (const auto entry = std::dynamic_pointer_cast<AddToPlaylistChange>(change)) {
                assert(entry->filePath() != fiveFile && "no entry of it is left");
            }
            if (const auto place = std::dynamic_pointer_cast<PlaceInPlaylistChange>(change)) {
                assert(place->files().size() == 3 && std::find(place->files().begin(), place->files().end(), fiveFile)
                                                         == place->files().end());
            }
        }
        assert(rowsMatchTheChanges() && rowsOf("rows").size() == 3);
        assert(shown.trackCountChange(QStringLiteral("Rows")) == 3 && shown.stagedCount() == 2);
        assert(shown.dropStagedTrack(baseEntryId));
        assert(!rowsSession->hasChange(baseEntryId) && rowsMatchTheChanges() && rowsOf("rows").size() == 2);
        assert(shown.trackCountChange(QStringLiteral("Rows")) == 2);
        writeWav(in / "r seven.wav", 44100, 66);
        assert(offerRows(QVariantList{pathToQString(in / "r seven.wav")}, QStringLiteral("Rows"), false));
        assert(rowsMatchTheChanges());
        rows = rowsOf("rows");
        assert(rows.size() == 3 && nameOfRow(rows[0]) == "r four.wav" && nameOfRow(rows[1]) == "r six.wav"
               && nameOfRow(rows[2]) == "r seven.wav" && "what was taken back stays out of the joined batch");
        assert(shown.stagedImport().value("atIndex").toInt() == 1);
        std::cout << "case 25 (one row taken back: its unit goes, the run moves without it, and it stays out) OK\n";

        // 26. A save that fails goes back whole: the rows are staged again
        //     as they were. Saved, the playlist is in the order shown.
        const QVariantList beforeFailing = rowsOf("rows");
        const fs::path blocked = pathFromUtf8(beforeFailing[0].toMap().value("filePath").toString().toStdString());
        fs::create_directories(blocked);  // in the way of the copy
        const QVariantMap failed = saveRows();
        assert(!failed.value("error").toString().isEmpty());
        assert(rowsOf("rows") == beforeFailing && rowsOf("landed").isEmpty() && rowsMatchTheChanges());
        assert(shown.trackCountChange(QStringLiteral("Rows")) == 3);
        fs::remove(blocked);
        assert(saveRows().value("error").toString().isEmpty());
        assert(rowsOf("rows").isEmpty() && rowsOf("landed") == beforeFailing);
        {
            const std::vector<std::string> expected{"r one.wav", "r four.wav", "r six.wav", "r seven.wav", "r two.wav",
                                                    "r three.wav"};
            assert(playlistOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(rowsStick.pioneer)), "Rows") == expected);
            assert(playlistOf(infrastructure::engine::LibdjinteropEngineReader(rowsStick.engineUtf8()), "Rows") == expected);
        }
        std::cout << "case 26 (a failed save: the rows staged again; saved, the playlist as the rows put it) OK\n";

        // 27. A discard takes the rows away, landed ones too. In a new
        //     playlist the first track taken back hands the playlist on to
        //     the next; the last takes the batch back whole, the playlist
        //     too.
        writeWav(in / "r eight.wav", 44100, 67);
        assert(offerRows(QVariantList{pathToQString(in / "r eight.wav")}, QStringLiteral("Rows"), false));
        assert(rowsOf("rows").size() == 1 && rowsOf("landed").isEmpty());
        rowsSession->discard();
        assert(rowsOf("rows").isEmpty() && rowsOf("landed").isEmpty() && shown.trackCountChange(QStringLiteral("Rows")) == 0);
        writeWav(in / "r nine.wav", 44100, 68);
        writeWav(in / "r ten.wav", 44100, 69);
        assert(offerRows(QVariantList{pathToQString(in / "r nine.wav"), pathToQString(in / "r ten.wav")},
                         QStringLiteral("Rows Two"), true));
        rows = rowsOf("rows");
        assert(rows.size() == 2 && rowsMatchTheChanges());
        const auto made = [&]() -> std::shared_ptr<CreatePlaylistChange> {
            for (const auto &change : rowsSession->pendingChanges()) {
                if (auto create = std::dynamic_pointer_cast<CreatePlaylistChange>(change)) {
                    return create;
                }
            }
            return nullptr;
        };
        const QString tenKey =
            ImportTrackChange::unitKeyFor("/" + pathToGenericUtf8(fs::relative(
                                                    pathFromUtf8(rows[1].toMap().value("filePath").toString().toStdString()),
                                                    rowsStick.root)));
        assert(made() && !made()->unitKeys().contains(tenKey));
        assert(shown.dropStagedTrack(rows[0].toMap().value("changeId").toString()));
        assert(made() && made()->unitKeys().contains(tenKey) && "the playlist goes with the next import now");
        assert(rowsSession->pendingCount() == 3 && rowsMatchTheChanges());
        assert(shown.dropStagedTrack(rows[1].toMap().value("changeId").toString()));
        assert(rowsSession->pendingCount() == 0 && !made() && "the last row takes the batch back, the playlist too");
        assert(rowsOf("rows").isEmpty() && shown.stagedPlaylist().isEmpty());
        std::cout << "case 27 (a discard takes the rows; the first and the last of a new playlist's batch taken back) OK\n";
    }

    std::cout << "track_import_controller_test: all cases passed\n";
    return 0;
}
