// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The player's own analysis kept before Seabass replaces it, and put back
// (LibdjinteropEngineCueWriter with a KeptEngineAnalysisStore), on scratch
// copies of tests/fixtures/testshark_t73's m-after-R1b.db: rows a Prime 4
// analysed (13: N1, FLAC 48 kHz; 14: N3, FLAC 44.1 kHz) and one it did
// not (9: B01, FLAC 44.1 kHz).
//
// 1. A copy over an analysed row keeps it first, exactly; a second copy
//    keeps nothing new (first kept wins); restore puts every column and
//    the overview file back byte for byte (an overview file the player had
//    comes back, one it did not have goes), and drops the entry.
// 2. Restore refuses a row changed since Seabass wrote it (the grid locked
//    on the player; the player's new analysis), each with its reason. Force
//    writes over the lock, never over the player's new analysis.
// 3. After the player analysed the row anew, the next copy keeps that
//    newer analysis in place of the first one.
// 4. A failure to keep leaves the row untouched: the store refusing
//    throws; no key, or a damaged entry, is CannotKeepEngines in a preview
//    as in a write, never a throw.
// 5. Let the player analyse again: the row waits for the player's analysis,
//    the entry stays, and restore still puts it back.
// 6. A row the player never analysed keeps nothing when completed, and its
//    length is the file's own count (a lossless file without one is
//    NoSampleCount, nothing written). A later copy over it keeps nothing
//    either: the analysis there is Seabass's.
// 7. An entry kept for another row is Stale; writeBeatGrid() keeps too.
// 8. A write cut short never costs the kept original: the overview file's
//    write failing after the columns (copy), the columns' transaction
//    failing after the entry was written (copy again), the file failing
//    after the columns are put back (restore). Each time the next run takes
//    the row for Seabass's own: the original stays byte for byte, and a
//    restore gives the player's analysis back.
// 9. A flag that is not a number, or up over beat data that does not
//    decode: RowUnreadable, the row left alone, never filled.
// 10. No entry yet, the row's grid the ledger's over the player's own
//    waveform: a copy of the waveform keeps the row first.
// 11. A grid the DJ moved after Seabass wrote the row, replaced with
//    overwriteEngineEdits: the kept original takes the DJ's grid.
//
// usage: engine_analysis_keep_test <tests/fixtures>

#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <djinterop/engine/v3/beat_data_blob.hpp>
#include <djinterop/engine/v3/track_data_blob.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::engine;
using application::EngineAnalysisColumns;
using Restore = application::EngineAnalysisRestoreResult::Outcome;
using Status = application::KeptEngineAnalysisLookup::Status;
using Outcome = BeatGridWriteResult::Outcome;
using Waveform = AnalysisImportResult::Waveform;
using infrastructure::local::StickKeptEngineAnalysisStore;

