// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::baselineFrom and the item keys of Sync after Rekordbox Export.
// The baseline is the B of a three-way merge, so a member out of order or
// a cue that arrives with a confident origin is a wrong proposal later.
// Every value below is pinned by hand from the input as written.

#include "domain/rekordbox_baseline.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using seabass::domain::baselineFrom;
using seabass::domain::BaselineGaps;
using seabass::domain::CuePoint;
using seabass::domain::ItemKey;
using seabass::domain::itemKey;
using seabass::domain::parseItemKey;
using seabass::domain::PlaylistInfo;
using seabass::domain::PlaylistMembership;
using seabass::domain::RekordboxBaseline;
using seabass::domain::Track;
using seabass::domain::ValueOrigin;

namespace
{

using Members = std::vector<std::string>;

const std::string StickRoot = "/media/X/";

// The test's own stand-ins for the infrastructure and application
// functions: strip the mount point, lowercase. Lowercasing makes the key
// visibly differ from the relative path, so a baseline that skips
// pathKeyOf fails.
std::string stickRelative(const std::string &filePath)
{
    return filePath.rfind(StickRoot, 0) == 0 ? filePath.substr(StickRoot.size()) : std::string();
}

std::string lowerKey(const std::string &path)
{
    std::string key = path;
    for (char &c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return key;
}

Track rekordboxTrack(const std::string &id, const std::string &relative, std::vector<PlaylistMembership> playlists)
{
    Track t;
    t.sourceId = id;
    t.format = "rekordbox";
    t.filePath = relative.empty() ? std::string() : StickRoot + relative;
    t.playlists = std::move(playlists);
    return t;
}

struct Input
{
    std::vector<Track> tracks;
    std::vector<PlaylistInfo> playlists;
};

Input sampleInput()
{
    Input in;
    in.playlists = {
        {"Techno", true, 10},
        {"Techno/Peak", false, 11},
        {"Techno/Warmup", false, 12},  // empty: no track names it
        {"Loose", false, 20},
        {"Lost/Child", false, 30},  // "Lost" is in no PlaylistInfo
    };

    Track one = rekordboxTrack("101", "Contents/A/One.mp3",
                               {{"Techno/Peak", 2}, {"Loose", -1}, {"Lost/Child", 0}});
    one.analysisFile = "/PIONEER/USBANLZ/P001/0000ABCD/ANLZ0000.DAT";
    one.rating = 4;
    one.comment = "keep";
    one.bpm = 128.0;
    one.durationSeconds = 300.4567;
    one.cues = {
        CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"},
        CuePoint{CuePoint::Kind::Memory, 0, 500.5, "", ""},
    };

    Track two = rekordboxTrack("102", "Contents/B/Two.mp3",
                               {{"Techno/Peak", 0}, {"Loose", -1}, {"Techno", 0}, {"Gone", 4}});

    Track noPath = rekordboxTrack("103", "", {{"Loose", 0}});

    Track three = rekordboxTrack("abc", "Contents/C/Three.mp3",
                                 {{"Techno/Peak", 1}, {"Loose", 3}, {"Techno/Peak", 5}, {"Lost/Child", 0}});
    three.bpm = 174.0;

    in.tracks = {one, two, noPath, three};
    return in;
}

void testTracksCarriedWithUnknownOrigin()
{
    const Input in = sampleInput();
    BaselineGaps gaps;
    const RekordboxBaseline b = baselineFrom(in.tracks, in.playlists, 15132, stickRelative, lowerKey, &gaps);

    assert(b.pdbSequence == 15132);
    assert(b.engineUuid.empty());
    assert(b.recordedAtUnix == 0);
    assert(b.declined.empty());

    // Three of four: the row with no path is reported, not recorded.
    assert(b.tracks.size() == 3);
    assert(gaps.unkeyedTracks == Members{"103"});

    const auto &one = b.tracks[0];
    assert(one.pathKey == "contents/a/one.mp3");
    assert(one.stickRelativePath == "Contents/A/One.mp3");
    assert(one.analysisFile == "/PIONEER/USBANLZ/P001/0000ABCD/ANLZ0000.DAT");
    assert(one.pdbId == 101);
    assert(one.rating == 4);
    assert(one.ratingOrigin == ValueOrigin::Unknown);
    assert(one.comment == "keep");
    assert(one.bpm == 128.0);
    assert(one.durationMs == 300457);
    assert(one.cues.size() == 2);
    assert(one.cues[0].cue.kind == CuePoint::Kind::Hot);
    assert(one.cues[0].cue.hotCueNumber == 1);
    assert(one.cues[0].cue.positionMs == 1000.0);
    assert(one.cues[0].cue.color == "#FF0000");
    assert(one.cues[0].cue.comment == "drop");
    assert(one.cues[0].origin == ValueOrigin::Unknown);
    assert(one.cues[1].cue.kind == CuePoint::Kind::Memory);
    assert(one.cues[1].cue.positionMs == 500.5);
    assert(one.cues[1].origin == ValueOrigin::Unknown);

    const auto &two = b.tracks[1];
    assert(two.pathKey == "contents/b/two.mp3");
    assert(two.pdbId == 102);
    assert(!two.rating.has_value());
    assert(two.cues.empty());

    // A sourceId that is not a number has no pdb id, and is still kept.
    const auto &three = b.tracks[2];
    assert(three.pathKey == "contents/c/three.mp3");
    assert(three.pdbId == 0);
    assert(three.bpm == 174.0);
}

void testPlaylistsTreeAndOrder()
{
    const Input in = sampleInput();
    BaselineGaps gaps;
    const RekordboxBaseline b = baselineFrom(in.tracks, in.playlists, 1, stickRelative, lowerKey, &gaps);

    assert(b.playlists.size() == 5);

    const auto *techno = b.findPlaylist(10);
    assert(techno && techno->folder && techno->parentId == 0 && techno->path == "Techno");
    // The membership naming the folder is dropped and reported.
    assert(techno->members.empty());

    // By position: Two at 0, Three at 1, One at 2, Three again at 5.
    const auto *peak = b.findPlaylist(11);
    assert(peak && !peak->folder && peak->parentId == 10);
    assert((peak->members ==
            Members{"contents/b/two.mp3", "contents/c/three.mp3", "contents/a/one.mp3", "contents/c/three.mp3"}));

    // In the list though no track names it.
    const auto *warmup = b.findPlaylist(12);
    assert(warmup && warmup->parentId == 10 && warmup->members.empty());

    // Three at 3 first; One and Two (position -1) after it, in reader order.
    const auto *loose = b.findPlaylist(20);
    assert(loose && loose->parentId == 0);
    assert((loose->members == Members{"contents/c/three.mp3", "contents/a/one.mp3", "contents/b/two.mp3"}));

    // Equal positions keep reader order; an unknown parent leaves it at the top.
    const auto *child = b.findPlaylist(30);
    assert(child && child->parentId == 0);
    assert((child->members == Members{"contents/a/one.mp3", "contents/c/three.mp3"}));
    assert(gaps.orphanPlaylists == Members{"Lost/Child"});

    assert((gaps.unknownMemberships == Members{"102 in Techno", "102 in Gone"}));
}

void testLookups()
{
    const Input in = sampleInput();
    const RekordboxBaseline b = baselineFrom(in.tracks, in.playlists, 1, stickRelative, lowerKey);

    const auto *three = b.findTrack("contents/c/three.mp3");
    assert(three && three->stickRelativePath == "Contents/C/Three.mp3");
    // Keys, not paths: the relative spelling finds nothing.
    assert(b.findTrack("Contents/C/Three.mp3") == nullptr);
    assert(b.findTrack("") == nullptr);

    assert(b.findPlaylist(99) == nullptr);
    const auto *warmup = b.findPlaylistByPath("Techno/Warmup");
    assert(warmup && warmup->id == 12);
    assert(b.findPlaylistByPath("Warmup") == nullptr);

    // Two rows for one file are both kept; the first is found.
    std::vector<Track> twice = {rekordboxTrack("1", "Contents/Same.mp3", {}),
                                rekordboxTrack("2", "Contents/SAME.mp3", {})};
    const RekordboxBaseline dup = baselineFrom(twice, {}, 1, stickRelative, lowerKey);
    assert(dup.tracks.size() == 2);
    assert(dup.findTrack("contents/same.mp3")->pdbId == 1);
}

void testItemKeys()
{
    using seabass::domain::commentItemKey;
    using seabass::domain::cueItemKey;
    using seabass::domain::memberItemKey;
    using seabass::domain::playlistItemKey;
    using seabass::domain::ratingItemKey;
    using seabass::domain::trackItemKey;

    // The spellings, pinned: these end up in the baseline file.
    const std::string path = "contents/a:b/one.mp3";  // a ':' inside the path stays intact
    assert(trackItemKey(path) == "track:contents/a:b/one.mp3");
    assert(playlistItemKey(11) == "playlist:11");
    assert(memberItemKey(11, path) == "member:11:contents/a:b/one.mp3");
    assert(ratingItemKey(path) == "rating:contents/a:b/one.mp3");
    assert(commentItemKey(path) == "comment:contents/a:b/one.mp3");
    assert(cueItemKey(path, CuePoint{CuePoint::Kind::Hot, 3, 1234.0, "", ""}) == "cue:hot:3:contents/a:b/one.mp3");
    assert(cueItemKey(path, CuePoint{CuePoint::Kind::Memory, 0, 500.5, "", ""}) ==
           "cue:memory:501:contents/a:b/one.mp3");
    CuePoint loop{CuePoint::Kind::Memory, 0, 2000.4, "", ""};
    loop.isLoop = true;
    loop.loopEndMs = 4000.0;
    assert(cueItemKey(path, loop) == "cue:loop:2000:contents/a:b/one.mp3");
    // A hot loop is named by its pad, like a hot cue.
    CuePoint hotLoop{CuePoint::Kind::Hot, 2, 100.0, "", ""};
    hotLoop.isLoop = true;
    assert(cueItemKey(path, hotLoop) == "cue:hot:2:contents/a:b/one.mp3");

    // Round trip, every kind.
    auto key = [&](ItemKey::Kind kind) {
        ItemKey k;
        k.kind = kind;
        k.pathKey = path;
        return k;
    };
    std::vector<ItemKey> keys = {key(ItemKey::Kind::Track), key(ItemKey::Kind::Rating), key(ItemKey::Kind::Comment)};
    ItemKey playlist;
    playlist.kind = ItemKey::Kind::Playlist;
    playlist.playlistId = 4294967295u;
    keys.push_back(playlist);
    ItemKey member = key(ItemKey::Kind::Member);
    member.playlistId = 7;
    keys.push_back(member);
    ItemKey hot = key(ItemKey::Kind::HotCue);
    hot.pad = 8;
    keys.push_back(hot);
    ItemKey memory = key(ItemKey::Kind::MemoryCue);
    memory.positionMs = 0;
    keys.push_back(memory);
    ItemKey memoryLoop = key(ItemKey::Kind::MemoryLoop);
    memoryLoop.positionMs = 359999;
    keys.push_back(memoryLoop);
    for (const auto &k : keys) {
        const auto parsed = parseItemKey(itemKey(k));
        assert(parsed && *parsed == k);
    }
    assert(itemKey(playlist) == "playlist:4294967295");

    // What the grammar does not produce.
    for (const std::string bad : {"", "track", "track:", "playlist:", "playlist:12x", "playlist:-1",
                                  "playlist:4294967296", "member:7", "member:x:a.mp3", "member:7:", "rating:",
                                  "cue:hot:0:a.mp3", "cue:hot:1", "cue:hot:a:a.mp3", "cue:memory:12:",
                                  "cue:side:1:a.mp3", "folder:a.mp3"}) {
        if (parseItemKey(bad)) {
            std::cerr << "parsed what it should refuse: " << bad << "\n";
            assert(false);
        }
    }
}

}  // namespace

int main()
{
    testTracksCarriedWithUnknownOrigin();
    testPlaylistsTreeAndOrder();
    testLookups();
    testItemKeys();
    std::cout << "rekordbox_baseline_test: all passed\n";
    return 0;
}
