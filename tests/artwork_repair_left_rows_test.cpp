// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// #120: after the Cover Art repair, the AlbumArt rows the repaired tracks
// left and nothing references any more are gone, in the same save, after
// its backup; Undo brings every one back, byte for byte.
//
// On a COPY of tests/fixtures/anonymized_library (schema 3.0.2, covers as
// files, every row a link to another stick's PIONEER/Artwork). In it:
// track 1 alone on row 1 (a link here, but row 1 is ALBUM_ART_ID_NONE to
// libdjinterop and Engine, so it stays), tracks 2, 3, 4 each alone on
// rows 2, 3, 4, tracks 469 and 1562 sharing row 469, and rows 104 and 108
// that no track used before any repair.

#include <QCoreApplication>
#include <QString>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine_change_fixture.hpp"
#include "gui/edit/changes/repair_artwork_change.hpp"
#include "infrastructure/engine/engine_artwork.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using gui::RepairArtworkChange;
using gui::SaveLoopResult;

namespace
{

std::string blobLiteral(const std::vector<std::uint8_t> &hash)
{
    static const char *Hex = "0123456789abcdef";
    std::string out = "x'";
    for (const std::uint8_t b : hash) {
        out += Hex[b >> 4];
        out += Hex[b & 0xf];
    }
    return out + "'";
}

void writeFile(const fs::path &file, const std::string &bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

// Every file under the library's Artwork/, with its bytes.
std::map<std::string, std::string> artworkFiles(const testing::EngineChangeStick &stick)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(stick.engine / "Artwork", ec)) {
        if (entry.is_regular_file()) {
            out[pathToUtf8(entry.path())] = testing::fileBytes(entry.path());
        }
    }
    return out;
}

bool rowExists(const testing::EngineChangeStick &stick, std::int64_t id)
{
    return testing::sqlScalar(stick.db, "SELECT count(*) FROM AlbumArt WHERE id = " + std::to_string(id) + ";") == 1;
}

