// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Issue #59: a OneLibrary track's cues are the ones in the analysis file
// its row names (content.analysisDataFilePath), not the rows of
// exportLibrary.db's cue table. An OMNIS-DUO showed the file where the two
// disagreed and wrote its own pads into the file alone. Checked here on a
// copy of the anonymized fixture, whose exportLibrary.db names an analysis
// file for every row:
//
// 1. the reader takes the file's cues, with the table planted to disagree;
// 2. the writer writes the file (through RekordboxCueWriter) and the table,
//    leaves a file that already holds the cues alone, and announces every
//    file it is about to write so a save can back it up;
// 3. the Sync pairing finds no conflict where only the table disagrees,
//    and does not pair a OneLibrary row with Engine when a DeviceLibrary
//    row names its analysis file.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/use_cases/onelibrary_sync_rows.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "domain/track.hpp"
#include "domain/track_matching.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "fixture_copy.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::CuePoint;
using seabass::domain::Track;
using namespace seabass::infrastructure;

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

CuePoint hot(int slot, double ms)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = slot;
    cue.positionMs = ms;
    return cue;
}

CuePoint memory(double ms)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Memory;
    cue.positionMs = ms;
    return cue;
}

// Replaces one row's cue-table rows with `cues`, the way a table that no
// player reads can drift from the file.
void plantTableCues(const std::string &pioneerRoot, const std::string &contentId, const std::vector<CuePoint> &cues)
{
    onelibrary::SqlCipherLibrary lib;
    onelibrary::SqlCipherDb db(lib, onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/false);
    db.exec("PRAGMA key = '" + onelibrary::deriveOneLibraryKey() + "';");
    db.exec("DELETE FROM cue WHERE content_id = " + contentId + ";");
    for (const auto &cue : cues) {
        const long long in = static_cast<long long>(cue.positionMs * 1000.0);
        db.exec("INSERT INTO cue (content_id, kind, colorTableIndex, isActiveLoop, inUsec, outUsec) VALUES ("
                + contentId + ", " + std::to_string(cue.kind == CuePoint::Kind::Hot ? cue.hotCueNumber : 0)
                + ", 0, 0, " + std::to_string(in) + ", " + std::to_string(in) + ");");
    }
}

// The cue table's rows for one content row, as (kind, inUsec).
std::multiset<std::pair<long long, long long>> tableCues(const std::string &pioneerRoot, const std::string &contentId)
{
    onelibrary::SqlCipherLibrary lib;
    onelibrary::SqlCipherDb db(lib, onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + onelibrary::deriveOneLibraryKey() + "';");
    onelibrary::SqlCipherStatement rows(db, "SELECT kind, inUsec FROM cue WHERE content_id = ?");
    rows.bindInt64(1, std::stoll(contentId));
    std::multiset<std::pair<long long, long long>> out;
    while (rows.step()) {
        out.insert({rows.columnInt64(0), rows.columnInt64(1)});
    }
    return out;
}

std::vector<CuePoint> fileCues(const std::string &pioneerRoot, const std::string &analysisFile)
{
    rekordbox::FilesystemAnlzSource source(pioneerRoot);
    auto cues = rekordbox::readAnalysisFileCues(source, analysisFile);
    return cues ? *cues : std::vector<CuePoint>{};
}

