// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "domain/audio_content_probe.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/junk_cue.hpp"
#include "domain/matching_policy.hpp"

using namespace seabass::domain;

namespace
{

Track makeTrack(std::string id, std::string title, std::string artist, double durationSeconds,
                 std::string filePath = {})
{
    Track t;
    t.sourceId = std::move(id);
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.durationSeconds = durationSeconds;
    t.filePath = filePath.empty() ? t.sourceId + ".mp3" : std::move(filePath);
    return t;
}

CuePoint makeCue(CuePoint::Kind kind, double positionMs, bool isLoop = false)
{
    CuePoint c;
    c.kind = kind;
    c.positionMs = positionMs;
    c.isLoop = isLoop;
    return c;
}

// Answers from a table, and records what it was asked. Nothing here
// decodes anything: the point of these cases is what the finder does
// with an answer, not how the answer is produced.
class FakeAudioProbe : public AudioContentProbe
{
public:
    std::vector<std::pair<std::string, AudioContentSpan>> answers;
    std::vector<std::string> asked;

    std::optional<AudioContentSpan> measure(const std::string &absoluteFilePath) override
    {
        asked.push_back(absoluteFilePath);
        for (const auto &[path, span] : answers) {
            if (path == absoluteFilePath) {
                return span;
            }
        }
        return std::nullopt;
    }

    void answer(const std::string &path, double total, double lead, double trail)
    {
        answers.push_back({path, AudioContentSpan{total, lead, trail}});
    }
};

}  // namespace

