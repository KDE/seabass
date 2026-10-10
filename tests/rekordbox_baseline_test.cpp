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
#include <map>
#include <set>
#include <string>
#include <vector>

using seabass::domain::baselineFrom;
using seabass::domain::nextBaseline;
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
    using seabass::domain::orderItemKey;
    using seabass::domain::playlistItemKey;
    using seabass::domain::ratingItemKey;
    using seabass::domain::trackItemKey;

    // The spellings, pinned: these end up in the baseline file.
    const std::string path = "contents/a:b/one.mp3";  // a ':' inside the path stays intact
    assert(trackItemKey(path) == "track:contents/a:b/one.mp3");
    assert(playlistItemKey(11) == "playlist:11");
    assert(memberItemKey(11, path) == "member:11:contents/a:b/one.mp3");
    assert(orderItemKey(11) == "order:11");
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
    ItemKey order;
    order.kind = ItemKey::Kind::Order;
    order.playlistId = 7;
    keys.push_back(order);
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
                                  "cue:side:1:a.mp3", "folder:a.mp3", "order:", "order:7:a.mp3",
                                  "order:x"}) {
        if (parseItemKey(bad)) {
            std::cerr << "parsed what it should refuse: " << bad << "\n";
            assert(false);
        }
    }
}

// nextBaseline: the stick as Seabass last recorded it, and rekordbox as
// it is now after an export that changed track One's rating, comment and
// cues, rated Two, added Three, renamed "Peak", deleted "Warmup" and
// created "New".
struct NextInput
{
    RekordboxBaseline previous;
    std::vector<Track> now;
    std::vector<PlaylistInfo> playlists;
};

const std::string One = "contents/a/one.mp3";
const std::string Two = "contents/b/two.mp3";
const std::string Three = "contents/c/three.mp3";

NextInput nextInput()
{
    NextInput in;
    Track one = rekordboxTrack("1", "Contents/A/One.mp3", {{"Peak", 0}});
    one.rating = 3;
    one.comment = "old";
    one.cues = {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "", ""}, CuePoint{CuePoint::Kind::Memory, 0, 500.0, "", ""}};
    Track two = rekordboxTrack("2", "Contents/B/Two.mp3", {{"Peak", 1}, {"Warmup", 0}});
    in.previous = baselineFrom({one, two}, {{"Peak", false, 11}, {"Warmup", false, 12}}, 100, stickRelative,
                               lowerKey);
    in.previous.engineUuid = "uuid-1";
    in.previous.tracks[0].cues[0].origin = ValueOrigin::Seabass;    // a pad Seabass synced
    in.previous.tracks[0].cues[1].origin = ValueOrigin::Rekordbox;  // the export's memory cue
    in.previous.tracks[1].ratingOrigin = ValueOrigin::Rekordbox;
    in.previous.declined = {{"rating:" + Two, "h-old"}, {"comment:" + Three, "h-stale"}};

    Track oneNow = rekordboxTrack("1", "Contents/A/One.mp3", {{"Peak Time", 1}});
    oneNow.rating = 5;
    oneNow.comment = "new";
    oneNow.cues = {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#00FF00", ""},
                   CuePoint{CuePoint::Kind::Hot, 2, 2000.0, "", ""}};
    Track twoNow = rekordboxTrack("2", "Contents/B/Two.mp3", {{"Peak Time", 0}});
    twoNow.rating = 2;
    Track threeNow = rekordboxTrack("3", "Contents/C/Three.mp3", {{"Peak Time", 2}, {"New", 0}});
    in.now = {oneNow, twoNow, threeNow};
    in.playlists = {{"Peak Time", false, 11}, {"New", false, 13}};
    return in;
}

