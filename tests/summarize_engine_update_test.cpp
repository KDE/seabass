// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// application::summarizeEngineUpdate, the stick card badge's cheap answer
// for Sync after Rekordbox Export: every branch, from hand-built Tracks
// stage rows (no cues), with the planner's own path functions. Each
// expected value is pinned by hand from the input as written.

#include "application/use_cases/summarize_engine_update.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/engine_update_planning.hpp"

using namespace seabass;
using namespace seabass::domain;
using seabass::application::EngineUpdateNeed;
using seabass::application::engineUpdateNeedName;

namespace
{

const std::string Root = "/media/me/STICK";

std::string relativeOf(const std::string &filePath)
{
    return application::stickRelativePathOf(filePath, Root);
}
std::string keyOf(const std::string &relative)
{
    return application::normalizedPathKey(relative);
}

Track track(const std::string &format, const std::string &id, const std::string &relative,
            std::vector<PlaylistMembership> playlists = {})
{
    Track t;
    t.format = format;
    t.sourceId = id;
    t.filePath = Root + "/" + relative;
    t.filename = relative.substr(relative.rfind('/') + 1);
    t.title = t.filename;
    t.playlists = std::move(playlists);
    return t;
}

// rekordbox now: two tracks in "Folder/List" (id 7), b before a; c alone.
std::vector<Track> rekordbox()
{
    std::vector<Track> r;
    r.push_back(track("rekordbox", "1", "Contents/a.mp3", {{"Folder/List", 1, 7}}));
    r.push_back(track("rekordbox", "2", "Contents/b.mp3", {{"Folder/List", 0, 7}}));
    r.push_back(track("rekordbox", "3", "Contents/c.mp3"));
    r[0].rating = 3;
    return r;
}

// Engine with the same files and the same playlist, as the player's
// import leaves it.
std::vector<Track> engine()
{
    std::vector<Track> e;
    e.push_back(track("engine", "11", "Contents/a.mp3", {{"Folder/List", 1, 0}}));
    e.push_back(track("engine", "12", "Contents/b.mp3", {{"Folder/List", 0, 0}}));
    e.push_back(track("engine", "13", "Contents/c.mp3"));
    return e;
}

// The baseline Seabass would have recorded from rekordbox() at `sequence`.
RekordboxBaseline baselineOf(const std::vector<Track> &r, std::uint64_t sequence)
{
    const std::vector<PlaylistInfo> tree = {{"Folder", true, 3}, {"Folder/List", false, 7}, {"Empty", false, 9}};
    BaselineGaps gaps;
    RekordboxBaseline b = baselineFrom(r, tree, sequence, relativeOf, keyOf, &gaps);
    assert(gaps.unkeyedTracks.empty() && gaps.unknownMemberships.empty() && gaps.orphanPlaylists.empty());
    return b;
}

EngineUpdateNeed summarize(const std::vector<Track> &r, const std::vector<Track> &e,
                           const std::optional<RekordboxBaseline> &b, std::uint64_t sequence, std::uint64_t counter)
{
    return application::summarizeEngineUpdate(r, e, b, sequence, counter, relativeOf, keyOf);
}

void decline(RekordboxBaseline &b, const std::string &itemKey, const std::string &state)
{
    b.declined[itemKey] = engineUpdateStateHash(state);
}

void testNames()
{
    assert(engineUpdateNeedName(EngineUpdateNeed::None).empty());
    assert(engineUpdateNeedName(EngineUpdateNeed::Cues) == "cues");
    assert(engineUpdateNeedName(EngineUpdateNeed::Library) == "library");
    std::cout << "names OK\n";
}

void testWithBaseline()
{
    const auto r = rekordbox();
    const auto b = baselineOf(r, 500);
    assert(b.findPlaylist(7)->members.size() == 2 && "the baseline the cases compare against holds the list");

    // Level, and rekordbox has not exported since.
    assert(summarize(r, engine(), b, 500, 500) == EngineUpdateNeed::None);
    // Engine's counter plays no part with a baseline: the baseline says
    // what rekordbox changed.
    assert(summarize(r, engine(), b, 500, 14) == EngineUpdateNeed::None);
    // Nor does Engine's side: it may lack anything, the badge asks only
    // what rekordbox changed since the record.
    assert(summarize(r, {}, b, 500, 500) == EngineUpdateNeed::None);
    // rekordbox exported, nothing in the library moved: only the cue pass
    // can tell whether a cue did.
    assert(summarize(r, engine(), b, 501, 500) == EngineUpdateNeed::Cues);

    // A track added.
    {
        auto more = r;
        more.push_back(track("rekordbox", "4", "Contents/d.mp3"));
        assert(summarize(more, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        // Declined at that state: quiet, the page leaves it out too.
        auto declined = b;
        decline(declined, trackItemKey("contents/d.mp3"), "track:present:Contents/d.mp3");
        assert(summarize(more, engine(), declined, 501, 500) == EngineUpdateNeed::Cues);
        // Declined at another state (it was a different file then): counts.
        auto stale = b;
        decline(stale, trackItemKey("contents/d.mp3"), "track:present:Contents/D.mp3");
        assert(summarize(more, engine(), stale, 501, 500) == EngineUpdateNeed::Library);
    }
    // A track removed.
    {
        auto fewer = r;
        fewer.pop_back();
        assert(summarize(fewer, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        auto declined = b;
        decline(declined, trackItemKey("contents/c.mp3"), "track:absent");
        assert(summarize(fewer, engine(), declined, 501, 500) == EngineUpdateNeed::Cues);
    }
    // A path is a key: the same file under another folder is a change.
    {
        auto moved = r;
        moved[2].filePath = Root + "/Contents/Other/c.mp3";
        assert(summarize(moved, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        // Case alone is not: the key folds it.
        auto cased = r;
        cased[2].filePath = Root + "/Contents/C.MP3";
        assert(summarize(cased, engine(), b, 500, 500) == EngineUpdateNeed::None);
    }
    // A rating.
    {
        auto rated = r;
        rated[1].rating = 5;
        assert(summarize(rated, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        auto declined = b;
        decline(declined, ratingItemKey("contents/b.mp3"), "rating:5");
        assert(summarize(rated, engine(), declined, 501, 500) == EngineUpdateNeed::Cues);
        // Unrated and zero stars are one value.
        auto zero = r;
        zero[1].rating = 0;
        assert(summarize(zero, engine(), b, 500, 500) == EngineUpdateNeed::None);
        auto cleared = r;
        cleared[0].rating.reset();
        assert(summarize(cleared, engine(), b, 501, 500) == EngineUpdateNeed::Library);
    }
    // A comment is not among what the badge compares.
    {
        auto commented = r;
        commented[0].comment = "new";
        assert(summarize(commented, engine(), b, 500, 500) == EngineUpdateNeed::None);
    }
    // A playlist renamed: same id, another path.
    {
        auto renamed = r;
        renamed[0].playlists = {{"Folder/Renamed", 1, 7}};
        renamed[1].playlists = {{"Folder/Renamed", 0, 7}};
        assert(summarize(renamed, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        auto declined = b;
        decline(declined, playlistItemKey(7), "list:Folder/Renamed");
        assert(summarize(renamed, engine(), declined, 501, 500) == EngineUpdateNeed::Cues);
    }
    // The members' order.
    {
        auto reordered = r;
        reordered[0].playlists = {{"Folder/List", 0, 7}};
        reordered[1].playlists = {{"Folder/List", 1, 7}};
        assert(summarize(reordered, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        // Declining one of the two moves leaves the other.
        auto one = b;
        decline(one, memberItemKey(7, "contents/a.mp3"), "member:in:after:");
        assert(summarize(reordered, engine(), one, 501, 500) == EngineUpdateNeed::Library);
        auto both = one;
        decline(both, memberItemKey(7, "contents/b.mp3"), "member:in:after:contents/a.mp3");
        assert(summarize(reordered, engine(), both, 501, 500) == EngineUpdateNeed::Cues);
    }
    // A member added to a list, and one taken out.
    {
        auto added = r;
        added[2].playlists = {{"Folder/List", 2, 7}};
        assert(summarize(added, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        auto removed = r;
        removed[0].playlists.clear();
        assert(summarize(removed, engine(), b, 501, 500) == EngineUpdateNeed::Library);
    }
    // A list nobody names any more: every member left it.
    {
        auto emptied = r;
        emptied[0].playlists.clear();
        emptied[1].playlists.clear();
        assert(summarize(emptied, engine(), b, 501, 500) == EngineUpdateNeed::Library);
    }
    // A playlist the baseline has never seen, by id, and by path when the
    // reader gave no id.
    {
        auto created = r;
        created[2].playlists = {{"New", 0, 12}};
        assert(summarize(created, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        auto unnamed = r;
        unnamed[2].playlists = {{"New", 0, 0}};
        assert(summarize(unnamed, engine(), b, 501, 500) == EngineUpdateNeed::Library);
        // No id from the reader, a path the baseline knows: that playlist.
        auto byPath = r;
        byPath[0].playlists = {{"Folder/List", 1, 0}};
        byPath[1].playlists = {{"Folder/List", 0, 0}};
        assert(summarize(byPath, engine(), b, 500, 500) == EngineUpdateNeed::None);
    }
    // An empty playlist is not seen from memberships: the record's
    // "Empty" (id 9) has no members and nobody names it. Quiet.
    assert(b.findPlaylist(9) != nullptr);
    // Streaming rows and rows off the stick are left out, as the planner
    // leaves them out.
    {
        auto extra = r;
        extra.push_back(track("rekordbox", "5", "Contents/stream.mp3"));
        extra.back().streamingSource = "TIDAL";
        extra.push_back(track("rekordbox", "6", "x.mp3"));
        extra.back().filePath = "/elsewhere/x.mp3";
        assert(summarize(extra, engine(), b, 500, 500) == EngineUpdateNeed::None);
    }
    std::cout << "with a baseline OK\n";
}

void testWithoutBaseline()
{
    const auto r = rekordbox();
    const std::optional<RekordboxBaseline> none;

    // Level, and the player would not offer its import.
    assert(summarize(r, engine(), none, 15132, 15132) == EngineUpdateNeed::None);
    // The counter test: the player would offer the import.
    assert(summarize(r, engine(), none, 15132, 14204) == EngineUpdateNeed::Cues);

    // A rekordbox track Engine lacks: an addition, checked on the page.
    {
        auto e = engine();
        e.pop_back();
        assert(summarize(r, e, none, 15132, 15132) == EngineUpdateNeed::Library);
        assert(summarize(r, e, none, 15132, 14204) == EngineUpdateNeed::Library);
    }
    // A rekordbox playlist path Engine lacks.
    {
        auto e = engine();
        e[0].playlists = {{"Folder/Other", 0, 0}};
        e[1].playlists = {{"Folder/Other", 1, 0}};
        assert(summarize(r, e, none, 15132, 15132) == EngineUpdateNeed::Library);
    }
    // What Engine has on its own is kept on the page, never raised here:
    // an extra track, an extra list, an extra member, another order, a
    // differing rating.
    {
        auto e = engine();
        e.push_back(track("engine", "14", "Contents/own.mp3", {{"Own", 0, 0}}));
        e[2].playlists = {{"Folder/List", 2, 0}};
        e[0].playlists = {{"Folder/List", 0, 0}};
        e[1].playlists = {{"Folder/List", 1, 0}};
        e[0].rating = 1;
        assert(summarize(r, e, none, 15132, 15132) == EngineUpdateNeed::None);
    }
    // An Engine streaming row is no stand-in for a rekordbox file.
    {
        auto e = engine();
        e[2].streamingSource = "TIDAL";
        assert(summarize(r, e, none, 15132, 15132) == EngineUpdateNeed::Library);
    }
    std::cout << "without a baseline OK\n";
}

}  // namespace

int main()
{
    testNames();
    testWithBaseline();
    testWithoutBaseline();
    std::cout << "summarize_engine_update_test OK\n";
    return 0;
}
