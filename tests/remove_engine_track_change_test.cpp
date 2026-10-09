// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RemoveEngineTrackChange through the save loop, on a copy of the
// anonymized fixture with both catalogs, its import counter levelled first
// so the save is about the removal alone.
//
// Track 380 ("1c3d44 Track", Contents/1c3d44.mp3) is in five playlists,
// never at the head: "Playlist 000" (100 entries, at 26), "Playlist 002"
// (75, at 1), "Playlist 004" (30, at 26), "Playlist 019" (110, at 26) and
// "Playlist 026" (89, at 1), with one PerformanceData row.
//
// 1. Removed, listed as it is (DeleteTracksChange would leave it alone):
//    no row names 380, each of the five lists is its old order without
//    it and still one chain, and the whole-database dump differs by the
//    Track and PerformanceData rows, the five entries and the five
//    entries before them that Engine's trigger relinked. Nothing else.
// 2. Staged again on the result: the row is gone, so the change is a skip.
// 3. Undo: m.db byte for byte as it was.
// 4. An id whose row no longer names the file it was read with fails the
//    change, nothing removed.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>

#include "domain/track.hpp"
#include "gui/edit/changes/remove_engine_track_change.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

const char *const Lists[] = {"Playlist 000", "Playlist 002", "Playlist 004", "Playlist 019", "Playlist 026"};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: remove_engine_track_change_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "remove_engine_track_home");
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "remove_engine_track_stick");
    testing::levelImportCounter(stick);

    // Engine's rows as the page reads them.
    domain::Track listed;
    domain::Track other;
    for (const auto &t : infrastructure::engine::LibdjinteropEngineReader(stick.engineUtf8()).readAll()) {
        if (t.sourceId == "380") {
            listed = t;
        } else if (t.sourceId == "37") {
            other = t;
        }
    }
    assert(listed.title == "1c3d44 Track" && listed.artist == "Artist 336");
    assert(listed.filePath == pathToUtf8(stick.engine / "Contents" / "1c3d44.mp3"));
    assert(other.title == "8f064c Track");

    const std::string originalBytes = testing::fileBytes(pathFromUtf8(stick.db));
    const testing::DatabaseDump original = testing::dumpDatabase(stick.db);
    std::map<std::string, std::vector<std::int64_t>> before;
    const std::map<std::string, std::pair<size_t, size_t>> sizeAndPlace{{"Playlist 000", {100, 26}},
                                                                         {"Playlist 002", {75, 1}},
                                                                         {"Playlist 004", {30, 26}},
                                                                         {"Playlist 019", {110, 26}},
                                                                         {"Playlist 026", {89, 1}}};
    for (const char *list : Lists) {
        before[list] = testing::rootPlaylistTracks(stick.db, list);
        assert(before[list].size() == sizeAndPlace.at(list).first);
        assert(before[list].at(sizeAndPlace.at(list).second) == 380);
    }
    assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM PlaylistEntity WHERE trackId = 380;") == 5);
    assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM PerformanceData WHERE trackId = 380;") == 1);

    // 1. Removed with its five entries.
    auto change = std::make_shared<RemoveEngineTrackChange>(stick.enginePath(), listed, 1);
    assert(change->id() == QStringLiteral("rekordbox-export-sync:engine-remove:380"));
    assert(change->owner() == QStringLiteral("rekordbox-export-sync"));
    assert(change->unit() == QStringLiteral("tracks") && change->verb() == QStringLiteral("removed"));
    assert(change->unitsWritten() == 1);
    assert(change->description() == QStringLiteral("Remove \"1c3d44 Track, Artist 336\" from Engine"));
    const SaveLoopResult saved = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
    if (!saved.error.isEmpty()) {
        std::cerr << "save: " << saved.error.toStdString() << "\n";
    }
    assert(saved.error.isEmpty() && saved.warning.isEmpty());
    assert(saved.appliedIds == QStringList{change->id()} && saved.skippedIds.isEmpty());
    std::string error;
    assert(infrastructure::engine::engineRowsNamingTrack(stick.db, 380, &error) == 0);
    for (const char *list : Lists) {
        std::vector<std::int64_t> expected = before[list];
        std::erase(expected, 380);
        assert(testing::rootPlaylistTracks(stick.db, list) == expected && "the old order without it, one chain");
    }
    {
        const auto diffs = testing::differences(original, testing::dumpDatabase(stick.db));
        std::map<std::string, std::pair<size_t, size_t>> counts;
        for (const auto &d : diffs) {
            counts[d.table] = {d.onlyBefore.size(), d.onlyAfter.size()};
        }
        if (counts != std::map<std::string, std::pair<size_t, size_t>>{{"PerformanceData", {1, 0}},
                                                                        {"PlaylistEntity", {10, 5}},
                                                                        {"Track", {1, 0}}}) {
            testing::printDifferences(diffs);
            assert(false && "only the row, its entries and the five relinked entries before them changed");
        }
    }
    std::cout << "case 1 (a track in five playlists removed with its entries and performance data; nothing else "
                 "changed) OK\n";

    // 2. Again: gone, a skip.
    {
        const std::string now = testing::fileBytes(pathFromUtf8(stick.db));
        auto again = std::make_shared<RemoveEngineTrackChange>(stick.enginePath(), listed, 1);
        const SaveLoopResult skipped = testing::saveChanges({again}, stick.pioneerPath(), stick.enginePath());
        assert(skipped.error.isEmpty() && skipped.skippedIds == QStringList{again->id()});
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == now);
        std::cout << "case 2 (a row already gone is a skip) OK\n";
    }

    // 3. Undo of the removal.
    {
        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        assert(undone.error.isEmpty() && undone.appliedIds.size() == 1);
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == originalBytes && "Undo puts m.db back byte for byte");
        for (const char *list : Lists) {
            assert(testing::rootPlaylistTracks(stick.db, list) == before[list]);
        }
        std::cout << "case 3 (Undo: m.db byte for byte, 380 back in its five playlists) OK\n";
    }

    // 4. The id with another track's file: refused, nothing removed.
    {
        domain::Track mismatched = listed;
        mismatched.filePath = other.filePath;
        auto wrong = std::make_shared<RemoveEngineTrackChange>(stick.enginePath(), mismatched, 1);
        const SaveLoopResult refused = testing::saveChanges({wrong}, stick.pioneerPath(), stick.enginePath());
        assert(refused.failedId == wrong->id());
        assert(refused.error.contains(QStringLiteral("no longer names")));
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == originalBytes);
        domain::Track bad = listed;
        bad.sourceId = "380x";
        const SaveLoopResult notAnId = testing::saveChanges({std::make_shared<RemoveEngineTrackChange>(stick.enginePath(), bad, 1)},
                                                            stick.pioneerPath(), stick.enginePath());
        assert(notAnId.error.contains(QStringLiteral("is not an Engine track id")));
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == originalBytes);
        std::cout << "case 4 (an id that no longer names its file, or no id at all: refused, nothing removed) OK\n";
    }

    std::cout << "remove_engine_track_change_test: all cases passed\n";
    return 0;
}
