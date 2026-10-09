// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// application::buildEngineUpdateInput, the input seabass-cli
// sync-after-export and the GUI hand EngineUpdatePlanner: how a
// Track::filePath becomes a stick-relative path and a key, that the
// pdbImportKey map and the playlist tree (folders too) reach the planner
// as given, and that fileExists decides between an add and a refusal.
// Every expected value is pinned by hand from the input as written.

#include "application/use_cases/plan_engine_update.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"

using namespace seabass;
using namespace seabass::domain;
using seabass::application::buildEngineUpdateInput;
using seabass::application::EngineUpdateStickFacts;
using seabass::application::stickRelativePathOf;

namespace
{

Track track(const std::string &format, const std::string &id, const std::string &filePath,
            std::vector<PlaylistMembership> playlists = {})
{
    Track t;
    t.format = format;
    t.sourceId = id;
    t.filePath = filePath;
    t.filename = filePath.substr(filePath.rfind('/') + 1);
    t.title = t.filename;
    t.artist = "Artist";
    t.durationSeconds = 200.0;
    t.fileSizeBytes = 1000;  // present: matchTracks pairs by path alone
    t.playlists = std::move(playlists);
    return t;
}

void testStickRelativePath()
{
    assert(stickRelativePathOf("/media/me/STICK/Contents/A/b.mp3", "/media/me/STICK") == "Contents/A/b.mp3");
    assert(stickRelativePathOf("/media/me/STICK/Contents/A/b.mp3", "/media/me/STICK/") == "Contents/A/b.mp3");
    // An Engine row's path, as the reader joins it: the library folder
    // plus "../Contents/...".
    assert(stickRelativePathOf("/media/me/STICK/Engine Library/../Contents/A/b.mp3", "/media/me/STICK")
           == "Contents/A/b.mp3");
    // Not under the root: no key, rather than the bare filename.
    assert(stickRelativePathOf("/media/me/OTHER/Contents/A/b.mp3", "/media/me/STICK").empty());
    assert(stickRelativePathOf("/media/me/STICK", "/media/me/STICK").empty());
    assert(stickRelativePathOf("", "/media/me/STICK").empty());
    assert(stickRelativePathOf("/media/me/STICK/a.mp3", "").empty());
}

void testPathFunctionsAndFacts()
{
    EngineUpdateStickFacts facts;
    facts.stickRoot = "/media/me/STICK";
    facts.rekordboxPlaylists = {{"Folder", true, 3}, {"Folder/List", false, 7}};
    facts.enginePlaylists = {{"Own", false, 1}};
    facts.currentSequence = 15132;
    facts.enginePdbImportKey = {{"11", 42}, {"12", 0}};
    const EngineUpdateInput in = buildEngineUpdateInput({}, {}, facts);

    // The key is normalizedPathKey of the stick-relative path: case and
    // the decomposed accent folded, slashes kept.
    const std::string decomposed = "Contents/Ko\xCC\x88lsch/Song.MP3";  // "o" + U+0308
    assert(in.stickRelativeOf("/media/me/STICK/" + decomposed) == decomposed);
    assert(in.pathKeyOf(decomposed) == "contents/k\xC3\xB6lsch/song.mp3");  // U+00F6
    assert(in.stickRelativeOf("/elsewhere/Contents/a.mp3").empty());

    assert(in.currentSequence == 15132);
    assert(!in.baseline);
    assert(in.enginePdbImportKey.size() == 2);
    assert(in.enginePdbImportKey.at("11") == 42);
    assert(in.enginePdbImportKey.at("12") == 0);
    assert(in.rekordboxPlaylists.size() == 2);
    assert(in.rekordboxPlaylists[0].path == "Folder" && in.rekordboxPlaylists[0].folder
           && in.rekordboxPlaylists[0].id == 3);
    assert(in.rekordboxPlaylists[1].path == "Folder/List" && !in.rekordboxPlaylists[1].folder
           && in.rekordboxPlaylists[1].id == 7);
    assert(in.enginePlaylists.size() == 1 && in.enginePlaylists[0].path == "Own");
    assert(!in.fileExists);
}

// The fallback on a hand-built stick, through the planner: what each fact
// changes in the proposal.
void testPlanFromBuiltInput()
{
    const std::string root = "/media/me/STICK";
    std::vector<Track> rekordbox = {
        track("rekordbox", "1", root + "/Contents/A/a.mp3", {{"Folder/List", 1}}),
        track("rekordbox", "2", root + "/Contents/B/b.mp3"),  // file missing
        track("rekordbox", "3", root + "/Contents/E/e.mp3"),  // file present, Engine lacks it
    };
    std::vector<Track> engine = {
        // The same file as rekordbox's 1, in another case: one key.
        track("engine", "10", root + "/Contents/a/A.MP3"),
        track("engine", "11", root + "/Contents/C/c.mp3"),  // imported, rekordbox dropped it
        track("engine", "12", root + "/Contents/D/d.mp3"),  // Engine's own
    };

    EngineUpdateStickFacts facts;
    facts.stickRoot = root;
    facts.rekordboxPlaylists = {{"Folder", true, 3}, {"Folder/List", false, 7}};
    facts.currentSequence = 15132;
    facts.enginePdbImportKey = {{"10", 1}, {"11", 2}, {"12", 0}};
    facts.fileExists = [](const std::string &relative) { return relative != "Contents/B/b.mp3"; };

    const EngineUpdateProposal p = EngineUpdatePlanner::plan(buildEngineUpdateInput(rekordbox, engine, facts));

    assert(!p.hasBaseline);
    assert(p.currentSequence == 15132);
    // e.mp3: added, checked. b.mp3: refused, the file is not on the stick.
    assert(p.tracksToAdd.size() == 1);
    assert(p.tracksToAdd[0].stickRelativePath == "Contents/E/e.mp3");
    assert(p.tracksToAdd[0].pathKey == "contents/e/e.mp3");
    assert(p.tracksToAdd[0].header.checkedByDefault);
    assert(p.notAdded.size() == 1);
    assert(p.notAdded[0].stickRelativePath == "Contents/B/b.mp3");
    assert(p.notAdded[0].header.reason == EngineUpdateReason::FileNotOnStick);
    // a.mp3 is one file in two spellings: neither added nor removed.
    for (const auto &add : p.tracksToAdd) {
        assert(add.pathKey != "contents/a/a.mp3");
    }
    // c.mp3: pdbImportKey 2, a conflict. d.mp3: pdbImportKey 0, kept.
    int importedConflicts = 0;
    for (const auto &c : p.conflicts) {
        if (c.header.reason == EngineUpdateReason::NoBaselineImportedRow) {
            ++importedConflicts;
            assert(c.pathKey == "contents/c/c.mp3");
        }
    }
    assert(importedConflicts == 1);
    int ownTracks = 0;
    for (const auto &k : p.engineOwnKept) {
        if (k.header.reason == EngineUpdateReason::NoBaselineEngineOnly && k.playlistPath.empty()) {
            ++ownTracks;
            assert(k.engine.sourceId == "12");
        }
    }
    assert(ownTracks == 1);
    assert(p.tracksToRemove.empty());
    // The folder from the tree is created, before the list inside it.
    assert(p.playlistsToCreate.size() == 2);
    assert(p.playlistsToCreate[0].path == "Folder" && p.playlistsToCreate[0].folder);
    assert(p.playlistsToCreate[1].path == "Folder/List" && !p.playlistsToCreate[1].folder);
    // Unset fileExists: every file is taken to be there, and b.mp3 is
    // proposed as an add.
    facts.fileExists = nullptr;
    const EngineUpdateProposal unchecked = EngineUpdatePlanner::plan(buildEngineUpdateInput(rekordbox, engine, facts));
    assert(unchecked.tracksToAdd.size() == 2);
    assert(unchecked.notAdded.empty());
}

void testFileExistsUnder()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path()
        / ("plan_engine_update_test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "Contents" / "A");
    std::ofstream(root / "Contents" / "A" / "a.mp3") << "x";
    const auto exists = application::fileExistsUnder(pathToUtf8(root));
    assert(exists("Contents/A/a.mp3"));
    assert(!exists("Contents/A/b.mp3"));
    assert(!exists("Contents/A"));  // a directory is not a file
    assert(!exists(""));
    fs::remove_all(root);
}

}  // namespace

int main()
{
    testStickRelativePath();
    testPathFunctionsAndFacts();
    testPlanFromBuiltInput();
    testFileExistsUnder();
    std::cout << "plan_engine_update_test: all passed\n";
    return 0;
}
