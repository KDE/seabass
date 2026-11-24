// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Reading a stick does not change it (#110), the side files of a WAL-mode
// database included.
//
// The first SQLite connection to a WAL-mode database writes its -shm,
// read-only or not (it truncates the file and rebuilds the index in it),
// and makes a -wal when there is none. So every read of OneLibrary's
// exportLibrary.db, which rekordbox keeps in WAL mode, and every read of
// an Engine m.db a player left in WAL mode, touched the stick.
//
// A stick-shaped copy of tests/fixtures/anonymized_library (OneLibrary
// with its -wal and -shm, Engine with a rollback journal) is read the
// ways the app reads a stick: a scan of each catalog, the cue reads
// (Browse's cue stage, the Library Health cue table, the details pane's
// grid and waveform), and a sync analysis. Every file under the stick
// must have the same bytes and modification time afterwards. Then the
// same with the copy's m.db switched to WAL mode, once with a -wal still
// holding rows (a player pulled mid-session) and once without one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "application/use_cases/onelibrary_sync_rows.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "infrastructure/database_read_copy.hpp"
#include "infrastructure/engine/engine_analysis_state.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/engine/engine_sample_rates.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::pathToUtf8;

// The stick's m.db switched to WAL mode (stick_read_wal_switch.cpp: it
// needs <sqlite3.h>, which the SQLCipher header here cannot sit beside).
bool switchEngineToWal(const fs::path &database2, const fs::path &staging, bool rowsInWal);
// The rows of the table switchEngineToWal() added, read from `db`; -1 when
// the table is not there.
int walProbeRows(const fs::path &db);

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

struct FileState
{
    std::uintmax_t size = 0;
    fs::file_time_type modified;
    std::string bytes;
};

