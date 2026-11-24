// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Cues a few milliseconds off (#121), on tests/fixtures/cue-offsets: a
// whole small stick with rekordbox's own export and a Prime 4's own
// analysis of seven click tracks, one per kind of file, each Engine row
// put into one of the states found on a real stick (see the fixture's
// README and expected.json). Through SyncController and the library's
// edit session, as Sync Cue Points drives them.
//
//   classify  each row gets its state; the three "change nothing" rows
//             are not listed at all
//   repair    the three known rows moved by one save: each cue where the
//             sound puts it, every other row byte for byte as it was, a
//             backup made, a second scan lists only the unknown row, and
//             Undo puts m.db, the analysis files and exportLibrary.db
//             back byte for byte
//   unknown   with the log gone the two logged rows are unknown too:
//             nothing moves them but the DJ's pick of a side
//   player    the player's shift is recognised only on a row whose mark
//             is gone, only within a sample of the shift its file's tag
//             gets: 2292 samples (Lavc) and 229 (no Info frame)
//   loops     loops and memory cues in the same three states
//   sync      Sync Cue Points still calls all seven pairs the same cue:
//             the repair owns these, not Sync's tolerance
//   order     a sync and a repair of one pair staged on two pages and
//             saved together, either first: the second writes nothing, is
//             not counted, and Undo takes back the first
//   handles   the check before a write leaves m.db closed mid-save
//   mixed     a known cue keeps its side; only the unknown one waits for
//             the pick
//   pairing   a hot cue pairs only with its own pad
//
// usage: cue_offset_repair_test FIXTURE_DIR CASE

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/sync_libraries.hpp"
#include "domain/cue_offset_repair.hpp"
#include "engine_change_fixture.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/changes/cue_offset_repair_change.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/save_context.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/ports/cancellation_token.hpp"
#include "gui/sync_controller.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "nothing_else_running.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
namespace testing = seabass::testing;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;
using seabass::domain::CueOffsetCause;
using seabass::domain::CueOffsetFinding;
using seabass::domain::CueOffsetSide;
using seabass::domain::CuePoint;
using seabass::domain::Track;
using seabass::gui::EditSessionRegistry;
using seabass::gui::LibraryEditSession;
using seabass::gui::SyncController;

namespace
{

fs::path g_fixture;

struct Expected
{
    std::string file;
    int rate = 0;
    double rekordboxPadMs = 0.0;
    double enginePadMs = 0.0;
    double shiftMs = 0.0;
    std::string side;  // "none", "engine", "rekordbox", "unknown: ..."
    double moveToMs = 0.0;
};

// The rows on the stick, or with `derived` the states the test makes on
// a copy (the player's 229 samples).
std::vector<Expected> expectedRows(bool derived = false)
{
    QFile file(seabass::gui::pathToQString(g_fixture / "stick" / "expected.json"));
    const bool opened = file.open(QIODevice::ReadOnly);
    assert(opened);
    (void)opened;
    std::vector<Expected> rows;
    for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
        const QJsonObject o = value.toObject();
        if (o.value("derived").toBool() != derived) {
            continue;
        }
        Expected e;
        e.file = o.value("file").toString().toStdString();
        e.rate = o.value("rate").toInt();
        e.rekordboxPadMs = o.value("rekordboxPadA_ms").toDouble();
        e.enginePadMs = o.value("enginePad1_ms").toDouble();
        e.shiftMs = o.value("modelShift_ms").toDouble();
        e.side = o.value("sideToMove").toString().toStdString();
        e.moveToMs = o.value("moveTo_ms").toDouble();
        rows.push_back(e);
    }
    assert(rows.size() == (derived ? 1u : 7u));
    return rows;
}

const Expected &expectedFor(const std::string &file)
{
    static const std::vector<Expected> rows = expectedRows();
    for (const auto &row : rows) {
        if (row.file == file) {
            return row;
        }
    }
    assert(false && "a file of the fixture");
    return rows.front();
}

struct Stick
{
    fs::path root;
    fs::path pioneer;
    fs::path engine;
    std::string db;
    QString pioneerPath() const { return seabass::gui::pathToQString(pioneer); }
    QString enginePath() const { return seabass::gui::pathToQString(engine); }
};

Stick copyStick(const std::string &name)
{
    Stick stick;
    stick.root = testing::scratchRoot() / name;
    fs::remove_all(stick.root);
    fs::create_directories(stick.root.parent_path());
    fs::copy(g_fixture / "stick", stick.root, fs::copy_options::recursive);
    stick.pioneer = stick.root / "PIONEER";
    stick.engine = stick.root / "Engine Library";
    stick.db = pathToUtf8(stick.engine / "Database2" / "m.db");
    return stick;
}

std::string fileNameOf(const Track &track)
{
    return pathToUtf8(pathFromUtf8(track.filePath).filename());
}

std::vector<Track> readEngine(const Stick &stick, bool onRekordboxTimeline)
{
    seabass::infrastructure::engine::LibdjinteropEngineReader reader(pathToUtf8(stick.engine));
    reader.setCorrectMp3CueTiming(onRekordboxTimeline);
    return reader.readAll();
}

std::vector<Track> readRekordbox(const Stick &stick)
{
    return seabass::infrastructure::rekordbox::KaitaiRekordboxReader(pathToUtf8(stick.pioneer)).readAll();
}

std::vector<Track> readOneLibrary(const Stick &stick)
{
    return seabass::infrastructure::onelibrary::OneLibraryReader(pathToUtf8(stick.pioneer)).readAll();
}

Track trackOf(const std::vector<Track> &tracks, const std::string &file)
{
    for (const auto &track : tracks) {
        if (fileNameOf(track) == file) {
            return track;
        }
    }
    std::cerr << "no row for " << file << "\n";
    assert(false);
    return tracks.front();
}