void testNextBaseline()
{
    const NextInput in = nextInput();
    const std::set<std::string> offered = {
        "track:" + Three,  "playlist:11", "playlist:12", "playlist:13", "member:11:" + Three,
        "member:13:" + Three, "rating:" + One, "comment:" + One, "cue:hot:2:" + One, "cue:memory:500:" + One,
        "rating:" + Two,
    };
    const std::set<std::string> applied = {"track:" + Three, "playlist:11", "member:11:" + Three, "rating:" + One,
                                           "cue:hot:2:" + One};
    const std::map<std::string, std::string> declined = {{"comment:" + One, "h1"}, {"playlist:12", "h2"}};
    const RekordboxBaseline next =
        nextBaseline(&in.previous, in.now, in.playlists, 200, offered, applied, declined, stickRelative, lowerKey);

    assert(next.pdbSequence == 200);
    assert(next.engineUuid == "uuid-1");
    assert(next.tracks.size() == 3);

    const auto &one = *next.findTrack(One);
    assert(one.rating == 5 && "applied: rekordbox's value now");
    assert(one.ratingOrigin == ValueOrigin::Unknown && "a new value carries no origin");
    assert(one.comment == "old" && "declined: the previous value");
    // The pad that did not move keeps Seabass's origin (its colour changed,
    // which does not make it another cue); the applied pad is rekordbox's
    // now; the memory cue rekordbox dropped, unresolved, is put back with
    // the export's origin, after the cues of the read.
    assert(one.cues.size() == 3);
    assert(one.cues[0].cue.hotCueNumber == 1 && one.cues[0].cue.color == "#00FF00"
           && one.cues[0].origin == ValueOrigin::Seabass);
    assert(one.cues[1].cue.hotCueNumber == 2 && one.cues[1].cue.positionMs == 2000.0
           && one.cues[1].origin == ValueOrigin::Unknown);
    assert(one.cues[2].cue.kind == CuePoint::Kind::Memory && one.cues[2].cue.positionMs == 500.0
           && one.cues[2].origin == ValueOrigin::Rekordbox);

    const auto &two = *next.findTrack(Two);
    assert(!two.rating && two.ratingOrigin == ValueOrigin::Rekordbox && "unresolved: the previous value and origin");
    assert(next.findTrack(Three) && "the added track is there");

    // "Peak" renamed (applied) with rekordbox's order; "New" (unresolved)
    // taken out; "Warmup" (declined delete) put back, empty, after them.
    assert(next.playlists.size() == 2);
    assert(next.playlists[0].id == 11 && next.playlists[0].path == "Peak Time");
    assert((next.playlists[0].members == Members{Two, One, Three}));
    assert(next.playlists[1].id == 12 && next.playlists[1].path == "Warmup" && next.playlists[1].members.empty());
    assert(!next.findPlaylist(13));

    // The two new declines, and the earlier decline of an item kept again;
    // the stale one of an item not kept is dropped.
    assert((next.declined
            == std::map<std::string, std::string>{
                {"comment:" + One, "h1"}, {"playlist:12", "h2"}, {"rating:" + Two, "h-old"}}));

    // Everything level or applied: rekordbox now, origins carried.
    const RekordboxBaseline level =
        nextBaseline(&in.previous, in.now, in.playlists, 200, {}, {}, {}, stickRelative, lowerKey);
    assert(level.tracks.size() == 3 && level.playlists.size() == 2 && level.declined.empty());
    assert(level.findTrack(One)->cues.size() == 2 && level.findTrack(Two)->rating == 2);

    // No previous baseline: a kept item was absent from it, so it is
    // taken out.
    const RekordboxBaseline first = nextBaseline(nullptr, in.now, in.playlists, 200, {"track:" + Three, "rating:" + Two},
                                                 {}, {}, stickRelative, lowerKey);
    assert(first.tracks.size() == 2 && !first.findTrack(Three));
    assert(!first.findTrack(Two)->rating && first.engineUuid.empty());
    std::cout << "nextBaseline (applied advance, declined and unresolved keep, x entries, origins) OK\n";
}

void testKeepMemberOrder()
{
    const NextInput in = nextInput();
    RekordboxBaseline next = baselineFrom(in.now, in.playlists, 200, stickRelative, lowerKey);
    assert((next.findPlaylist(11)->members == Members{Two, One, Three}));
    // One sat first in "Peak": taken out of next's list and put back at 0.
    seabass::domain::keepPreviousItems(next, &in.previous, {"member:11:" + One, "not a key"});
    assert((next.findPlaylist(11)->members == Members{One, Two, Three}));
    // Three was in no previous list: taken out.
    seabass::domain::keepPreviousItems(next, &in.previous, {"member:11:" + Three});
    assert((next.findPlaylist(11)->members == Members{One, Two}));
    std::cout << "keepPreviousItems (member order) OK\n";
}

