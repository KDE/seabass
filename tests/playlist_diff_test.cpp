// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::diffPlaylists and domain::overlapOf: the set is the finding,
// the order is a side note. Every count below is pinned by hand from
// the lists as written, so a diff that drops or doubles an entry fails
// here rather than in a view that happily shows whatever it is given.

#include "domain/playlist_diff.hpp"

#include <cassert>
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using seabass::domain::DiffEntryKind;
using seabass::domain::diffPlaylists;
using seabass::domain::overlapOf;
using seabass::domain::PlaylistRelation;

namespace
{

using List = std::vector<std::string>;

// The rows as one string per row, "a-kind:index|b-kind:index", so a whole
// layout is checked at once and a failure prints the layout it got.
std::string layout(const seabass::domain::PlaylistDiff &diff)
{
    auto kind = [](DiffEntryKind k) {
        switch (k) {
        case DiffEntryKind::None:
            return "_";
        case DiffEntryKind::Same:
            return "=";
        case DiffEntryKind::Only:
            return "!";
        case DiffEntryKind::Moved:
            return "~";
        }
        return "?";
    };
    std::string out;
    for (const auto &row : diff.rows) {
        if (!out.empty()) {
            out += " ";
        }
        out += kind(row.a.kind) + std::to_string(row.a.index) + "|" + kind(row.b.kind) + std::to_string(row.b.index);
    }
    return out;
}

void expectLayout(const seabass::domain::PlaylistDiff &diff, const std::string &expected)
{
    const std::string got = layout(diff);
    if (got != expected) {
        std::cerr << "layout\n  expected: " << expected << "\n  got:      " << got << "\n";
        assert(false);
    }
}

// What every diff must hold, whatever common subsequence it picked:
// each entry of either list on exactly one row, in list order; a Same
// row pairs equal tracks; a moved entry's partner is a moved entry of the
// same track that points back; copies pair one to one, so the counts are
// the multiset ones (shared = sum of min(copies in A, copies in B)).
void expectInvariants(const List &a, const List &b, const seabass::domain::PlaylistDiff &diff)
{
    std::vector<const seabass::domain::DiffEntry *> entryA(a.size(), nullptr);
    std::vector<const seabass::domain::DiffEntry *> entryB(b.size(), nullptr);
    int lastA = -1;
    int lastB = -1;
    int same = 0;
    int movedA = 0;
    int movedB = 0;
    int onlyA = 0;
    int onlyB = 0;
    for (const auto &row : diff.rows) {
        if (row.a.kind != DiffEntryKind::None) {
            assert(row.a.index == lastA + 1);
            lastA = row.a.index;
            entryA[static_cast<std::size_t>(row.a.index)] = &row.a;
        }
        if (row.b.kind != DiffEntryKind::None) {
            assert(row.b.index == lastB + 1);
            lastB = row.b.index;
            entryB[static_cast<std::size_t>(row.b.index)] = &row.b;
        }
        assert((row.a.kind == DiffEntryKind::Same) == (row.b.kind == DiffEntryKind::Same));
        if (row.a.kind == DiffEntryKind::Same) {
            assert(a[static_cast<std::size_t>(row.a.index)] == b[static_cast<std::size_t>(row.b.index)]);
            ++same;
        }
        movedA += row.a.kind == DiffEntryKind::Moved;
        movedB += row.b.kind == DiffEntryKind::Moved;
        onlyA += row.a.kind == DiffEntryKind::Only;
        onlyB += row.b.kind == DiffEntryKind::Only;
    }
    assert(lastA + 1 == static_cast<int>(a.size()) && lastB + 1 == static_cast<int>(b.size()));
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (entryA[i]->kind == DiffEntryKind::Moved) {
            const int p = entryA[i]->partner;
            assert(p >= 0 && p < static_cast<int>(b.size()));
            assert(entryB[static_cast<std::size_t>(p)]->kind == DiffEntryKind::Moved);
            assert(entryB[static_cast<std::size_t>(p)]->partner == static_cast<int>(i));
            assert(a[i] == b[static_cast<std::size_t>(p)]);
        }
    }
    for (std::size_t j = 0; j < b.size(); ++j) {
        if (entryB[j]->kind == DiffEntryKind::Moved) {
            const int p = entryB[j]->partner;
            assert(p >= 0 && p < static_cast<int>(a.size()));
            assert(entryA[static_cast<std::size_t>(p)]->partner == static_cast<int>(j));
        }
    }
    std::map<std::string, std::pair<int, int>> copies;
    for (const auto &s : a) {
        ++copies[s].first;
    }
    for (const auto &s : b) {
        ++copies[s].second;
    }
    int shared = 0;
    int extraA = 0;
    int extraB = 0;
    for (const auto &[s, c] : copies) {
        shared += std::min(c.first, c.second);
        if (c.second > 0) {
            extraA += std::max(0, c.first - c.second);
        }
        if (c.first > 0) {
            extraB += std::max(0, c.second - c.first);
        }
    }
    assert(movedA == movedB && movedA == diff.moved);
    assert(diff.shared == same + movedA && diff.shared == shared);
    assert(diff.onlyA == onlyA && diff.onlyA == static_cast<int>(a.size()) - shared);
    assert(diff.onlyB == onlyB && diff.onlyB == static_cast<int>(b.size()) - shared);
    assert(diff.extraA == extraA && diff.extraB == extraB);
}

