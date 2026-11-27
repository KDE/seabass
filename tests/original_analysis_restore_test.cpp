// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Analyse again keeps the analysis rekordbox and the player made, and
// Restore original analysis puts it back (gui/edit/changes/
// restore_analysis_change.hpp, gui/original_analysis_controller.hpp), on a
// copy of the anonymised fixture stick with the real edit session, no
// real audio (synthetic WAVs) and no real stick.
//
// The tracks are imported by Seabass, so their analysis is Seabass's own.
// "k one" and "k three" are then made to stand for tracks rekordbox and
// the player analysed: their waveforms changed in their analysis files,
// their tempo changed in both rekordbox libraries, their analysis ledger
// entries and beat grid ledger lines taken out (as a stick rekordbox
// exported over would have them), and their audio replaced by the same
// music 30 ms later at the same size, so that Analyse again finds a grid
// 30 ms later than the one each library has. "k one" has no .2EX (Analyse
// again makes one) and a hot cue on a beat of rekordbox's grid in every
// library; "k three" no cue. "k two" stays Seabass's alone.
//
// 1. Analyse again keeps rekordbox's analysis files as they were and the
//    rows' tempo and key, and Engine's row; nothing for "k two".
// 2. Analysing again a second time keeps the kept original as it was.
// 3. The count of the playlist's tracks with an original kept, cached for
//    the menu until the next save.
// 4. Restore with the cue switch off (the review's default for a 30 ms
//    move): the analysis files are the originals but for the cue lists,
//    which are the current ones; .2EX taken away again; tempo and key in
//    export.pdb and exportLibrary.db and Engine's grid back; the ledger
//    entry and the beat grid line gone; the kept entries gone; the cue
//    where Analyse again left it. A second restore finds nothing.
// 5. Undo Last Save of the restore: everything as before it, kept entries
//    included.
// 6. Restore with the cue switch on: the cue back on rekordbox's beat in
//    all three libraries. "k three" byte for byte its original files.
// 7. Refused: the audio file of another size; the grid changed by a third
//    party. Nothing written, the kept entries stay.
// 8. A save that goes back with a restore in it, and one with Analyse
//    again in it: the kept store and the files as before. Undo Last Save
//    of Analyse again takes away what it kept.
// 9. Free Up Space lists the kept originals with their sizes, every group
//    unticked (those of a track gone too), removes them, and the restore
//    is no longer offered for those tracks.
// 10. A five-minute track: what is kept of it, for the size it takes.

#include <QCoreApplication>
#include <QSignalSpy>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <vector>

#include "application/path_key.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/changes/import_track_change.hpp"
#include "gui/edit/changes/restore_analysis_change.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/track_import_prepare.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/original_analysis_controller.hpp"
#include "gui/track_import_controller.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/anlz_analysis_merge.hpp"
#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;
namespace local = infrastructure::local;
using infrastructure::rekordbox::AnlzFile;

namespace
{

// The stick's log, whose Analyse again and restore lines a failure shows:
// the scratch tree goes with the process.
fs::path &stickLog()
{
    static fs::path log;
    return log;
}

void require(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << std::endl;
        std::ifstream in(stickLog());
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            if (line.find("track-reanalysis") != std::string::npos) {
                lines.push_back(line);
            }
        }
        for (std::size_t i = lines.size() > 12 ? lines.size() - 12 : 0; i < lines.size(); ++i) {
            std::cerr << "  log: " << lines[i].substr(0, 700) << std::endl;
        }
        std::exit(1);
    }
}

void writeMonoWav(const fs::path &file, const std::vector<std::int16_t> &samples)
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
    u32(44100);
    u32(88200);
    u16(2);
    u16(16);
    out << "data";
    u32(frames * 2);
    out.write(reinterpret_cast<const char *>(samples.data()), frames * 2);
}

// A kick on every beat at `bpm`, the first at `firstAt` seconds.
void writeBeatWav(const fs::path &file, int seconds, double bpm, double firstAt = 0.25)
{
    const std::uint32_t frames = static_cast<std::uint32_t>(seconds) * 44100;
    std::vector<std::int16_t> samples(frames, 0);
    const double beat = 60.0 / bpm * 44100.0;
    for (double at = firstAt * 44100.0; at < frames; at += beat) {
        for (std::uint32_t i = 0; i < 6615 && static_cast<std::uint32_t>(at) + i < frames; ++i) {
            const double t = i / 44100.0;
            samples[static_cast<std::uint32_t>(at) + i] =
                static_cast<std::int16_t>(24000.0 * std::exp(-t * 30.0) * std::sin(2.0 * 3.14159265358979 * 60.0 * t));
        }
    }
    writeMonoWav(file, samples);
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

std::string bytesOf(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint32_t fourcc(const char (&text)[5])
{
    return (std::uint32_t(std::uint8_t(text[0])) << 24) | (std::uint32_t(std::uint8_t(text[1])) << 16)
           | (std::uint32_t(std::uint8_t(text[2])) << 8) | std::uint32_t(std::uint8_t(text[3]));
}

std::uint32_t readBE(const std::string &bytes, std::size_t at)
{
    return (std::uint32_t(std::uint8_t(bytes[at])) << 24) | (std::uint32_t(std::uint8_t(bytes[at + 1])) << 16)
           | (std::uint32_t(std::uint8_t(bytes[at + 2])) << 8) | std::uint32_t(std::uint8_t(bytes[at + 3]));
}

void writeBE(std::string &bytes, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        bytes[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (24 - 8 * i)) & 0xff);
    }
}