std::optional<CuePoint> hotCue(const Track &track, int pad, bool loop = false)
{
    for (const auto &cue : track.cues) {
        if (cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == pad && cue.isLoop == loop) {
            return cue;
        }
    }
    return std::nullopt;
}

std::optional<CuePoint> memoryCue(const Track &track)
{
    for (const auto &cue : track.cues) {
        if (cue.kind == CuePoint::Kind::Memory && !cue.isLoop) {
            return cue;
        }
    }
    return std::nullopt;
}

// Engine's cues of `file` set to `raw`, positions on Engine's own
// timeline, through the app's writer (which takes the file's shift off
// what it is given). `mark` puts the import mark back on afterwards.
void setEngineCues(const Stick &stick, const std::string &file, std::vector<CuePoint> raw, bool mark = false)
{
    const Track &row = trackOf(readEngine(stick, true), file);
    seabass::infrastructure::engine::LibdjinteropEngineCueWriter writer(pathToUtf8(stick.engine));
    const double shift = writer.cueTimelineShiftMs(row.sourceId);
    for (auto &cue : raw) {
        cue.positionMs += shift;
        if (cue.isLoop) {
            cue.loopEndMs += shift;
        }
    }
    writer.writeHotCues(row.sourceId, raw);
    const bool done = testing::sqlExec(stick.db, "UPDATE PerformanceData SET thirdPartySourceId = "
                                                     + std::string(mark ? "1" : "NULL") + " WHERE trackId = "
                                                     + row.sourceId + ";");
    assert(done);
    (void)done;
}

CuePoint hot(int pad, double ms)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = pad;
    cue.positionMs = ms;
    return cue;
}

CuePoint loop(int pad, double startMs, double endMs)
{
    CuePoint cue = hot(pad, startMs);
    cue.isLoop = true;
    cue.loopEndMs = endMs;
    return cue;
}

CuePoint memory(double ms)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Memory;
    cue.positionMs = ms;
    return cue;
}

void waitUntilIdle(SyncController &controller)
{
    QSignalSpy busy(&controller, &SyncController::busyChanged);
    for (int i = 0; i < 3000 && controller.busy(); ++i) {
        busy.wait(100);
    }
    assert(!controller.busy());
}

void analyze(SyncController &controller, const Stick &stick)
{
    controller.analyze(stick.pioneerPath(), stick.enginePath());
    waitUntilIdle(controller);
    if (!controller.errorMessage().isEmpty()) {
        std::cerr << "sync analysis: " << controller.errorMessage().toStdString() << "\n";
    }
    assert(controller.errorMessage().isEmpty());
}

LibraryEditSession *sessionOf(const Stick &stick)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(stick.pioneerPath()));
    assert(session);
    return session;
}

QVariantMap saveAndWait(const Stick &stick, SyncController &controller, bool undo = false)
{
    LibraryEditSession *session = sessionOf(stick);
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    if (undo) {
        controller.undoLastOperation();
    } else {
        session->save();
    }
    for (int i = 0; i < 1200 && finished.isEmpty(); ++i) {
        finished.wait(100);
    }
    assert(!finished.isEmpty() && "the save finished");
    waitUntilIdle(controller);
    const QVariantMap summary = finished.takeFirst().at(0).toMap();
    std::cout << "  " << (undo ? "undo" : "save") << ": " << summary.value("written").toInt() << " of "
              << summary.value("total").toInt() << "; error \"" << summary.value("error").toString().toStdString()
              << "\"\n";
    assert(summary.value("error").toString().isEmpty());
    return summary;
}

// Each listed row, by file: the side the repair would move.
std::map<std::string, CueOffsetSide> listed(SyncController &controller)
{
    std::map<std::string, CueOffsetSide> rows;
    for (const CueOffsetFinding &finding : controller.cueOffsetsModel()->findings()) {
        const std::string file = fileNameOf(finding.engine);
        std::cout << "  listed " << file << ": " << seabass::gui::sideName(finding.sideToMove).toStdString() << ", "
                  << finding.cues.size() << " cue(s): " << finding.reason() << "\n";
        rows[file] = finding.sideToMove;
    }
    return rows;
}