int main()
{
    // Case 1: the defaults are what the code used as constants before
    // any of this was a setting. A change here changes what every
    // existing user's next scan finds, so it is pinned.
    {
        MatchingPolicy::reset();
        assert(MatchingPolicy::exactMatchSeconds() == 2.0);
        assert(MatchingPolicy::compareAudioSeconds() == 10.0);
        assert(MatchingPolicy::ignoreCuesAtStart());
        std::cout << "case 1 (the defaults are 2 s, 10 s and on) OK\n";
    }

    // Case 2: both numbers are clamped into the range Preferences
    // offers, so a hand-edited settings file cannot widen the window
    // past what the page would allow.
    {
        MatchingPolicy::set(-5.0, 500.0, true);
        assert(MatchingPolicy::exactMatchSeconds() == MatchingPolicy::MinExactMatchSeconds);
        assert(MatchingPolicy::compareAudioSeconds() == MatchingPolicy::MaxCompareAudioSeconds);
        MatchingPolicy::set(1000.0, 1000.0, true);
        assert(MatchingPolicy::exactMatchSeconds() == MatchingPolicy::MaxExactMatchSeconds);
        std::cout << "case 2 (both numbers are clamped) OK\n";
    }

    // Case 3: the wider window is never below the exact one. A band
    // that runs from 5 s down to 3 s is empty, so the audio comparison
    // would be switched on and never run -- the shape of quiet
    // do-nothing setting this project keeps finding.
    {
        MatchingPolicy::set(5.0, 3.0, true);
        assert(MatchingPolicy::exactMatchSeconds() == 5.0);
        assert(MatchingPolicy::compareAudioSeconds() == 5.0 && "raised to the exact window, not left below it");
        std::cout << "case 3 (the wider window is never narrower than the exact one) OK\n";
    }

    // Case 4: the exact-match window really is what groups duplicates.
    // Two copies 4 s apart are one group at 5 s and two at 2 s.
    {
        MatchingPolicy::reset();
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", 300.0),
            makeTrack("b", "Song", "Artist", 304.0),
        };
        assert(DuplicateTrackFinder::find(tracks).empty() && "4 s apart is not a match at 2 s");

        MatchingPolicy::set(5.0, 5.0, true);
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.size() == 1 && groups[0].tracks.size() == 2);
        MatchingPolicy::reset();
        std::cout << "case 4 (the exact window decides what groups) OK\n";
    }

    // Case 5: inside the wider window, the audio settles it. Both files
    // hold the same 300 s of music; one carries 6 s of silence the
    // other does not, which is exactly the encoder-padding case this
    // exists for.
    {
        MatchingPolicy::set(2.0, 10.0, true);
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", 300.0, "/stick/a.mp3"),
            makeTrack("b", "Song", "Artist", 306.0, "/stick/b.mp3"),
        };
        assert(DuplicateTrackFinder::find(tracks).empty() && "without a probe, 6 s apart is not a match");

        FakeAudioProbe probe;
        probe.answer("/stick/a.mp3", 300.0, 0.0, 0.0);
        probe.answer("/stick/b.mp3", 306.0, 4.0, 2.0);  // same 300 s of music
        auto groups = DuplicateTrackFinder::find(tracks, &probe);
        assert(groups.size() == 1 && groups[0].tracks.size() == 2);
        assert(probe.asked.size() == 2 && "both files, once each");
        std::cout << "case 5 (silence taken off, the music is the same length) OK\n";
    }

    // Case 6: the audio can also say no. Same stored lengths as case 5,
    // but the music itself differs -- a genuinely longer edit, which is
    // the mistake the whole duration rule exists to prevent.
    {
        MatchingPolicy::set(2.0, 10.0, true);
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", 300.0, "/stick/a.mp3"),
            makeTrack("b", "Song", "Artist", 306.0, "/stick/b.mp3"),
        };
        FakeAudioProbe probe;
        probe.answer("/stick/a.mp3", 300.0, 0.0, 0.0);
        probe.answer("/stick/b.mp3", 306.0, 0.0, 0.0);  // 6 s more music
        assert(DuplicateTrackFinder::find(tracks, &probe).empty());
        std::cout << "case 6 (more music, not more silence, is not a match) OK\n";
    }

    // Case 7: a pair beyond the wider window is not decoded at all.
    // Decoding is the expensive part, and a probe called for every pair
    // that shares a title would make a scan of a real library
    // unusable.
    {
        MatchingPolicy::set(2.0, 10.0, true);
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", 167.0, "/stick/a.mp3"),   // 2:47 radio edit
            makeTrack("b", "Song", "Artist", 391.0, "/stick/b.mp3"),   // 6:31 extended mix
        };
        FakeAudioProbe probe;
        assert(DuplicateTrackFinder::find(tracks, &probe).empty());
        assert(probe.asked.empty() && "nothing was decoded for a pair that is plainly different");
        std::cout << "case 7 (beyond the wider window nothing is decoded) OK\n";
    }

    // Case 8: a probe with no answer is no opinion, never a match. A
    // build with no decoder, a file the backend refused and a file no
    // longer on the stick all land here, and this feeds a caller that
    // offers to delete things.
    {
        MatchingPolicy::set(2.0, 10.0, true);
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", 300.0, "/stick/a.mp3"),
            makeTrack("b", "Song", "Artist", 306.0, "/stick/b.mp3"),
        };
        FakeAudioProbe silent;  // answers nothing
        assert(DuplicateTrackFinder::find(tracks, &silent).empty());

        // One side answering is still not enough.
        FakeAudioProbe half;
        half.answer("/stick/a.mp3", 300.0, 0.0, 0.0);
        assert(DuplicateTrackFinder::find(tracks, &half).empty());
        std::cout << "case 8 (no answer is not a match) OK\n";
    }

    // Case 9: two files that decoded to nothing but silence are not
    // copies of each other. Their content lengths are both zero, which
    // would otherwise "agree" perfectly and group every silent file on
    // a stick with every other. The library this was written against
    // had 114 zero-byte files.
    {
        MatchingPolicy::set(2.0, 10.0, true);
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", 300.0, "/stick/a.mp3"),
            makeTrack("b", "Song", "Artist", 305.0, "/stick/b.mp3"),
        };
        FakeAudioProbe probe;
        probe.answer("/stick/a.mp3", 300.0, 300.0, 0.0);
        probe.answer("/stick/b.mp3", 305.0, 305.0, 0.0);
        assert(DuplicateTrackFinder::find(tracks, &probe).empty());
        std::cout << "case 9 (silence is not a copy of silence) OK\n";
    }

    // Case 10: "Ignore cues at 0:00", off, leaves cues in the first
    // second alone everywhere.
    {
        MatchingPolicy::reset();
        assert(isJunkCue(makeCue(CuePoint::Kind::Memory, 500.0)));
        assert(isJunkCue(makeCue(CuePoint::Kind::Hot, 7.0)));

        MatchingPolicy::set(2.0, 10.0, false);
        assert(!isJunkCue(makeCue(CuePoint::Kind::Memory, 500.0)));
        assert(!isJunkCue(makeCue(CuePoint::Kind::Hot, 7.0)));
        assert(!isJunkCue(makeCue(CuePoint::Kind::Hot, 0.0)));

        // A loop on the first bar is untouched either way, and was
        // already.
        assert(!isJunkCue(makeCue(CuePoint::Kind::Memory, 0.0, true)));
        std::cout << "case 10 (the setting off keeps cues at the start) OK\n";
    }

    // Case 11: a cue at a negative position is junk whatever the
    // setting says. It is a format's "no cue set" sentinel read back as
    // a position, and there is nowhere in the track for it to point --
    // so "keep my cues at 0:00" cannot be read as "hand me a cue I
    // cannot navigate to".
    {
        MatchingPolicy::set(2.0, 10.0, false);
        assert(isJunkCue(makeCue(CuePoint::Kind::Memory, -0.5)));
        assert(isJunkCue(makeCue(CuePoint::Kind::Hot, -1.0)));
        MatchingPolicy::reset();
        assert(isJunkCue(makeCue(CuePoint::Kind::Memory, -0.5)));
        std::cout << "case 11 (a cue before the start is junk either way) OK\n";
    }

    // Case 12: and the finder follows the same rule, not just the
    // predicate -- the two used to be able to disagree only because
    // they share isJunkCue(), and this is what holds that.
    {
        MatchingPolicy::set(2.0, 10.0, false);
        Track t = makeTrack("a", "Song", "Artist", 300.0);
        t.cues = {makeCue(CuePoint::Kind::Memory, 300.0), makeCue(CuePoint::Kind::Hot, -0.5)};
        std::vector<Track> tracks = {t};
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.size() == 1 && "only the negative one, with the setting off");
        assert(issues[0].cue.positionMs < 0.0);
        assert(withoutJunkCues(t.cues).size() == 1);
        MatchingPolicy::reset();
        std::cout << "case 12 (the finder and the filter agree with the setting) OK\n";
    }

    // Case 13: switching the audio comparison ON must never make a
    // group the exact window already found disappear. A=300, B=306,
    // C=308 with the window at 2 s: alone, B and C are 2 s apart and
    // {B, C} is offered. A greedy single pass that consulted the probe
    // inline let the seed A claim B by audio, mark it used, and leave C
    // by itself -- so turning the setting on REMOVED a duplicate pair
    // from Clean Up. Found by /code-review, 2026-09-19.
    //
    // The probe must confirm A and B and REFUSE A and C for this to
    // bite. A first version of this case had all three agree, so the
    // greedy code grouped all three and the test passed against the
    // bug it was written for -- checked by reverting the fix, which is
    // the only way that is ever actually known.
    {
        MatchingPolicy::set(2.0, 10.0, true);
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", 300.0, "/stick/a.mp3"),
            makeTrack("b", "Song", "Artist", 306.0, "/stick/b.mp3"),
            makeTrack("c", "Song", "Artist", 308.0, "/stick/c.mp3"),
        };
        auto without = DuplicateTrackFinder::find(tracks);
        assert(without.size() == 1 && without[0].tracks.size() == 2 && "B and C, 2 s apart");

        FakeAudioProbe probe;
        probe.answer("/stick/a.mp3", 300.0, 0.0, 0.0);    // 300 s of music
        probe.answer("/stick/b.mp3", 306.0, 3.0, 3.0);    // 300 s too: same as A
        probe.answer("/stick/c.mp3", 308.0, 1.5, 1.5);    // 305 s: NOT A's
        auto with = DuplicateTrackFinder::find(tracks, &probe);

        std::set<std::string> groupedWithout;
        for (const auto &g : without) {
            for (const auto &t : g.tracks) {
                groupedWithout.insert(t.sourceId);
            }
        }
        std::set<std::string> groupedWith;
        for (const auto &g : with) {
            for (const auto &t : g.tracks) {
                groupedWith.insert(t.sourceId);
            }
        }
        for (const auto &id : groupedWithout) {
            assert(groupedWith.count(id) == 1 && "the audio must never un-group anything");
        }
        assert(groupedWith.count("a") == 1 && "and A joins them");
        std::cout << "case 13 (the audio only ever adds, never takes away) OK\n";
    }

    // Case 14: case 13 is one instance of a property, so the property
    // itself is checked over randomly generated libraries rather than
    // over hand-picked numbers -- hand-picked numbers are how case 13
    // first managed to agree with the bug.
    //
    // Each track belongs to a real underlying recording. The probe
    // answers with that recording's true length, so the audio agrees
    // exactly when two tracks really are the same recording, while the
    // stored lengths wander. The property: every track grouped without
    // a probe is still grouped with one.
    {
        MatchingPolicy::set(2.0, 12.0, true);
        std::mt19937 rng(20260919);
        std::uniform_int_distribution<int> countDist(2, 6);
        std::uniform_int_distribution<int> recordingDist(0, 2);
        std::uniform_real_distribution<double> padDist(0.0, 14.0);
        const double recordingLengths[3] = {180.0, 300.0, 420.0};

        for (int trial = 0; trial < 400; ++trial) {
            const int count = countDist(rng);
            std::vector<Track> tracks;
            FakeAudioProbe probe;
            for (int i = 0; i < count; ++i) {
                const std::string id(1, static_cast<char>('a' + i));
                const double content = recordingLengths[recordingDist(rng)];
                const double lead = padDist(rng) / 4.0;
                const double trail = padDist(rng);
                const std::string path = "/stick/" + id + ".mp3";
                tracks.push_back(makeTrack(id, "Song", "Artist", content + lead + trail, path));
                probe.answer(path, content + lead + trail, lead, trail);
            }

            std::set<std::string> before;
            for (const auto &g : DuplicateTrackFinder::find(tracks)) {
                for (const auto &t : g.tracks) {
                    before.insert(t.sourceId);
                }
            }
            std::set<std::string> after;
            for (const auto &g : DuplicateTrackFinder::find(tracks, &probe)) {
                for (const auto &t : g.tracks) {
                    after.insert(t.sourceId);
                }
            }
            for (const auto &id : before) {
                if (after.count(id) != 1) {
                    std::cerr << "trial " << trial << ": track " << id
                              << " was grouped by stored lengths and un-grouped once the audio was "
                                 "consulted. Lengths:";
                    for (const auto &t : tracks) {
                        std::cerr << " " << t.sourceId << "=" << t.durationSeconds;
                    }
                    std::cerr << "\n";
                    assert(false && "a probe may only ever add");
                }
            }
        }
        std::cout << "case 14 (that property holds over 400 random libraries) OK\n";
    }

    // Case 15: the backup stores' identity window never widens, however
    // wide the exact-match window is set. LocalCueStore::upsert() takes
    // the first stored row inside it, then DELETEs that row's cues and
    // writes the incoming ones -- so a wider window there lets a 3:00
    // radio edit overwrite the backed-up cues of a 3:25 extended mix
    // filed under the same artist and title, silently. Narrowing is
    // safe and is honoured. Found by /code-review, 2026-09-19.
    {
        MatchingPolicy::set(30.0, 30.0, true);
        assert(MatchingPolicy::exactMatchSeconds() == 30.0);
        assert(MatchingPolicy::backupIdentitySeconds() == 2.0 && "capped, not widened");

        MatchingPolicy::set(1.0, 10.0, true);
        assert(MatchingPolicy::backupIdentitySeconds() == 1.0 && "a stricter setting is obeyed");

        MatchingPolicy::reset();
        assert(MatchingPolicy::backupIdentitySeconds() == 2.0);
        std::cout << "case 15 (the backup stores' window never widens) OK\n";
    }

    std::cout << "All matching_policy tests passed.\n";
    return 0;
}