// Every beat of the PQTZ `shiftMs` later.
void shiftGrid(AnlzFile &dat, int shiftMs)
{
    for (auto &section : dat.sections) {
        if (section.fourcc == fourcc("PQTZ")) {
            const std::uint32_t lenHeader = readBE(section.rawBytes, 4);
            const std::uint32_t count = readBE(section.rawBytes, 20);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::size_t at = lenHeader + i * 8 + 4;
                writeBE(section.rawBytes, at, readBE(section.rawBytes, at) + static_cast<std::uint32_t>(shiftMs));
            }
        }
    }
}

// One byte of every waveform section's body changed.
void touchWaveforms(AnlzFile &file)
{
    for (auto &section : file.sections) {
        if (infrastructure::rekordbox::isReplacedAnalysisSection(section.fourcc) && section.fourcc != fourcc("PQTZ")
            && section.fourcc != fourcc("PQT2") && section.rawBytes.size() > 40) {
            section.rawBytes[section.rawBytes.size() - 1] ^= 0x5a;
        }
    }
}

bool isCueList(std::uint32_t f)
{
    return infrastructure::rekordbox::isCueListSection(f);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: original_analysis_restore_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "original_analysis_restore_home");
    testing::sandboxSettings(testing::scratchRoot() / "original_analysis_restore_config");
    QCoreApplication app(argc, argv);
    const fs::path computer = testing::scratchRoot() / "original_analysis_restore_computer";
    fs::remove_all(computer);
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "original_analysis_restore_stick");
    testing::levelImportCounter(stick);
    const QString pioneer = stick.pioneerPath();
    const QString engine = stick.enginePath();
    stickLog() = stick.root / "Seabass" / "seabass.log";
    const fs::path onStick = stick.root / "Contents" / "UnknownArtist" / "UnknownAlbum";
    const std::string pdbFile = pathToUtf8(stick.pioneer / "rekordbox" / "export.pdb");

    TrackImportController importer;
    importer.setExperimentalEnabled(true);
    OriginalAnalysisController restorer;
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(pioneer));
    require(session != nullptr, "the stick has an edit session");
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    const auto saveNow = [&]() {
        finished.clear();
        session->save();
        require(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }), "save ends");
        if (!session->lastSummary().value("error").toString().isEmpty()) {
            std::cerr << "  save: " << session->lastSummary().value("error").toString().toStdString() << std::endl;
        }
        return session->lastSummary();
    };
    const auto undoNow = [&]() {
        finished.clear();
        session->undoLastSave();
        require(waitFor(session, &LibraryEditSession::saveFinished, [&] { return !finished.isEmpty(); }), "undo ends");
    };
    const auto analyseAgain = [&](const QString &playlist, bool moveCues) {
        require(importer.analyseAgain(pioneer, engine, playlist), "Analyse again starts");
        require(waitFor(&importer, &TrackImportController::busyChanged, [&] { return !importer.busy(); }),
                "Analyse again ends");
        require(importer.errorMessage().isEmpty(), "Analyse again: " + importer.errorMessage().toStdString());
        importer.setMoveCuesForAll(moveCues);
        return importer.lastReanalysis();
    };
    const auto keptCount = [&](const QString &playlist) {
        restorer.refreshKeptCount(pioneer, engine, playlist);
        require(waitFor(&restorer, &OriginalAnalysisController::keptCountChanged,
                        [&] { return restorer.keptCountPlaylist() == playlist && restorer.keptCount() >= 0; }),
                "the count is answered");
        const int n = restorer.keptCount();
        // Asked again later, the answer must be a new one.
        restorer.refreshKeptCount(pioneer, engine, QStringLiteral("-"));
        waitFor(&restorer, &OriginalAnalysisController::keptCountChanged,
                [&] { return restorer.keptCountPlaylist() == QLatin1String("-"); });
        return n;
    };
    // A restore that has nothing of Analyse again's to give back is
    // refused, saying so, and stages nothing.
    const auto restoreRefused = [&](const QString &playlist) {
        require(restorer.restoreOriginalAnalysis(pioneer, engine, playlist), "the restore starts");
        require(waitFor(&restorer, &OriginalAnalysisController::busyChanged, [&] { return !restorer.busy(); }),
                "the restore is prepared");
        return restorer.errorMessage().contains(QStringLiteral("has an original analysis that Analyse again kept"))
               && session->pendingCount() == 0;
    };
    const auto restore = [&](const QString &playlist) {
        require(restorer.restoreOriginalAnalysis(pioneer, engine, playlist),
                "the restore starts: " + restorer.errorMessage().toStdString());
        require(waitFor(&restorer, &OriginalAnalysisController::busyChanged, [&] { return !restorer.busy(); }),
                "the restore is prepared");
        require(restorer.errorMessage().isEmpty(), "restore: " + restorer.errorMessage().toStdString());
        return restorer.lastRestore();
    };
    const auto logText = [&]() { return bytesOf(stick.root / "Seabass" / "seabass.log"); };
    const auto keyOf = [](const std::string &name) {
        return application::normalizedPathKey("Contents/UnknownArtist/UnknownAlbum/" + name);
    };
    const auto rowOf = [](auto &&reader, const std::string &name) {
        for (const auto &track : reader.readAll()) {
            if (pathToUtf8(pathFromUtf8(track.filePath).filename()) == name) {
                return track;
            }
        }
        require(false, name + " is in the library");
        return domain::Track{};
    };
    const auto pdbRow = [&](const std::string &name) {
        return rowOf(infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)), name);
    };
    const auto engineRow = [&](const std::string &name) {
        return rowOf(infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()), name);
    };
    const auto oneLibraryRow = [&](const std::string &name) {
        return rowOf(infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)), name);
    };
    const auto anlzOf = [&](const std::string &name, const char *extension) {
        const std::string dat = pdbRow(name).analysisFile;
        return stick.root / pathFromUtf8(dat.substr(1, dat.size() - 4) + extension);
    };
    const auto keptRecordFile = [&](const std::string &name) {
        return local::keptRekordboxRecordFile(stick.root, keyOf(name));
    };
    const auto keptFile = [&](const std::string &name) {
        return local::keptRekordboxAnalysisFile(stick.root, keyOf(name));
    };
    const auto keptEngineFile = [&](const std::string &name) {
        return local::keptEngineAnalysisFile(stick.root, keyOf(name));
    };
    const auto ledger = [&]() {
        std::string error;
        const auto read = local::readAnalysisLedger(stick.root, &error);
        require(error.empty(), "the analysis ledger reads: " + error);
        return read;
    };
    const auto gridLedger = [&]() {
        std::string error;
        const auto read = local::readBeatGridLedger(stick.root, &error);
        require(error.empty(), "the beat grid ledger reads: " + error);
        return read;
    };
    const auto pdbTempoKey = [&](const std::string &name) {
        const auto id = static_cast<std::uint32_t>(std::stoul(pdbRow(name).sourceId));
        return infrastructure::rekordbox::PdbRowWriter(pdbFile).trackTempoAndKeyId(id).value();
    };
    const auto oneLibraryTempo = [&](const std::string &name) {
        const auto rows =
            infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(stick.pioneer)).tempoAndKeyIdsForPath(pathToUtf8(onStick / name));
        require(rows.size() == 1, "one OneLibrary row of " + name);
        return rows[0].bpmx100.value_or(-1);
    };
    const auto engineGrid = [&](const std::string &name) {
        return infrastructure::engine::LibdjinteropEngineCueWriter(stick.engineUtf8()).gridFingerprintOf(engineRow(name).sourceId);
    };
    const auto hotCueOf = [](const domain::Track &track) {
        for (const auto &cue : track.cues) {
            if (cue.kind == domain::CuePoint::Kind::Hot && cue.hotCueNumber == 1 && !cue.isLoop) {
                return cue.positionMs;
            }
        }
        return -1.0;
    };
    const auto changeIdOf = [&](const std::string &name) {
        return RestoreAnalysisChange::idFor("/Contents/UnknownArtist/UnknownAlbum/" + name);
    };
    const auto invalidate = [&]() { LibraryCatalogCache::instance().invalidateEveryCatalogOn(pathToUtf8(stick.root)); };

    // The import: three tracks into "Kept", Seabass's analysis.
    writeBeatWav(computer / "k one.wav", 40, 120.0);
    writeBeatWav(computer / "k two.wav", 41, 120.0);
    writeBeatWav(computer / "k three.wav", 42, 120.0);
    require(importer.importFiles(pioneer, engine,
                                 QVariantList{pathToQString(computer / "k one.wav"), pathToQString(computer / "k two.wav"),
                                              pathToQString(computer / "k three.wav")},
                                 QStringLiteral("Kept"), true),
            "the import starts");
    require(waitFor(&importer, &TrackImportController::busyChanged, [&] { return !importer.busy(); }), "import ends");
    require(saveNow().value("error").toString().isEmpty(), "the import is saved");
    require(ledger().count(keyOf("k one.wav")) == 1, "the import recorded k one");

    // k one and k three made rekordbox's.
    const std::map<std::string, std::string> rekordboxNames{{"k one.wav", "one"}, {"k three.wav", "three"}};
    for (const auto &[name, unused] : rekordboxNames) {
        (void)unused;
        const int seconds = name == "k one.wav" ? 40 : 42;
        writeBeatWav(onStick / name, seconds, 120.0, 0.28);
        AnlzFile dat = AnlzFile::readRaw(pathToUtf8(anlzOf(name, "DAT")));
        touchWaveforms(dat);
        dat.writeRaw(pathToUtf8(anlzOf(name, "DAT")));
        AnlzFile ext = AnlzFile::readRaw(pathToUtf8(anlzOf(name, "EXT")));
        touchWaveforms(ext);
        ext.writeRaw(pathToUtf8(anlzOf(name, "EXT")));
        if (name == "k one.wav") {
            fs::remove(anlzOf(name, "2EX"));
        } else {
            AnlzFile twoEx = AnlzFile::readRaw(pathToUtf8(anlzOf(name, "2EX")));
            touchWaveforms(twoEx);
            twoEx.writeRaw(pathToUtf8(anlzOf(name, "2EX")));
        }
        infrastructure::rekordbox::PdbRowWriter rows(pdbFile);
        rows.setTrackTempoAndKey(static_cast<std::uint32_t>(std::stoul(pdbRow(name).sourceId)), 119.5, "");
        require(rows.commit(), "export.pdb takes rekordbox's tempo");
        infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(stick.pioneer))
            .setTempoAndKeyForPath(pathToUtf8(onStick / name), 119.5, "");
        local::AnalysisLedger without = ledger();
        without.erase(keyOf(name));
        std::string error;
        require(local::writeAnalysisLedger(stick.root, without, [](const std::string &) {}, &error), error);
        local::BeatGridLedger lines = gridLedger();
        lines.erase(keyOf(name));
        require(local::writeBeatGridLedger(stick.root, lines, &error), error);
    }
    invalidate();
    // A hot cue on beat 8 of rekordbox's grid of k one, in every library.
    const auto rekordboxGrid = infrastructure::rekordbox::readBeatGrid(pathToUtf8(stick.pioneer), pdbRow("k one.wav").sourceId);
    require(rekordboxGrid.size() > 20, "k one has rekordbox's grid");
    const double onBeat = std::round(rekordboxGrid[8].timeMs);
    require(session->stage(std::make_unique<AddCueChange>(QStringLiteral("rekordbox"), pioneer,
                                                          QString::fromStdString(pdbRow("k one.wav").sourceId), onBeat,
                                                          QStringLiteral("hot"), 1, QStringLiteral("#00FF00"),
                                                          QStringLiteral("drop"), false, 0.0, QStringLiteral("k one"))),
            "the rekordbox cue is staged");
    require(session->stage(std::make_unique<AddCueChange>(QStringLiteral("engine"), engine,
                                                          QString::fromStdString(engineRow("k one.wav").sourceId), onBeat,
                                                          QStringLiteral("hot"), 1, QStringLiteral("#00FF00"),
                                                          QStringLiteral("drop"), false, 0.0, QStringLiteral("k one"))),
            "the Engine cue is staged");
    require(saveNow().value("error").toString().isEmpty(), "the cues are saved");
    require(std::abs(hotCueOf(pdbRow("k one.wav")) - onBeat) < 1.0 && std::abs(hotCueOf(engineRow("k one.wav")) - onBeat) < 1.0,
            "the cue is on rekordbox's beat in rekordbox and Engine");

    // rekordbox's analysis as it is now: what a restore has to give back.
    std::map<std::string, std::map<std::string, std::string>> original;
    std::map<std::string, std::string> originalEngineGrid;
    for (const auto &[name, unused] : rekordboxNames) {
        (void)unused;
        for (const char *extension : {"DAT", "EXT", "2EX"}) {
            if (fs::exists(anlzOf(name, extension))) {
                original[name][extension] = bytesOf(anlzOf(name, extension));
            }
        }
        originalEngineGrid[name] = engineGrid(name);
    }
    const auto originalKeyId = pdbTempoKey("k one.wav").keyId;
    require(pdbTempoKey("k one.wav").tempo == 11950 && oneLibraryTempo("k one.wav") == 11950,
            "rekordbox's tempo in both libraries");

    // 1. Analyse again, the cues moved with the grid.
    {
        const QVariantMap again = analyseAgain(QStringLiteral("Kept"), true);
        require(again.value("analysed").toInt() == 3, "three tracks analysed again");
        require(saveNow().value("error").toString().isEmpty(), "Analyse again is saved");
        for (const auto &[name, unused] : rekordboxNames) {
            (void)unused;
            local::StickKeptRekordboxAnalysisStore store(stick.root);
            const auto kept = store.find(keyOf(name));
            require(kept.status == local::KeptRekordboxAnalysisLookup::Status::Found, "1: " + name + "'s analysis kept");
            require(kept.entry.files.size() == 1, "1: one file set");
            const auto &set = kept.entry.files[0];
            require(set.dat && *set.dat == original[name]["DAT"] && set.ext && *set.ext == original[name]["EXT"],
                    "1: the .DAT and .EXT kept as they were");
            require(name == "k one.wav" ? !set.twoEx : (set.twoEx && *set.twoEx == original[name]["2EX"]),
                    "1: the .2EX kept as it was, or none where there was none");
            int pdbRows = 0;
            int oneLibraryRows = 0;
            for (const auto &row : kept.entry.rows) {
                require(row.tempo && *row.tempo == 11950, "1: the rows' tempo kept");
                pdbRows += row.library == "rekordbox" ? 1 : 0;
                oneLibraryRows += row.library == "onelibrary" ? 1 : 0;
            }
            require(pdbRows == 1 && oneLibraryRows == 1, "1: a row of each rekordbox library kept");
            require(fs::exists(keptEngineFile(name)), "1: Engine's analysis of " + name + " kept");
            require(fs::exists(anlzOf(name, "2EX")), "1: a .2EX where Analyse again made one");
            require(pdbTempoKey(name).tempo != 11950, "1: the tempo is Seabass's now");
        }
        require(!fs::exists(keptFile("k two.wav")) && !fs::exists(keptEngineFile("k two.wav")),
                "1: k two, Seabass's alone, keeps nothing");
        const double moved = hotCueOf(pdbRow("k one.wav"));
        std::cerr << "  the cue from " << onBeat << " to " << moved << " ms" << std::endl;
        require(std::abs(moved - (onBeat + 30.0)) <= 3.0 && std::abs(hotCueOf(engineRow("k one.wav")) - moved) < 1.5,
                "1: the cue followed Seabass's grid in rekordbox and Engine");
        std::cout << "case 1 (Analyse again keeps rekordbox's and the player's analysis; nothing for a track Seabass "
                     "alone analysed) OK\n";
    }
    const double seabassCue = hotCueOf(pdbRow("k one.wav"));

    // 2. Analyse again once more: the kept original stays.
    {
        const std::string before = bytesOf(keptFile("k one.wav"));
        const auto keptBefore = local::StickKeptRekordboxAnalysisStore(stick.root).find(keyOf("k one.wav")).entry;
        analyseAgain(QStringLiteral("Kept"), false);
        require(saveNow().value("error").toString().isEmpty(), "the second Analyse again is saved");
        const auto keptAfter = local::StickKeptRekordboxAnalysisStore(stick.root).find(keyOf("k one.wav")).entry;
        require(keptAfter.files == keptBefore.files && keptAfter.rows == keptBefore.rows
                    && keptAfter.originalGrid == keptBefore.originalGrid,
                "2: the second Analyse again does not replace the kept original");
        const auto record = local::StickKeptRekordboxAnalysisStore(stick.root).findRecord(keyOf("k one.wav"));
        require(record.status == local::KeptRekordboxRecordLookup::Status::Found && !record.record.seabassGrids.empty(),
                "2: the kept entry's record knows Seabass's grids");
        (void)before;
        require(!fs::exists(keptFile("k two.wav")), "2: still nothing for k two");
        std::cout << "case 2 (analysing again twice keeps the first original) OK\n";
    }

    // 3.
    require(keptCount(QStringLiteral("Kept")) == 2, "3: two tracks of Kept have an original kept");
    require(restorer.cachedKeptCount(pioneer, engine, QStringLiteral("Kept")) == 2,
            "3: the answer is there at once for the menu's next opening");
    std::cout << "case 3 (the count of a playlist's tracks with an original kept) OK\n";

    // What a restore must leave as it was found, for 5.
    const std::map<std::string, std::string> beforeRestore{
        {"dat", bytesOf(anlzOf("k one.wav", "DAT"))},
        {"ext", bytesOf(anlzOf("k one.wav", "EXT"))},
        {"2ex", bytesOf(anlzOf("k one.wav", "2EX"))},
        {"kept", bytesOf(keptFile("k one.wav"))},
        {"keptEngine", bytesOf(keptEngineFile("k one.wav"))},
        {"ledger", bytesOf(infrastructure::paths::stickAnalysisLedger(stick.root))},
        {"grids", bytesOf(infrastructure::paths::stickBeatGridLedger(stick.root))}};
    const std::string seabassEngineGrid = engineGrid("k one.wav");

    // 4. Restore, the cue switch as the review sets it: off for 30 ms.
    {
        const QVariantMap staged = restore(QStringLiteral("Kept"));
        require(staged.value("staged").toInt() == 2 && staged.value("askedCueTracks").toInt() == 1
                    && staged.value("autoCueTracks").toInt() == 0,
                "4: two restores staged, one track's cues asked about");
        const QVariantList review = restorer.cueReview();
        require(review.size() == 1 && !review[0].toMap().value("moveCues").toBool()
                    && review[0].toMap().value("changeId").toString() == changeIdOf("k one.wav"),
                "4: the cue review starts off");
        const AnlzFile datNow = AnlzFile::readRaw(pathToUtf8(anlzOf("k one.wav", "DAT")));
        require(saveNow().value("error").toString().isEmpty(), "4: the restore is saved");
        require(restorer.cachedKeptCount(pioneer, engine, QStringLiteral("Kept")) == 0,
                "4: after the save the cached answer follows what the save put back, with no new count asked");
        // The rule: every section the original's, the cue lists the
        // current file's.
        const AnlzFile datOriginal = AnlzFile::fromBytes(original["k one.wav"]["DAT"], "original");
        const AnlzFile datAfter = AnlzFile::readRaw(pathToUtf8(anlzOf("k one.wav", "DAT")));
        require(datAfter.headerBytes == datOriginal.headerBytes && datAfter.sections.size() == datOriginal.sections.size(),
                "4: the .DAT has the original's header and sections");
        std::size_t cueAt = 0;
        std::vector<const infrastructure::rekordbox::AnlzRawSection *> cuesNow;
        for (const auto &section : datNow.sections) {
            if (isCueList(section.fourcc)) {
                cuesNow.push_back(&section);
            }
        }
        for (std::size_t i = 0; i < datAfter.sections.size(); ++i) {
            if (isCueList(datAfter.sections[i].fourcc)) {
                require(cueAt < cuesNow.size() && datAfter.sections[i].rawBytes == cuesNow[cueAt++]->rawBytes,
                        "4: a cue list is the current one");
            } else {
                require(datAfter.sections[i].rawBytes == datOriginal.sections[i].rawBytes,
                        "4: every other section is the original's");
            }
        }
        require(!fs::exists(anlzOf("k one.wav", "2EX")), "4: the .2EX Analyse again made is taken away");
        require(bytesOf(anlzOf("k three.wav", "DAT")) == original["k three.wav"]["DAT"]
                    && bytesOf(anlzOf("k three.wav", "EXT")) == original["k three.wav"]["EXT"]
                    && bytesOf(anlzOf("k three.wav", "2EX")) == original["k three.wav"]["2EX"],
                "4: k three, its cues untouched, byte for byte its original files");
        for (const auto &[name, unused] : rekordboxNames) {
            (void)unused;
            require(pdbTempoKey(name).tempo == 11950 && oneLibraryTempo(name) == 11950,
                    "4: rekordbox's tempo back in export.pdb and exportLibrary.db");
            require(engineGrid(name) == originalEngineGrid[name], "4: Engine's grid back");
            require(ledger().count(keyOf(name)) == 0, "4: the analysis ledger entry gone");
            require(gridLedger().count(keyOf(name)) == 0, "4: the beat grid ledger line gone");
            require(!fs::exists(keptFile(name)) && !fs::exists(keptEngineFile(name)), "4: the kept entries gone");
        }
        require(pdbTempoKey("k one.wav").keyId == originalKeyId, "4: the key as it was");
        require(ledger().count(keyOf("k two.wav")) == 1, "4: k two keeps its entry");
        require(std::abs(hotCueOf(pdbRow("k one.wav")) - seabassCue) < 1.0
                    && std::abs(hotCueOf(engineRow("k one.wav")) - seabassCue) < 1.5,
                "4: the cue stays where Analyse again left it, its switch off");
        require(keptCount(QStringLiteral("Kept")) == 0, "4: nothing kept any more");
        require(restoreRefused(QStringLiteral("Kept")), "4: a second restore finds nothing, and says so");
        std::cout << "case 4 (restore: the originals with the current cue lists, rows and Engine back, the ledgers "
                     "and kept entries cleared; the cue left by its switch) OK\n";
    }

    // 5. Undo Last Save.
    {
        undoNow();
        invalidate();
        require(bytesOf(anlzOf("k one.wav", "DAT")) == beforeRestore.at("dat")
                    && bytesOf(anlzOf("k one.wav", "EXT")) == beforeRestore.at("ext")
                    && bytesOf(anlzOf("k one.wav", "2EX")) == beforeRestore.at("2ex"),
                "5: the analysis files as before the restore");
        require(bytesOf(keptFile("k one.wav")) == beforeRestore.at("kept")
                    && bytesOf(keptEngineFile("k one.wav")) == beforeRestore.at("keptEngine"),
                "5: the kept entries back");
        require(bytesOf(infrastructure::paths::stickAnalysisLedger(stick.root)) == beforeRestore.at("ledger")
                    && bytesOf(infrastructure::paths::stickBeatGridLedger(stick.root)) == beforeRestore.at("grids"),
                "5: the ledgers back");
        require(engineGrid("k one.wav") == seabassEngineGrid && pdbTempoKey("k one.wav").tempo != 11950,
                "5: Engine and export.pdb hold Seabass's analysis again");
        require(keptCount(QStringLiteral("Kept")) == 2, "5: the restore is offered again");
        std::cout << "case 5 (Undo Last Save of a restore puts everything back, kept entries too) OK\n";
    }

    // 7. Refusals, while Seabass's analysis is there again. Each save
    //    restores k three and leaves k one alone; Undo puts k three back.
    {
        // The audio file of another size.
        restore(QStringLiteral("Kept"));
        const fs::path audio = onStick / "k one.wav";
        const auto size = fs::file_size(audio);
        {
            std::ofstream out(audio, std::ios::binary | std::ios::app);
            out << std::string(100, '\0');
        }
        std::size_t logBefore = logText().size();
        QVariantMap saved = saveNow();
        require(saved.value("error").toString().isEmpty() && saved.value("written").toInt() == 1,
                "7: the save lands with k three alone");
        require(logText().substr(logBefore).find("its audio file changed since its analysis was kept") != std::string::npos,
                "7: refused for an audio file of another size, saying why");
        require(bytesOf(anlzOf("k one.wav", "DAT")) == beforeRestore.at("dat")
                    && bytesOf(keptFile("k one.wav")) == beforeRestore.at("kept"),
                "7: nothing of k one written, its kept entry as it was");
        undoNow();
        fs::resize_file(audio, size);
        invalidate();

        // The grid changed by a third party after the restore was staged.
        restore(QStringLiteral("Kept"));
        AnlzFile dat = AnlzFile::readRaw(pathToUtf8(anlzOf("k one.wav", "DAT")));
        shiftGrid(dat, 5);
        dat.writeRaw(pathToUtf8(anlzOf("k one.wav", "DAT")));
        const std::string theirs = bytesOf(anlzOf("k one.wav", "DAT"));
        logBefore = logText().size();
        saved = saveNow();
        require(saved.value("error").toString().isEmpty() && saved.value("written").toInt() == 1,
                "7: the save lands with k three alone");
        require(logText().substr(logBefore).find("it was changed outside Seabass since the original was kept")
                    != std::string::npos,
                "7: refused for a grid changed by someone else, saying why");
        require(bytesOf(anlzOf("k one.wav", "DAT")) == theirs && bytesOf(keptFile("k one.wav")) == beforeRestore.at("kept"),
                "7: the other grid not written over, the kept entry as it was");
        undoNow();
        {
            std::ofstream out(anlzOf("k one.wav", "DAT"), std::ios::binary | std::ios::trunc);
            out << beforeRestore.at("dat");
        }
        invalidate();
        require(keptCount(QStringLiteral("Kept")) == 2, "7: both still offered");
        std::cout << "case 7 (refused for an audio file of another size and for a grid changed by someone else; "
                     "nothing written, the kept entries stay) OK\n";
    }

    // A save that goes back: an import into a place a folder is in.
    const auto failingImport = [&](const std::string &name) {
        writeBeatWav(computer / name, 30, 120.0);
        ImportRequest request;
        request.offered = {pathToUtf8(computer / name)};
        request.rekordboxPath = pioneer;
        request.enginePath = engine;
        request.date = "2026-11-26";
        const PreparedImport prepared = prepareTrackImport(request, application::CancellationToken{});
        require(prepared.refusal.empty() && !prepared.staged.changes.empty(), "the blocked import is prepared");
        require(session->stageAll(prepared.staged.changes), "the blocked import is staged");
        QStringList ids;
        fs::path destination;
        for (const auto &staged : prepared.staged.changes) {
            ids << staged->id();
            if (const auto import = std::dynamic_pointer_cast<ImportTrackChange>(staged)) {
                destination = pathFromUtf8(import->destinationFile());
            }
        }
        require(!destination.empty(), "the import has a destination");
        fs::create_directories(destination);  // in the way
        return std::pair{ids, destination};
    };

    // 8a. A save with a restore in it that goes back.
    {
        restore(QStringLiteral("Kept"));
        const auto [ids, destination] = failingImport("k blocked.wav");
        const QVariantMap failed = saveNow();
        require(!failed.value("error").toString().isEmpty(), "8a: the save goes back");
        require(bytesOf(anlzOf("k one.wav", "DAT")) == beforeRestore.at("dat")
                    && bytesOf(anlzOf("k one.wav", "2EX")) == beforeRestore.at("2ex")
                    && bytesOf(keptFile("k one.wav")) == beforeRestore.at("kept")
                    && bytesOf(keptEngineFile("k one.wav")) == beforeRestore.at("keptEngine")
                    && bytesOf(infrastructure::paths::stickAnalysisLedger(stick.root)) == beforeRestore.at("ledger"),
                "8a: the files, the kept store and the ledger as before");
        fs::remove_all(destination);
        session->unstageAll(ids);
        restorer.dropStaged();
        require(session->pendingCount() == 0, "8a: nothing waits");
        std::cout << "case 8a (a save with a restore that goes back leaves the kept store as it was) OK\n";
    }

    // 6. Restore with the cue switch on: the cue back on rekordbox's beat.
    {
        restore(QStringLiteral("Kept"));
        require(restorer.setMoveCuesBack(changeIdOf("k one.wav"), true), "6: the switch is set");
        require(saveNow().value("error").toString().isEmpty(), "6: the restore is saved");
        require(std::abs(hotCueOf(pdbRow("k one.wav")) - onBeat) < 1.0
                    && std::abs(hotCueOf(oneLibraryRow("k one.wav")) - onBeat) < 1.0
                    && std::abs(hotCueOf(engineRow("k one.wav")) - onBeat) < 1.5,
                "6: the cue back on rekordbox's beat in all three libraries");
        const AnlzFile datOriginal = AnlzFile::fromBytes(original["k one.wav"]["DAT"], "original");
        const AnlzFile datAfter = AnlzFile::readRaw(pathToUtf8(anlzOf("k one.wav", "DAT")));
        require(datAfter.sections.size() == datOriginal.sections.size(), "6: the original's sections");
        for (std::size_t i = 0; i < datAfter.sections.size(); ++i) {
            if (!isCueList(datAfter.sections[i].fourcc)) {
                require(datAfter.sections[i].rawBytes == datOriginal.sections[i].rawBytes,
                        "6: every section but the cue lists is the original's");
            }
        }
        require(engineGrid("k one.wav") == originalEngineGrid["k one.wav"], "6: Engine's grid back");
        std::cout << "case 6 (restore with the cue switch on: the cue moves back with the grid in every library) OK\n";
    }

    // 8b. Analyse again in a save that goes back, then saved, then undone.
    {
        const std::string datBefore = bytesOf(anlzOf("k one.wav", "DAT"));
        analyseAgain(QStringLiteral("Kept"), false);
        const auto [ids, destination] = failingImport("k blocked two.wav");
        const QVariantMap failed = saveNow();
        require(!failed.value("error").toString().isEmpty(), "8b: the save goes back");
        require(!fs::exists(keptFile("k one.wav")) && !fs::exists(keptEngineFile("k one.wav"))
                    && bytesOf(anlzOf("k one.wav", "DAT")) == datBefore,
                "8b: nothing kept, nothing written");
        fs::remove_all(destination);
        session->unstageAll(ids);
        require(saveNow().value("error").toString().isEmpty(), "8b: Analyse again saved");
        require(fs::exists(keptFile("k one.wav")) && fs::exists(keptEngineFile("k one.wav")), "8b: kept now");
        undoNow();
        invalidate();
        require(!fs::exists(keptFile("k one.wav")) && !fs::exists(keptEngineFile("k one.wav"))
                    && bytesOf(anlzOf("k one.wav", "DAT")) == datBefore,
                "8b: Undo Last Save takes away what Analyse again kept");
        std::cout << "case 8b (Analyse again in a save that goes back keeps nothing; Undo takes away what it kept) OK\n";
    }

    // 9. Free Up Space.
    {
        analyseAgain(QStringLiteral("Kept"), false);
        require(saveNow().value("error").toString().isEmpty(), "9: Analyse again saved");
        local::KeptRekordboxAnalysis gone =
            local::StickKeptRekordboxAnalysisStore(stick.root).find(keyOf("k one.wav")).entry;
        gone.pathKey = "contents/unknownartist/unknownalbum/k gone.wav";
        gone.stickPath = "/Contents/UnknownArtist/UnknownAlbum/k gone.wav";
        local::KeptRekordboxRecord goneRecord;
        goneRecord.pathKey = gone.pathKey;
        local::StickKeptRekordboxAnalysisStore(stick.root).keep(gone, goneRecord);
        KeptAnalysisSpaceController space;
        QSignalSpy removed(&space, &KeptAnalysisSpaceController::removeFinished);
        space.refresh(pioneer, engine);
        require(waitFor(&space, &KeptAnalysisSpaceController::busyChanged, [&] { return !space.busy(); }), "9: listed");
        const QVariantList groups = space.groups();
        std::map<std::string, QVariantMap> byName;
        for (const QVariant &g : groups) {
            const QVariantMap group = g.toMap();
            byName[group.value("kind").toString().toStdString() + (group.value("gone").toBool() ? ":gone" : "")] = group;
        }
        const auto sizeOf = [&](const std::vector<fs::path> &files) {
            qlonglong sum = 0;
            for (const fs::path &file : files) {
                sum += static_cast<qlonglong>(fs::file_size(file));
            }
            return sum;
        };
        require(groups.size() == 3 && byName.count("rekordbox") && byName.count("engine") && byName.count("rekordbox:gone"),
                "9: three groups: rekordbox's, the player's, and one of a track gone");
        require(byName["rekordbox"].value("count").toInt() == 2
                    && byName["rekordbox"].value("bytes").toLongLong()
                           == sizeOf({keptFile("k one.wav"), keptRecordFile("k one.wav"), keptFile("k three.wav"),
                                      keptRecordFile("k three.wav")})
                    && !byName["rekordbox"].value("included").toBool(),
                "9: rekordbox's kept analyses of two tracks, their size with their records', unticked");
        require(byName["engine"].value("count").toInt() == 2
                    && byName["engine"].value("bytes").toLongLong()
                           == sizeOf({keptEngineFile("k one.wav"), keptEngineFile("k three.wav")})
                    && !byName["engine"].value("included").toBool(),
                "9: the player's kept analyses of two tracks, their size, unticked");
        require(byName["rekordbox:gone"].value("count").toInt() == 1 && !byName["rekordbox:gone"].value("included").toBool()
                    && space.includedCount() == 0,
                "9: the one of a track not on the stick under its name unticked too");
        std::cerr << "  Free Up Space: " << space.totalCount() << " kept, " << space.totalBytesHuman().toStdString()
                  << std::endl;
        for (int i = 0; i < groups.size(); ++i) {
            space.setIncluded(i, true);
        }
        space.removeIncluded();
        require(waitFor(&space, &KeptAnalysisSpaceController::removeFinished, [&] { return removed.count() > 0; }),
                "9: removed");
        const QVariantMap summary = removed.at(0).at(0).toMap();
        require(summary.value("written").toInt() == 5 && summary.value("total").toInt() == 5
                    && summary.value("warning").toString().isEmpty(),
                "9: all five removed");
        require(!fs::exists(keptFile("k one.wav")) && !fs::exists(keptEngineFile("k one.wav"))
                    && !fs::exists(keptRecordFile("k one.wav"))
                    && !fs::exists(local::keptRekordboxAnalysisFile(stick.root, gone.pathKey))
                    && !fs::exists(local::keptRekordboxRecordFile(stick.root, gone.pathKey)),
                "9: their files are gone, the records with them");
        require(keptCount(QStringLiteral("Kept")) == 0, "9: no track of Kept has an original kept");
        require(restoreRefused(QStringLiteral("Kept")), "9: the restore is no longer offered");
        require(pdbTempoKey("k one.wav").tempo != 11950, "9: Seabass's analysis stays");
        std::cout << "case 9 (Free Up Space lists, sizes and removes the kept originals; restore is not offered "
                     "after) OK\n";
    }

    // 10. A five-minute track, for the size what is kept of it takes.
    {
        writeBeatWav(computer / "k long.wav", 300, 124.0);
        require(importer.importFiles(pioneer, engine, QVariantList{pathToQString(computer / "k long.wav")},
                                     QStringLiteral("Long"), true),
                "10: the import starts");
        require(waitFor(&importer, &TrackImportController::busyChanged, [&] { return !importer.busy(); }), "10: import");
        require(saveNow().value("error").toString().isEmpty(), "10: imported");
        local::AnalysisLedger without = ledger();
        without.erase(keyOf("k long.wav"));
        std::string error;
        require(local::writeAnalysisLedger(stick.root, without, [](const std::string &) {}, &error), error);
        writeBeatWav(onStick / "k long.wav", 300, 124.0, 0.28);
        local::BeatGridLedger lines = gridLedger();
        lines.erase(keyOf("k long.wav"));
        require(local::writeBeatGridLedger(stick.root, lines, &error), error);
        analyseAgain(QStringLiteral("Long"), false);
        require(saveNow().value("error").toString().isEmpty(), "10: analysed again");
        std::uintmax_t files = 0;
        for (const char *extension : {"DAT", "EXT", "2EX"}) {
            const auto size = fs::file_size(anlzOf("k long.wav", extension));
            files += size;
            std::cout << "  five minutes: ." << extension << " " << size << " bytes" << std::endl;
        }
        const auto kept = fs::file_size(keptFile("k long.wav"));
        std::cout << "  five minutes: the three files " << files << " bytes, kept on the stick as " << kept
                  << " bytes; the player's analysis kept as " << fs::file_size(keptEngineFile("k long.wav"))
                  << " bytes" << std::endl;
        require(kept > 0 && kept < files, "10: the kept entry is smaller than the files, deflated");
        std::cout << "case 10 (what is kept of a five-minute track) OK\n";
    }

    std::cout << "original_analysis_restore_test: all cases passed\n";
    return 0;
}
