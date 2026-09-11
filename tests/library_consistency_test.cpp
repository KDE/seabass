// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/library_consistency.hpp"

using namespace seabass::domain;

namespace
{

Track makeTrack(std::string id, std::string filename, std::string title, std::string artist, double duration,
                 std::vector<CuePoint> cues = {})
{
    Track t;
    t.sourceId = std::move(id);
    t.filename = std::move(filename);
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.durationSeconds = duration;
    t.cues = std::move(cues);
    return t;
}

const LibraryConsistencyIssue *findIssueContaining(const std::vector<LibraryConsistencyIssue> &issues,
                                                    const std::string &brokenSourceId)
{
    for (const auto &issue : issues) {
        for (const auto &t : issue.brokenGroup) {
            if (t.sourceId == brokenSourceId) {
                return &issue;
            }
        }
    }
    return nullptr;
}

}  // namespace

int main()
{
    // Case 1: a broken row with a cue-free healthy sibling (different
    // filename, same title+artist, mirrors the real "07_..." survivor
    // vs "11_..." orphan pattern), Repairable, nothing to write onto
    // the survivor.
    {
        Track survivor = makeTrack("847", "07_Domek-In My Head.mp3", "In My Head", "Domek", 245.0);
        Track broken = makeTrack("86", "11_Domek-In My Head.mp3", "In My Head", "Domek", 245.0);

        auto issues = LibraryConsistencyChecker::check({survivor}, {broken});
        const auto *issue = findIssueContaining(issues, "86");
        assert(issue != nullptr);
        assert(issue->kind == LibraryConsistencyIssue::Kind::Repairable);
        assert(issue->survivor.has_value() && issue->survivor->sourceId == "847");
        assert(issue->survivorCues.empty());
        std::cout << "case 1 (broken row with a cue-free healthy sibling) OK\n";
    }

    // Case 2: the broken row has cues the survivor doesn't, Repairable,
    // but survivorCues carries what needs to be written onto the
    // survivor before removing the broken row.
    {
        CuePoint hotCue{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"};
        Track survivor = makeTrack("392", "17_Joris Voorn, Underworld-Too Little Too La.mp3", "Too Little Too Late",
                                    "Joris Voorn, Underworld", 300.0);
        Track broken = makeTrack("644", "10_Joris Voorn, Underworld-Too Little Too La.mp3", "Too Little Too Late",
                                   "Joris Voorn, Underworld", 300.0, {hotCue});

        auto issues = LibraryConsistencyChecker::check({survivor}, {broken});
        const auto *issue = findIssueContaining(issues, "644");
        assert(issue != nullptr);
        assert(issue->kind == LibraryConsistencyIssue::Kind::Repairable);
        assert(issue->survivorCues.size() == 1);
        assert(issue->survivorCues[0].comment == "drop");
        std::cout << "case 2 (broken row's cues get merged onto the survivor) OK\n";
    }

    // Case 3: survivor and broken row both have cues, and they genuinely
    // differ, Conflict, never guessed.
    {
        CuePoint cueA{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"};
        CuePoint cueB{CuePoint::Kind::Hot, 1, 9000.0, "#00ff00", "breakdown"};
        Track survivor = makeTrack("1", "song.mp3", "Song", "Artist", 200.0, {cueA});
        Track broken = makeTrack("2", "song_1.mp3", "Song", "Artist", 200.0, {cueB});

        auto issues = LibraryConsistencyChecker::check({survivor}, {broken});
        const auto *issue = findIssueContaining(issues, "2");
        assert(issue != nullptr);
        assert(issue->kind == LibraryConsistencyIssue::Kind::Conflict);
        std::cout << "case 3 (genuinely differing cues, Conflict, not guessed) OK\n";
    }

    // Case 4: a broken row with no match anywhere in this catalog,
    // Missing.
    {
        Track healthy = makeTrack("1", "unrelated.mp3", "Unrelated Song", "Someone Else", 180.0);
        Track broken = makeTrack("2", "gone.mp3", "Completely Different", "Nobody", 999.0);

        auto issues = LibraryConsistencyChecker::check({healthy}, {broken});
        const auto *issue = findIssueContaining(issues, "2");
        assert(issue != nullptr);
        assert(issue->kind == LibraryConsistencyIssue::Kind::Missing);
        assert(!issue->survivor.has_value());
        std::cout << "case 4 (no match anywhere, Missing) OK\n";
    }

    // Case 5: two broken siblings sharing one healthy survivor, mirrors
    // the real "In My Head" 86+87 -> 847 case exactly. One Repairable
    // issue, both broken rows folded into the same brokenGroup.
    {
        Track survivor = makeTrack("847", "07_Domek-In My Head.mp3", "In My Head", "Domek", 245.0);
        Track brokenA = makeTrack("86", "11_Domek-In My Head.mp3", "In My Head", "Domek", 245.0);
        Track brokenB = makeTrack("87", "11_Domek-In My Head_1.mp3", "In My Head", "Domek", 245.0);

        auto issues = LibraryConsistencyChecker::check({survivor}, {brokenA, brokenB});
        const auto *issueA = findIssueContaining(issues, "86");
        const auto *issueB = findIssueContaining(issues, "87");
        assert(issueA != nullptr && issueB != nullptr);
        assert(issueA == issueB);  // same issue object, one shared brokenGroup
        assert(issueA->kind == LibraryConsistencyIssue::Kind::Repairable);
        assert(issueA->brokenGroup.size() == 2);
        std::cout << "case 5 (two broken siblings share one Repairable issue) OK\n";
    }

    // Case 6: every copy in a duplicate group is broken, Missing, even
    // though DuplicateTrackFinder still recognized them as copies of the
    // same song (there's simply nothing healthy to survive onto).
    {
        Track brokenA = makeTrack("1", "song.mp3", "Song", "Artist", 200.0);
        Track brokenB = makeTrack("2", "song_1.mp3", "Song", "Artist", 200.0);

        auto issues = LibraryConsistencyChecker::check({}, {brokenA, brokenB});
        const auto *issue = findIssueContaining(issues, "1");
        assert(issue != nullptr);
        assert(issue->kind == LibraryConsistencyIssue::Kind::Missing);
        assert(issue->brokenGroup.size() == 2);
        std::cout << "case 6 (every copy broken, Missing, nothing to survive onto) OK\n";
    }

    std::cout << "All library_consistency_test cases passed.\n";
    return 0;
}