void testIdenticalListsAreOneSameRowPerEntry()
{
    const List a{"x", "y", "z"};
    const auto diff = diffPlaylists(a, a);
    assert(diff.identical());
    assert(diff.shared == 3 && diff.onlyA == 0 && diff.onlyB == 0 && diff.moved == 0);
    expectLayout(diff, "=0|=0 =1|=1 =2|=2");
}

void testAppendedTracksAreOnlyInB()
{
    // The WHALESHARK2 shape: B is A with ten more at the end.
    const List a{"a", "b", "c"};
    const List b{"a", "b", "c", "d", "e"};
    const auto diff = diffPlaylists(a, b);
    assert(!diff.sameTracks());
    assert(diff.shared == 3 && diff.onlyA == 0 && diff.onlyB == 2 && diff.moved == 0);
    expectLayout(diff, "=0|=0 =1|=1 =2|=2 _-1|!3 _-1|!4");
}

void testRemovedTracksAreOnlyInA()
{
    const List a{"a", "b", "c", "d"};
    const List b{"a", "d"};
    const auto diff = diffPlaylists(a, b);
    assert(diff.shared == 2 && diff.onlyA == 2 && diff.onlyB == 0 && diff.moved == 0);
    expectLayout(diff, "=0|=0 !1|_-1 !2|_-1 =3|=1");
}

void testAMovedTrackIsSharedNotMissing()
{
    // "c" went from the end to the front: still in both, so shared and
    // moved, never onlyA or onlyB. One side of the pin is the agreed
    // run a, b; the other c shows twice, once per side, pointing at its
    // partner's index.
    const List a{"a", "b", "c"};
    const List b{"c", "a", "b"};
    const auto diff = diffPlaylists(a, b);
    assert(diff.sameTracks());
    assert(!diff.identical());
    assert(diff.shared == 3 && diff.onlyA == 0 && diff.onlyB == 0 && diff.moved == 1);
    expectLayout(diff, "_-1|~0 =0|=1 =1|=2 ~2|_-1");
    assert(diff.rows[0].b.partner == 2);
    assert(diff.rows[3].a.partner == 0);
}

void testDifferentTracksBetweenPinsShareARow()
{
    // Between the agreed a and d, A has b and B has x then y: top aligned,
    // b beside x, and y alone on the second line.
    const List a{"a", "b", "d"};
    const List b{"a", "x", "y", "d"};
    const auto diff = diffPlaylists(a, b);
    assert(diff.shared == 2 && diff.onlyA == 1 && diff.onlyB == 2 && diff.moved == 0);
    expectLayout(diff, "=0|=0 !1|!1 _-1|!2 =2|=3");
}

void testADuplicateEntryIsTwoEntries()
{
    // B lists "a" twice, A once. The first copy pins against A's "a"; the
    // second is a copy A does not have, so it is only in B, an extra
    // copy, and never a move (nothing in A moved to stand beside it).
    const List a{"a", "b"};
    const List b{"a", "b", "a"};
    const auto diff = diffPlaylists(a, b);
    assert(diff.onlyB == 1 && diff.extraB == 1);
    assert(diff.shared == 2 && diff.moved == 0);
    expectLayout(diff, "=0|=0 =1|=1 _-1|!2");
}

void testAnExtraCopyInBIsNotIdentical()
{
    // The review's case: A = [x], B = [x, x] was reported identical.
    const auto diff = diffPlaylists({"x"}, {"x", "x"});
    assert(!diff.identical());
    assert(!diff.sameTracks());
    assert(diff.shared == 1 && diff.onlyA == 0 && diff.onlyB == 1 && diff.extraB == 1 && diff.moved == 0);
    expectLayout(diff, "=0|=0 _-1|!1");
    // A copy that is out of place and also extra: B = [y, x, x], A = [x, y].
    // One x pins or moves against A's x; the other is extra either way.
    const auto shuffled = diffPlaylists({"x", "y"}, {"y", "x", "x"});
    assert(shuffled.shared == 2 && shuffled.onlyB == 1 && shuffled.extraB == 1 && shuffled.onlyA == 0);
}

