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
    return out;
}

}  // namespace

PlaylistDiff diffPlaylists(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const Interned ids = intern(a, b);
    std::unordered_map<int, int> firstInA;
    std::unordered_map<int, int> firstInB;
    for (int i = 0; i < static_cast<int>(ids.a.size()); ++i) {
        firstInA.emplace(ids.a[i], i);
    }
    for (int j = 0; j < static_cast<int>(ids.b.size()); ++j) {
        firstInB.emplace(ids.b[j], j);
    }

    PlaylistDiff diff;
    auto pairs = longestCommonSubsequence(ids.a, ids.b);
    // A closing sentinel so the tail after the last agreed row is laid
    // out by the same loop.
    pairs.emplace_back(static_cast<int>(ids.a.size()), static_cast<int>(ids.b.size()));

    int i = 0;
    int j = 0;
    for (const auto &[ai, bj] : pairs) {
        std::vector<DiffEntry> left;
        std::vector<DiffEntry> right;
        for (; i < ai; ++i) {
            DiffEntry entry;
            entry.index = i;
            auto other = firstInB.find(ids.a[i]);
            if (other == firstInB.end()) {
                entry.kind = DiffEntryKind::Only;
                ++diff.onlyA;
            } else {
                entry.kind = DiffEntryKind::Moved;
                entry.partner = other->second;
                ++diff.shared;
                ++diff.moved;
            }
            left.push_back(entry);
        }
        for (; j < bj; ++j) {
            DiffEntry entry;
            entry.index = j;
            auto other = firstInA.find(ids.b[j]);
            if (other == firstInA.end()) {
                entry.kind = DiffEntryKind::Only;
                ++diff.onlyB;
            } else {
                entry.kind = DiffEntryKind::Moved;
                entry.partner = other->second;
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
        if (ai < static_cast<int>(ids.a.size())) {
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