std::int64_t rowOf(const testing::EngineChangeStick &stick, std::int64_t track)
{
    return testing::sqlScalar(stick.db, "SELECT albumArtId FROM Track WHERE id = " + std::to_string(track) + ";");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: artwork_repair_left_rows_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);

    for (const int countHint : {5, 5000}) {
        const bool scratch = countHint > 1000;
        const testing::EngineChangeStick stick =
            testing::makeEngineChangeStick(fixture, scratch ? "left_rows_120_scratch" : "left_rows_120");
        // Both catalogs, level, so the save leaves the import counter alone
        // and its Undo can be byte for byte.
        testing::levelImportCounter(stick);

        // Two rows made what a stick pulled mid-write leaves: a file hash
        // whose file under Artwork/ is cut short. Row 3's file is named by
        // row 3 alone; row 4's also by row 104, which no track uses.
        const std::vector<std::uint8_t> onlyHash(20, 0x33);
        const std::vector<std::uint8_t> sharedHash(20, 0x44);
        assert(testing::sqlExec(stick.db, "UPDATE AlbumArt SET hash = " + blobLiteral(onlyHash) + " WHERE id = 3;"));
        assert(testing::sqlExec(stick.db, "UPDATE AlbumArt SET hash = " + blobLiteral(sharedHash)
                                              + " WHERE id IN (4, 104);"));
        const fs::path onlyFile =
            stick.engine / "Artwork" / (infrastructure::engine::artworkFileName(onlyHash) + ".jpg");
        const fs::path sharedFile =
            stick.engine / "Artwork" / (infrastructure::engine::artworkFileName(sharedHash) + ".jpg");
        writeFile(onlyFile, std::string("\xFF\xD8", 2));
        writeFile(sharedFile, std::string("\xFF", 1));

        // What the fixture is, checked rather than assumed.
        assert(rowOf(stick, 1) == 1 && rowOf(stick, 2) == 2 && rowOf(stick, 3) == 3 && rowOf(stick, 4) == 4);
        assert(rowOf(stick, 469) == 469 && rowOf(stick, 1562) == 469);
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM Track WHERE albumArtId IN (1, 2, 3, 4, 104, 108);")
               == 4);
        const std::int64_t orphansBefore = testing::sqlScalar(
            stick.db, "SELECT count(*) FROM AlbumArt a WHERE NOT EXISTS (SELECT 1 FROM Track t WHERE t.albumArtId = a.id);");
        assert(orphansBefore == 2);

        // The covers the repair copies in: one image per track, on the stick.
        std::vector<infrastructure::engine::ArtworkEntry> entries;
        for (const std::int64_t track : {1, 2, 3, 4, 469}) {
            const fs::path image = stick.root / "covers" / ("t" + std::to_string(track) + ".jpg");
            writeFile(image, std::string("\xFF\xD8\xFF", 3) + "COVER OF TRACK " + std::to_string(track));
            infrastructure::engine::ArtworkEntry entry;
            entry.trackId = track;
            entry.storage = track == 3 || track == 4 ? infrastructure::engine::ArtworkStorage::Cached
                                                     : infrastructure::engine::ArtworkStorage::ImportedPath;
            entry.imageOnStick = pathToUtf8(image);
            entry.libraryCoversInDatabase = false;
            entries.push_back(entry);
        }

        const std::string dbBefore = testing::fileBytes(pathFromUtf8(stick.db));
        const testing::DatabaseDump dumpBefore = testing::dumpDatabase(stick.db);
        const auto artworkBefore = artworkFiles(stick);

        std::vector<std::shared_ptr<gui::PendingChange>> changes;
        for (const auto &entry : entries) {
            changes.push_back(
                std::make_shared<RepairArtworkChange>(stick.enginePath(), entry, countHint, changes.empty(), nullptr));
        }
        const SaveLoopResult saved = testing::saveChanges(changes, stick.pioneerPath(), stick.enginePath());
        if (!saved.error.isEmpty()) {
            std::cerr << "save: " << saved.error.toStdString() << "\n";
        }
        assert(saved.error.isEmpty() && saved.appliedIds.size() == 5 && saved.skippedIds.isEmpty());

        // Every repaired track on a row of its own kind now, its file there.
        for (const std::int64_t track : {1, 2, 3, 4, 469}) {
            const std::int64_t row = rowOf(stick, track);
            assert(row > 1557 && "a new row");
            assert(testing::sqlScalar(stick.db, "SELECT typeof(hash) = 'blob' AND length(hash) = 20 FROM AlbumArt "
                                                "WHERE id = " + std::to_string(row) + ";")
                   == 1);
        }
        assert(rowExists(stick, 1) && "row 1 is ALBUM_ART_ID_NONE: it stays");
        assert(!rowExists(stick, 2) && "left by track 2, used by nothing: gone");
        assert(!rowExists(stick, 3) && !rowExists(stick, 4));
        assert(rowExists(stick, 469) && rowOf(stick, 1562) == 469 && "track 1562 still uses row 469");
        assert(rowExists(stick, 104) && rowExists(stick, 108) && "rows nothing used before the repair are not its");
        assert(!fs::exists(onlyFile) && "the file only row 3 named went with it");
        assert(testing::fileBytes(sharedFile) == std::string("\xFF", 1) && "row 104 still names this one");

        // Nothing else: the AlbumArt rows that went and came, and the five
        // tracks pointed at their new rows.
        const auto diffs = testing::differences(dumpBefore, testing::dumpDatabase(stick.db));
        std::map<std::string, std::pair<size_t, size_t>> counts;
        for (const auto &d : diffs) {
            counts[d.table] = {d.onlyBefore.size(), d.onlyAfter.size()};
        }
        if (counts != std::map<std::string, std::pair<size_t, size_t>>{{"AlbumArt", {3, 5}},
                                                                        {"Track", {5, 5}},
                                                                        {"sqlite_sequence", {1, 1}}}) {
            testing::printDifferences(diffs);
            assert(false && "three rows gone, five new ones, five tracks moved; nothing else");
        }

        bool said = false;
        // Informational, not a note: it must not keep a whole save from
        // being said with a toast, and the toast says it as a clause.
        for (const QString &note : saved.info) {
            said = said || note == QStringLiteral("Removed 3 cover row(s) that no track used any more.");
        }
        assert(saved.infoClauses == QStringList{QStringLiteral("3 unused cover rows removed")});
        for (const QString &note : saved.notes) {
            assert(!note.startsWith(QStringLiteral("Removed")) && "not a note the user has to act on");
        }
        if (!said) {
            for (const QString &note : saved.info) {
                std::cerr << "note: " << note.toStdString() << "\n";
            }
        }
        assert(said && "the outcome says how many rows went");

        // Undo: m.db byte for byte, and every file under Artwork/ that was
        // there before, with its bytes.
        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        if (!undone.error.isEmpty()) {
            std::cerr << "undo: " << undone.error.toStdString() << "\n";
        }
        assert(undone.error.isEmpty());
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == dbBefore && "Undo puts m.db back byte for byte");
        const auto artworkAfter = artworkFiles(stick);
        for (const auto &[file, bytes] : artworkBefore) {
            const auto it = artworkAfter.find(file);
            assert(it != artworkAfter.end() && it->second == bytes && "every cover file back, byte for byte");
        }
        std::cout << (scratch ? "case 2" : "case 1")
                  << " (left rows go, row 1, a shared row and old orphans stay; a file only a left row named goes; "
                     "Undo byte for byte"
                  << (scratch ? ", through a scratch copy" : "") << ") OK\n";
    }

    std::cout << "artwork_repair_left_rows_test: all cases passed\n";
    return 0;
}