namespace
{

fs::path g_fixtures;
int failures = 0;
const std::string kUuid = "f5d66faf-8dac-4d2d-bd45-b9bf7ff7f926";

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

struct Stick
{
    fs::path root;     // the stick
    fs::path library;  // its Engine Library
    std::string mdb;
};

Stick freshStick(const std::string &name)
{
    const fs::path root = testing::scratchRoot() / "engine_analysis_keep_test" / name;
    fs::remove_all(root);
    fs::create_directories(root / "Engine Library" / "Database2");
    const fs::path mdb = root / "Engine Library" / "Database2" / "m.db";
    fs::copy_file(g_fixtures / "testshark_t73" / "engine" / "m-after-R1b.db", mdb);
    return {root, root / "Engine Library", pathToUtf8(mdb)};
}

void exec(const std::string &mdb, const std::string &sql)
{
    sqlite3 *db = nullptr;
    sqlite3_open_v2(mdb.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    char *message = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        std::cerr << "SQL failed: " << sql << ": " << (message ? message : "") << "\n";
        ++failures;
    }
    sqlite3_free(message);
    sqlite3_close(db);
}

std::string overviewPath(std::int64_t id)
{
    return "Database2/OverviewData/" + kUuid + "/" + std::to_string(id) + ".rgb";
}

// The row as stored, and its overview file.
EngineAnalysisColumns snapshot(const Stick &stick, std::int64_t id)
{
    std::string error;
    auto row = readEngineAnalysisRow(stick.mdb, id, &error);
    if (!row) {
        throw std::runtime_error(error);
    }
    EngineAnalysisColumns c = row->columns;
    c.overviewFileRelativePath = overviewPath(id);
    const fs::path file = stick.library / pathFromUtf8(c.overviewFileRelativePath);
    if (fs::exists(file)) {
        std::ifstream in(file, std::ios::binary);
        c.overviewFile = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return c;
}

std::string synthetic(std::size_t bytes, unsigned seed)
{
    std::string out(bytes, '\0');
    unsigned state = seed;
    for (char &c : out) {
        state = state * 1103515245u + 12345u;
        c = static_cast<char>((state >> 16) & 0x1f);
    }
    return out;
}

RekordboxAnalysis analysisOf(double seconds, double bpm, unsigned seed)
{
    RekordboxAnalysis a;
    const double beatMs = 60000.0 / bpm;
    for (int i = 0; i * beatMs < seconds * 1000.0; ++i) {
        a.beats.push_back({std::round(37.0 + i * beatMs), i % 4 + 1});
    }
    a.threeBandPreview = synthetic(1200 * 3, seed);
    a.scrollEntries = static_cast<std::uint32_t>(std::ceil(seconds * 150.0));
    a.bpm = bpm;
    a.key = 5;
    return a;
}

LibdjinteropEngineCueWriter writerFor(const Stick &stick, StickKeptEngineAnalysisStore &store)
{
    LibdjinteropEngineCueWriter writer(pathToUtf8(stick.library));
    writer.setLeadInSource([](const std::string &) { return std::optional<domain::Mp3LeadIn>(); });
    writer.setKeptAnalysisStore(&store);
    return writer;
}

AnalysisImportOptions keyed(const std::string &key)
{
    AnalysisImportOptions o;
    o.keepKey = key;
    return o;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_analysis_keep_test <tests/fixtures>\n";
        return 2;
    }
    g_fixtures = pathFromUtf8(argv[1]);
    const RekordboxAnalysis first = analysisOf(240.0, 120.0, 1);
    const RekordboxAnalysis second = analysisOf(240.0, 121.0, 2);

    // 1. Kept first, exactly; first kept wins; put back byte for byte.
    for (const bool hadFile : {false, true}) {
        const Stick stick = freshStick(hadFile ? "restore-with-file" : "restore");
        if (hadFile) {
            // An overview file the player wrote (any bytes will do: they
            // must come back as they were).
            const fs::path file = stick.library / pathFromUtf8(overviewPath(13));
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << synthetic(3000, 9);
        }
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        const EngineAnalysisColumns before = snapshot(stick, 13);
        check(before.isAnalyzed.integer == 1 && !before.trackData.isNull(), "row 13 is the player's analysis");
        const auto copied = writer.importAnalysis("13", first, keyed("k13"));
        check(copied.grid.outcome == Outcome::Written && copied.waveform == Waveform::Written, "the copy writes");
        check(copied.keptEngineAnalysis, "and keeps the player's analysis first");
        auto kept = store.find("k13");
        check(kept.status == Status::Found && kept.entry.original == before,
              "the kept analysis is the row before the copy, every column and the file");
        check(kept.entry.trackId == 13 && kept.entry.libraryUuid == kUuid
                  && kept.entry.trackFile == "../Contents/Seabass Test/T73/T73-N1-click-48k-120bpm.flac",
              "with what it belongs to");
        check(!(snapshot(stick, 13) == before), "the row changed");
        AnalysisImportOptions ours = keyed("k13");
        ours.ownGridFingerprint = copied.grid.gridFingerprint;  // as the ledger says
        const auto again = writer.importAnalysis("13", second, ours);
        check(again.grid.outcome == Outcome::Written && !again.keptEngineAnalysis, "a second copy keeps nothing new");
        check(store.find("k13").entry.original == before, "first kept wins");
        const auto restored = writer.restoreEngineAnalysis("13", "k13", {});
        check(restored.outcome == Restore::Restored, "restored");
        check(snapshot(stick, 13) == before, std::string("the row is as it was, byte for byte")
                                                 + (hadFile ? ", its overview file too" : ", with no overview file"));
        check(store.find("k13").status == Status::None, "the entry is dropped");
        check(writer.restoreEngineAnalysis("13", "k13", {}).outcome == Restore::NothingKept, "nothing left to restore");
    }

    // 2. Changed since Seabass wrote it: refused, unless forced.
    {
        const Stick stick = freshStick("changed-since");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        const EngineAnalysisColumns before = snapshot(stick, 13);
        writer.importAnalysis("13", first, keyed("k13"));
        exec(stick.mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = 13");
        const auto preview = writer.restoreEngineAnalysis("13", "k13", {false, true});
        check(preview.outcome == Restore::ChangedSince && preview.reason.find("changed on the player") != std::string::npos,
              "a grid locked on the player since: " + preview.reason);
        const auto locked = snapshot(stick, 13);
        check(writer.restoreEngineAnalysis("13", "k13", {}).outcome == Restore::ChangedSince && snapshot(stick, 13) == locked,
              "refused, nothing written");
        check(writer.restoreEngineAnalysis("13", "k13", {true, false}).outcome == Restore::Restored, "forced");
        check(snapshot(stick, 13) == before, "and then the row is as it was");

        // The player's new analysis since: refused, and forced too, since
        // it is newer than what was kept and would be lost unkept.
        writer.importAnalysis("13", first, keyed("k13"));
        exec(stick.mdb, "UPDATE PerformanceData SET trackData = (SELECT trackData FROM PerformanceData WHERE trackId = 5)"
                        " WHERE trackId = 13");
        const auto newer = snapshot(stick, 13);
        const auto reanalysed = writer.restoreEngineAnalysis("13", "k13", {});
        check(reanalysed.outcome == Restore::ChangedSince && reanalysed.reason.find("analysed it again") != std::string::npos,
              "the player's new analysis since: " + reanalysed.reason);
        const auto forced = writer.restoreEngineAnalysis("13", "k13", {true, false});
        check(forced.outcome == Restore::ChangedSince && forced.reason.find("analysed it again") != std::string::npos,
              "forced over the player's new analysis: refused all the same");
        check(snapshot(stick, 13) == newer && store.find("k13").status == Status::Found,
              "the new analysis stays, and so does what was kept");
    }

    // 3. The player analysed the row anew: the next copy keeps that.
    {
        const Stick stick = freshStick("reanalysed");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        const EngineAnalysisColumns before = snapshot(stick, 13);
        writer.importAnalysis("13", first, keyed("k13"));
        // A new analysis by the player: other blobs (row 15's: 48 kHz,
        // 240 s, like this one).
        exec(stick.mdb, "UPDATE PerformanceData SET trackData = (SELECT trackData FROM PerformanceData WHERE trackId = 15),"
                        " overviewWaveFormData = (SELECT overviewWaveFormData FROM PerformanceData WHERE trackId = 15),"
                        " beatData = (SELECT beatData FROM PerformanceData WHERE trackId = 15) WHERE trackId = 13");
        const EngineAnalysisColumns newer = snapshot(stick, 13);
        const auto copied = writer.importAnalysis("13", second, keyed("k13"));
        check(copied.waveform == Waveform::Written && copied.keptEngineAnalysis, "the newer analysis is kept");
        check(store.find("k13").entry.original == newer && !(newer == before), "in place of the first");
        // A grid the DJ moved is not an analysis: nothing new is kept.
        exec(stick.mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = 13");
        AnalysisImportOptions force = keyed("k13");
        force.overwriteEngineEdits = true;
        const auto over = writer.importAnalysis("13", first, force);
        check(over.grid.outcome == Outcome::Written && !over.keptEngineAnalysis
                  && store.find("k13").entry.original == newer,
              "a lock set on the player keeps nothing new");
    }

    // 4. No keep, no write.
    {
        const Stick stick = freshStick("keep-fails");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        const EngineAnalysisColumns before = snapshot(stick, 13);
        const auto refused = [&](const AnalysisImportOptions &options, const std::string &why) {
            bool threw = false;
            try {
                writer.importAnalysis("13", first, options);
            } catch (const std::runtime_error &e) {
                threw = std::string(e.what()).find("could not be kept") != std::string::npos;
            }
            check(threw, why + ": a failure to keep fails the write, saying so");
            check(snapshot(stick, 13) == before, why + ": the row is untouched");
        };
        store.setBeforeChange([](const std::string &) { throw std::runtime_error("the stick refused"); });
        refused(keyed("k13"), "the store refuses");
        store.setBeforeChange({});
        // No key (the file is not under the stick root): a refusal of its
        // own, in a preview as in a write, never a throw.
        for (const bool preview : {true, false}) {
            AnalysisImportOptions noKey = keyed("");
            noKey.previewOnly = preview;
            const auto r = writer.importAnalysis("13", first, noKey);
            check(r.grid.outcome == Outcome::CannotKeepEngines && r.waveform == Waveform::CannotKeepEngines
                      && r.grid.gridFingerprint.empty() && r.overviewFileRelativePath.empty(),
                  std::string("no key: cannot be kept, so not replaced") + (preview ? " (preview)" : ""));
            check(snapshot(stick, 13) == before, "no key: the row is untouched");
            BeatGridWriteOptions grid;
            grid.previewOnly = preview;
            check(writer.writeBeatGrid("13", first.beats, grid).outcome == Outcome::CannotKeepEngines,
                  "no key: writeBeatGrid() refuses the same way");
            check(snapshot(stick, 13) == before, "no key: the row is still untouched");
        }
        const fs::path file = infrastructure::local::keptEngineAnalysisFile(stick.root, "k13");
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << "seabass-engine-analysis\t1\ndamaged";
        // A damaged entry: refused as a row with no key is, in a preview as
        // in a write, so a preview never offers what the save would refuse
        // (and a save with other rows is not rolled back for it).
        for (const bool preview : {true, false}) {
            AnalysisImportOptions options = keyed("k13");
            options.previewOnly = preview;
            const auto r = writer.importAnalysis("13", first, options);
            check(r.grid.outcome == Outcome::CannotKeepEngines && r.waveform == Waveform::CannotKeepEngines
                      && r.overviewFileRelativePath.empty() && !r.keptEngineAnalysis,
                  std::string("a damaged entry: cannot be kept, so not replaced") + (preview ? " (preview)" : ""));
            BeatGridWriteOptions grid;
            grid.keepKey = "k13";
            grid.previewOnly = preview;
            check(writer.writeBeatGrid("13", first.beats, grid).outcome == Outcome::CannotKeepEngines,
                  "a damaged entry: writeBeatGrid() refuses the same way");
            check(snapshot(stick, 13) == before, "a damaged entry: the row is untouched");
        }
        check(writer.restoreEngineAnalysis("13", "k13", {}).outcome == Restore::KeptDamaged, "restore says damaged");
        check(writer.engineAnalysisFacts("13", "k13").kept == EngineAnalysisFacts::Kept::Damaged, "facts say damaged");
    }

    // 5. Let the player analyse again, then put it back all the same.
    {
        const Stick stick = freshStick("let-player");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        const EngineAnalysisColumns before = snapshot(stick, 13);
        writer.importAnalysis("13", first, keyed("k13"));
        check(writer.letPlayerAnalyseAgain("13", "k13", true).outcome == LetPlayerAnalyseResult::Outcome::Marked,
              "a preview says it would mark");
        check(writer.letPlayerAnalyseAgain("13", "k13", false).outcome == LetPlayerAnalyseResult::Outcome::Marked,
              "marked");
        const auto waiting = snapshot(stick, 13);
        check(waiting.isAnalyzed.integer == 0 && waiting.trackData.isNull() && waiting.overviewWaveFormData.isNull()
                  && waiting.beatData.isNull(),
              "the row waits for the player's analysis");
        const auto facts = writer.engineAnalysisFacts("13", "k13");
        check(!facts.rowAnalysed && facts.kept == EngineAnalysisFacts::Kept::Kept, "not analysed, the analysis kept");
        check(writer.letPlayerAnalyseAgain("13", "k13", false).outcome == LetPlayerAnalyseResult::Outcome::NotAnalysed,
              "marking it again does nothing");
        check(writer.restoreEngineAnalysis("13", "k13", {}).outcome == Restore::Restored, "restored from waiting");
        // Seabass's overview file goes; the player had none.
        check(snapshot(stick, 13) == before, "and the row is as it was");
    }

    // 6. A row the player never analysed keeps nothing; the file's count.
    {
        const Stick stick = freshStick("completed");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        const RekordboxAnalysis b01 = analysisOf(180.0, 102.0, 3);
        AnalysisImportOptions options = keyed("k9");
        options.fileSampleRateHz = 44100.0;
        options.fileCountRequired = true;
        const EngineAnalysisColumns before = snapshot(stick, 9);
        const auto noCount = writer.importAnalysis("9", b01, options);
        check(noCount.waveform == Waveform::NoSampleCount && !noCount.noSampleCountReason.empty()
                  && snapshot(stick, 9) == before,
              "a lossless file without its own count is not completed: " + noCount.noSampleCountReason);
        options.fileSampleCount = 7938000;
        const auto done = writer.importAnalysis("9", b01, options);
        check(done.completedRow && !done.keptEngineAnalysis, "completed, nothing kept");
        check(store.find("k9").status == Status::None, "no entry for a row the player never analysed");
        const std::string bytes = snapshot(stick, 9).trackData.bytes;
        const auto *start = reinterpret_cast<const std::byte *>(bytes.data());
        const auto record = djinterop::engine::v3::track_data_blob::from_blob(
            std::vector<std::byte>(start, start + bytes.size()));
        check(record.samples == 7938000 && record.sample_rate == 44100.0, "the length is the file's own count");
        AnalysisImportOptions again = keyed("k9");
        again.ownGridFingerprint = done.grid.gridFingerprint;
        const auto over = writer.importAnalysis("9", analysisOf(180.0, 103.0, 4), again);
        check(over.grid.outcome == Outcome::Written && store.find("k9").status == Status::None,
              "a copy over Seabass's own completion keeps nothing");
        check(writer.engineAnalysisFacts("9", "k9").rowAnalysed, "the completed row reads as analysed");
    }

    // 7. Stale entries; writeBeatGrid() keeps too.
    {
        const Stick stick = freshStick("stale");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        writer.importAnalysis("13", first, keyed("k13"));
        auto entry = store.find("k13").entry;
        entry.trackId = 99;
        store.keep(entry);
        check(writer.restoreEngineAnalysis("13", "k13", {}).outcome == Restore::Stale, "an entry of another row is stale");
        check(writer.engineAnalysisFacts("13", "k13").kept == EngineAnalysisFacts::Kept::Stale, "facts say stale");

        const EngineAnalysisColumns before = snapshot(stick, 14);
        BeatGridWriteOptions grid;
        grid.keepKey = "k14";
        check(writer.writeBeatGrid("14", analysisOf(240.0, 128.0, 5).beats, grid).outcome == Outcome::Written,
              "a grid written to row 14");
        check(store.find("k14").entry.original == before, "writeBeatGrid() kept the player's analysis first");
    }

    // 8. Cut short.
    for (const bool hadFile : {false, true}) {
        const std::string tag = hadFile ? " (the player had an overview file)" : " (no overview file)";
        const Stick stick = freshStick(hadFile ? "cut-short-with-file" : "cut-short");
        if (hadFile) {
            const fs::path file = stick.library / pathFromUtf8(overviewPath(13));
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << synthetic(3000, 11);
        }
        StickKeptEngineAnalysisStore store(stick.root);
        const EngineAnalysisColumns before = snapshot(stick, 13);
        const auto keptOriginal = [&]() { return store.find("k13").entry.original; };
        const auto throws = [&](const std::function<void()> &run) {
            try {
                run();
            } catch (const std::exception &) {
                return true;
            }
            return false;
        };
        const auto failingSink = [](const std::string &, const std::string &) {
            throw std::runtime_error("the stick is gone");
        };

        // a. The first copy: the columns written, the overview file not.
        {
            auto writer = writerFor(stick, store);
            writer.setOverviewFileSink(failingSink);
            check(throws([&] { writer.importAnalysis("13", first, keyed("k13")); }), "a. the file fails the copy" + tag);
        }
        const auto half = snapshot(stick, 13);
        check(!(half.trackData == before.trackData) || !(half.beatData == before.beatData)
                  || !(half.overviewWaveFormData == before.overviewWaveFormData),
              "a. the columns were written" + tag);
        check(half.overviewFile == before.overviewFile, "a. the file was not" + tag);
        check(keptOriginal() == before, "a. the player's analysis was kept first" + tag);
        {
            // The rerun redoes the write and keeps nothing new.
            auto writer = writerFor(stick, store);
            const auto again = writer.importAnalysis("13", first, keyed("k13"));
            check(again.waveform == Waveform::Written && !again.keptEngineAnalysis,
                  "a. the rerun writes the file, keeping nothing new" + tag);
            check(keptOriginal() == before, "a. the kept original is the player's, byte for byte" + tag);
        }
        const auto copied = snapshot(stick, 13);

        // b. A copy of another analysis: the entry written, then the
        // columns' transaction refused: another connection holds the
        // database's write lock, so the writer's BEGIN IMMEDIATE is busy
        // (after its 5 s wait). A lock, not a read-only file: root, as
        // KDE's CI runs, writes to one anyway.
        {
            auto writer = writerFor(stick, store);
            sqlite3 *holder = nullptr;
            store.setBeforeChange([&](const std::string &) {
                if (holder == nullptr) {
                    sqlite3_open_v2(stick.mdb.c_str(), &holder, SQLITE_OPEN_READWRITE, nullptr);
                    check(sqlite3_exec(holder, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK,
                          "b. the write lock is taken" + tag);
                }
            });
            check(throws([&] { writer.importAnalysis("13", second, keyed("k13")); }),
                  "b. the columns' transaction fails the copy" + tag);
            store.setBeforeChange({});
            check(holder != nullptr, "b. the entry was written before the columns" + tag);
            sqlite3_exec(holder, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(holder);
        }
        check(snapshot(stick, 13) == copied, "b. the row is the first copy's still" + tag);
        {
            auto writer = writerFor(stick, store);
            const auto again = writer.importAnalysis("13", second, keyed("k13"));
            check(again.waveform == Waveform::Written && !again.keptEngineAnalysis,
                  "b. the rerun writes it, keeping nothing new" + tag);
            check(keptOriginal() == before, "b. the kept original is still the player's" + tag);
        }

        // c. A restore: the columns put back, the file step failing.
        {
            auto writer = writerFor(stick, store);
            writer.setOverviewFileSink(failingSink);
            writer.setOverviewFileRemover([](const std::string &) { throw std::runtime_error("the stick is gone"); });
            check(throws([&] { writer.restoreEngineAnalysis("13", "k13", {}); }), "c. the file fails the restore" + tag);
        }
        const auto halfBack = snapshot(stick, 13);
        check(halfBack.trackData == before.trackData && halfBack.beatData == before.beatData
                  && !(halfBack.overviewFile == before.overviewFile),
              "c. the columns are back, the file is Seabass's" + tag);
        check(keptOriginal() == before, "c. the entry stays" + tag);
        {
            // A copy now takes the row for Seabass's own, not the player's.
            auto writer = writerFor(stick, store);
            const auto over = writer.importAnalysis("13", first, keyed("k13"));
            check(!over.keptEngineAnalysis && keptOriginal() == before,
                  "c. a copy over the half restore keeps the original" + tag);
        }
        {
            auto writer = writerFor(stick, store);
            writer.setOverviewFileSink(failingSink);
            writer.setOverviewFileRemover([](const std::string &) { throw std::runtime_error("the stick is gone"); });
            check(throws([&] { writer.restoreEngineAnalysis("13", "k13", {}); }), "c. a second restore cut short" + tag);
        }
        {
            auto writer = writerFor(stick, store);
            const auto preview = writer.restoreEngineAnalysis("13", "k13", {false, true});
            check(preview.outcome == Restore::Restored, "c. the rerun of the restore is not refused: " + preview.reason + tag);
            check(writer.restoreEngineAnalysis("13", "k13", {}).outcome == Restore::Restored, "c. restored" + tag);
        }
        check(snapshot(stick, 13) == before, "c. the player's analysis is back, byte for byte" + tag);
        check(store.find("k13").status == Status::None, "c. and the entry is used up" + tag);
    }

    // 9. Unsure whether the player analysed the row: left alone.
    {
        const Stick stick = freshStick("unreadable-flag");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        exec(stick.mdb, "UPDATE Track SET isAnalyzed = NULL WHERE id IN (9, 13)");
        exec(stick.mdb, "UPDATE Track SET isAnalyzed = 1 WHERE id = 14");
        exec(stick.mdb, "UPDATE PerformanceData SET beatData = x'00010203' WHERE trackId = 14");
        for (const std::int64_t id : {9, 13, 14}) {
            const std::string name = "row " + std::to_string(id);
            const EngineAnalysisColumns before = snapshot(stick, id);
            AnalysisImportOptions options = keyed("k" + std::to_string(id));
            options.fileSampleRateHz = 44100.0;
            options.fileSampleCount = 7938000;
            const auto r = writer.importAnalysis(std::to_string(id), analysisOf(180.0, 102.0, 3), options);
            check(r.grid.outcome == Outcome::RowUnreadable && r.waveform == Waveform::RowUnreadable && !r.completedRow,
                  name + ": the analysis is not written");
            BeatGridWriteOptions grid;
            grid.unanalysed = UnanalysedRowGrid::Fill;
            grid.fileSampleRateHz = 44100.0;
            grid.fileDurationMs = 180000.0;
            grid.keepKey = options.keepKey;
            check(writer.writeBeatGrid(std::to_string(id), analysisOf(180.0, 102.0, 3).beats, grid).outcome
                      == Outcome::RowUnreadable,
                  name + ": nor the grid alone");
            check(snapshot(stick, id) == before, name + ": the row is untouched");
            check(store.find(options.keepKey).status == Status::None, name + ": nothing kept");
        }
        // Beat data the decoder must never be handed: a length and no
        // stream (four bytes: libdjinterop indexed past the end, an abort
        // on MSVC), too short for a length, a stream that never ends
        // (libdjinterop looped for ever), lengths no blob has. Each is
        // unreadable, the row left alone; a length of 0 with bytes after
        // it decodes to nothing, which is no beat data either.
        for (const auto &[blob, what] : std::vector<std::pair<std::string, std::string>>{
                 {"x'01'", "1 byte"},
                 {"x'000102'", "3 bytes"},
                 {"x'00010203'", "4 bytes, a length and nothing else"},
                 {"x'0001020378'", "5 bytes"},
                 {"x'00000000deadbeef'", "length 0, bytes after it"},
                 {"x'00000040789c4b4c4a'", "a stream cut short"},
                 {"x'ffffffff789c030000000001'", "a length of 4 GiB less one"},
                 {"x'7fffffff789c030000000001'", "a length of 2 GiB"}}) {
            exec(stick.mdb, "UPDATE PerformanceData SET beatData = " + blob + " WHERE trackId = 14");
            const EngineAnalysisColumns before = snapshot(stick, 14);
            AnalysisImportOptions options = keyed("k14");
            options.fileSampleRateHz = 44100.0;
            options.fileSampleCount = 7938000;
            const auto r = writer.importAnalysis("14", analysisOf(180.0, 102.0, 3), options);
            check(r.grid.outcome == Outcome::RowUnreadable && r.waveform == Waveform::RowUnreadable,
                  "beat data of " + what + ": unreadable");
            check(writer.engineAnalysisFacts("14", "k14").kept == EngineAnalysisFacts::Kept::None,
                  "beat data of " + what + ": nothing kept, no throw");
            check(snapshot(stick, 14) == before, "beat data of " + what + ": the row is untouched");
        }
    }

    // 10. Nothing kept yet, the row's grid Seabass's (the ledger's) over
    //     the player's own waveform: a copy that replaces the waveform too
    //     keeps the row first, and it can be put back. (Seen on a real
    //     stick: an earlier build had written only the grid.)
    {
        const Stick stick = freshStick("grid-ours-waveform-theirs");
        StickKeptEngineAnalysisStore store(stick.root);
        // The earlier grid write, by a writer that kept nothing.
        BeatGridWriteResult earlier;
        {
            LibdjinteropEngineCueWriter unkept(pathToUtf8(stick.library));
            unkept.setLeadInSource([](const std::string &) { return std::optional<domain::Mp3LeadIn>(); });
            earlier = unkept.writeBeatGrid("13", first.beats, {});
        }
        check(earlier.outcome == Outcome::Written && !earlier.gridFingerprint.empty(), "10. the earlier grid write");
        const EngineAnalysisColumns before = snapshot(stick, 13);
        auto writer = writerFor(stick, store);
        AnalysisImportOptions ledger = keyed("k13");
        ledger.ownGridFingerprint = earlier.gridFingerprint;
        AnalysisImportOptions gridOnly = ledger;
        gridOnly.waveforms = false;
        gridOnly.previewOnly = true;
        check(writer.importAnalysis("13", second, gridOnly).grid.outcome == Outcome::Written,
              "10. a grid-only copy would replace only Seabass's grid");
        const auto copied = writer.importAnalysis("13", second, ledger);
        check(copied.waveform == Waveform::Written && copied.keptEngineAnalysis,
              "10. the player's waveform is kept before it is replaced");
        check(store.find("k13").entry.original == before, "10. kept as the row was");
        check(writer.restoreEngineAnalysis("13", "k13", {}).outcome == Restore::Restored
                  && snapshot(stick, 13) == before,
              "10. and put back");
    }

    // 11. A grid the DJ moved on the player after Seabass wrote the row,
    //     replaced with overwriteEngineEdits: the kept original takes the
    //     DJ's grid, its analysis staying the player's first.
    {
        const Stick stick = freshStick("dj-moved-grid");
        StickKeptEngineAnalysisStore store(stick.root);
        auto writer = writerFor(stick, store);
        const EngineAnalysisColumns before = snapshot(stick, 13);
        const auto copied = writer.importAnalysis("13", first, keyed("k13"));
        check(copied.keptEngineAnalysis && store.find("k13").entry.original == before, "11. kept first");
        // The DJ moves the grid on the player by 4000 samples.
        EngineAnalysisColumns moved = snapshot(stick, 13);
        const auto blobOf = [](const std::string &bytes) {
            const auto *start = reinterpret_cast<const std::byte *>(bytes.data());
            return djinterop::engine::v3::beat_data_blob::from_blob(std::vector<std::byte>(start, start + bytes.size()));
        };
        auto beat = blobOf(moved.beatData.bytes);
        for (auto &marker : beat.adjusted_beat_grid) {
            marker.sample_offset += 4000.0;
        }
        const auto encoded = beat.to_blob();
        moved.beatData = application::EngineSqlValue::ofBlob(
            std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size()));
        std::string error;
        check(writeEngineAnalysisColumns(stick.mdb, 13, moved, &error), "11. the DJ's move: " + error);
        AnalysisImportOptions over = keyed("k13");
        over.overwriteEngineEdits = true;
        over.ownGridFingerprint = copied.grid.gridFingerprint;  // the ledger
        const auto again = writer.importAnalysis("13", second, over);
        check(again.grid.outcome == Outcome::Written && again.keptEngineAnalysis, "11. the DJ's grid is kept");
        const auto original = store.find("k13").entry.original;
        const auto keptBeat = blobOf(original.beatData.bytes);
        const auto beforeBeat = blobOf(before.beatData.bytes);
        check(keptBeat.adjusted_beat_grid.size() == beat.adjusted_beat_grid.size()
                  && std::abs(keptBeat.adjusted_beat_grid.front().sample_offset
                              - beat.adjusted_beat_grid.front().sample_offset)
                      < 0.5,
              "11. the kept adjusted grid is the DJ's");
        check(keptBeat.default_beat_grid.size() == beforeBeat.default_beat_grid.size()
                  && keptBeat.sample_rate == beforeBeat.sample_rate && keptBeat.samples == beforeBeat.samples
                  && original.trackData == before.trackData
                  && original.overviewWaveFormData == before.overviewWaveFormData,
              "11. and the rest is the player's first analysis");
        // Seabass's own grid replaced again keeps nothing new.
        const auto third = writer.importAnalysis("13", first, over);
        check(third.grid.outcome == Outcome::Written && !third.keptEngineAnalysis
                  && store.find("k13").entry.original == original,
              "11. Seabass's own grid is never taken for the DJ's");
    }

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "engine_analysis_keep_test: all checks passed\n";
    return 0;
}