std::string contentsOf(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every file under the stick: size, modification time and bytes.
std::map<std::string, FileState> stateOf(const fs::path &stick)
{
    std::map<std::string, FileState> files;
    for (const auto &entry : fs::recursive_directory_iterator(stick)) {
        if (entry.is_regular_file()) {
            files[pathToUtf8(entry.path().lexically_relative(stick))] =
                FileState{entry.file_size(), entry.last_write_time(), contentsOf(entry.path())};
        }
    }
    return files;
}

void expectUnchanged(const std::map<std::string, FileState> &before, const fs::path &stick, const std::string &what)
{
    const auto after = stateOf(stick);
    for (const auto &[file, was] : before) {
        const auto now = after.find(file);
        if (now == after.end()) {
            check(false, what + ": " + file + " is gone");
        } else {
            check(now->second.size == was.size, what + ": " + file + " changed size");
            check(now->second.modified == was.modified, what + ": " + file + " has a new modification time");
            check(now->second.bytes == was.bytes, what + ": " + file + " has new bytes");
        }
    }
    for (const auto &[file, now] : after) {
        check(before.count(file) == 1, what + ": " + file + " appeared");
    }
}

void readTheStick(const fs::path &stick, const std::string &what)
{
    const std::string pioneer = pathToUtf8(stick / "PIONEER");
    const std::string engine = pathToUtf8(stick / "Engine Library");
    namespace infra = seabass::infrastructure;

    // Settle the modification times first: a file written in the same
    // clock tick as the read would hide a write.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto before = stateOf(stick);

    // The scan: each catalog counted, then read, as Browse does.
    infra::rekordbox::KaitaiRekordboxReader rekordboxReader(pioneer);
    const auto rekordboxTracks = rekordboxReader.readAll();
    infra::onelibrary::OneLibraryReader oneLibraryReader(pioneer);
    check(oneLibraryReader.countTracks().value_or(0) > 0, what + ": OneLibrary is counted");
    auto oneLibraryTracks = oneLibraryReader.readTracks();
    infra::engine::LibdjinteropEngineReader engineReader(engine);
    check(engineReader.countTracks().value_or(0) > 0, what + ": Engine is counted");
    auto engineTracks = engineReader.readTracks();
    check(!rekordboxTracks.empty() && !oneLibraryTracks.empty() && !engineTracks.empty(),
          what + ": every catalog has tracks");
    expectUnchanged(before, stick, what + ", scan");

    // The cue reads: Browse's cue and cover stages, the cue table Library
    // Health reads, the playlists, and the details pane's grid and
    // waveform of one track.
    oneLibraryReader.fillCues(oneLibraryTracks);
    engineReader.fillCues(engineTracks);
    engineReader.fillArtwork(engineTracks);
    const infra::onelibrary::OneLibraryCueTables cueTables(pioneer);
    (void)cueTables;
    (void)infra::engine::listEnginePlaylists(engine);
    (void)infra::engine::danglingPlaylistEntries(engine);
    const std::string trackId = engineTracks.front().sourceId;
    (void)infra::engine::readBeatGrid(engine, trackId);
    (void)infra::engine::readWaveformPreview(engine, trackId);
    (void)infra::engine::readTrackAnalysis(engine, trackId, 4096);
    expectUnchanged(before, stick, what + ", cue reads");

    // The sync analysis: the pairs planned, and the player's import state.
    const auto epoch = std::chrono::system_clock::time_point{};
    const auto plans = seabass::application::SyncLibraries().execute(rekordboxTracks, engineTracks, epoch, epoch);
    const auto oneLibraryPlans =
        seabass::application::planEngineWithOneLibrary(engineTracks, oneLibraryTracks, rekordboxTracks, epoch, epoch);
    check(!plans.empty() && !oneLibraryPlans.empty(), what + ": the sync analysis planned something");
    const auto importState = infra::engine::readRekordboxImportState(engine, pioneer);
    check(importState.error.empty(), what + ": the import state is read (" + importState.error + ")");
    (void)infra::engine::readEnginePdbImportKeys(engine);
    expectUnchanged(before, stick, what + ", sync analysis");

    // Library Health's Engine checks.
    (void)infra::engine::auditSampleRates(engine);
    (void)infra::engine::auditArtwork(engine);
    (void)infra::engine::auditAnalysisState(engine);
    (void)infra::engine::unresolvedPlaylistEntries(engine);
    (void)infra::engine::enginePlaylistCountAtPath(engine, "x", infra::ReadFor::Reading);
    expectUnchanged(before, stick, what + ", Library Health");
}

std::uint64_t copiesSoFar()
{
    return seabass::infrastructure::readCopiesMade().load();
}

// How many whole-database copies the reads take: a scan as Browse runs it
// (each catalog counted, read, its cues and covers filled), then a Browse
// page's rows asking for their waveform and grid. One copy per WAL-mode
// database for the scan, none more for the page.
void countCopies(const fs::path &stick)
{
    const std::string pioneer = pathToUtf8(stick / "PIONEER");
    const std::string engine = pathToUtf8(stick / "Engine Library");
    namespace infra = seabass::infrastructure;
    infra::StickReadSnapshots::instance().forget();

    std::uint64_t start = copiesSoFar();
    infra::onelibrary::OneLibraryReader oneLibraryReader(pioneer);
    (void)oneLibraryReader.countTracks();
    auto oneLibraryTracks = oneLibraryReader.readTracks();
    oneLibraryReader.fillCues(oneLibraryTracks);
    const std::uint64_t oneLibraryScan = copiesSoFar() - start;

    start = copiesSoFar();
    infra::engine::LibdjinteropEngineReader engineReader(engine);
    (void)engineReader.countTracks();
    auto engineTracks = engineReader.readTracks();
    engineReader.fillCues(engineTracks);
    engineReader.fillArtwork(engineTracks);
    const std::uint64_t engineScan = copiesSoFar() - start;

    start = copiesSoFar();
    for (std::size_t row = 0; row < 40 && row < engineTracks.size(); ++row) {
        (void)infra::engine::readTrackAnalysis(engine, engineTracks[row].sourceId);
        (void)infra::engine::readBeatGrid(engine, engineTracks[row].sourceId);
    }
    const std::uint64_t browsePage = copiesSoFar() - start;

    std::cout << "copies: OneLibrary scan " << oneLibraryScan << ", Engine scan " << engineScan
              << ", Browse page of 40 rows " << browsePage << "\n";
    check(oneLibraryScan == 1, "a OneLibrary scan copies its database once, not " + std::to_string(oneLibraryScan));
    check(engineScan == 1, "an Engine scan copies a WAL-mode m.db once, not " + std::to_string(engineScan));
    check(browsePage == 0, "a Browse page reuses the scan's copy, made " + std::to_string(browsePage));

    // The rows only the stick's -wal holds are in the copy reads read.
    const infra::StickDatabaseRead read = infra::StickDatabaseRead::engine(engine);
    check(read.copied(), "a WAL-mode m.db is read from a copy");
    check(walProbeRows(read.path()) == 1, "the copy holds the row only the stick's -wal has");
}

// A read that only asks whether a copy is ready (a row on the GUI thread),
// forget() and setWriting() never wait while another thread takes a copy.
void noWaitForACopy(const fs::path &stick)
{
    namespace infra = seabass::infrastructure;
    auto &snapshots = infra::StickReadSnapshots::instance();
    snapshots.forget();
    const fs::path db = stick / "Engine Library" / "Database2" / "m.db";
    const fs::path relative = fs::path("Database2") / "m.db";
    std::atomic<bool> copying{false};
    infra::StickReadSnapshots::copyHookForTesting() = [&copying]() {
        copying = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    };
    std::thread slow([&]() { (void)snapshots.snapshotOf(db, relative); });
    while (!copying) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto start = std::chrono::steady_clock::now();
    const bool none = snapshots.snapshotOf(db, relative, /*mayCopy=*/false) == nullptr;
    snapshots.setWriting(true);
    snapshots.setWriting(false);
    snapshots.forget();
    const auto waited = std::chrono::steady_clock::now() - start;
    slow.join();
    infra::StickReadSnapshots::copyHookForTesting() = nullptr;
    check(none, "no copy is ready while the first one is being taken");
    check(waited < std::chrono::milliseconds(300),
          "a GUI-thread call waited " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count())
              + " ms for a copy being taken");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: stick_read_leaves_stick_alone_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    const fs::path fixture = argv[1];
    const fs::path root = seabass::testing::scratchRoot() / "stick_read_leaves_stick_alone";
    seabass::testing::sandboxSeabassHome(root / "home");
    const fs::path stick = root / "stick";
    fs::remove_all(stick);
    std::error_code ec;
    seabass::testing::copyPioneerFixture(fixture / "rekordbox", stick / "PIONEER", ec);
    if (ec) {
        std::cerr << "could not copy the fixture's rekordbox tree: " << ec.message() << "\n";
        return 2;
    }
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    check(fs::exists(stick / "PIONEER" / "rekordbox" / "exportLibrary.db-wal"), "OneLibrary has its -wal");
    check(fs::exists(stick / "PIONEER" / "rekordbox" / "exportLibrary.db-shm"), "OneLibrary has its -shm");

    readTheStick(stick, "OneLibrary in WAL mode, Engine with a rollback journal");

    const fs::path database2 = stick / "Engine Library" / "Database2";
    check(switchEngineToWal(database2, root / "staging", /*rowsInWal=*/true), "m.db is in WAL mode with a -wal");
    readTheStick(stick, "Engine in WAL mode with rows in its -wal");
    countCopies(stick);
    noWaitForACopy(stick);

    for (const char *suffix : {"-wal", "-shm"}) {
        fs::remove(database2 / (std::string("m.db") + suffix));
    }
    fs::copy_file(fixture / "engine" / "Database2" / "m.db", database2 / "m.db", fs::copy_options::overwrite_existing);
    check(switchEngineToWal(database2, root / "staging", /*rowsInWal=*/false), "m.db is in WAL mode without a -wal");
    readTheStick(stick, "Engine in WAL mode without a -wal");

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "reading the stick left every file alone\n";
    return 0;
}
