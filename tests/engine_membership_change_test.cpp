// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// EngineMembershipChange through the save loop, on a copy of the
// anonymized fixture with both catalogs, the counter levelled first.
// "Playlist 004" holds 30 tracks.
//
// 1. One save: AddEngineTrackChange for a planted WAV, then one membership
//    change for "Playlist 004" that takes its 6th member out and adds, in
//    this order: the new row after the 10th member (found by its file,
//    made by the change before it in the same save), a track first (no
//    anchor), one whose anchor's file has no Engine row (appended), one
//    whose anchor has a row that is not in the playlist (appended), and
//    the 1st member again (already there, left where it is). The list is
//    exactly the order worked out by hand, one chain; no other playlist
//    changed.
// 2. Undo: m.db byte for byte.
// 3. Refused, nothing written: a path two playlists spell, a playlist
//    Engine does not have, an added file with no Engine row. (An added
//    file with two rows cannot be planted: Track.path is UNIQUE here.)

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "gui/edit/changes/add_engine_track_change.hpp"
#include "gui/edit/changes/engine_membership_change.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

class RateProbe : public application::TrackMetadataProbe
{
public:
    std::optional<application::FileMetadata> read(const std::string &) override
    {
        application::FileMetadata m;
        m.sampleRate = 44100;
        return m;
    }
};

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_membership_change_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "engine_membership_home");
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "engine_membership_stick");
    testing::levelImportCounter(stick);
    const std::string library = stick.engineUtf8();

    const std::vector<std::int64_t> before = testing::rootPlaylistTracks(stick.db, "Playlist 004");
    assert(before.size() == 30);
    const std::set<std::int64_t> members(before.begin(), before.end());
    std::map<std::int64_t, domain::Track> rows;
    std::map<std::string, int> filesSeen;
    for (const auto &t : infrastructure::engine::LibdjinteropEngineReader(library).readAll()) {
        rows[std::stoll(t.sourceId)] = t;
        ++filesSeen[t.filePath];
    }
    // Three tracks outside the playlist, each the one row of its file.
    std::vector<std::int64_t> outsiders;
    for (const auto &[id, t] : rows) {
        if (!members.count(id) && !t.filePath.empty() && filesSeen[t.filePath] == 1 && outsiders.size() < 3) {
            outsiders.push_back(id);
        }
    }
    assert(outsiders.size() == 3);
    const auto fileOf = [&](std::int64_t id) { return rows.at(id).filePath; };
    const std::int64_t taken = before.at(5);
    const std::int64_t anchor = before.at(9);
    assert(filesSeen[fileOf(taken)] == 1 && filesSeen[fileOf(anchor)] == 1 && filesSeen[fileOf(before.at(0))] == 1);

    const std::string originalBytes = testing::fileBytes(pathFromUtf8(stick.db));
    const testing::DatabaseDump original = testing::dumpDatabase(stick.db);
    std::map<std::string, std::vector<std::int64_t>> otherLists;
    for (const char *list : {"Playlist 000", "Playlist 002", "Playlist 019"}) {
        otherLists[list] = testing::rootPlaylistTracks(stick.db, list);
    }

    // 1. The new track and the playlist in one save.
    const fs::path wav = stick.root / "Music" / "New Member.wav";
    testing::writeSilentWav(wav, 44100, 2);
    domain::Track newTrack;
    newTrack.format = "rekordbox";
    newTrack.title = "New Member";
    newTrack.filePath = pathToUtf8(wav);
    auto add = std::make_shared<AddEngineTrackChange>(stick.enginePath(), newTrack, std::make_shared<RateProbe>(), 3);
    // A track with a row that is not in the playlist and that this save
    // does not add: the fourth add's anchor.
    std::int64_t notMember = 0;
    for (const auto &[id, t] : rows) {
        if (!members.count(id) && id != outsiders[0] && id != outsiders[1] && id != outsiders[2] && !t.filePath.empty()
            && filesSeen[t.filePath] == 1) {
            notMember = id;
            break;
        }
    }
    assert(notMember != 0);
    const std::vector<EngineMembershipChange::Add> adds{
        {newTrack.filePath, fileOf(anchor), "New Member"},
        {fileOf(outsiders[0]), std::nullopt, "first"},
        {fileOf(outsiders[1]), pathToUtf8(stick.root / "Music" / "never exported.wav"), "anchor without a row"},
        {fileOf(outsiders[2]), fileOf(notMember), "anchor not a member"},
        {fileOf(before.at(0)), std::nullopt, "already a member"},
    };
    auto membership = std::make_shared<EngineMembershipChange>(
        stick.enginePath(), "Playlist 004", adds,
        std::vector<EngineMembershipChange::Remove>{{std::to_string(taken), fileOf(taken), "taken out"}}, 3);
    assert(membership->id() == QStringLiteral("rekordbox-export-sync:engine-membership:Playlist 004"));
    assert(membership->owner() == QStringLiteral("rekordbox-export-sync"));
    assert(membership->unit() == QStringLiteral("playlists") && membership->verb() == QStringLiteral("updated"));
    assert(membership->description()
           == QStringLiteral("Update the playlist \"Playlist 004\" in Engine (5 tracks to add, 1 track to take out)"));

    const SaveLoopResult saved = testing::saveChanges({add, membership}, stick.pioneerPath(), stick.enginePath());
    if (!saved.error.isEmpty()) {
        std::cerr << "save: " << saved.error.toStdString() << "\n";
    }
    assert(saved.error.isEmpty() && saved.warning.isEmpty() && saved.skippedIds.isEmpty());
    assert((saved.appliedIds == QStringList{add->id(), membership->id()}));
    assert(add->createdId() == 1575);
    assert(membership->entriesAdded() == 4 && membership->entriesRemoved() == 1 && membership->entriesAlreadyDone() == 1);

    // By hand: the 6th out; 1575 after the 10th (index 8 once the 6th is
    // gone); outsiders[0] first; outsiders[1] and outsiders[2] last, in
    // that order; the 1st member still right after outsiders[0].
    std::vector<std::int64_t> expected;
    expected.push_back(outsiders[0]);
    for (size_t i = 0; i < before.size(); ++i) {
        if (i == 5) {
            continue;
        }
        expected.push_back(before[i]);
        if (i == 9) {
            expected.push_back(1575);
        }
    }
    expected.push_back(outsiders[1]);
    expected.push_back(outsiders[2]);
    const auto after = testing::rootPlaylistTracks(stick.db, "Playlist 004");
    assert(after.size() == 33 && "30, one out, four in");
    assert(after == expected && "exactly the order worked out by hand");
    assert(after.at(0) == outsiders[0] && after.at(1) == before.at(0) && after.at(9) == before.at(9) && after.at(10) == 1575);
    for (const auto &[list, order] : otherLists) {
        assert(testing::rootPlaylistTracks(stick.db, list) == order && "other playlists unchanged");
    }
    {
        const auto diffs = testing::differences(original, testing::dumpDatabase(stick.db));
        std::set<std::string> tables;
        for (const auto &d : diffs) {
            tables.insert(d.table);
        }
        if (tables != std::set<std::string>{"PerformanceData", "PlaylistEntity", "Track", "sqlite_sequence"}) {
            testing::printDifferences(diffs);
            assert(false && "only the new row and Playlist 004's entries changed");
        }
        assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM PlaylistEntity;")
               == testing::sqlScalar(stick.db, "SELECT count(*) FROM PlaylistEntity WHERE listId <> 249;") + 33);
    }
    std::cout << "case 1 (a new row and four tracks in, one out, anchors honoured, missing anchors appended, a member "
                 "left where it is) OK\n";

    // 2. Undo.
    {
        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        assert(undone.error.isEmpty() && undone.appliedIds.size() == 1);
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == originalBytes && "Undo puts m.db back byte for byte");
        assert(testing::rootPlaylistTracks(stick.db, "Playlist 004") == before);
        std::cout << "case 2 (Undo: m.db byte for byte) OK\n";
    }

    // 3. Refusals.
    {
        const auto refusedWith = [&](const std::string &playlist, std::vector<EngineMembershipChange::Add> a,
                                     const QString &expect) {
            const std::string bytes = testing::fileBytes(pathFromUtf8(stick.db));
            auto change = std::make_shared<EngineMembershipChange>(stick.enginePath(), playlist, std::move(a),
                                                                   std::vector<EngineMembershipChange::Remove>{}, 1);
            const SaveLoopResult r = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
            if (r.failedId != change->id() || !r.error.contains(expect)) {
                std::cerr << playlist << ": \"" << r.error.toStdString() << "\"\n";
            }
            assert(r.failedId == change->id() && r.error.contains(expect));
            assert(testing::fileBytes(pathFromUtf8(stick.db)) == bytes && "nothing written");
        };
        const EngineMembershipChange::Add one{fileOf(outsiders[0]), std::nullopt, "one"};
        (void)infrastructure::engine::createEnginePlaylist(library, "Twice/Child");
        {
            auto db = djinterop::engine::load_database(library);
            db.create_root_playlist("Twice/Child");
        }
        refusedWith("Twice/Child", {one}, QStringLiteral("Engine has 2 playlists named \"Twice/Child\""));
        refusedWith("Playlist 404", {one}, QStringLiteral("Engine has no playlist \"Playlist 404\""));
        refusedWith("Playlist 004", {{newTrack.filePath, std::nullopt, "never added"}},
                    QStringLiteral("Engine has no row for ") + QString::fromStdString(newTrack.filePath));
        // Two rows for one file cannot be planted here: this schema's Track
        // has UNIQUE(path), and engineTrackIdsForFile matches the path
        // exactly, so the change's two-row refusal is unreachable on it
        // (a library with two spellings of one file is the planner's
        // DuplicateEngineRows, which stages nothing).
        std::cout << "case 3 (an ambiguous path, a missing playlist, a file with no row: refused, nothing written) "
                     "OK\n";
    }

    std::cout << "engine_membership_change_test: all cases passed\n";
    return 0;
}