void testAnExtraCopyInAIsOnlyInA()
{
    // A = [x, x], B = [x]: one copy pins, the other is A's alone. It was
    // counted shared and moved, partnered with B's pinned row.
    const List a{"x", "x"};
    const List b{"x"};
    const auto diff = diffPlaylists(a, b);
    assert(diff.shared == 1 && diff.onlyA == 1 && diff.extraA == 1 && diff.onlyB == 0 && diff.moved == 0);
    expectLayout(diff, "=0|=0 !1|_-1");
    expectInvariants(a, b, diff);
}

void testMovedCopiesPartnerEachOther()
{
    // A = [x, y, x], B = [y, x, x]: two of the three pin, and the third
    // pair is one track moved. Its partner must be the other unpinned
    // copy, never a copy already pinned to a Same row.
    const List a{"x", "y", "x"};
    const List b{"y", "x", "x"};
    const auto diff = diffPlaylists(a, b);
    assert(diff.shared == 3 && diff.moved == 1 && diff.onlyA == 0 && diff.onlyB == 0);
    expectInvariants(a, b, diff);
    // And the cases above, all of them, hold the same rules.
    expectInvariants({"x"}, {"x", "x"}, diffPlaylists({"x"}, {"x", "x"}));
    expectInvariants({"x", "y"}, {"y", "x", "x"}, diffPlaylists({"x", "y"}, {"y", "x", "x"}));
    expectInvariants({"a", "b", "c"}, {"c", "a", "b"}, diffPlaylists({"a", "b", "c"}, {"c", "a", "b"}));
    expectInvariants({"a", "x", "a", "b", "x"}, {"x", "a", "b", "a", "a"},
                     diffPlaylists({"a", "x", "a", "b", "x"}, {"x", "a", "b", "a", "a"}));
}

void testEmptyLists()
{
    assert(diffPlaylists({}, {}).rows.empty());
    const auto onlyRight = diffPlaylists({}, {"a"});
    assert(onlyRight.onlyB == 1 && onlyRight.rows.size() == 1);
    const auto onlyLeft = diffPlaylists({"a"}, {});
    assert(onlyLeft.onlyA == 1 && onlyLeft.rows.size() == 1);
}

void testOverlapRelations()
{
    const List base{"a", "b", "c"};
    assert(overlapOf(base, base).relation == PlaylistRelation::Identical);
    assert(overlapOf(base, {"c", "b", "a"}).relation == PlaylistRelation::Identical);
    auto sup = overlapOf(base, {"a", "b", "c", "d"});
    assert(sup.relation == PlaylistRelation::Superset && sup.shared == 3 && sup.onlyB == 1 && sup.onlyA == 0);
    auto sub = overlapOf(base, {"a"});
    assert(sub.relation == PlaylistRelation::Subset && sub.shared == 1 && sub.onlyA == 2);
    auto mixed = overlapOf(base, {"a", "z"});
    assert(mixed.relation == PlaylistRelation::Overlap && mixed.shared == 1 && mixed.onlyA == 2 && mixed.onlyB == 1);
    assert(mixed.jaccard > 0.24 && mixed.jaccard < 0.26);
    assert(overlapOf(base, {"x"}).relation == PlaylistRelation::Disjoint);
    assert(overlapOf({}, {}).relation == PlaylistRelation::Disjoint);
    // Distinct tracks, not entries: a doubled entry does not inflate it.
    assert(overlapOf({"a", "a"}, {"a"}).relation == PlaylistRelation::Identical);
}

}  // namespace

int main()
{
    testIdenticalListsAreOneSameRowPerEntry();
    testAppendedTracksAreOnlyInB();
    testRemovedTracksAreOnlyInA();
    testAMovedTrackIsSharedNotMissing();
    testDifferentTracksBetweenPinsShareARow();
    testADuplicateEntryIsTwoEntries();
    testAnExtraCopyInBIsNotIdentical();
    testAnExtraCopyInAIsOnlyInA();
    testMovedCopiesPartnerEachOther();
    testEmptyLists();
    testOverlapRelations();
    std::cout << "playlist_diff_test: ok\n";
    return 0;
}
