// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playlist_diff.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace seabass::domain
{

namespace
{

// Index pairs of one longest common subsequence of a and b, in order.
// Playlists are a few hundred entries at most, so the full n*m table is
// cheap; a and b hold small ints so the comparison is a word compare.
std::vector<std::pair<int, int>> longestCommonSubsequence(const std::vector<int> &a, const std::vector<int> &b)
{
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    std::vector<std::pair<int, int>> out;
    if (n == 0 || m == 0) {
        return out;
    }
    std::vector<std::uint32_t> dp(static_cast<std::size_t>(n + 1) * static_cast<std::size_t>(m + 1), 0);
    auto at = [&](int i, int j) -> std::uint32_t & {
        return dp[static_cast<std::size_t>(i) * static_cast<std::size_t>(m + 1) + static_cast<std::size_t>(j)];
    };
    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            at(i, j) = a[i] == b[j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        }
    }
    int i = 0;
    int j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.emplace_back(i, j);
            ++i;
            ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}

// The same string, the same small int, across both lists.
struct Interned
{
    std::vector<int> a;
    std::vector<int> b;
    int distinct = 0;  // ids run 0 .. distinct - 1
};

Interned intern(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    std::unordered_map<std::string, int> ids;
    auto idOf = [&ids](const std::string &s) {
        auto [it, inserted] = ids.emplace(s, static_cast<int>(ids.size()));
        return it->second;
    };
    Interned out;
    out.a.reserve(a.size());
    out.b.reserve(b.size());
    for (const auto &s : a) {
        out.a.push_back(idOf(s));
    }
    for (const auto &s : b) {
        out.b.push_back(idOf(s));
    }
    out.distinct = static_cast<int>(ids.size());
    return out;
}

}  // namespace

PlaylistDiff diffPlaylists(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const Interned ids = intern(a, b);
    const int n = static_cast<int>(ids.a.size());
    const int m = static_cast<int>(ids.b.size());

    PlaylistDiff diff;
    auto pairs = longestCommonSubsequence(ids.a, ids.b);

    // The copies of each track the common subsequence left unpinned, in
    // list order. The k-th such copy in A and the k-th in B are one entry
    // moved, each the other's partner. A copy beyond what the other side
    // has unpinned is an extra copy that side alone lists: never a move,
    // since nothing on the other side moved to stand for it.
    std::vector<char> pinnedA(static_cast<std::size_t>(n), 0);
    std::vector<char> pinnedB(static_cast<std::size_t>(m), 0);
    for (const auto &[ai, bj] : pairs) {
        pinnedA[static_cast<std::size_t>(ai)] = 1;
        pinnedB[static_cast<std::size_t>(bj)] = 1;
    }
    const auto tracks = static_cast<std::size_t>(ids.distinct);
    std::vector<int> copiesInA(tracks, 0);
    std::vector<int> copiesInB(tracks, 0);
    std::vector<std::vector<int>> looseA(tracks);
    std::vector<std::vector<int>> looseB(tracks);
    for (int i = 0; i < n; ++i) {
        const auto id = static_cast<std::size_t>(ids.a[static_cast<std::size_t>(i)]);
        ++copiesInA[id];
        if (!pinnedA[static_cast<std::size_t>(i)]) {
            looseA[id].push_back(i);
        }
    }
    for (int j = 0; j < m; ++j) {
        const auto id = static_cast<std::size_t>(ids.b[static_cast<std::size_t>(j)]);
        ++copiesInB[id];
        if (!pinnedB[static_cast<std::size_t>(j)]) {
            looseB[id].push_back(j);
        }
    }
    std::vector<int> partnerInB(static_cast<std::size_t>(n), -1);
    std::vector<int> partnerInA(static_cast<std::size_t>(m), -1);
    for (std::size_t id = 0; id < tracks; ++id) {
        const std::size_t moves = std::min(looseA[id].size(), looseB[id].size());
        for (std::size_t k = 0; k < moves; ++k) {
            partnerInB[static_cast<std::size_t>(looseA[id][k])] = looseB[id][k];
            partnerInA[static_cast<std::size_t>(looseB[id][k])] = looseA[id][k];
        }
    }

    // A closing sentinel so the tail after the last agreed row is laid
    // out by the same loop.
    pairs.emplace_back(n, m);

    int i = 0;
    int j = 0;
    for (const auto &[ai, bj] : pairs) {
        std::vector<DiffEntry> left;
        std::vector<DiffEntry> right;
        for (; i < ai; ++i) {
            DiffEntry entry;
            entry.index = i;
            const int partner = partnerInB[static_cast<std::size_t>(i)];
            if (partner >= 0) {
                entry.kind = DiffEntryKind::Moved;
                entry.partner = partner;
                ++diff.shared;
                ++diff.moved;
            } else {
                entry.kind = DiffEntryKind::Only;
                ++diff.onlyA;
                if (copiesInB[static_cast<std::size_t>(ids.a[static_cast<std::size_t>(i)])] > 0) {
                    ++diff.extraA;
                }
            }
            left.push_back(entry);
        }
        for (; j < bj; ++j) {
            DiffEntry entry;
            entry.index = j;
            const int partner = partnerInA[static_cast<std::size_t>(j)];
            if (partner >= 0) {
                entry.kind = DiffEntryKind::Moved;
                entry.partner = partner;
            } else {
                entry.kind = DiffEntryKind::Only;
                ++diff.onlyB;
                if (copiesInA[static_cast<std::size_t>(ids.b[static_cast<std::size_t>(j)])] > 0) {
                    ++diff.extraB;
                }
            }
            right.push_back(entry);
        }
        // Between two agreed rows the two sides are laid out top-aligned,
        // one row per line of the longer side.
        const std::size_t lines = std::max(left.size(), right.size());
        for (std::size_t k = 0; k < lines; ++k) {
            DiffRow row;
            if (k < left.size()) {
                row.a = left[k];
            }
            if (k < right.size()) {
                row.b = right[k];
            }
            diff.rows.push_back(row);
        }
        if (ai < n) {
            DiffRow row;
            row.a.kind = DiffEntryKind::Same;
            row.a.index = ai;
            row.b.kind = DiffEntryKind::Same;
            row.b.index = bj;
            diff.rows.push_back(row);
            ++diff.shared;
        }
        i = ai + 1;
        j = bj + 1;
    }
    return diff;
}

PlaylistOverlap overlapOf(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const std::unordered_set<std::string> setA(a.begin(), a.end());
    const std::unordered_set<std::string> setB(b.begin(), b.end());
    PlaylistOverlap out;
    for (const auto &s : setA) {
        if (setB.count(s)) {
            ++out.shared;
        } else {
            ++out.onlyA;
        }
    }
    out.onlyB = static_cast<int>(setB.size()) - out.shared;
    const int either = out.shared + out.onlyA + out.onlyB;
    out.jaccard = either > 0 ? static_cast<double>(out.shared) / either : 0.0;
    if (out.shared == 0) {
        out.relation = PlaylistRelation::Disjoint;
    } else if (out.onlyA == 0 && out.onlyB == 0) {
        out.relation = PlaylistRelation::Identical;
    } else if (out.onlyA == 0) {
        out.relation = PlaylistRelation::Superset;
    } else if (out.onlyB == 0) {
        out.relation = PlaylistRelation::Subset;
    } else {
        out.relation = PlaylistRelation::Overlap;
    }
    return out;
}

}  // namespace seabass::domain