int rowOf(SyncController &controller, const std::string &file)
{
    const auto &findings = controller.cueOffsetsModel()->findings();
    for (size_t i = 0; i < findings.size(); ++i) {
        if (fileNameOf(findings[i].engine) == file) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

CueOffsetSide sideOf(const std::string &side)
{
    if (side == "engine") {
        return CueOffsetSide::Engine;
    }
    if (side == "rekordbox") {
        return CueOffsetSide::Rekordbox;
    }
    return CueOffsetSide::Unknown;
}

void checkWithin(double actual, double expected, double within, const std::string &what)
{
    std::cout << "  " << what << ": " << actual << " ms, expected " << expected << " ms (within " << within << ")\n";
    assert(std::abs(actual - expected) <= within);
}

// 1. Each row gets its state; the "change nothing" rows are not listed.
void testClassify()
{
    const Stick stick = copyStick("cue_offsets_classify");
    SyncController controller;
    analyze(controller, stick);
    const auto rows = listed(controller);
    for (const auto &[file, side] : rows) {
        (void)side;
        assert(expectedFor(file).side != "none" && "a row to leave alone is not listed");
    }
    for (const auto *file : {"beats-D-lame-48000.mp3", "beats-B-noencoder-44100.mp3", "beats-B-lavc-44100.mp3",
                             "beats-D-lame-44100.mp3"}) {
        assert(rows.count(file) == 1 && "every row a repair is about is listed");
        assert(rows.at(file) == sideOf(expectedFor(file).side));
    }
    assert(rows.size() == 4);
    assert(controller.cueOffsetKnownCount() == 3);
    const auto &lavc = controller.cueOffsetsModel()->findings()[static_cast<size_t>(rowOf(controller, "beats-B-lavc-44100.mp3"))];
    assert(lavc.cues.size() == 1 && lavc.cues[0].cause == CueOffsetCause::MovedByPlayer);
    // A row the log explains never says the stick has nothing to say; the
    // row it does not explain says so.
    const auto reasonOf = [&](const char *file) {
        return controller.cueOffsetsModel()->findings()[static_cast<size_t>(rowOf(controller, file))].reason();
    };
    for (const auto *file : {"beats-D-lame-48000.mp3", "beats-B-noencoder-44100.mp3", "beats-B-lavc-44100.mp3"}) {
        assert(reasonOf(file).find("No copy of this track is logged") == std::string::npos);
    }
    assert(reasonOf("beats-D-lame-48000.mp3").find("2026-09-30") != std::string::npos);
    assert(reasonOf("beats-D-lame-44100.mp3").find("No copy of this track is logged") != std::string::npos);
    std::cout << "classify: 3 rows left alone, 3 known, 1 unknown OK\n";
}

std::string performanceRow(const Stick &stick, const std::string &id)
{
    std::string line;
    for (const auto &table : {"Track", "PerformanceData"}) {
        const std::string key = std::string(table) == "Track" ? "id" : "trackId";
        for (const auto &row : testing::sqlRows(stick.db, "SELECT * FROM " + std::string(table) + " WHERE " + key
                                                               + " = " + id + ";")) {
            for (const auto &value : row) {
                line += value + "|";
            }
        }
    }
    return line;
}

// 2. The three known rows moved by one save.
void testRepair()
{
    const Stick stick = copyStick("cue_offsets_repair");
    SyncController controller;
    analyze(controller, stick);
    const testing::DatabaseDump dumpBefore = testing::dumpDatabase(stick.db);
    const auto engineBefore = readEngine(stick, true);
    const auto rekordboxBefore = readRekordbox(stick);
    const std::vector<std::string> untouched = {"beats-ref-44100.wav", "beats-A-notag-44100.mp3",
                                                "beats-C-badcrc-44100.mp3", "beats-D-lame-44100.mp3"};
    std::map<std::string, std::string> rowsBefore;
    std::map<std::string, std::string> anlzBefore;
    for (const auto &file : untouched) {
        rowsBefore[file] = performanceRow(stick, trackOf(engineBefore, file).sourceId);
        const Track &rb = trackOf(rekordboxBefore, file);
        for (const char *ext : {".DAT", ".EXT", ".2EX"}) {
            fs::path anlz = stick.root / pathFromUtf8(rb.analysisFile.substr(1));
            anlz.replace_extension(ext);
            if (fs::exists(anlz)) {
                anlzBefore[pathToUtf8(anlz)] = testing::fileBytes(anlz);
            }
        }
    }
    assert(!anlzBefore.empty());
    // What Undo must put back besides m.db: the moved row's analysis
    // files and exportLibrary.db, byte for byte.
    std::map<std::string, std::string> writtenBefore;
    {
        const Track &rb = trackOf(rekordboxBefore, "beats-B-noencoder-44100.mp3");
        for (const char *ext : {".DAT", ".EXT", ".2EX"}) {
            fs::path anlz = stick.root / pathFromUtf8(rb.analysisFile.substr(1));
            anlz.replace_extension(ext);
            if (fs::exists(anlz)) {
                writtenBefore[pathToUtf8(anlz)] = testing::fileBytes(anlz);
            }
        }
        const fs::path oneLibrary = stick.pioneer / "rekordbox" / "exportLibrary.db";
        writtenBefore[pathToUtf8(oneLibrary)] = testing::fileBytes(oneLibrary);
    }
    const auto oneLibraryBefore = readOneLibrary(stick);

    assert(controller.stageKnownCueOffsets() == 3);
    assert(controller.cueOffsetStagedCount() == 3);
    assert(controller.cueOffsetKnownCount() == 0);
    const QVariantMap summary = saveAndWait(stick, controller);
    assert(summary.value("written").toInt() == 3);

    // A backup first: the save can be undone, from backups on the stick.
    assert(controller.canUndo());
    const fs::path backups = seabass::infrastructure::paths::stickBackupsDir(stick.root);
    assert(fs::exists(backups) && !fs::is_empty(backups) && "the save backed the catalogs up on the stick");

    const auto engineRaw = readEngine(stick, false);
    for (const auto *file : {"beats-D-lame-48000.mp3", "beats-B-lavc-44100.mp3"}) {
        const Expected &e = expectedFor(file);
        const auto pad = hotCue(trackOf(engineRaw, file), 1);
        assert(pad);
        checkWithin(pad->positionMs, e.moveToMs, 1000.0 / e.rate, std::string(file) + " Engine pad 1");
    }
    {
        // rekordbox keeps whole milliseconds: within one of where the
        // sound is, on both its formats.
        const Expected &e = expectedFor("beats-B-noencoder-44100.mp3");
        const auto padA = hotCue(trackOf(readRekordbox(stick), e.file), 1);
        assert(padA);
        checkWithin(padA->positionMs, e.moveToMs, 1.0, "beats-B-noencoder-44100.mp3 rekordbox pad A");
        const auto oneLibraryA = hotCue(trackOf(readOneLibrary(stick), e.file), 1);
        assert(oneLibraryA);
        checkWithin(oneLibraryA->positionMs, e.moveToMs, 1.0, "beats-B-noencoder-44100.mp3 OneLibrary pad A");
    }
    for (const auto &file : untouched) {
        assert(performanceRow(stick, trackOf(engineBefore, file).sourceId) == rowsBefore.at(file)
               && "every other Engine row as it was");
    }
    for (const auto &[file, bytes] : anlzBefore) {
        assert(testing::fileBytes(pathFromUtf8(file)) == bytes && "every other analysis file as it was");
    }
    std::cout << "repair: three cues where the sound is, the other rows byte for byte, backed up OK\n";

    // A second scan lists only the row nothing explains.
    analyze(controller, stick);
    const auto rows = listed(controller);
    assert(rows.size() == 1 && rows.count("beats-D-lame-44100.mp3") == 1);
    assert(rows.at("beats-D-lame-44100.mp3") == CueOffsetSide::Unknown);
    std::cout << "repair: a second scan lists only the unknown row OK\n";

    // Undoable: m.db as before the save.
    saveAndWait(stick, controller, true);
    testing::DatabaseDump after = testing::dumpDatabase(stick.db);
    testing::DatabaseDump before = dumpBefore;
    before.erase("Information");
    after.erase("Information");
    const auto diffs = testing::differences(before, after);
    testing::printDifferences(diffs);
    assert(diffs.empty());
    for (const auto &[file, bytes] : writtenBefore) {
        std::cout << "  undo: " << pathToUtf8(pathFromUtf8(file).filename()) << " as before\n";
        assert(testing::fileBytes(pathFromUtf8(file)) == bytes && "Undo puts every written file back byte for byte");
    }
    {
        const auto padA = hotCue(trackOf(readOneLibrary(stick), "beats-B-noencoder-44100.mp3"), 1);
        const auto padABefore = hotCue(trackOf(oneLibraryBefore, "beats-B-noencoder-44100.mp3"), 1);
        assert(padA && padABefore && padA->positionMs == padABefore->positionMs && "OneLibrary reads as before");
    }
    analyze(controller, stick);
    assert(listed(controller).size() == 4);
    std::cout << "repair: Undo puts m.db, the analysis files and exportLibrary.db back OK\n";
}

// 3. Without the log, a pair at 0 is unknown: nothing moves it but the
// DJ's pick.
void testUnknown()
{
    const Stick stick = copyStick("cue_offsets_unknown");
    fs::remove(seabass::infrastructure::paths::stickOperationLog(stick.root));
    SyncController controller;
    analyze(controller, stick);
    const auto rows = listed(controller);
    assert(rows.size() == 4);
    for (const auto *file : {"beats-D-lame-48000.mp3", "beats-B-noencoder-44100.mp3", "beats-D-lame-44100.mp3"}) {
        assert(rows.at(file) == CueOffsetSide::Unknown);
        const int row = rowOf(controller, file);
        assert(!controller.stageCueOffset(row) && "an unknown row is not moved on its own");
    }
    // The player's row needs no log.
    assert(rows.at("beats-B-lavc-44100.mp3") == CueOffsetSide::Engine);
    assert(controller.cueOffsetStagedCount() == 0);
    assert(controller.stageKnownCueOffsets() == 1 && "Move All Known leaves the unknown rows alone");
    assert(controller.cueOffsetStagedCount() == 1);
    // Unstaged again, nothing is staged, and the page no longer says so.
    assert(!controller.statusMessage().isEmpty());
    controller.unstageCueOffset(rowOf(controller, "beats-B-lavc-44100.mp3"));
    assert(controller.cueOffsetStagedCount() == 0 && controller.statusMessage().isEmpty());
    assert(controller.stageKnownCueOffsets() == 1);

    // The DJ picks a side: that is what moves an unknown row.
    const Expected &e = expectedFor("beats-D-lame-48000.mp3");
    assert(controller.stageCueOffset(rowOf(controller, e.file), QStringLiteral("engine")));
    assert(controller.cueOffsetStagedCount() == 2);
    saveAndWait(stick, controller);
    const auto pad = hotCue(trackOf(readEngine(stick, false), e.file), 1);
    assert(pad);
    checkWithin(pad->positionMs, e.moveToMs, 1000.0 / e.rate, "picked side: Engine pad 1");
    const auto after = listed(controller);
    assert(after.size() == 2 && after.count("beats-B-noencoder-44100.mp3") && after.count("beats-D-lame-44100.mp3"));
    std::cout << "unknown: nothing offered without the log, moved only on the DJ's pick OK\n";
}

std::map<std::string, CueOffsetSide> listedAfter(const Stick &stick)
{
    SyncController controller;
    analyze(controller, stick);
    return listed(controller);
}

// 4. The player's shift: 2292 samples on the Lavc file, 229 on the file
// with no Info frame, each only on a row whose mark is gone and only to
// the sample.
void testPlayer()
{
    const double lavcRb = expectedFor("beats-B-lavc-44100.mp3").rekordboxPadMs;
    const double notagRb = expectedFor("beats-A-notag-44100.mp3").rekordboxPadMs;
    const auto samples = [](double n) { return n * 1000.0 / 44100.0; };
    {
        const Stick stick = copyStick("cue_offsets_player_marked");
        setEngineCues(stick, "beats-B-lavc-44100.mp3", {hot(1, lavcRb - samples(2292))}, true);
        assert(listedAfter(stick).count("beats-B-lavc-44100.mp3") == 0 && "a marked row is the player's to move");
        std::cout << "player: 2292 samples on a row still marked: not listed OK\n";
    }
    for (const double off : {2290.0, 2294.0}) {
        const Stick stick = copyStick("cue_offsets_player_near");
        setEngineCues(stick, "beats-B-lavc-44100.mp3", {hot(1, lavcRb - samples(off))});
        assert(listedAfter(stick).count("beats-B-lavc-44100.mp3") == 0 && "two samples off is not the player's shift");
    }
    std::cout << "player: 2290 and 2294 samples: not listed OK\n";
    {
        const Stick stick = copyStick("cue_offsets_player_wrong_tag");
        setEngineCues(stick, "beats-B-lavc-44100.mp3", {hot(1, lavcRb - samples(229))});
        setEngineCues(stick, "beats-A-notag-44100.mp3", {hot(1, notagRb - samples(2292))});
        const auto rows = listedAfter(stick);
        assert(rows.count("beats-B-lavc-44100.mp3") == 0 && "229 is not the Lavc file's shift");
        assert(rows.count("beats-A-notag-44100.mp3") == 0 && "2292 is not the untagged file's shift");
        std::cout << "player: each file's shift only, not the other's OK\n";
    }
    {
        // The 229 case, measured on the Prime 4 on 23 November: the file
        // with no Info frame, its mark gone, pad 1 229 samples early; the
        // model says 0, so it goes back onto rekordbox's number.
        const Expected derived = expectedRows(true).front();
        assert(derived.file == "beats-A-notag-44100.mp3"
               && std::abs(derived.enginePadMs - (notagRb - samples(229))) < 0.001);
        const Stick stick = copyStick("cue_offsets_player_229");
        setEngineCues(stick, derived.file, {hot(1, derived.enginePadMs)});
        SyncController controller;
        analyze(controller, stick);
        const auto rows = listed(controller);
        assert(rows.count("beats-A-notag-44100.mp3") == 1);
        assert(rows.at("beats-A-notag-44100.mp3") == CueOffsetSide::Engine);
        const auto &finding = controller.cueOffsetsModel()->findings()[static_cast<size_t>(
            rowOf(controller, "beats-A-notag-44100.mp3"))];
        assert(finding.playerShiftSamples == 229 && finding.cues.size() == 1
               && finding.cues[0].cause == CueOffsetCause::MovedByPlayer);
        assert(controller.stageCueOffset(rowOf(controller, "beats-A-notag-44100.mp3")));
        saveAndWait(stick, controller);
        const auto pad = hotCue(trackOf(readEngine(stick, false), "beats-A-notag-44100.mp3"), 1);
        assert(pad);
        checkWithin(pad->positionMs, derived.moveToMs, samples(1), "229 case: Engine pad 1");
        std::cout << "player: 229 samples on the untagged file recognised and moved back OK\n";
    }
}

// 5. A loop slot and a memory cue in each state: copied to Engine,
// copied to rekordbox (both logged), and the player's shift (a loop
// only: the player was not measured moving a set cue point).
void testLoops()
{
    const Stick stick = copyStick("cue_offsets_loops");
    const auto rekordboxTracks = readRekordbox(stick);
    seabass::infrastructure::rekordbox::RekordboxCueWriter rekordboxWriter(pathToUtf8(stick.pioneer));
    struct Row
    {
        std::string file;
        double engineDifferenceMs;  // rekordbox minus Engine's stored number
        bool withMemoryCue;
    };
    const double playerMs = 2292 * 1000.0 / 44100.0;
    const std::vector<Row> rows = {
        {"beats-D-lame-48000.mp3", 0.0, true},
        {"beats-B-noencoder-44100.mp3", 0.0, true},
        {"beats-B-lavc-44100.mp3", playerMs, false},
    };
    constexpr double LoopIn = 4000.0;
    constexpr double LoopOut = 6000.0;
    constexpr double Memory = 8000.0;
    for (const Row &row : rows) {
        const Expected &e = expectedFor(row.file);
        std::vector<CuePoint> rekordbox = {hot(1, e.rekordboxPadMs), loop(2, LoopIn, LoopOut)};
        if (row.withMemoryCue) {
            rekordbox.push_back(memory(Memory));
        }
        rekordboxWriter.writeHotCues(trackOf(rekordboxTracks, row.file).sourceId, rekordbox);
        std::vector<CuePoint> engine = {hot(1, e.enginePadMs), loop(2, LoopIn - row.engineDifferenceMs,
                                                                     LoopOut - row.engineDifferenceMs)};
        if (row.withMemoryCue) {
            engine.push_back(memory(Memory - row.engineDifferenceMs));
        }
        setEngineCues(stick, row.file, engine);
    }
    SyncController controller;
    analyze(controller, stick);
    listed(controller);
    for (const Row &row : rows) {
        const int listedRow = rowOf(controller, row.file);
        assert(listedRow >= 0 && "the row is listed");
        const auto &finding = controller.cueOffsetsModel()->findings()[static_cast<size_t>(listedRow)];
        assert(finding.sideToMove == sideOf(expectedFor(row.file).side));
        bool loopListed = false;
        bool memoryListed = false;
        for (const auto &offset : finding.cues) {
            const CuePoint &cue = finding.engine.cues[offset.engineIndex];
            loopListed = loopListed || cue.isLoop;
            memoryListed = memoryListed || cue.kind == CuePoint::Kind::Memory;
        }
        assert(loopListed && "the loop is listed with the hot cue");
        assert(memoryListed == row.withMemoryCue && "the memory cue is listed with the hot cue");
    }
    assert(controller.stageKnownCueOffsets() == 3);
    saveAndWait(stick, controller);

    const auto engineRaw = readEngine(stick, false);
    const auto rekordboxAfter = readRekordbox(stick);
    for (const Row &row : rows) {
        const Expected &e = expectedFor(row.file);
        if (sideOf(e.side) == CueOffsetSide::Engine) {
            const Track &track = trackOf(engineRaw, row.file);
            const auto l = hotCue(track, 2, true);
            assert(l);
            const double sample = 1000.0 / e.rate;
            checkWithin(l->positionMs, LoopIn - e.shiftMs, sample, row.file + " Engine loop in");
            checkWithin(l->loopEndMs, LoopOut - e.shiftMs, sample, row.file + " Engine loop out");
            if (row.withMemoryCue) {
                const auto m = memoryCue(track);
                assert(m);
                checkWithin(m->positionMs, Memory - e.shiftMs, sample, row.file + " Engine cue point");
            }
        } else {
            const Track &track = trackOf(rekordboxAfter, row.file);
            const auto l = hotCue(track, 2, true);
            assert(l);
            checkWithin(l->positionMs, LoopIn + e.shiftMs, 1.0, row.file + " rekordbox loop in");
            checkWithin(l->loopEndMs, LoopOut + e.shiftMs, 1.0, row.file + " rekordbox loop out");
            const auto m = memoryCue(track);
            assert(m);
            checkWithin(m->positionMs, Memory + e.shiftMs, 1.0, row.file + " rekordbox memory cue");
        }
    }
    analyze(controller, stick);
    const auto left = listed(controller);
    assert(left.size() == 1 && left.count("beats-D-lame-44100.mp3"));
    std::cout << "loops: loops and memory cues moved with the hot cues, in all three states OK\n";
}

int planIndexOf(SyncController &controller, const std::string &file)
{
    const auto &plans = controller.plansModel()->plans();
    for (size_t i = 0; i < plans.size(); ++i) {
        if (fileNameOf(plans[i].match.trackA) == file || fileNameOf(plans[i].match.trackB) == file) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void waitIdle(SyncController &a, SyncController &b)
{
    waitUntilIdle(a);
    waitUntilIdle(b);
}

// Stages the sync of `file` on `controller`, the only row it ticks.
void stageSync(SyncController &controller, const std::string &file)
{
    const int plan = planIndexOf(controller, file);
    assert(plan >= 0);
    controller.setAllIncluded(false);
    controller.setIncluded(plan, true);
    controller.stageSelected();
    assert(controller.stagedCount() == 1 && "the sync is staged");
}

// H1 of the review: a sync and a repair of one pair, each read before the
// other wrote. One page never stages both (see the asserts below), so two
// pages over one library do, as two windows on one stick would: each
// stages through its own controller, one save writes both. The second
// change finds the cues it read gone and writes nothing; the summary says
// so, it is not counted as synced, and Undo takes back what the first
// wrote. Removing the check from either change fails this.
void testOrder()
{
    {
        // Sync first: Sync Cue Points copies rekordbox's cues (its memory
        // cue among them) onto Engine; the repair read before it would put
        // Engine's old set back without that cue.
        const std::string file = "beats-D-lame-48000.mp3";
        const Stick stick = copyStick("cue_offsets_order_sync_first");
        SyncController syncPage;
        SyncController repairPage;
        analyze(syncPage, stick);
        analyze(repairPage, stick);
        stageSync(syncPage, file);
        assert(!syncPage.stageCueOffset(rowOf(syncPage, file)) && "one page never stages both");
        assert(repairPage.stageCueOffset(rowOf(repairPage, file)));
        const QVariantMap summary = saveAndWait(stick, syncPage);
        waitIdle(syncPage, repairPage);
        std::cout << "  notes \"" << summary.value("notes").toString().toStdString() << "\"\n";
        assert(summary.value("written").toInt() == 1 && summary.value("skipped").toInt() == 1);
        assert(summary.value("notes").toString().contains(QStringLiteral("changed after they were read")));
        const Track after = trackOf(readEngine(stick, true), file);
        assert(memoryCue(after) && "the cue point the sync wrote is still there");
        checkWithin(hotCue(after, 1)->positionMs, expectedFor(file).rekordboxPadMs, 0.1, "pad 1 as the sync left it");
        assert(rowOf(syncPage, file) < 0 && rowOf(repairPage, file) < 0 && "read again: nothing left to move");
        std::cout << "order: sync first, the repair read before it writes nothing OK\n";
    }
    {
        // Repair first: rekordbox's off cue is moved onto the sound; the
        // sync read before it would copy rekordbox's old number onto Engine.
        const std::string file = "beats-B-noencoder-44100.mp3";
        const Stick stick = copyStick("cue_offsets_order_repair_first");
        SyncController repairPage;
        SyncController syncPage;
        analyze(repairPage, stick);
        analyze(syncPage, stick);
        const double engineBefore = hotCue(trackOf(readEngine(stick, false), file), 1)->positionMs;
        const double rekordboxBefore = hotCue(trackOf(readRekordbox(stick), file), 1)->positionMs;
        assert(repairPage.stageCueOffset(rowOf(repairPage, file)));
        repairPage.setAllIncluded(false);
        repairPage.setIncluded(planIndexOf(repairPage, file), true);
        repairPage.stageSelected();
        assert(repairPage.stagedCount() == 0 && "one page never stages both");
        stageSync(syncPage, file);
        const QVariantMap summary = saveAndWait(stick, syncPage);
        waitIdle(syncPage, repairPage);
        std::cout << "  notes \"" << summary.value("notes").toString().toStdString() << "\"\n";
        assert(summary.value("written").toInt() == 1 && summary.value("skipped").toInt() == 1);
        assert(syncPage.syncedCueCount() == 0 && "a sync that wrote nothing is not counted as synced");
        checkWithin(hotCue(trackOf(readEngine(stick, false), file), 1)->positionMs, engineBefore, 0.01,
                    "Engine pad 1 as it was");
        checkWithin(hotCue(trackOf(readRekordbox(stick), file), 1)->positionMs, expectedFor(file).moveToMs, 1.0,
                    "rekordbox pad A as the repair left it");
        // Read again, the sync carries rekordbox's moved cue, if any.
        if (const int fresh = planIndexOf(syncPage, file); fresh >= 0) {
            for (const auto &cue : syncPage.plansModel()->plans()[static_cast<size_t>(fresh)].cuesToApply) {
                if (cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == 1) {
                    checkWithin(cue.positionMs, expectedFor(file).moveToMs, 1.0, "the sync read again: pad 1");
                }
            }
        }
        std::cout << "order: repair first, the sync read before it writes nothing, not counted OK\n";

        // Undo after a save with a skip: what the repair wrote goes back,
        // and nothing the skipped sync did not write moves.
        assert(syncPage.canUndo());
        saveAndWait(stick, syncPage, true);
        waitIdle(syncPage, repairPage);
        checkWithin(hotCue(trackOf(readRekordbox(stick), file), 1)->positionMs, rekordboxBefore, 0.5,
                    "Undo: rekordbox pad A back");
        checkWithin(hotCue(trackOf(readEngine(stick, false), file), 1)->positionMs, engineBefore, 0.01,
                    "Undo: Engine pad 1 as it was");
        assert(rowOf(syncPage, file) >= 0 && "Undo: listed again");
        std::cout << "order: Undo after a save with a skip puts back what was written OK\n";
    }
}

// Whether this process holds `file` open: its descriptors on Linux; where
// there are none to list, a rename, which Windows refuses on an open file.
bool heldOpen(const fs::path &file)
{
    const fs::path fds("/proc/self/fd");
    std::error_code ec;
    if (fs::exists(fds, ec)) {
        const fs::path target = fs::canonical(file);
        for (const auto &entry : fs::directory_iterator(fds, ec)) {
            std::error_code linkEc;
            if (fs::read_symlink(entry.path(), linkEc) == target) {
                return true;
            }
        }
        return false;
    }
    fs::path aside = file;
    aside += ".renamed";
    fs::rename(file, aside, ec);
    if (ec) {
        return true;
    }
    fs::rename(aside, file);
    return false;
}

// H2 of the second review: the check before a write reads Engine on a
// connection of its own and closes it, so nothing holds m.db while a batch
// written through the scratch copy is renamed over it. A repair of
// rekordbox's side reads Engine (its source) and writes no Engine row.
void testHandles()
{
    const std::string file = "beats-B-noencoder-44100.mp3";
    const Stick stick = copyStick("cue_offsets_handles");
    SyncController controller;
    analyze(controller, stick);
    const CueOffsetFinding finding =
        controller.cueOffsetsModel()->findings()[static_cast<size_t>(rowOf(controller, file))];
    seabass::gui::CueOffsetRepairChange change(stick.pioneerPath(), stick.enginePath(), finding,
                                               CueOffsetSide::Rekordbox, std::nullopt, 1);
    seabass::application::CancellationToken token;
    seabass::gui::SaveContext ctx(token, seabass::application::NullProgressReporter::instance(), {},
                                  stick.pioneerPath(), stick.enginePath());
    const auto outcome = change.apply(ctx);
    assert(outcome.ok && !outcome.skipped && "the repair wrote");
    const bool held = heldOpen(pathFromUtf8(stick.db));
    std::cout << "  m.db " << (held ? "still open" : "closed") << " after the check, mid-save\n";
    assert(!held && "the check holds no connection to m.db across the save");
    const bool finished = !ctx.runFinishHooks(true).error;
    assert(finished);
    (void)finished;
    std::cout << "handles: the check before the write leaves m.db closed OK\n";
}

// M1 of the review: one track with a cue the player moved (known: Engine)
// and one at the same number with no log line (unknown). Each keeps its
// own: the known one moves on Engine whatever the DJ picks, the unknown
// one only on the pick.
void testMixed()
{
    const std::string file = "beats-B-lavc-44100.mp3";
    const Expected &e = expectedFor(file);
    const double rekordboxPad2 = hotCue(trackOf(readRekordbox(copyStick("cue_offsets_mixed_read")), file), 2)->positionMs;
    const auto make = [&](const std::string &name) {
        const Stick stick = copyStick(name);
        // Pad 1 as the player left it; pad 2 at rekordbox's number.
        setEngineCues(stick, file, {hot(1, e.enginePadMs), hot(2, rekordboxPad2)});
        return stick;
    };
    {
        const Stick stick = make("cue_offsets_mixed_known");
        SyncController controller;
        analyze(controller, stick);
        const int row = rowOf(controller, file);
        assert(row >= 0);
        const auto &finding = controller.cueOffsetsModel()->findings()[static_cast<size_t>(row)];
        assert(finding.cues.size() == 2 && finding.hasKnown() && finding.hasUnknown());
        assert(finding.sideToMove == CueOffsetSide::Unknown);
        // No pick: only the known cue.
        assert(controller.stageCueOffset(row));
        saveAndWait(stick, controller);
        const Track engine = trackOf(readEngine(stick, false), file);
        checkWithin(hotCue(engine, 1)->positionMs, e.moveToMs, 1000.0 / e.rate, "known: Engine pad 1 moved");
        checkWithin(hotCue(engine, 2)->positionMs, rekordboxPad2, 0.01, "unknown: Engine pad 2 left alone");
        checkWithin(hotCue(trackOf(readRekordbox(stick), file), 2)->positionMs, rekordboxPad2, 0.5,
                    "unknown: rekordbox pad 2 left alone");
    }
    {
        // "Engine Is Right" for the unknown cue: rekordbox's pad 2 moves;
        // the player's cue still moves on Engine, not on rekordbox.
        const Stick stick = make("cue_offsets_mixed_pick");
        SyncController controller;
        analyze(controller, stick);
        assert(controller.stageCueOffset(rowOf(controller, file), QStringLiteral("rekordbox")));
        saveAndWait(stick, controller);
        const Track engine = trackOf(readEngine(stick, false), file);
        const Track rekordbox = trackOf(readRekordbox(stick), file);
        checkWithin(hotCue(engine, 1)->positionMs, e.moveToMs, 1000.0 / e.rate, "known: Engine pad 1 moved");
        checkWithin(hotCue(rekordbox, 1)->positionMs, e.rekordboxPadMs, 0.5, "known: rekordbox pad A stays");
        checkWithin(hotCue(rekordbox, 2)->positionMs, rekordboxPad2 + e.shiftMs, 1.0, "picked: rekordbox pad 2 moved");
        checkWithin(hotCue(engine, 2)->positionMs, rekordboxPad2, 0.01, "picked: Engine pad 2 stays");
    }
    std::cout << "mixed: the known cue keeps its side, the unknown one moves only on the pick OK\n";
}

// M2 of the review: a hot cue pairs only with the one on its own pad.
// Engine's pad 3 at rekordbox pad 1's number is not pad 1's copy.
void testPairing()
{
    const std::string file = "beats-D-lame-44100.mp3";
    const Stick stick = copyStick("cue_offsets_pairing");
    const Expected &e = expectedFor(file);
    setEngineCues(stick, file, {hot(1, e.enginePadMs), hot(3, e.enginePadMs)});
    SyncController controller;
    analyze(controller, stick);
    const int row = rowOf(controller, file);
    assert(row >= 0);
    const auto &finding = controller.cueOffsetsModel()->findings()[static_cast<size_t>(row)];
    assert(finding.cues.size() == 1 && "pad 3 is not paired with rekordbox's pad 1");
    assert(finding.engine.cues[finding.cues[0].engineIndex].hotCueNumber == 1);
    std::cout << "pairing: a hot cue pairs only with its own pad OK\n";
}

// 6. Sync Cue Points calls all seven pairs the same cue, as it did
// before the repair existed: half a beat at 120 BPM is 250 ms.
void testSync()
{
    const Stick stick = copyStick("cue_offsets_sync");
    const auto rekordbox = readRekordbox(stick);
    const auto engine = readEngine(stick, true);
    std::vector<Track> rekordboxClicks;
    std::vector<Track> engineClicks;
    for (const auto *file : {"beats-ref-44100.wav", "beats-A-notag-44100.mp3", "beats-C-badcrc-44100.mp3",
                             "beats-D-lame-48000.mp3", "beats-B-noencoder-44100.mp3", "beats-B-lavc-44100.mp3",
                             "beats-D-lame-44100.mp3"}) {
        rekordboxClicks.push_back(trackOf(rekordbox, file));
        engineClicks.push_back(trackOf(engine, file));
    }
    // The rows carry more than pad 1 (rekordbox's memory cues, a pad or a
    // loop one side lacks), which Sync copies as it always did. Pad 1, the
    // cue this fixture is about, is the same cue to it on every row, and
    // never what a choice is about.
    const auto plans = seabass::application::SyncLibraries().execute(rekordboxClicks, engineClicks, {}, {});
    assert(plans.size() == 7 && "every click track is matched");
    for (const auto &plan : plans) {
        const auto rekordboxPad = hotCue(plan.match.trackA, 1);
        const auto enginePad = hotCue(plan.match.trackB, 1);
        assert(rekordboxPad && enginePad);
        const bool same = seabass::domain::sameCuePlace(*rekordboxPad, *enginePad, plan.positionToleranceMs);
        std::cout << "  " << fileNameOf(plan.match.trackA) << ": pad 1 " << rekordboxPad->positionMs << " / "
                  << enginePad->positionMs << " ms, tolerance " << plan.positionToleranceMs << " ms, "
                  << (same ? "same cue" : "DIFFERENT") << (plan.needsChoice ? ", a choice: " + plan.reasonText : "")
                  << "\n";
        assert(same && "Sync calls pad 1 the same cue");
        // D-lame-44100's rekordbox pad 2, which Engine lacks, is a choice;
        // pad 1 never is.
        assert(plan.reasonText.find("Pad 1") == std::string::npos && "pad 1 is no choice for Sync");
    }

    SyncController controller;
    analyze(controller, stick);
    assert(controller.cueOffsetsModel()->count() == 4 && "the repair lists them instead");
    std::cout << "sync: pad 1 the same cue to Sync Cue Points on all seven rows OK\n";
}

}  // namespace

int main(int argc, char **argv)
{
    seabass::tests::assumeNothingElseRunning();
    if (argc < 3) {
        std::cerr << "usage: cue_offset_repair_test FIXTURE_DIR classify|repair|unknown|player|loops|order|handles|mixed|pairing|sync\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    std::cout << std::unitbuf;
    g_fixture = pathFromUtf8(argv[1]);
    const std::string which = argv[2];
    const fs::path scratch = testing::scratchRoot() / ("cue_offset_repair_" + which);
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);
    if (which == "classify") {
        testClassify();
    } else if (which == "repair") {
        testRepair();
    } else if (which == "unknown") {
        testUnknown();
    } else if (which == "player") {
        testPlayer();
    } else if (which == "loops") {
        testLoops();
    } else if (which == "order") {
        testOrder();
    } else if (which == "handles") {
        testHandles();
    } else if (which == "mixed") {
        testMixed();
    } else if (which == "pairing") {
        testPairing();
    } else if (which == "sync") {
        testSync();
    } else {
        std::cerr << "unknown case " << which << "\n";
        return 2;
    }
    return 0;
}