void testKeepOrder()
{
    const NextInput in = nextInput();
    RekordboxBaseline next = baselineFrom(in.now, in.playlists, 200, stickRelative, lowerKey);
    assert((next.findPlaylist(11)->members == Members{Two, One, Three}));
    // The order of a playlist: the members the previous record has go
    // back to its order, in the places they take in next; a member it
    // lacks keeps its place, and no member comes or goes.
    seabass::domain::keepPreviousItems(next, &in.previous, {"order:11"});
    assert((next.findPlaylist(11)->members == Members{One, Two, Three}));
    {
        RekordboxBaseline moved = baselineFrom(in.now, in.playlists, 200, stickRelative, lowerKey);
        for (auto &p : moved.playlists) {
            if (p.id == 11) {
                p.members = {Three, Two, One};
            }
        }
        seabass::domain::keepPreviousItems(moved, &in.previous, {"order:11"});
        assert((moved.findPlaylist(11)->members == Members{Three, One, Two}));
    }
    // A playlist the previous record lacks, or no previous record: no
    // order was recorded, so next's stands.
    RekordboxBaseline fresh = baselineFrom(in.now, in.playlists, 200, stickRelative, lowerKey);
    seabass::domain::keepPreviousItems(fresh, &in.previous, {"order:13"});
    seabass::domain::keepPreviousItems(fresh, nullptr, {"order:11"});
    assert((fresh.findPlaylist(11)->members == Members{Two, One, Three}));
    assert((fresh.findPlaylist(13)->members == Members{Three}));
    // Through nextBaseline: an order offered and not applied keeps the
    // previous order, and every member of now.
    const RekordboxBaseline kept = nextBaseline(&in.previous, in.now, in.playlists, 200, {"order:11"}, {}, {},
                                                stickRelative, lowerKey);
    assert((kept.findPlaylist(11)->members == Members{One, Two, Three}));
    std::cout << "keepPreviousItems (a playlist's order, its members left alone) OK\n";
}

void testRecordSeabassWrites()
{
    const NextInput in = nextInput();
    RekordboxBaseline b = baselineFrom(in.now, in.playlists, 200, stickRelative, lowerKey);
    const auto unlisted = seabass::domain::recordSeabassWrites(
        b, {{One, std::vector<CuePoint>{CuePoint{CuePoint::Kind::Hot, 3, 3000.0, "", ""}}, 0},
            {Two, std::nullopt, 4},
            {"contents/x/none.mp3", std::vector<CuePoint>{}, std::nullopt}});
    assert((unlisted == std::vector<std::string>{"contents/x/none.mp3"}));
    const auto &one = *b.findTrack(One);
    assert(one.cues.size() == 1 && one.cues[0].cue.hotCueNumber == 3 && one.cues[0].origin == ValueOrigin::Seabass);
    assert(!one.rating && one.ratingOrigin == ValueOrigin::Seabass && "0 stars is unrated, written by Seabass");
    const auto &two = *b.findTrack(Two);
    assert(two.rating == 4 && two.ratingOrigin == ValueOrigin::Seabass);
    assert(two.cues.empty() && "no cues written, none touched");
    assert(b.findTrack(Three)->ratingOrigin == ValueOrigin::Unknown && "a track not written is left alone");
    std::cout << "recordSeabassWrites (cues and rating of Seabass origin, unlisted reported) OK\n";
}

}  // namespace

int main()
{
    testTracksCarriedWithUnknownOrigin();
    testPlaylistsTreeAndOrder();
    testLookups();
    testItemKeys();
    testNextBaseline();
    testKeepMemberOrder();
    testKeepOrder();
    testRecordSeabassWrites();
    std::cout << "rekordbox_baseline_test: all passed\n";
    return 0;
}