const Track *bySourceId(const std::vector<Track> &tracks, const std::string &id)
{
    for (const auto &t : tracks) {
        if (t.sourceId == id) {
            return &t;
        }
    }
    return nullptr;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: onelibrary_anlz_cues_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    const fs::path fixture = seabass::pathFromUtf8(argv[1]) / "rekordbox";
    const fs::path scratch = seabass::testing::scratchRoot() / "seabass_onelibrary_anlz_cues_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    const fs::path pioneer = scratch / "PIONEER";
    seabass::testing::copyPioneerFixture(fixture, pioneer, ec);
    if (ec) {
        std::cerr << "could not copy the fixture: " << ec.message() << "\n";
        return 2;
    }
    const std::string root = seabass::pathToUtf8(pioneer);

    // Pick the rows from what the catalogs say, not from ids written down
    // here: one row DeviceLibrary also lists (by analysis file), and one
    // only OneLibrary holds, each with an analysis file holding cues.
    std::vector<Track> deviceLibrary = rekordbox::KaitaiRekordboxReader(root).readTracks();
    std::set<std::string> deviceLibraryFiles;
    for (const auto &t : deviceLibrary) {
        deviceLibraryFiles.insert(t.analysisFile);
    }
    std::vector<Track> catalog = onelibrary::OneLibraryReader(root).readTracks();
    const Track *shared = nullptr;
    const Track *ownOnly = nullptr;
    for (const auto &t : catalog) {
        if (t.analysisFile.empty() || fileCues(root, t.analysisFile).empty()) {
            continue;
        }
        if (!shared && deviceLibraryFiles.count(t.analysisFile)) {
            shared = &t;
        } else if (!ownOnly && !deviceLibraryFiles.count(t.analysisFile)) {
            ownOnly = &t;
        }
    }
    if (!shared || !ownOnly) {
        std::cerr << "the fixture no longer has the rows this test needs\n";
        return 2;
    }
    const Track sharedRow = *shared;
    const Track ownRow = *ownOnly;
    // Expected total, pinned, so a reader that read nothing cannot pass.
    check(catalog.size() == 1644, "OneLibrary lists 1644 rows, found " + std::to_string(catalog.size()));
    check(sharedRow.cues.empty(), "readTracks() is the catalog alone, no cues");

    // --- 1. Reader: the file wins over a table planted to disagree ---
    {
        const std::vector<CuePoint> inFile = fileCues(root, ownRow.analysisFile);
        plantTableCues(root, ownRow.sourceId, {hot(1, 1234.0), hot(2, 5678.0), memory(91011.0), hot(7, 4242.0)});
        check(!seabass::domain::cueSetsEqual(inFile, {hot(1, 1234.0), hot(2, 5678.0), memory(91011.0), hot(7, 4242.0)}),
              "the planted table really differs from the file");

        const std::vector<Track> tracks = onelibrary::OneLibraryReader(root).readAll();
        const Track *read = bySourceId(tracks, ownRow.sourceId);
        check(read != nullptr, "the OneLibrary-only row is read");
        if (read) {
            check(seabass::domain::cueSetsEqual(read->cues, inFile),
                  "OneLibrary cues are the analysis file's (" + std::to_string(inFile.size()) + "), read "
                      + std::to_string(read->cues.size()));
            check(read->analysisFile == ownRow.analysisFile, "the row's analysis file is carried");
            check(read->metadataModifiedAt > 0, "the cues' edit time is the analysis file's");
        }
        // The progressive read agrees with readAll().
        std::vector<Track> staged = onelibrary::OneLibraryReader(root).readTracks();
        onelibrary::OneLibraryReader(root).fillCues(staged);
        const Track *stagedRow = bySourceId(staged, ownRow.sourceId);
        check(stagedRow && seabass::domain::cueSetsEqual(stagedRow->cues, inFile),
              "readTracks() + fillCues() reads the file too");
        std::cout << "reader: cues come from the analysis file\n";
    }

    // --- 3a. No false conflict where only the table disagrees ---
    {
        const std::vector<Track> tracks = onelibrary::OneLibraryReader(root).readAll();
        const Track *read = bySourceId(tracks, ownRow.sourceId);
        Track engine = read ? *read : ownRow;
        engine.format = "engine";
        engine.sourceId = "9001";
        engine.analysisFile.clear();
        engine.cues = fileCues(root, ownRow.analysisFile);
        const auto plans = seabass::application::SyncLibraries().execute(
            {engine}, seabass::application::oneLibraryRowsToPairWithEngine(deviceLibrary, tracks), {}, {});
        bool matched = false;
        bool actionable = false;
        for (const auto &plan : plans) {
            if (plan.match.trackB.sourceId == ownRow.sourceId) {
                matched = true;
                actionable = actionable || plan.direction != seabass::domain::SyncPlan::Direction::None;
            }
        }
        check(matched, "the Engine copy is matched to the OneLibrary-only row");
        check(!actionable, "Engine holding the file's cues is in step with OneLibrary, whatever the table says");
        std::cout << "planner: no conflict from the cue table\n";
    }

    // --- 3b. A row sharing a DeviceLibrary analysis file is not paired ---
    {
        Track deviceRow;
        deviceRow.format = "rekordbox";
        deviceRow.sourceId = "1";
        deviceRow.filePath = "/elsewhere/another-copy.mp3";
        deviceRow.analysisFile = "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT";
        Track sharing;
        sharing.format = "onelibrary";
        sharing.sourceId = "11";
        sharing.filePath = "/stick/Contents/one.mp3";
        // Spelled with other case: the stick's filesystem does not care.
        sharing.analysisFile = "/PIONEER/USBANLZ/p001/00000001/ANLZ0000.DAT";
        Track samePath;
        samePath.format = "onelibrary";
        samePath.sourceId = "12";
        samePath.filePath = "/elsewhere/another-copy.mp3";
        samePath.analysisFile = "/PIONEER/USBANLZ/P002/00000002/ANLZ0000.DAT";
        Track own;
        own.format = "onelibrary";
        own.sourceId = "13";
        own.filePath = "/stick/Contents/three.mp3";
        own.analysisFile = "/PIONEER/USBANLZ/P003/00000003/ANLZ0000.DAT";
        const auto rows = seabass::application::oneLibraryRowsToPairWithEngine({deviceRow}, {sharing, samePath, own});
        check(rows.size() == 1 && rows.front().sourceId == "13",
              "only the row no DeviceLibrary row speaks for is paired, got " + std::to_string(rows.size()));

        // On the fixture: the shared row is never offered to the Engine pair.
        const std::vector<Track> tracks = onelibrary::OneLibraryReader(root).readTracks();
        const auto fixtureRows = seabass::application::oneLibraryRowsToPairWithEngine(deviceLibrary, tracks);
        check(bySourceId(fixtureRows, sharedRow.sourceId) == nullptr,
              "the fixture row sharing a DeviceLibrary analysis file is left to the DeviceLibrary pair");
        check(bySourceId(fixtureRows, ownRow.sourceId) != nullptr, "the OneLibrary-only fixture row is paired");
        std::cout << "planner: a shared analysis file is decided once\n";
    }

    // --- 2. Writer: the file through RekordboxCueWriter, then the table ---
    {
        const std::vector<CuePoint> wanted = {hot(1, 2000.0), hot(3, 64000.0), memory(32000.0)};
        std::vector<std::string> written;
        rekordbox::RekordboxCueWriter::setAfterWriteForTesting(
            [&written](const std::string &path) { written.push_back(path); });
        std::vector<std::string> announced;
        {
            onelibrary::OneLibraryCueWriter writer(root);
            const std::vector<std::string> declared = writer.cueFilesForPath(ownRow.filePath);
            check(!declared.empty(), "the writer names the analysis files it may write");
            writer.setBeforeCueFileWrite([&announced](const std::string &file) { announced.push_back(file); });
            writer.writeCuesForPath(ownRow.filePath, wanted);
            writer.finishWriting();
            const std::string ext = rekordbox::extAnlzPath(root, ownRow.analysisFile);
            check(std::find(written.begin(), written.end(), ext) != written.end(),
                  "the .EXT was written through RekordboxCueWriter");
            check(std::find(announced.begin(), announced.end(), ext) != announced.end(),
                  "the .EXT was announced before it was written, for a backup");
            check(std::find(declared.begin(), declared.end(), ext) != declared.end(),
                  "cueFilesForPath() names the .EXT");
        }
        check(seabass::domain::cueSetsEqual(fileCues(root, ownRow.analysisFile), wanted),
              "the analysis file holds the written cues");
        const auto table = tableCues(root, ownRow.sourceId);
        const std::multiset<std::pair<long long, long long>> expectedTable = {
            {1, 2000000}, {3, 64000000}, {0, 32000000}};
        check(table == expectedTable, "the cue table is kept in step, " + std::to_string(table.size()) + " rows");
        const std::vector<Track> tracks = onelibrary::OneLibraryReader(root).readAll();
        const Track *read = bySourceId(tracks, ownRow.sourceId);
        check(read && seabass::domain::cueSetsEqual(read->cues, wanted), "the reader sees the written cues");

        // The same cues again: the file already holds them and is left alone.
        written.clear();
        {
            onelibrary::OneLibraryCueWriter writer(root);
            writer.writeCuesForPath(ownRow.filePath, wanted);
            writer.finishWriting();
        }
        check(written.empty(), "a file that already holds the cues is not rewritten");

        // A row DeviceLibrary shares: one file, so DeviceLibrary reads what
        // the OneLibrary write put there.
        written.clear();
        {
            onelibrary::OneLibraryCueWriter writer(root);
            writer.writeCuesForPath(sharedRow.filePath, wanted);
            writer.finishWriting();
        }
        rekordbox::RekordboxCueWriter::setAfterWriteForTesting({});
        std::vector<Track> deviceAfter = rekordbox::KaitaiRekordboxReader(root).readAll();
        bool sawShared = false;
        for (const auto &t : deviceAfter) {
            if (t.analysisFile == sharedRow.analysisFile) {
                sawShared = true;
                check(seabass::domain::cueSetsEqual(t.cues, wanted),
                      "DeviceLibrary reads the cues written for the OneLibrary row sharing its file");
            }
        }
        check(sawShared, "DeviceLibrary lists the shared analysis file");
        std::cout << "writer: the analysis file first, the table in step\n";
    }

    fs::remove_all(scratch, ec);
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "onelibrary_anlz_cues_test OK\n";
    return 0;
}
